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
;   6. Poll GEMDOS Cconis; if ESC pressed, restore the original VBL
;      vector + screen base and rts back to the cartridge dispatcher.

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

; Both halves of _dskbufp ($4C6..$4C9). userfw doesn't do disk I/O,
; so the whole longword is free scratch. The VBL flag claims the low
; word ($4C6); the SR-save scratch for the STE blitter uses the high
; word ($4C8). Two separate uses, two separate addresses, no aliasing.
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

GEMDOS_Cconis         equ 11
GEMDOS_Cnecin         equ 8

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
    ; iteration always blits (counter starts at 0 after chandler_init).
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
    ; ESC exits; any other key is ignored. Cconis returns -1 if a key
    ; is waiting, 0 otherwise.
    move.w  #GEMDOS_Cconis, -(sp)
    trap    #1
    addq.l  #2, sp
    tst.l   d0
    beq     .vbl_loop

    move.w  #GEMDOS_Cnecin, -(sp)
    trap    #1
    addq.l  #2, sp
    cmp.b   #27, d0                   ; ESC
    bne     .vbl_loop

    ; ESC pressed -- restore TOS's VBL handler and screen base, then
    ; return to the cartridge dispatcher (which boots GEM).
    move.l  d3, VBL_VECTOR.w          ; restore TOS VBL
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
; firing. Keyboard input still works because IKBD packets come in
; via the ACIA IRQ ($118), which we leave untouched.
userfw_vbl:
    clr.w   UFW_VBL_FLAG.w
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
