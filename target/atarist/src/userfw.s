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
;      ST screen page selected by A4. The copy is a pure 68000 CPU
;      MOVEM burst expanded inline via FBDRV_INLINE -- same code on
;      plain ST / STE / MegaSTE / TT / Falcon (no _MCH cookie
;      dispatch, no STE blitter path).
;   4. Flip the video base to the screen we just wrote.
;   5. Toggle A4 between SCREEN_A and SCREEN_B for the next frame.
;   6. Poll CMD_MAGIC_SENTINEL_ADDR ($FA4000). The RP-side IKBD demux
;      writes CMD_BOOT_GEM there when it decodes an ESC press. On
;      match, restore vectors / MFP / VBL / screen base and rts back
;      to the cartridge dispatcher.
;
; IRQ ownership: TOS's HBL ($68), Timer-A/B/C/D
; ($134/$120/$114/$110), and ACIA ($118) handlers are stubbed to
; single-rte dummies and their MFP IERA/IERB bits are cleared so no
; MFP source can fire. Only the custom VBL handler at $70 stays
; active.
;
; IKBD bytes are forwarded inline from FBDRV_INLINE. The MOVEM-burst
; framebuffer copy emits an IKBD poll block every FBDRV_IKBD_POLL_EVERY
; iters (~20 HBLs / 1.24 ms): btst the ACIA RX-ready bit, and if set,
; read the byte and emit it via a cart-bus read at IKBD_WINDOW_BASE +
; byte ($FB8200..$FB82FF, md-devops single-byte ABI). The RP captures
; the read via the commemul PIO+DMA ring (no per-read CPU overhead)
; and runs the IKBD demux from its main loop.
;
; --- Constants ----------------------------------------------------

; Atari ST shifter video base registers (68000-compatible, present
; on every ST/STE/MegaSTE/TT/Falcon). Only HIGH+MID are written;
; the STE-only LOW byte at $FFFF820D stays at TOS's default of 0,
; which matches our 256-byte-aligned hidden screens at $70000 and
; $78000.
VIDEO_BASE_ADDR_HIGH  equ $FFFF8201
VIDEO_BASE_ADDR_MID   equ $FFFF8203

; Palette index 0 doubles as the border colour. We poke it at three
; points in the VBL loop so the ST border visualises blit timing
; (cherry-picked from md-sprites-demo). Foreground text in the FB also
; uses idx 0, so during BLIT_MARK_RUNNING the white text momentarily
; becomes black -- harmless since the blit is only a few ms long.
PALETTE_IDX0          equ $FFFF8240
BLIT_MARK_VSYNC       equ $000               ; black: vsync returned, copy not yet started
BLIT_MARK_RUNNING     equ $777               ; white: cart->ST copy in flight
BLIT_MARK_DONE        equ $070               ; green: FBDRV_INLINE returned

; Per-VBL state area at the end of SCREEN_A's 32 KB allocation.
; Used by FBDRV_INLINE to spill A7 (SP) around the MOVEM-burst that
; includes A7 in its register list; the current-page pointer
; UFW_SCREEN_PAGE and the saved TOS VBL vector / Physbase result
; also live here. 16 bytes used; SCREEN_A's tail at $77D00 has 768
; bytes available (shifter only reads 200*160 = 32000 B of each
; screen page, allocation is 32 KB).
UFW_VBL_VEC_SAVE      equ $00077FE0          ; longword: TOS VBL vector ($70)
UFW_PHYSBASE_SAVE     equ $00077FE8          ; longword: XBIOS Physbase result
UFW_SCREEN_PAGE       equ $00077FEC          ; longword: current draw page address
UFW_SP_SAVE           equ $00077FF0          ; longword: SP (A7) shadow during FBDRV_INLINE

; fbdrv iteration arithmetic. Pulled out as equs so the macro body
; below doesn't carry literal magic numbers. FBDRV_TOTAL_BYTES is
; derived from FB_COPY_LINES (defined further down with the other
; framebuffer constants); change FB_COPY_LINES in one place to
; throttle how many ST scanlines the per-VBL copy touches.
;
; FBDRV_TOTAL_BYTES must be divisible by FBDRV_ITER_BYTES (48) so
; the unrolled REPT covers the full byte count without a tail.
; FB_COPY_LINES * 160 byte rows / 48 byte iters: 150*160/48=500,
; 200*160/48=666r32. For values that don't divide evenly the trailing
; bytes are simply not copied (they remain stale on the screen page).
FBDRV_ITER_BYTES      equ 56                            ; 14 longwords: D0-D7 + A0-A4 + A7 (A6=src, A5=dst). A7 saved/restored around the macro.
FBDRV_IKBD_POLL_EVERY equ 40                            ; insert inline IKBD poll every Nth MOVEM iter. 40 iters * ~31us = ~1.24ms (~20 HBLs).
FBDRV_TOTAL_BYTES     equ (FB_COPY_LINES * FB_ROW_BYTES) ; honours FB_COPY_LINES
FBDRV_MAIN_ITERS      equ (FBDRV_TOTAL_BYTES / FBDRV_ITER_BYTES)

;----------------------------------------------------------------
; FBDRV_DISP -- compile-time displacement counter used by the
; FBDRV_INLINE unroll below. Each REPT iteration uses FBDRV_DISP
; as the 16-bit signed displacement into d16(a5) for that block's
; store, then increments by FBDRV_ITER_BYTES. The maximum
; displacement after the last iteration is
; (FBDRV_MAIN_ITERS - 1) * FBDRV_ITER_BYTES, must be inside the
; 16-bit signed range (32767).
FBDRV_DISP            set 0

;----------------------------------------------------------------
; FBDRV_INLINE -- fully unrolled cart->ST screen framebuffer copy.
;
; Each REPT iteration emits:
;   movem.l (a4)+, d0-d7/a0-a3              ; 4 B, 108 cycles
;   movem.l d0-d7/a0-a3, FBDRV_DISP(a5)     ; 6 B, 108 cycles
; FBDRV_DISP is bumped by FBDRV_ITER_BYTES every iteration at
; assembly time, so the destination is reached via a 16-bit signed
; displacement instead of a runtime `add.l #N, a5`. (Predec store
; `movem.l ..., -(a5)` would save 4 cycles + 2 bytes per iter, but
; needs RP-side c2p to write 48-byte chunks in reverse order to
; reconstruct the image correctly -- deferred.)
;
; Caller protocol (must be set up BEFORE the macro expansion):
;   A5 = destination ST screen page ($70000 or $78000 in this
;        template's double-buffer scheme).
;
; Clobbers: D0-D7, A0-A3, A4. A5 is NOT modified (the d16
; displacement varies per iter instead of advancing A5).
;
; Code size: 10 B per unrolled iteration * FBDRV_MAIN_ITERS (500)
; + 6 B setup = ~5 KB inline. Sits comfortably inside userfw's
; 14 KB cart section budget.
FBDRV_INLINE          macro
    movea.l #UFW_FB_SRC, a6
FBDRV_DISP            set 0
FBDRV_POLL_CTR        set 0
    rept    FBDRV_MAIN_ITERS
    movem.l (a6)+, d0-d7/a0-a4/a7
    movem.l d0-d7/a0-a4/a7, FBDRV_DISP(a5)
FBDRV_DISP            set FBDRV_DISP + FBDRV_ITER_BYTES
FBDRV_POLL_CTR        set FBDRV_POLL_CTR + 1
    ifeq    FBDRV_POLL_CTR - FBDRV_IKBD_POLL_EVERY
FBDRV_POLL_CTR        set 0
    ; Inline IKBD poll. Clobbers D0/A0 -- safe because the next
    ; MOVEM iter reloads D0-D7/A0-A4 from cart, and the .after_copy
    ; code after the macro overwrites D0 with UFW_SCREEN_PAGE before
    ; using it.now even 
    btst    #0, ACIA_KBD_STATUS.w
    beq.s   *+18                          ; skip the 16-byte body if no data
    moveq   #0, d0                        ; pre-zero D0 so move.b yields a clean 0..255 word
    lea     IKBD_WINDOW_BASE, a0
    move.b  ACIA_KBD_DATA.w, d0
    tst.b   (a0, d0.w)                    ; emit byte via cart-bus read
    endc
    endr
                      endm

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
FB_COPY_LINES         equ 200
FB_ROW_BYTES          equ 160                 ; 320 px * 4 bpp / 8

; --- IKBD ownership (Epic 3 Story 3.1) -----------------------------

; Keyboard ACIA at $FFFFFC00/02. MIDI ACIA at $FFFFFC04/06 is not
; touched. Status bit 0 = RX-data-ready; bit 1 = TX-empty.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02

; MC68901 MFP registers (subset we manipulate).
MFP_IERA              equ $FFFFFA07          ; interrupt enable A (Timer-A = bit 5)
MFP_IERB              equ $FFFFFA09          ; interrupt enable B
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-A ack = bit 5)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_TACR              equ $FFFFFA19          ; Timer-A control register (cleared at boot for safety)
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register (cleared at boot for safety)

; IRQ vector slots we take over. $70 (VBL) already handled by the
; original userfw code path (D3 holds the save).
VEC_HBL               equ $68
VEC_TIMERD            equ $110
VEC_TIMERC            equ $114
VEC_ACIA              equ $118
VEC_TIMERB            equ $120
VEC_TIMERA            equ $134

; IKBD cart-bus emit window (Epic 3 W1, ROM3). The inline IKBD poll
; in FBDRV_INLINE reads (IKBD_WINDOW_BASE + byte).b to forward `byte`
; to RP; the RP side filters commemul ring samples whose low 16 bits
; fall in [$8200, $8300) and extracts the IKBD byte from the low 8
; bits.
IKBD_WINDOW_BASE      equ $FB8200

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
    move.l  d0, UFW_PHYSBASE_SAVE    ; saved screen base lives in RAM now

    ; Save TOS's VBL vector and install ours. We're in supervisor mode
    ; (entered via CA_INIT) so writing $70.w is legal.
    move.l  VBL_VECTOR.w, UFW_VBL_VEC_SAVE   ; TOS VBL vector saved in RAM
    lea     userfw_vbl(pc), a0
    move.l  a0, VBL_VECTOR.w

    ; --- IKBD ownership setup (Epic 3 Story 3.1) ------------------
    ;
    ; A5 = save area pointer (physbase - 32). Used at boot to save
    ; the 6 IRQ vectors + MFP IER/IMR; ESC exit recomputes A5 from
    ; UFW_PHYSBASE_SAVE before reading the save area, so A5 doesn't
    ; need to survive the per-VBL FBDRV_INLINE expansion.
    movea.l UFW_PHYSBASE_SAVE, a5
    lea     -UFW_SAVE_SIZE(a5), a5

    ; The command sentinel at CMD_MAGIC_SENTINEL is RP-owned (m68k
    ; can't write to the cart shared region) and is zeroed by the
    ; RP's ERASE_FIRMWARE_IN_RAM at boot, so we don't need to clear
    ; it from here. It's already CMD_NOP=0 on first userfw entry.

    ; Mask all maskable IRQs while we rewrite vectors + MFP state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr

    ; Save the 6 vectors we're about to overwrite ($70 already saved
    ; to UFW_VBL_VEC_SAVE above).
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

    ; Install dummies at HBL / Timer-A / Timer-B / Timer-C / Timer-D
    ; / ACIA. userfw_dummy_irq is a single rte; PC-relative for the
    ; same runtime-vs-link-address reason userfw_vbl uses lea(pc).
    ; With IERA/IERB cleared below no MFP source actually fires, but
    ; the dummies cover the boot window between vector install and
    ; the IERA/IERB clears.
    lea     userfw_dummy_irq(pc), a0
    move.l  a0, VEC_HBL.w
    move.l  a0, VEC_TIMERD.w
    move.l  a0, VEC_TIMERC.w
    move.l  a0, VEC_ACIA.w
    move.l  a0, VEC_TIMERB.w
    move.l  a0, VEC_TIMERA.w

    ; Stop both timers (kills any prior TOS event).
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w

    ; Disable + mask everything in MFP A/B. No MFP IRQ ever fires
    ; under userfw -- only the VBL ($70) at IPL 4 reaches the m68k.
    clr.b   MFP_IERA.w
    clr.b   MFP_IERB.w
    clr.b   MFP_IMRA.w
    clr.b   MFP_IMRB.w

    ; Interrupts back on (caller's level, typically $2300).
    move.w  (sp)+, sr

    ; Initialise the hidden-page pointer. UFW_SCREEN_PAGE holds the
    ; page currently being drawn into; .after_copy toggles it between
    ; UFW_SCREEN_A and UFW_SCREEN_B via XOR with UFW_SCREEN_XOR.
    move.l  #UFW_SCREEN_A, UFW_SCREEN_PAGE

    ; Shifter base HIGH byte ($07) is the same for both screen pages
    ; ($70000 and $78000), so we write it ONCE here and only update
    ; the MID byte per VBL in .after_copy below (saves ~20 cyc/VBL).
    move.b  #(UFW_SCREEN_A >> 16), VIDEO_BASE_ADDR_HIGH.w

    ; Pin IRQ state for the duration of .vbl_loop:
    ;   - MFP IERA/IERB cleared: no MFP source (Timer-A/B/C/D, HBL,
    ;     ACIA, etc.) can generate an interrupt request. Boot above
    ;     already cleared these; reasserting here is a belt-and-
    ;     suspenders guarantee.
    ;   - SR = $2300: supervisor mode, IPL=3. Blocks levels 1-3 (HBL
    ;     at IPL 2), allows VBL at IPL 4 and MFP at IPL 6. MFP can't
    ;     fire anyway since IERA/IERB are 0, so in practice only the
    ;     VBL ($70) IRQ reaches the m68k.
    move.b  #0, MFP_IERA.w
    move.b  #0, MFP_IERB.w
    move.w  #$2300, sr

    ; --- Per-VBL loop ---
.vbl_loop:
    ; CPU-halt wait for the next vsync. `stop #$2300` loads SR with
    ; #$2300 (supervisor, IPL=3) and halts the m68k until an IRQ at
    ; level > 3 arrives. MFP IERA/IERB are all 0 so the only IRQ
    ; that can fire is the VBL at IPL=4 -- when it does, userfw_vbl
    ; runs and RTEs to the instruction below. No spin, no memory
    ; traffic on the bus while we wait.
    stop    #$2300

    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark

    ; A5 = the screen page we are about to fill (loaded from the
    ; RAM-resident UFW_SCREEN_PAGE). FBDRV_INLINE uses A5 as a
    ; dst-pointer base with d16 displacements.
    movea.l UFW_SCREEN_PAGE, a5

    ; Pure 68000 CPU copy via the FBDRV_INLINE macro (defined in
    ; the constants block). Same code path on plain ST / STE /
    ; MegaSTE / TT / Falcon -- no _MCH cookie dispatch, no STE
    ; blitter.
    ;
    ; FBDRV_INLINE clobbers D0-D7, A0-A4, A6, A7. A7 (SP) is saved to
    ; UFW_SP_SAVE and restored around the macro -- a VBL firing with
    ; a corrupted A7 would push to a garbage SP and crash. A6 is the
    ; macro's own src pointer (overwritten at macro entry) so no save
    ; is needed. D0-D7 / A0-A4 are scratch and not consumed after.
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = white (blit in flight)
    move.l  a7, UFW_SP_SAVE
    FBDRV_INLINE                      ; inline cart->ST screen copy
    movea.l UFW_SP_SAVE, a7
    move.w  #BLIT_MARK_DONE, PALETTE_IDX0.w     ; border = green (copy done)

.after_copy:

    ; Flip the video base to the just-written page. A5 still holds
    ; UFW_SCREEN_PAGE (preserved by FBDRV_INLINE). Only the MID byte
    ; of the screen base differs between the two pages -- HIGH was
    ; written once at boot (constant $07 for both $70000 / $78000).
    move.l  a5, d0
    move.l  d0, d1
    lsr.w   #8, d1
    move.b  d1, VIDEO_BASE_ADDR_MID.w

    ; Toggle UFW_SCREEN_PAGE between SCREEN_A and SCREEN_B for the
    ; next frame.
    eor.l   #UFW_SCREEN_XOR, d0
    move.l  d0, UFW_SCREEN_PAGE

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

    ; Recompute the save-area pointer from UFW_PHYSBASE_SAVE in
    ; case anything clobbered A5 during the run.
    movea.l UFW_PHYSBASE_SAVE, a5
    lea     -UFW_SAVE_SIZE(a5), a5

    ; Stop both timers so no IRQ can fire mid-restore.
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w

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

    ; Restore TOS's VBL vector ($70 save from UFW_VBL_VEC_SAVE).
    move.l  UFW_VBL_VEC_SAVE, VBL_VECTOR.w

    ; Restore SR to TOS's usual IPL=3 (matches md-oric main.s:230).
    ; From here on TOS handles HBL / Timer / ACIA again -- IKBD will
    ; re-pump GEMDOS's keyboard buffer, GEM mouse cursor revives, etc.
    move.w  #$2300, sr

    ; Restore screen base via XBIOS Setscreen.
    move.w  #-1, -(sp)                ; no rez change
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; physical screen
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; logical screen
    move.w  #5, -(sp)                 ; XBIOS Setscreen
    trap    #14
    lea     12(sp), sp
    rts

; -------------------------------------------------------------------
; userfw_vbl -- minimal VBL interrupt handler. Just RTEs (wakes the
; m68k from the `stop #$2300` in .vbl_loop). Uses zero registers,
; zero stack beyond the SR+PC that the m68k pushed on IRQ entry.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. The ACIA IRQ ($118) is stubbed too, so GEMDOS's keyboard
; buffer is no longer filled; ESC detection runs through the inline
; IKBD poll in FBDRV_INLINE.
userfw_vbl:
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134, Timer-C $114, Timer-D $110, ACIA
; $118). Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
userfw_dummy_irq:
    rte
