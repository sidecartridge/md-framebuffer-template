; userfw.s -- user firmware module.
;
; Entry point: USERFW ($FA0800), reached from main.s either directly
; (early-boot fast path) or via the rom_function dispatcher when the
; RP issues CMD_START.
;
; Pipeline (per VBL):
;   1. Custom VBL handler at $70 (installed once at boot) wakes the
;      main loop by clearing a flag in ST RAM. We DON'T use XBIOS
;      Vsync (trap #14, #37) -- that trips through TOS's GEMDOS-aware
;      dispatch and adds latency / jitter.
;   2. Read FB_FRAME_COUNTER_ADDR ($FA400C). The RP increments this
;      after every fb_render_frame() with a memory barrier. If
;      unchanged since last iteration (D4), the FB has nothing new
;      and we skip the blit + flip entirely.
;   3. Copy the 32 KB cart framebuffer ($FA8300) into the hidden
;      ST screen page selected by A4. The copy path is picked once
;      at boot by walking the _MCH cookie:
;        - plain ST (D7=0): jsr fbdrv  -- MOVEM-loop copy.
;        - STE/MegaSTE/TT/Falcon (D7=1): INLINE blitter in HOG mode.
;      We do NOT jsr to a cart-ROM blitter subroutine: a previous
;      iteration of this file lived at $FA3C00 and bombed with
;      "Illegal Instruction" (4 bombs) on STE -- after HOG completes
;      and CPU prefetch resumes on the cart bus, RTS fetches garbage.
;      md-sprites-demo dodges this by running its whole main loop from
;      ST RAM (start_rom_code is copied there from pre_auto); we dodge
;      it by keeping the blitter setup inline in userfw.
;   4. Flip the video base to the screen we just wrote.
;   5. Toggle A4 between SCREEN_A and SCREEN_B for the next frame.
;   6. Poll CMD_MAGIC_SENTINEL_ADDR ($FA4000). The RP-side IKBD demux
;      writes CMD_BOOT_GEM there when it decodes an ESC press. On
;      match, restore vectors / MFP / VBL / screen base and rts back
;      to the cartridge dispatcher.
;
; Epic 3 IKBD ownership: TOS's HBL ($68), Timer-A/C/D
; ($134/$114/$110), and ACIA ($118) handlers are stubbed to single-
; rte dummies. Timer-B ($120) is reprogrammed to fire every 10 HBLs
; (~1500 Hz) into userfw_timerb_ikbd, which drains the ACIA RX buffer
; and forwards each byte to RP via a single cart-bus read of
; IKBD_WINDOW_BASE + byte (md-devops single-byte ABI). Story 3.5
; moved ESC detection to the RP-side demux; the m68k Timer-B handler
; is now state-free (just forwards bytes).
;
; IKBD window now lives at $FB8200..$FB82FF (ROM3) instead of in
; ROM4. ROM3 reads are captured by the dedicated commemul PIO+DMA
; ring with no per-read CPU overhead -- the RP simply polls the ring
; from the main loop. The earlier ROM4 DMA-IRQ approach lost bytes
; during fbdrv blits because the IRQ rate (millions/sec from cart
; reads under blit load) saturated the Cortex-M0+; switching to a
; dedicated capture buffer eliminated the loss.
;
; Keyboard-only configuration: at boot we send the IKBD reset
; ($80 $01), wait ~110 ms for the self-test, then $12 (disable
; mouse) and $1A (disable joysticks). Both mouse and joystick
; decoding proved unreliable in practice (same byte-loss / demux
; desync class) and are documented as deferred in
; docs/epics/epic-99-backlog.md.

; --- Constants ----------------------------------------------------

; Atari ST shifter / blitter / video base registers.
BLT_BASE              equ $FFFF8A00
BLT_SRC_XINC          equ (BLT_BASE + $20)
BLT_SRC_YINC          equ (BLT_BASE + $22)
BLT_SRC_ADDR          equ (BLT_BASE + $24)
BLT_ENDMASK1          equ (BLT_BASE + $28)
BLT_ENDMASK2          equ (BLT_BASE + $2A)
BLT_ENDMASK3          equ (BLT_BASE + $2C)
BLT_DST_XINC          equ (BLT_BASE + $2E)
BLT_DST_YINC          equ (BLT_BASE + $30)
BLT_DST_ADDR          equ (BLT_BASE + $32)
BLT_XCNT              equ (BLT_BASE + $36)
BLT_YCNT              equ (BLT_BASE + $38)
BLT_HOP               equ (BLT_BASE + $3A)
BLT_OP                equ (BLT_BASE + $3B)
BLT_CTRL              equ (BLT_BASE + $3C)
BLT_SKEW              equ (BLT_BASE + $3D)
BLT_HOG_MODE          equ %11000000          ; BUSY | HOG

VIDEO_BASE_ADDR_HIGH  equ $FFFF8201
VIDEO_BASE_ADDR_MID   equ $FFFF8203
VIDEO_BASE_ADDR_LOW   equ $FFFF820D          ; STE-only

; Palette index 0 doubles as the border colour. We poke it at three
; points in the VBL loop so the ST border visualises blit timing
; (cherry-picked from md-sprites-demo). Foreground text in the FB also
; uses idx 0, so during BLIT_MARK_RUNNING the white text momentarily
; becomes black -- harmless since the blit is only a few ms long.
PALETTE_IDX0          equ $FFFF8240
BLIT_MARK_VSYNC       equ $000               ; black: vsync returned, copy not yet started
BLIT_MARK_RUNNING     equ $777               ; white: cart->ST copy in flight
BLIT_MARK_DONE_ST     equ $070               ; green: ST path done (jsr fbdrv returned)
BLIT_MARK_DONE_STE    equ $005               ; blue:  STE path done (blitter HOG returned)

; Both halves of _dskbufp ($4C6..$4C9). userfw doesn't do disk I/O so
; the whole longword is free scratch. The VBL flag claims the low
; word ($4C6); the SR-save scratch for the STE blitter uses the high
; word ($4C8). Distinct uses, distinct addresses, no aliasing.
UFW_VBL_FLAG          equ $4C6               ; cleared by userfw_vbl, polled by main loop
UFW_DSKBUFP           equ $4C8               ; SR save scratch for STE blitter

; Atari ST VBL interrupt vector. Replacing TOS's handler here drops
; mouse / cursor-blink / keyboard-repeat updates -- harmless for the
; framebuffer template because we own the screen until ESC exit.
VBL_VECTOR            equ $70

; FB dirty-frame counter (lives in cart shared region at $FA400C). The
; RP fills the framebuffer and then writes a new value here as the
; LAST step of the frame. If this matches D4 (last seen) we skip the
; cart->ST blit + video flip entirely.
FB_FRAME_COUNTER      equ $00FA400C

; RP→m68k command sentinel at $FA4000. The RP IKBD demux writes
; CMD_BOOT_GEM here when it decodes an ESC keypress; userfw's main
; loop polls and exits back to GEM on match (Story 3.5). Must agree
; with main.s's CMD_MAGIC_SENTINEL_ADDR / CMD_BOOT_GEM equs.
CMD_MAGIC_SENTINEL    equ $00FA4000
CMD_BOOT_GEM          equ 2

; Screen pages live just below TOS RAM top (TT-style 256 KB ST RAM
; assumption -- screens land at $70000/$78000, matching md-sprites-demo).
UFW_SCREEN_A          equ $00070000
UFW_SCREEN_B          equ $00078000
UFW_SCREEN_XOR        equ (UFW_SCREEN_A ^ UFW_SCREEN_B)

UFW_FB_SRC            equ $00FA8300           ; FRAMEBUFFER_ADDR

; Number of 320-px lines the cart->ST blit covers per frame. Full ST
; low-res is 200; copying fewer leaves the bottom band of the
; destination ST page untouched (useful for a status row or to bound
; the blitter cost).
FB_COPY_LINES         equ 192
FB_ROW_BYTES          equ 160                 ; 320 px * 4 bpp / 8

; --- IKBD ownership (Epic 3 Story 3.1) -----------------------------

; Keyboard ACIA at $FFFFFC00/02. MIDI ACIA at $FFFFFC04/06 is not
; touched. Status bit 0 = RX-data-ready; bit 1 = TX-empty.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02

; MC68901 MFP registers (subset we manipulate).
MFP_IERA              equ $FFFFFA07          ; interrupt enable A (Timer-B = bit 0)
MFP_IERB              equ $FFFFFA09          ; interrupt enable B
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-B ack = bit 0)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register
MFP_TBDR              equ $FFFFFA21          ; Timer-B data register

; IRQ vector slots we take over. $70 (VBL) already handled by the
; original userfw code path (D3 holds the save).
VEC_HBL               equ $68
VEC_TIMERD            equ $110
VEC_TIMERC            equ $114
VEC_ACIA              equ $118
VEC_TIMERB            equ $120
VEC_TIMERA            equ $134

; Timer-B programming: HBL event-count mode, fires every N HBL events.
; N=10 matches md-oric (TIMERB_COUNT_SCAN_LINES); ~1500 Hz at 50 Hz / 313 lines.
TIMERB_HBL_DIVIDER    equ 10
TIMERB_CTRL_EVENT     equ $08                ; control value: event-count mode

; IKBD cart-bus emit window (Epic 3 W1, ROM3). The Timer-B handler
; reads (IKBD_WINDOW_BASE + byte).b to forward `byte` to RP; the RP
; side filters commemul ring samples whose low 16 bits fall in
; [$8200, $8300) and extracts the IKBD byte from the low 8 bits.
IKBD_WINDOW_BASE      equ $FB8200

; IKBD commands used at boot.
IKBD_CMD_RESET_HDR    equ $80    ; reset command header (followed by $01)
IKBD_CMD_RESET_RUN    equ $01    ; perform reset + self-test
IKBD_CMD_MOUSE_OFF    equ $12    ; disable mouse reporting
IKBD_CMD_JOY_OFF      equ $1A    ; disable joysticks

; IKBD self-test takes ~100 ms. Wait counter is iterations of a
; (subq.l + bne.s) inner loop, ~18 cycles each at 8 MHz m68k =>
; ~50000 iterations covers ~110 ms with margin. During the wait the
; Timer-B handler runs normally and drains the eventual $F1 reset
; ack into the RP demux (where it currently appears as a ghost
; "release of $71" -- harmless, no app cares).
IKBD_RESET_WAIT_ITERS equ 50000

; Save area for vectors + MFP regs we'll restore on ESC exit. Lives
; in the top 32 bytes of the 4 KB copied-code area below ST screen
; memory (pre_auto in main.s relocates start_rom_code..end_rom_code
; into that area; the bootstrap occupies the bottom ~1 KB, leaving
; the top free). A5 holds the pointer (physbase - UFW_SAVE_SIZE)
; throughout the userfw run; the exit path recomputes from D6
; (physbase save) in case anything clobbered A5.
;   offset  0: $68  HBL vector save (long)
;   offset  4: $110 Timer-D vector save (long)
;   offset  8: $114 Timer-C vector save (long)
;   offset 12: $118 ACIA vector save (long)
;   offset 16: $120 Timer-B vector save (long)
;   offset 20: $134 Timer-A vector save (long)
;   offset 24: MFP IERA save (byte)
;   offset 25: MFP IERB save (byte)
;   offset 26: MFP IMRA save (byte)
;   offset 27: MFP IMRB save (byte)
;   offset 28-31: reserved / padding (longword align)
UFW_SAVE_SIZE         equ 32

    section text

userfw:
    ; --- Boot setup (runs once) ---

    ; Save the original screen base so we can restore it on ESC exit.
    move.w  #2, -(sp)                ; XBIOS Physbase
    trap    #14
    addq.l  #2, sp
    move.l  d0, d6                   ; D6 = saved screen base

    ; Save TOS's VBL vector and install ours. We're in supervisor mode
    ; (entered via CA_INIT) so writing $70.w is legal.
    ;
    ; userfw_vbl's absolute address must be computed PC-relative: vlink
    ; resolves bare `#userfw_vbl` against the output-binary offset
    ; ($093C) instead of the runtime cart address ($FA093C), because the
    ; linker script places .text_userfw at file offset $0800, not at
    ; absolute $FA0800. `lea label(pc), An` gives the runtime address
    ; at execution time, regardless of where the code lives in cart.
    move.l  VBL_VECTOR.w, d3         ; D3 = saved TOS VBL vector
    lea     userfw_vbl(pc), a0
    move.l  a0, VBL_VECTOR.w

    ; D4 = last seen FB frame counter. Init to -1 so the first
    ; iteration always blits (counter starts at 0 from fb_init).
    moveq   #-1, d4

    ; Walk the cookie jar for _MCH. D7 = 0 on plain ST, 1 on STE-class.
    moveq   #0, d7
    move.l  $5A0.w, d1
    beq.s   .mch_done
    movea.l d1, a0
.mch_loop:
    move.l  (a0)+, d1
    beq.s   .mch_done
    cmp.l   #'_MCH', d1
    beq.s   .mch_found
    addq.w  #4, a0
    bra.s   .mch_loop
.mch_found:
    move.l  (a0), d1
    swap    d1                       ; high word into low half
    tst.w   d1
    beq.s   .mch_done                ; high word == 0 => plain ST
    moveq   #1, d7
.mch_done:

    ; --- IKBD ownership setup (Epic 3 Story 3.1) ------------------
    ;
    ; A5 = save area pointer (physbase - 32). Stays valid for the
    ; whole userfw run; fbdrv preserves A5 per its calling contract
    ; (clobbers D0-D7, A3, A4 only), the STE blitter and
    ; set_video_base don't touch address registers above A4.
    movea.l d6, a5
    lea     -UFW_SAVE_SIZE(a5), a5

    ; The command sentinel at CMD_MAGIC_SENTINEL is RP-owned (m68k
    ; can't write to the cart shared region) and is zeroed by the
    ; RP's ERASE_FIRMWARE_IN_RAM at boot, so we don't need to clear
    ; it from here. It's already CMD_NOP=0 on first userfw entry.

    ; Mask all maskable IRQs while we rewrite vectors + MFP state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr

    ; Save the 6 vectors we're about to overwrite ($70 already in D3).
    move.l  VEC_HBL.w, 0(a5)
    move.l  VEC_TIMERD.w, 4(a5)
    move.l  VEC_TIMERC.w, 8(a5)
    move.l  VEC_ACIA.w, 12(a5)
    move.l  VEC_TIMERB.w, 16(a5)
    move.l  VEC_TIMERA.w, 20(a5)

    ; Save MFP IER / IMR for A and B (4 bytes).
    move.b  MFP_IERA.w, 24(a5)
    move.b  MFP_IERB.w, 25(a5)
    move.b  MFP_IMRA.w, 26(a5)
    move.b  MFP_IMRB.w, 27(a5)

    ; Install dummies at HBL / Timer-A / Timer-C / Timer-D / ACIA.
    ; userfw_dummy_irq is a single rte; PC-relative for the same
    ; runtime-vs-link-address reason userfw_vbl uses lea(pc).
    lea     userfw_dummy_irq(pc), a0
    move.l  a0, VEC_HBL.w
    move.l  a0, VEC_TIMERD.w
    move.l  a0, VEC_TIMERC.w
    move.l  a0, VEC_ACIA.w
    move.l  a0, VEC_TIMERA.w

    ; Install the IKBD Timer-B poll handler.
    lea     userfw_timerb_ikbd(pc), a0
    move.l  a0, VEC_TIMERB.w

    ; Stop Timer-B before reprogramming (kills any prior TOS event).
    clr.b   MFP_TBCR.w

    ; Disable + mask everything in MFP A/B, then re-enable Timer-B
    ; only. This matches md-oric main.s:217-220 -- wholesale wipe is
    ; simpler than picking bits, and we restore the originals on exit.
    clr.b   MFP_IERA.w
    clr.b   MFP_IERB.w
    clr.b   MFP_IMRA.w
    clr.b   MFP_IMRB.w
    bset    #0, MFP_IERA.w
    bset    #0, MFP_IMRA.w

    ; Program Timer-B: fire every TIMERB_HBL_DIVIDER HBL events.
    move.b  #TIMERB_HBL_DIVIDER, MFP_TBDR.w
    move.b  #TIMERB_CTRL_EVENT, MFP_TBCR.w

    ; Drain any stale byte sitting in the ACIA RX buffer (something
    ; the user typed before we took over). Done with IRQs masked so
    ; Timer-B isn't racing us for the same byte.
.ikbd_predrain:
    btst    #0, ACIA_KBD_STATUS.w
    beq.s   .ikbd_predrain_done
    move.b  ACIA_KBD_DATA.w, d0            ; discard
    bra.s   .ikbd_predrain
.ikbd_predrain_done:

    ; Interrupts back on (caller's level, typically $2300). From here
    ; the Timer-B handler is live and any IKBD byte (including the
    ; $F1 reset ack that follows the $80 $01 we're about to send)
    ; flows through to the RP demux.
    move.w  (sp)+, sr

    ; --- IKBD configuration --------------------------------------
    ; Reset the IKBD, wait for self-test, then disable mouse and
    ; joysticks. The byte stream becomes pure scancodes.
    move.b  #IKBD_CMD_RESET_HDR, d0
    bsr.s   .ikbd_tx_byte
    move.b  #IKBD_CMD_RESET_RUN, d0
    bsr.s   .ikbd_tx_byte

    ; Busy-wait for the IKBD self-test to finish (~100 ms). The
    ; Timer-B handler keeps running underneath us, draining the ACIA
    ; into the RP-side demux as bytes arrive (the $F1 reset ack and
    ; any pre-disable mouse/joy noise).
    move.l  #IKBD_RESET_WAIT_ITERS, d0
.ikbd_reset_wait:
    subq.l  #1, d0
    bne.s   .ikbd_reset_wait

    ; Disable mouse and joystick reporting.
    move.b  #IKBD_CMD_MOUSE_OFF, d0
    bsr.s   .ikbd_tx_byte
    move.b  #IKBD_CMD_JOY_OFF, d0
    bsr.s   .ikbd_tx_byte

    bra.s   .ikbd_cfg_done

.ikbd_tx_byte:
    ; In:  D0.B = byte to send to IKBD
    ; Spins until ACIA TX-empty (status bit 1) then writes data.
    btst    #1, ACIA_KBD_STATUS.w
    beq.s   .ikbd_tx_byte
    move.b  d0, ACIA_KBD_DATA.w
    rts

.ikbd_cfg_done:

    ; A4 = hidden screen (the one the next blit writes to).
    movea.l #UFW_SCREEN_A, a4

    ; --- Per-VBL loop ---
.vbl_loop:
    ; Custom VBL wait: set flag = -1 then spin until userfw_vbl
    ; clears it. Cheaper than XBIOS Vsync (no trap dispatch through
    ; TOS) and gives deterministic loop entry timing.
    move.w  #-1, UFW_VBL_FLAG.w
.wait_vbl:
    tst.w   UFW_VBL_FLAG.w
    bne.s   .wait_vbl

    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark

    ; Skip the entire copy + flip if the RP hasn't published a new
    ; frame since our last iteration. This both prevents tearing
    ; (counter only increments after RP's writes commit) and saves
    ; ~4-10 ms / VBL when the FB is idle.
    move.l  FB_FRAME_COUNTER, d0
    cmp.l   d4, d0
    beq     .input_check
    move.l  d0, d4

    ; A3 = the screen page we are about to fill.
    movea.l a4, a3

    tst.w   d7
    beq.s   .copy_st
    bra     .copy_ste

.copy_st:
    ; Plain ST: jsr the MOVEM-loop fbdrv. fbdrv clobbers D0-D7, A3, A4.
    movem.l d6/a4, -(sp)
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = black (blit in flight)
    jsr     $00FA2000                 ; fbdrv (cart $FA2000)
    move.w  #BLIT_MARK_DONE_ST, PALETTE_IDX0.w  ; border = green (ST path done)
    movem.l (sp)+, d6/a4
    bra.s   .after_copy

.copy_ste:
    ; STE/MegaSTE/TT/Falcon: inline blitter copy. Mirrors the working
    ; pattern from md-sprites-demo verbatim (incl. the privileged SR
    ; mask -- pre_auto enters in supervisor mode so this is fine).
    ; Do NOT factor this out into a cart-ROM subroutine: see the filetarge
    ; header for the "4 bombs after RTS" gotcha.
    move.w  sr, UFW_DSKBUFP.w
    ori.w   #$0700, sr                ; mask interrupts

    move.w  #2, BLT_SRC_XINC.w
    move.w  #2, BLT_DST_XINC.w
    clr.w   BLT_SRC_YINC.w
    clr.w   BLT_DST_YINC.w
    move.w  #$FFFF, BLT_ENDMASK1.w
    move.w  #$FFFF, BLT_ENDMASK2.w
    move.w  #$FFFF, BLT_ENDMASK3.w
    clr.b   BLT_SKEW.w
    move.w  #((FB_COPY_LINES * FB_ROW_BYTES) / 2), BLT_XCNT.w
    move.w  #1, BLT_YCNT.w
    move.b  #$2, BLT_HOP.w
    move.b  #$3, BLT_OP.w
    move.l  #UFW_FB_SRC, BLT_SRC_ADDR
    move.l  a3, BLT_DST_ADDR.w
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = black (blit in flight)
    move.b  #BLT_HOG_MODE, BLT_CTRL.w ; kick off; CPU stalls until done
    move.w  #BLIT_MARK_DONE_STE, PALETTE_IDX0.w ; border = blue (STE path done)

    move.w  UFW_DSKBUFP.w, sr         ; restore SR

.after_copy:

    ; Flip the video base to the just-written page.
    move.l  a4, d0
    bsr     set_video_base

    ; Toggle A4 between SCREEN_A and SCREEN_B for the next frame.
    move.l  a4, d0
    eor.l   #UFW_SCREEN_XOR, d0
    movea.l d0, a4

.input_check:
    ; ESC detection (Story 3.5): the RP-side IKBD demux writes
    ; CMD_BOOT_GEM into CMD_MAGIC_SENTINEL on ESC press. Any other
    ; sentinel value (NOP, future commands) leaves the loop running.
    move.l  CMD_MAGIC_SENTINEL, d0
    cmp.l   #CMD_BOOT_GEM, d0
    bne     .vbl_loop

    ; --- ESC pressed: restore IRQ state and return to TOS ---------
    ;
    ; Mask interrupts before touching MFP / vectors.
    ori.w   #$0700, sr

    ; Recompute the save-area pointer from D6 (saved physbase) in
    ; case anything clobbered A5 during the run.
    movea.l d6, a5
    lea     -UFW_SAVE_SIZE(a5), a5

    ; Stop Timer-B so no IRQ can fire mid-restore.
    clr.b   MFP_TBCR.w

    ; Restore MFP IER / IMR.
    move.b  24(a5), MFP_IERA.w
    move.b  25(a5), MFP_IERB.w
    move.b  26(a5), MFP_IMRA.w
    move.b  27(a5), MFP_IMRB.w

    ; Restore the 6 vectors we overwrote.
    move.l  0(a5), VEC_HBL.w
    move.l  4(a5), VEC_TIMERD.w
    move.l  8(a5), VEC_TIMERC.w
    move.l  12(a5), VEC_ACIA.w
    move.l  16(a5), VEC_TIMERB.w
    move.l  20(a5), VEC_TIMERA.w

    ; Restore TOS's VBL vector ($70 save in D3).
    move.l  d3, VBL_VECTOR.w

    ; Restore SR to TOS's usual IPL=3 (matches md-oric main.s:230).
    ; From here on TOS handles HBL / Timer / ACIA again -- IKBD will
    ; re-pump GEMDOS's keyboard buffer, GEM mouse cursor revives, etc.
    move.w  #$2300, sr

    ; Restore screen base via XBIOS Setscreen.
    move.w  #-1, -(sp)                ; no rez change
    move.l  d6, -(sp)                 ; physical screen
    move.l  d6, -(sp)                 ; logical screen
    move.w  #5, -(sp)                 ; XBIOS Setscreen
    trap    #14
    lea     12(sp), sp
    rts

; -------------------------------------------------------------------
; userfw_vbl -- minimal VBL interrupt handler. Just clears the flag
; the main loop is polling on, then RTEs. Uses zero registers, zero
; stack beyond the SR+PC that the m68k pushed on IRQ entry.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. Epic 3 Story 3.1 also stubs the ACIA IRQ ($118), so
; GEMDOS's keyboard buffer is no longer filled either; ESC detection
; runs through userfw_timerb_ikbd's direct ACIA poll instead.
userfw_vbl:
    clr.w   UFW_VBL_FLAG.w
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134, Timer-C $114, Timer-D $110, ACIA
; $118). Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
userfw_dummy_irq:
    rte

; -------------------------------------------------------------------
; userfw_timerb_ikbd -- Timer-B IRQ handler (vector $120).
;
; Triggered every TIMERB_HBL_DIVIDER (=10) horizontal-blank events,
; ~1500 Hz at 50/60 Hz vertical refresh. Drains the IKBD ACIA's
; 1-byte RX buffer, forwarding each received byte to RP via a single
; cart-bus read of `IKBD_WINDOW_BASE + byte` (md-devops single-byte
; ABI, Epic 3 W1).
;
; The drain loop reads until the ACIA RX-ready bit clears -- a burst
; (key + mouse + joystick in the same 10-HBL window) can deposit
; multiple bytes before our next Timer-B fire, and the ACIA has only
; a 1-byte buffer.
;
; Story 3.5: handler is state-free. The RP-side IKBD demux owns ESC
; detection and packet framing; we just forward bytes.
;
; Single byte per fire (md-oric pattern), NOT a drain loop: the
; MC6850 ACIA's RX-ready status bit clears on data-register read but
; needs ~2 us (one E-clock period at 500 kHz) before the change is
; observable. A tight drain loop reads status faster than the ACIA
; can clear it, sees the bit still set, re-reads the same byte, and
; emits duplicates -- which desyncs the RP-side packet demux.
;
; At 1500 Hz Timer-B fire and 780 bytes/sec IKBD max serial rate,
; one-byte-per-fire has ~2x headroom: bytes can't physically arrive
; faster than Timer-B fires, so we never miss a byte.
userfw_timerb_ikbd:
    btst    #0, ACIA_KBD_STATUS.w          ; RX byte available?
    beq.s   .no_data
    movem.l d0/a0, -(sp)
    lea     IKBD_WINDOW_BASE, a0
    move.b  ACIA_KBD_DATA.w, d0            ; read it (clears status bit)
    and.w   #$FF, d0
    tst.b   (a0, d0.w)                     ; emit byte via cart-bus read
    movem.l (sp)+, d0/a0
.no_data:
    bclr    #0, MFP_ISRA.w                 ; ack Timer-B in-service bit
    rte

; -------------------------------------------------------------------
; set_video_base -- write D0.L into the shifter screen-base registers.
;
; In:  D0.L = screen base address (must be even-byte aligned).
;      D7.W = 0 -> ST (writes HIGH+MID only),
;             nonzero -> STE/MegaSTE/TT/Falcon (also writes LOW byte).
; Out: D1 clobbered. D0, D7, A4 preserved.
set_video_base:
    move.l  d0, d1
    swap    d1
    move.b  d1, VIDEO_BASE_ADDR_HIGH.w
    move.l  d0, d1
    lsr.w   #8, d1
    move.b  d1, VIDEO_BASE_ADDR_MID.w
    tst.w   d7
    beq.s   .svb_done
    move.b  d0, VIDEO_BASE_ADDR_LOW.w
.svb_done:
    rts
