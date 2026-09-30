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
;   2. Read the low word of FB_FRAME_COUNTER_ADDR ($FA400C), which the RP
;      bumps as the last write of every publish. If it has not moved
;      since the last blit (UFW_LAST_FRAME), there is nothing new: skip
;      the blit, the flip and the ack (see FB_FRAME_COUNTER_ADDR).
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
; IRQ ownership: TOS's HBL ($68), Timer-A/C/D ($134/$114/$110)
; handlers are stubbed to single-rte dummies and their MFP IERA/IERB
; bits are cleared so they cannot fire. The custom VBL handler at $70,
; Timer-B ($120, audio) and the keyboard ACIA ($118) stay active.
;
; IKBD bytes are forwarded by the keyboard ACIA's receive interrupt
; (userfw_acia_irq, MFP GPIP4): each byte as it arrives, emitted via a
; cart-bus read at IKBD_WINDOW_BASE + byte ($FB8200..$FB82FF). The RP
; captures the read via the commemul PIO+DMA ring (no per-read CPU
; overhead) and runs the IKBD demux from its main loop. The RP also
; decides what the IKBD reports (its input modes): .vbl_loop sends the IKBD
; the commands the RP puts at IKBD_OUT_ADDR.
;
; --- Constants ----------------------------------------------------

; The cartridge window, the command values and the ROM3 signalling windows
; (FB_FRAME_COUNTER_ADDR, CMD_MAGIC_SENTINEL_ADDR, PALETTE_ADDR,
; AUDIO_BUFFER_ADDR, FRAMEBUFFER_ADDR, FB_COPY_LINES, IKBD_WINDOW_BASE,
; VBLSYNC_ADDR...), shared with main.s and checked against the RP's
; cart_shared.h.
	include inc/sidecart_layout.s

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
PALETTE_BASE          equ $FFFF8240          ; 16 hardware palette words ($FFFF8240..$FFFF825E)
PALETTE_IDX0          equ PALETTE_BASE
BLIT_MARK_VSYNC       equ $000               ; black: vsync returned, copy not yet started
BLIT_MARK_RUNNING     equ $777               ; white: cart->ST copy in flight
BLIT_MARK_DONE        equ $070               ; green: FBDRV_INLINE returned

; FBDRV_DEBUG_MARKS = 1 paints palette-idx-0 with the three border
; band colours above (black/white/green) at vsync / blit-running /
; blit-done. Useful for timing measurement on a CRT but flickers
; any visible content drawn in palette idx 0 (incl. the cart-side
; palette publish below) -- keep it 0 for the demos. Set to
; 1 when measuring.
FBDRV_DEBUG_MARKS     equ 0

; TIME_STUDY = 1 turns MFP Timer-A into a free-running stopwatch: /10,
; 4.07 us a tick, its wraps (every 256 ticks) counted by an interrupt
; (userfw_timera_tick, about 1% of the CPU). The VBL and points of the loop
; (the wake, the copy's start and end, the loop's end) report the
; stopwatch's low 16 bits (STUDY_POINT, three ROM3 reads), so every duration
; is a difference on one timeline, even across a VBL. Debug builds of the RP
; keep the points and the slack before the VBL (fb.c; tools/dev/swd.py
; stopwatch). About 40 us a VBL: measuring builds only.
TIME_STUDY            equ 0
MFP_TADR              equ $FFFFFA1F          ; Timer-A data: the count while it runs
TIMERA_STUDY_DIV10    equ 2                  ; Timer-A control: delay mode, /10
STUDY_POINT_WAKE      equ 0
STUDY_POINT_COPY      equ 1
STUDY_POINT_COPIED    equ 2
STUDY_POINT_IDLE      equ 3
STUDY_POINT_VBL       equ 4

; STUDY_POINT n: tells the RP the loop reached point n, with the stopwatch's
; ticks (TIME_STUDY). Clobbers D0-D2 and A1.
STUDY_POINT macro
    ifne    TIME_STUDY
    moveq   #\1, d2
    bsr     study_report
    endc
    endm

; Per-VBL state area at the end of SCREEN_A's 32 KB allocation.
; Used by FBDRV_INLINE to spill A7 (SP) around the MOVEM-burst that
; includes A7 in its register list; the current-page pointer
; UFW_SCREEN_PAGE and the saved TOS VBL vector / Physbase result
; also live here. 20 bytes used; SCREEN_A's tail at $77D00 has 768
; bytes available (shifter only reads 200*160 = 32000 B of each
; screen page, allocation is 32 KB).
; UFW_RESET_STUB: .cold_reset copies userfw_reset_stub here and runs it,
; so the ST's last instructions before its cold reset come from RAM.
UFW_RESET_STUB        equ $00077F00          ; up to $77F7F
; The machine from the hello (family in bits 7..4), whether the DMA sound
; chip plays (-1) or Timer-B and the YM (0), and where the last copy into
; the DMA ring ended (see AUDIO_BUFFER_ADDR, UFW_DMA_RING).
UFW_MACHINE           equ $00077F80          ; word
UFW_AUDIO_DMA         equ $00077F82          ; word
UFW_DMA_FRONT         equ $00077F84          ; word: an offset in the ring
; A blitter found at boot (XBIOS Blitmode): 1, else 0 (see BLIT_MODE_ADDR).
UFW_HAS_BLITTER       equ $00077F86          ; word
; The profile read at boot (PROFILE_ADDR): the DMA chip's lead, the VBLs a
; frame, Timer-B's count, the DMA chip's mode. The VBLs counted by userfw_vbl,
; and the count when the last frame was taken.
UFW_DMA_LEAD          equ $00077F88          ; word
UFW_VBLS_A_FRAME      equ $00077F8A          ; word
UFW_TIMERB_COUNT      equ $00077F8C          ; word
UFW_DMA_MODE          equ $00077F8E          ; word
UFW_VBL_COUNT         equ $00077F90          ; word
UFW_FRAME_VBL         equ $00077F92          ; word
; The stopwatch's wraps (TIME_STUDY).
UFW_SW_WRAPS          equ $00077F94          ; word
UFW_VBL_VEC_SAVE      equ $00077FE0          ; longword: TOS VBL vector ($70)
UFW_PHYSBASE_SAVE     equ $00077FE8          ; longword: XBIOS Physbase result
UFW_SCREEN_PAGE       equ $00077FEC          ; longword: current draw page address
; Low word of FB_FRAME_COUNTER_ADDR at the last blit: .vbl_loop blits only when
; the counter has moved on since (see FB_FRAME_COUNTER_ADDR).
UFW_LAST_FRAME        equ $00077FF0          ; word
; .vbl_loop arms this to -1 then `stop`s; userfw_vbl clears it. The
; m68k re-stops on any non-VBL IRQ (Timer-B etc.) and only exits the
; wait when the VBL handler has cleared the flag. It used to borrow
; TOS's _dskbufp ($4C6); it lives in userfw's own state area now.
UFW_VBL_FLAG          equ $00077FF2          ; word: cleared by userfw_vbl, polled after each `stop`
; The audio slice Timer-B plays this frame (0..AUDIO_SLICES-1); userfw_vbl
; moves to the next one at every VBL.
UFW_AUDIO_SLICE       equ $00077FF4          ; word
; The generation of the IKBD commands .vbl_loop is sending (-1 at boot: no
; RP generation has bit 15 set, so the RP's commands are sent after every
; boot), and the next byte to send (-1: all sent and reported).
UFW_IKBD_OUT_GEN      equ $00077FF6          ; word
UFW_IKBD_OUT_STEP     equ $00077FF8          ; word
; IKBD bytes userfw_acia_irq has read; userfw_vbl reports its low byte.
UFW_IKBD_COUNT        equ $00077FFA          ; word

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
FBDRV_ITER_BYTES      equ 48                            ; 12 longwords: D0-D7 + A1-A4 (A6=src, A5=dst, A0=dedicated audio pointer, A7=SP preserved -- IRQs may fire during the macro).
FBDRV_TOTAL_BYTES     equ (FB_COPY_LINES * FB_ROW_BYTES) ; honours FB_COPY_LINES
FBDRV_MAIN_ITERS      equ (FBDRV_TOTAL_BYTES / FBDRV_ITER_BYTES)
FBDRV_MAIN_BYTES      equ (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)
FBDRV_TAIL_BYTES      equ (FBDRV_TOTAL_BYTES - FBDRV_MAIN_BYTES)  ; 20 bytes at FB_COPY_LINES=200 (= 5 longwords)
FBDRV_TAIL_DISP       equ FBDRV_MAIN_BYTES                        ; tail goes at page_start + FBDRV_MAIN_BYTES (= 31980)

;----------------------------------------------------------------
; FBDRV_INLINE -- fully unrolled cart->ST screen framebuffer copy.
;
; Each REPT iteration emits:
;   movem.l (a6)+, d0-d7/a1-a4   ; 4 B, ~108 cycles  -- read 48 B forward
;   movem.l d0-d7/a1-a4, -(a5)   ; 4 B, ~104 cycles  -- predec store
;
; A7 (SP) and A0 are intentionally NOT in the MOVEM list:
;   - A7: keeps the supervisor SP valid so IRQs can fire safely
;     during the macro.
;   - A0: dedicated to the Timer-B audio handler's read pointer.
;     With A0 stable across the macro, the IRQ handler doesn't have
;     to save/restore it (-24 cyc/IRQ * ~125 IRQ/VBL = ~3000 cyc/VBL).
;
; An IRQ can fire between a MOVEM load and its store, while D0-D7 and
; A1-A4 hold pixels: every handler saves what it uses (A0 aside).
;
; Predec mode is 4 cyc faster per iter than d16(a5) displacement
; (8+8n vs 12+8n on 68000). The catch: predec writes each 52-byte
; chunk into the destination ST page in REVERSE order relative to
; the source -- chunks land from the screen-page END (offset 31980)
; down to the START (offset 0). For the displayed image to look
; correct, the RP-side fb_chunky_to_planar pre-reverses chunks in
; the cart FB at $FA8300, so the m68k's reversal restores the
; natural image. See "Framebuffer chunk layout for the m68k MOVEM
; blit" in rp/src/include/cart_shared.h for the full spec.
;
; Caller protocol (must be set up BEFORE the macro expansion):
;   A5 = destination ST screen page END
;        ($70000 + 31980 or $78000 + 31980; .vbl_loop adds the
;        FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES offset via LEA after
;        loading UFW_SCREEN_PAGE).
;
; Clobbers: D0-D7, A1-A4, A6. A0 and A7 are PRESERVED (A0 for the
; Timer-B audio pointer, A7 for IRQ-safe SP).
; A5 IS modified: after the macro
; A5 = original SCREEN_PAGE end - (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)
; = original page START, which is the value .after_copy expects in A5.
;
; Code size: 8 B per unrolled iteration * FBDRV_MAIN_ITERS (615)
; + 6 B setup = ~5 KB inline, plus the small d16(a5) tail MOVEM at
; the end.
FBDRV_INLINE          macro
    movea.l #FRAMEBUFFER_ADDR, a6
    rept    FBDRV_MAIN_ITERS
    movem.l (a6)+, d0-d7/a1-a4
    movem.l d0-d7/a1-a4, -(a5)
    endr
    ;
    ; Tail: copy the last FBDRV_TAIL_BYTES bytes of the blitted
    ; region that the chunked main loop can't reach (FB_COPY_LINES *
    ; 160 isn't a multiple of FBDRV_ITER_BYTES=48). A6 is at
    ; FRAMEBUFFER_ADDR + FBDRV_MAIN_BYTES after the REPT; A5 is back at
    ; page_start. The RP-side fb_chunky_to_planar leaves these tail
    ; bytes in NATURAL (non-reversed) order in cart-FB, so this is a
    ; straight forward-direction copy via d16(a5).
    ;
    ; The register list must hold exactly FBDRV_TAIL_BYTES/4 longs.
    ; Current `d0-d7` covers 32 bytes (= FB_COPY_LINES=200, 32000 -
    ; 666*48 = 32). Other useful settings:
    ;   FB_COPY_LINES=180 -> 600*48 = 28800 exactly, 0-byte tail
    ;                       (the `ifne` skips the block entirely).
    ;   FB_COPY_LINES=198 -> 660*48 = 31680 exactly, 0-byte tail.
    ;   FB_COPY_LINES=199 -> 31840 - 663*48 = 16 B tail (d0-d3).
    ;   FB_COPY_LINES=190 -> 30400 - 633*48 = 16 B tail (d0-d3).
    ; For other tail sizes, manually adjust the register list to
    ; cover FBDRV_TAIL_BYTES/4 longwords.
    ifne    FBDRV_TAIL_BYTES
    movem.l (a6)+, d0-d7
    movem.l d0-d7, FBDRV_TAIL_DISP(a5)
    endc
                      endm

; Atari ST VBL interrupt vector. Replacing TOS's handler here drops
; mouse / cursor-blink / keyboard-repeat updates -- harmless for the
; framebuffer template because we own the screen until ESC exit.
VBL_VECTOR            equ $70

; FB frame counter (FB_FRAME_COUNTER_ADDR, $FA400C). The RP publishes a
; whole frame into the cart framebuffer and then bumps this counter as
; its LAST write. .vbl_loop blits only when the counter's low word has
; changed since the last blit (UFW_LAST_FRAME), and acknowledges only
; after a blit (VBLSYNC_ADDR). The RP starts the next publish only after
; that acknowledgement, so the two never overlap: the ST reads nothing
; between an acknowledgement and the next counter change, and a changed
; counter means the frame is complete. A slow app gets fewer frames,
; never a torn one. The low word is read with one move.w: a longword is
; two bus reads with its halves swapped and could mix old and new.

; RP→m68k command sentinel (CMD_MAGIC_SENTINEL_ADDR, $FA4000). The RP
; IKBD demux writes CMD_BOOT_GEM there when it decodes an ESC keypress;
; userfw's main loop polls and exits back to GEM on match. CMD_RESET comes
; before the RP reboots into Booster: userfw then cold-resets the ST
; (.cold_reset).
; Delay before the cold reset, so TOS scans the cartridge only once Booster
; serves it. Mirrors PRE_RESET_WAIT in main.s, the delay Booster uses too.
UFW_PRE_RESET_WAIT    equ $FFFFF

; The 16-entry ST palette at PALETTE_ADDR, published by the RP:
; .vbl_loop applies it to PALETTE_BASE each frame via a MOVEM-load +
; MOVEM-store.

; Screen pages live just below TOS RAM top (TT-style 256 KB ST RAM
; assumption -- screens land at $70000/$78000, matching md-sprites-demo).
UFW_SCREEN_A          equ $00070000
UFW_SCREEN_B          equ $00078000
UFW_SCREEN_XOR        equ (UFW_SCREEN_A ^ UFW_SCREEN_B)

; --- YM2149 sound chip (channels A and B as a fake DAC) --------------
;
; PSG access: write a register number to $FFFF8800 (latch), then
; write data to $FFFF8802. Regs 8 and 9 = ch A and ch B volume (low 4
; bits). Both channels are a "fake DAC": tone enabled, period = 0 (DC
; clamp above the audio band so the volume registers are the only
; thing driving the output). Reg 8 is latched at boot; each Timer-B
; fire writes ch A, latches reg 9, writes ch B and latches reg 8 again.
YM_SELECT             equ $FFFF8800
YM_DATA               equ $FFFF8802
YM_REG_MIXER          equ 7                  ; tone+noise enables
YM_REG_CHA_VOL        equ 8                  ; channel A volume (low 4 bits)
YM_REG_CHB_VOL        equ 9                  ; channel B volume (low 4 bits)
YM_MIXER_DAC_CHA      equ $FE                ; tone A enabled, all other tones/noise off, ports out
YM_MIXER_DAC_AB       equ $FC                ; tones A AND B enabled, tone C off, all noise off, ports out (Ghostbusters dual-channel fake DAC)

; The audio buffer at AUDIO_BUFFER_ADDR: (vA, vB) YM volume pairs, one
; pair per Timer-B fire; the VBL handler points A0 back at the start every
; VBL, so a frame reads its first ~224 bytes. Filled by the RP.

; FB_COPY_LINES (inc/sidecart_layout.s) is the number of 320-px lines the
; cart->ST blit covers per frame. Full ST low-res is 200 (32000 bytes = 666
; chunks * 48 B + a 32-byte tail); copying fewer leaves the bottom band of
; the destination ST page untouched (useful for a status row or to bound
; the blit's cost). The RP lays the framebuffer out for exactly this many
; lines (CART_FB_BLIT_LINES).

; --- IKBD ownership ------------------------------------------------

; Keyboard ACIA at $FFFFFC00/02. MIDI ACIA at $FFFFFC04/06 is not
; touched. Status bit 0 = RX-data-ready; bit 1 = TX-empty.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02

; IKBD commands. What the IKBD reports (keys only, the mouse, the
; joysticks) is the RP's choice: .vbl_loop sends the commands the RP puts at
; IKBD_OUT_ADDR, and the RP decodes whatever the IKBD sends.
;
; For a while after a reset the IKBD undoes a mouse-off command, and TOS
; resets it moments before this cartridge code runs: on a cold boot a $12
; sent then was sometimes lost and the mouse stayed on (seen on a Mega ST).
; So userfw resets the IKBD itself, waits for the reset's answer ($F0, $F1
; on a later IKBD release), throws it away and lets IKBD_SETTLE_ITERS pass
; before .vbl_loop sends the RP's commands. On the way out, TOS gets back
; what it set up: joystick events, then the relative mouse. The order
; matters: $14 also turns the mouse off.
IKBD_CMD_RESET_HDR    equ $80    ; reset command header (followed by $01)
IKBD_CMD_RESET_RUN    equ $01    ; reset + self-test; answers $F0 (or $F1)
IKBD_CMD_MOUSE_REL    equ $08    ; relative mouse reporting (TOS's mode)
IKBD_CMD_JOY_EVENTS   equ $14    ; joystick event reporting (TOS's mode)

; Waiting for the reset's answer: one pass of the wait loop is about 44
; cycles, so 400,000 passes are 0.55 s at 32 MHz and 2.2 s at 8 MHz. The
; IKBD answers after its self-test (about 60 ms); the timeout only matters
; when nothing answers (no keyboard). The settle delay after the answer
; (2 x 18 cycles per pass) is 20 ms at 32 MHz, 80 ms at 8 MHz.
IKBD_RESET_TIMEOUT    equ 400000
IKBD_SETTLE_ITERS     equ 36000

; Send one command byte to the IKBD once the ACIA can take it (status bit 1,
; TX-empty). \1: the byte.
IKBD_SEND             macro
.\@wait:
    btst    #1, ACIA_KBD_STATUS.w
    beq.s   .\@wait
    move.b  #\1, ACIA_KBD_DATA.w
                      endm

; MC68901 MFP registers (subset we manipulate).
MFP_IERA              equ $FFFFFA07          ; interrupt enable A (Timer-A = bit 5)
MFP_IERB              equ $FFFFFA09          ; interrupt enable B
MFP_IPRA              equ $FFFFFA0B          ; interrupt pending A (Timer-B = bit 0)
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-A ack = bit 5)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_VR                equ $FFFFFA17          ; vector register (high nibble = vector base, bit 3 = S: 1=software EOI, 0=auto-EOI)
MFP_TACR              equ $FFFFFA19          ; Timer-A control register (cleared at boot for safety)
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register (delay-mode + prescaler)
MFP_TBDR              equ $FFFFFA21          ; Timer-B data register (8-bit countdown)

; Timer-B audio rate, and everything that follows from it. MFP master
; clock = 2.4576 MHz, /4 prescaler, the profile's count (PROFILE_ADDR):
;   PROFILE_50FPS: 110 -> 5,585.45 Hz, 111.7 samples per PAL VBL;
;   PROFILE_25FPS:  28 -> 21,942.86 Hz, 438.2 samples per PAL VBL.
;   -> 2 bytes each (vA, vB): 224 or 878 bytes per VBL (the RP's
;      PROFILE_YM_BYTES_PER_VBL, rp/src/include/profile.h), within one
;      audio slice of AUDIO_SLICE_BYTES (inc/sidecart_layout.s);
;   -> the RP's rates (cart_shared.h CART_PROFILE_*_YM_RATE_HZ). YM sources
;      (.YMS files, the built-in jingle) stay at 5,585 Hz: the RP resamples.
; tests/host/test_layout.py checks that those places agree.
TIMERB_PRESCALER      equ 1                  ; /4 (delay mode)

; STE / Mega STE DMA sound. The chip plays 8-bit samples from ST RAM only,
; so userfw keeps a ring of AUDIO_DMA_RING_BYTES there, the second use of
; Atari RAM after the screen pages and on the same assumption: the 4 KB just
; below screen page A. The chip loops over it mono, at the profile's rate
; (12,517 or 25,033 Hz); every VBL userfw copies into it, from the audio
; buffer that mirrors it, what the RP has written ahead of the chip (see
; AUDIO_BUFFER_ADDR).
DMA_SND_CTRL          equ $FFFF8901          ; bit 0 play, bit 1 loop
DMA_SND_START         equ $FFFF8903          ; frame start: high, mid (+2), low (+4)
DMA_SND_COUNT_MID     equ $FFFF890B          ; where it plays (read only): middle byte
DMA_SND_COUNT_LOW     equ $FFFF890D          ;   and low byte
DMA_SND_END           equ $FFFF890F          ; frame end: high, mid (+2), low (+4)
DMA_SND_MODE          equ $FFFF8921          ; bits 1-0 rate, bit 7 mono
DMA_CTRL_LOOP_PLAY    equ 3
UFW_DMA_RING          equ $0006F000          ; AUDIO_DMA_RING_BYTES, up to $6FFFF
MACHINE_FAMILY_STE    equ $1                 ; the hello byte's family: STE, Mega STE
; The LMC1992 behind the Microwire: the DMA sound goes through it. userfw
; sets what TOS sets at boot, in case something changed it: master, left
; and right at 0 dB, bass and treble flat, the YM mixed in (GEM's bell).
MW_DATA               equ $FFFF8922
MW_MASK               equ $FFFF8924
MW_MASK_ALL           equ $07FF              ; also the mask at rest: a transfer ended
MW_WAIT_ITERS         equ 2000               ; a transfer takes 16 us

; The blitter (a Mega ST fitted with one, an STE, a Mega STE): its
; registers, from BLT_BASE ($FFFF8A20; halftone RAM below it is not used).
; The copy is HOP 2 (source) and OP 3 (source): a plain word copy.
;
; The blitter copies in hog mode: it owns the bus for a piece of chunks,
; the CPU and its interrupts wait. It needs 8 cycles a word against MOVEM's
; 9, both bound by the bus: 1.4 ms less for the full screen (measured on a
; Mega STE, where the DMA chip plays the sound: 3.3 ms left of the VBL
; after the blit against 1.9 ms with MOVEM, the sound unchanged). Pieces
; of BLIT_PIECE_DEFAULT chunks hold Timer-B's samples back and drop some
; (the sound breaks), and pieces short enough for a sample every 179 us
; (4 to 7 chunks) gained 0.2 to 0.8 ms on a Mega ST and still made the
; sound audibly rougher. So the blitter copies by default only on the DMA
; sound path, where Timer-B is off (BLIT_MODE_AUTO). Sharing the bus
; instead of owning it took longer than a VBL with the audio: 25 frames a
; second.
BLT_BASE              equ $FFFF8A20
BLT_SRC_X_INC         equ $FFFF8A20
BLT_SRC_Y_INC         equ $FFFF8A22
BLT_SRC_ADDR          equ $FFFF8A24
BLT_ENDMASK1          equ $FFFF8A28
BLT_ENDMASK2          equ $FFFF8A2A
BLT_ENDMASK3          equ $FFFF8A2C
BLT_DST_X_INC         equ $FFFF8A2E
BLT_DST_Y_INC_REG     equ $FFFF8A30
BLT_DST_ADDR          equ $FFFF8A32
BLT_X_COUNT           equ $FFFF8A36
BLT_Y_COUNT           equ $FFFF8A38
BLT_HOP               equ $FFFF8A3A          ; word: HOP (high byte), OP (low byte)
BLT_CTRL              equ $FFFF8A3C          ; bit 7 busy (start), bit 6 hog
BLT_SKEW              equ $FFFF8A3D
BLT_HOP_OP_COPY       equ $0203
BLT_BUSY_HOG          equ $C0          ; control: start, and own the bus
; From the last word of a chunk to the first of the chunk before it: the
; predecrement MOVEM's order (see FBDRV_INLINE).
BLT_DST_Y_INC         equ -(FBDRV_ITER_BYTES + FBDRV_ITER_BYTES - 2)

; IRQ vector slots we take over. $70 (VBL) already handled by the
; original userfw code path (D3 holds the save).
VEC_HBL               equ $68
VEC_TIMERD            equ $110
VEC_TIMERC            equ $114
VEC_ACIA              equ $118
VEC_TIMERB            equ $120
VEC_TIMERA            equ $134

; IKBD cart-bus emit window (ROM3). The ACIA interrupt
; (userfw_acia_irq) reads (IKBD_WINDOW_BASE + byte).b to forward `byte`
; to RP; the RP side filters commemul ring samples whose low 16 bits
; fall in [$8200, $8300) and extracts the IKBD byte from the low 8
; bits.

; The keyboard ACIA's receive interrupt reads every IKBD byte as it
; arrives (userfw_acia_irq). The 6850 holds one byte and the IKBD sends
; one every 1.28 ms: polling from the blit left gaps longer than that
; (between blits, and when audio interrupts stretched the poll interval),
; and a second byte overran the first -- md-oric measured overruns and a
; stuck key the same way. When the ACIA reports an overrun (status bit 5)
; the handler also reads IKBD_OVERRUN_ADDR, which the RP counts.
; The MIDI ACIA shares the GPIP4 line: its receive interrupt is turned
; off while userfw runs, or a MIDI byte nobody reads would hold the line
; down and stop the keyboard.
ACIA_MIDI_CTRL        equ $FFFFFC04
ACIA_KBD_CTRL_TOS     equ $96        ; RX interrupt on, 8N1, /64 (TOS's setting)
ACIA_MIDI_CTRL_OFF    equ $15        ; RX interrupt off, 8N1, /16
ACIA_MIDI_CTRL_TOS    equ $95        ; RX interrupt on, 8N1, /16 (TOS's setting)
MFP_GPIP4_BIT         equ 6          ; IERB / IMRB bit of the ACIA interrupt

; VBL frame-sync ack. After each blit completes (.after_copy)
; the m68k does a single dummy cart-bus read at VBLSYNC_ADDR to tell
; the RP "the blit is done, the cart framebuffer is free to overwrite".
; The m68k cannot WRITE the shared region (it's ROM from the m68k
; side), so the ack must be a READ captured by the RP's commemul ring
; -- the same mechanism IKBD uses. Distinct high byte ($84) from the
; IKBD window ($82) so the RP can tell the two apart. The value read
; is irrelevant; only the address matters.

; Hello (rp/src/include/st_session.h): the ST and the RP reboot
; independently and the RP keeps its state across an ST reset, the command
; sentinel included, so after an exit to GEM the next boot would read
; CMD_BOOT_GEM and leave at once. userfw says hello at every boot with three
; ROM3 reads, the TOS version's two bytes then the machine; on the hello the
; RP writes CMD_NOP to the sentinel and starts its session over. Windows and
; the machine byte: see cart_shared.h and st_session.h.
P_COOKIES             equ $5A0        ; _p_cookies: the cookie jar, 0 on TOS 1.0x
RESET_VECTOR_HI       equ $4          ; high word of the reset PC: $00FC on a 192 KB TOS
TOS_ROM_192K          equ $FC0000     ; TOS header (version word at +2), 192 KB TOS
TOS_ROM_256K          equ $E00000     ; TOS header, 256 KB TOS and later

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
;   offset 28: MFP VR save (byte) -- S-bit + vector base, switched to auto-EOI under userfw
;   offset 29-31: reserved / padding (longword align)
UFW_SAVE_SIZE         equ 32

    section text

userfw:
    ; --- Boot setup (runs once) ---

    ; Hello (see ST_HELLO_WINDOW). The TOS version comes from the ROM
    ; header, as md-microfirmware-template reads it; the machine from the
    ; _MCH cookie. The RP clears the sentinel on the hello, well before
    ; .vbl_loop first reads it: the IKBD reset below waits for the
    ; keyboard's answer first.
    lea     TOS_ROM_192K+2, a0
    cmpi.w  #(TOS_ROM_192K >> 16), RESET_VECTOR_HI.w
    beq.s   .hello_tos
    lea     TOS_ROM_256K+2, a0
.hello_tos:
    move.w  (a0), d1                      ; TOS version, e.g. $0206
    moveq   #0, d0
    move.b  d1, d0                        ; low byte
    lsr.w   #8, d1                        ; high byte
    lea     ST_TOS_HI_WINDOW, a0
    tst.b   (a0, d1.w)
    lea     ST_TOS_LO_WINDOW, a0
    tst.b   (a0, d0.w)
    moveq   #0, d0                        ; no cookie jar: an ST
    move.l  P_COOKIES.w, d1
    beq.s   .hello_send
    movea.l d1, a0
.hello_cookie:
    move.l  (a0)+, d1
    beq.s   .hello_send                   ; end of the jar, no _MCH: an ST
    cmpi.l  #'_MCH', d1
    beq.s   .hello_mch
    addq.w  #4, a0
    bra.s   .hello_cookie
.hello_mch:
    move.l  (a0), d1                      ; $000F00mm: F family, mm $10 on a Mega STE
    move.l  d1, d0
    swap    d0
    lsl.w   #4, d0                        ; family in bits 7..4
    lsr.w   #4, d1
    andi.w  #$000F, d1                    ; model in bits 3..0
    or.w    d1, d0
    andi.w  #$00FF, d0
.hello_send:
    lea     ST_HELLO_WINDOW, a0
    tst.b   (a0, d0.w)
    move.w  d0, UFW_MACHINE

    ; Save the original screen base so we can restore it on ESC exit.
    move.w  #2, -(sp)                ; XBIOS Physbase
    trap    #14
    addq.l  #2, sp
    move.l  d0, UFW_PHYSBASE_SAVE    ; saved screen base lives in RAM now

    ; A blitter? XBIOS Blitmode(-1): bit 1 set when there is one (an STE,
    ; a Mega STE, a Mega ST fitted with one). Told to the RP once.
    move.w  #-1, -(sp)
    move.w  #64, -(sp)                ; XBIOS Blitmode
    trap    #14
    addq.l  #4, sp
    moveq   #0, d1
    btst    #1, d0
    beq.s   .no_blitter
    moveq   #ST_FEATURE_BLITTER, d1
.no_blitter:
    move.w  d1, UFW_HAS_BLITTER
    lea     ST_FEATURES_WINDOW, a0
    tst.b   (a0, d1.w)

    ; Save TOS's VBL vector and install ours. We're in supervisor mode
    ; (entered via CA_INIT) so writing $70.w is legal.
    move.l  VBL_VECTOR.w, UFW_VBL_VEC_SAVE   ; TOS VBL vector saved in RAM
    lea     userfw_vbl(pc), a0
    move.l  a0, VBL_VECTOR.w

    ; --- IKBD ownership setup ------------------------------------
    ;
    ; A5 = save area pointer (physbase - 32). Used at boot to save
    ; the 6 IRQ vectors + MFP IER/IMR; ESC exit recomputes A5 from
    ; UFW_PHYSBASE_SAVE before reading the save area, so A5 doesn't
    ; need to survive the per-VBL FBDRV_INLINE expansion.
    movea.l UFW_PHYSBASE_SAVE, a5
    lea     -UFW_SAVE_SIZE(a5), a5

    ; The command sentinel at CMD_MAGIC_SENTINEL_ADDR is RP-owned (m68k
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

    ; Save MFP IER / IMR for A and B (4 bytes), plus VR (1 byte).
    move.b  MFP_IERA.w, 24(a5)
    move.b  MFP_IERB.w, 25(a5)
    move.b  MFP_IMRA.w, 26(a5)
    move.b  MFP_IMRB.w, 27(a5)
    move.b  MFP_VR.w, 28(a5)             ; TOS uses S=1 (software EOI); we override below

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

    ; Disable + mask everything in MFP A/B. We re-enable Timer-B
    ; explicitly below; everything else stays off.
    clr.b   MFP_IERA.w
    clr.b   MFP_IERB.w
    clr.b   MFP_IMRA.w
    clr.b   MFP_IMRB.w

    ; Reset the IKBD, wait for its answer and let it settle (see
    ; IKBD_CMD_RESET_HDR); .vbl_loop then sends it the RP's commands.
    ; Interrupts are masked, so nothing else reads the ACIA meanwhile;
    ; every byte that arrives before the answer (a mouse packet in flight,
    ; a key) is read and dropped.
    IKBD_SEND IKBD_CMD_RESET_HDR
    IKBD_SEND IKBD_CMD_RESET_RUN
    move.l  #IKBD_RESET_TIMEOUT, d0
.ikbd_reset_wait:
    btst    #0, ACIA_KBD_STATUS.w         ; a byte waiting?
    beq.s   .ikbd_reset_next
    move.b  ACIA_KBD_DATA.w, d1           ; read it (clears RX-ready)
    andi.b  #$FE, d1
    cmpi.b  #$F0, d1                      ; the answer: $F0 or $F1
    beq.s   .ikbd_reset_answered
.ikbd_reset_next:
    subq.l  #1, d0
    bne.s   .ikbd_reset_wait
.ikbd_reset_answered:
    move.l  #IKBD_SETTLE_ITERS, d0
.ikbd_settle:
    subq.l  #1, d0
    bne.s   .ikbd_settle

    ; Nothing sent to the IKBD yet: .vbl_loop sends the RP's commands and
    ; reports them (see UFW_IKBD_OUT_GEN). No IKBD byte read yet.
    move.w  #-1, UFW_IKBD_OUT_GEN
    move.w  #-1, UFW_IKBD_OUT_STEP
    clr.w   UFW_IKBD_COUNT

    ; Keyboard bytes by interrupt from here on (see IKBD_OVERRUN_ADDR).
    ; SR is still IPL 7, so nothing fires until the loop starts.
    lea     userfw_acia_irq(pc), a1
    move.l  a1, VEC_ACIA.w
    move.b  #ACIA_KBD_CTRL_TOS, ACIA_KBD_STATUS.w
    move.b  #ACIA_MIDI_CTRL_OFF, ACIA_MIDI_CTRL.w
    bset    #MFP_GPIP4_BIT, MFP_IERB.w
    bset    #MFP_GPIP4_BIT, MFP_IMRB.w

    ; --- YM2149 init: ch A + ch B as Ghostbusters dual-channel DAC
    ; Enable tones on BOTH ch A and ch B (mixer bits 0,1 = 0). Tone
    ; periods all 0 so the counters run at max -- effectively DC
    ; clamp above the audio band, so the volume registers alone
    ; shape each channel's output. The two channels sum
    ; acoustically; the Timer-B handler writes a (vA, vB) pair per
    ; fire, and the Ghostbusters 64-entry hand-tuned LUT picks the
    ; pair that best approximates the desired linear amplitude on
    ; the YM's logarithmic volume curve.
    ;
    ; Reg 8 (ch A volume) is latched LAST so the first Timer-B fire
    ; can write ch A immediately; the handler toggles to reg 9
    ; (ch B) mid-fire and back to reg 8 at the end.
    move.b  #YM_REG_MIXER, YM_SELECT.w
    move.b  #YM_MIXER_DAC_AB, YM_DATA.w   ; $FC: tones A+B on, tone C off, all noise off, ports out
    move.b  #0, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R0 = ch A fine period
    move.b  #1, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R1 = ch A coarse period
    move.b  #2, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R2 = ch B fine period
    move.b  #3, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R3 = ch B coarse period
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w  ; latch reg 9 to zero ch B
    move.b  #0, YM_DATA.w                 ; ch B vol = 0 (silence)
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w  ; latch reg 8 (next YM_DATA writes hit ch A volume)
    move.b  #0, YM_DATA.w                 ; ch A vol = 0 (silence)

    ; --- Timer-B setup (audio @ ~5,585 Hz) -----------------------
    ; Install our handler at $120 (overrides the dummy installed
    ; above). On the YM path (.sound_ym below) count -> TBDR, then
    ; prescaler -> TBCR starts the countdown, and Timer-B is enabled and
    ; unmasked at the MFP; the DMA path leaves it stopped. SR is
    ; still IPL=7 at this point (set by `ori.w #$0700, sr` at the
    ; very top of userfw), so no IRQ fires until SR is dropped to
    ; $2300 below.
    ;
    ; Also flip the MFP Vector Register to AUTO-EOI mode (clear
    ; the S bit, VR bit 3). With S=0 the MFP clears its own in-
    ; service bit on each IACK cycle, so the Timer-B handler can
    ; skip the explicit `move.b #$FE, MFP_ISRA.w` ACK -- saves
    ; ~12 cyc per IRQ * ~125 IRQ/VBL = ~1500 cyc/VBL.
    lea     userfw_timerb_audio(pc), a0
    move.l  a0, VEC_TIMERB.w
    move.b  28(a5), d0                    ; copy TOS's VR (saved above)
    andi.b  #$F7, d0                      ; clear bit 3 (S) -> auto-EOI
    move.b  d0, MFP_VR.w

    ; Initialise A0 to the first audio slice for the Timer-B handler.
    ; A0 is NOT in the FBDRV_INLINE MOVEM list and no other code in
    ; userfw touches it after this point (userfw_vbl moves it to the next
    ; slice with the interrupts masked), so the handler can rely on
    ; A0 holding a valid cart-buffer pointer at all times -- saves
    ; the push/pop around it in the hot IRQ path (-24 cyc/fire).
    clr.w   UFW_AUDIO_SLICE
    movea.l #AUDIO_BUFFER_ADDR, a0

    ; The app's profile (PROFILE_ADDR, written by the RP before we booted):
    ; the frames a VBL, Timer-B's count, the DMA chip's mode and lead.
    lea     .profile_50(pc), a1
    cmpi.w  #PROFILE_25FPS, PROFILE_ADDR
    bne.s   .profile_read
    lea     .profile_25(pc), a1
.profile_read:
    move.w  (a1)+, UFW_VBLS_A_FRAME
    move.w  (a1)+, UFW_TIMERB_COUNT
    move.w  (a1)+, UFW_DMA_MODE
    move.w  (a1)+, UFW_DMA_LEAD
    clr.w   UFW_VBL_COUNT
    clr.w   UFW_FRAME_VBL
    bra.s   .profile_done
.profile_50:
    dc.w    PROFILE_50FPS_VBLS, PROFILE_50FPS_TIMERB_COUNT
    dc.w    PROFILE_50FPS_DMA_MODE, PROFILE_50FPS_DMA_LEAD
.profile_25:
    dc.w    PROFILE_25FPS_VBLS, PROFILE_25FPS_TIMERB_COUNT
    dc.w    PROFILE_25FPS_DMA_MODE, PROFILE_25FPS_DMA_LEAD
.profile_done:

    ; The sound goes out through the DMA chip on an STE or a Mega STE,
    ; unless the RP keeps us to the YM (AUDIO_OUT_ADDR); through Timer-B and
    ; the YM elsewhere. The RP applies the same rule to what it writes.
    clr.w   UFW_AUDIO_DMA
    cmpi.w  #AUDIO_OUT_YM, AUDIO_OUT_ADDR
    beq     .sound_ym
    move.w  UFW_MACHINE, d0
    lsr.w   #4, d0
    cmpi.w  #MACHINE_FAMILY_STE, d0
    bne     .sound_ym

    ; DMA: stopped while it is set up; the ring silent (signed 0); the
    ; frame is the whole ring, looped; the mixer as TOS sets it; play.
    clr.b   DMA_SND_CTRL.w
    lea     UFW_DMA_RING, a1
    move.w  #(AUDIO_DMA_RING_BYTES / 4) - 1, d0
.dma_clear:
    clr.l   (a1)+
    dbf     d0, .dma_clear
    move.l  #UFW_DMA_RING, d0
    move.b  d0, DMA_SND_START+4.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_START+2.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_START.w
    move.l  #(UFW_DMA_RING + AUDIO_DMA_RING_BYTES), d0
    move.b  d0, DMA_SND_END+4.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_END+2.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_END.w
    move.b  UFW_DMA_MODE+1, DMA_SND_MODE.w
    lea     userfw_mw_cmds(pc), a1
    moveq   #((userfw_mw_cmds_end - userfw_mw_cmds) / 2) - 1, d1
.mw_next:
    move.w  #MW_WAIT_ITERS, d2
.mw_wait:
    cmpi.w  #MW_MASK_ALL, MW_MASK.w       ; the last transfer is over
    beq.s   .mw_send
    dbf     d2, .mw_wait
.mw_send:
    move.w  #MW_MASK_ALL, MW_MASK.w
    move.w  (a1)+, MW_DATA.w
    dbf     d1, .mw_next
    clr.w   UFW_DMA_FRONT
    move.b  #DMA_CTRL_LOOP_PLAY, DMA_SND_CTRL.w
    move.w  #-1, UFW_AUDIO_DMA
    bra.s   .sound_done

.sound_ym:
    move.b  UFW_TIMERB_COUNT+1, MFP_TBDR.w
    move.b  #TIMERB_PRESCALER, MFP_TBCR.w
    bset    #0, MFP_IERA.w                ; Timer-B IRQ enable (IERA bit 0)
    bset    #0, MFP_IMRA.w                ; Timer-B IRQ unmask (IMRA bit 0)
.sound_done:
    ifne    TIME_STUDY
    lea     userfw_timera_tick(pc), a1    ; the stopwatch (see TIME_STUDY)
    move.l  a1, VEC_TIMERA.w
    clr.b   MFP_TACR.w
    clr.b   MFP_TADR.w                    ; 256 ticks a wrap
    clr.w   UFW_SW_WRAPS
    bset    #5, MFP_IERA.w                ; its wraps (Timer-A)
    bset    #5, MFP_IMRA.w
    move.b  #TIMERA_STUDY_DIV10, MFP_TACR.w
    endc

    ; Interrupts back on (caller's level, typically $2300).
    move.w  (sp)+, sr

    ; Initialise the hidden-page pointer. UFW_SCREEN_PAGE holds the
    ; page currently being drawn into; .after_copy toggles it between
    ; UFW_SCREEN_A and UFW_SCREEN_B via XOR with UFW_SCREEN_XOR.
    move.l  #UFW_SCREEN_A, UFW_SCREEN_PAGE

    ; The frame now in the cart framebuffer counts as seen: the first blit
    ; waits for the RP's next publish (see FB_FRAME_COUNTER_ADDR).
    move.w  FB_FRAME_COUNTER_ADDR, UFW_LAST_FRAME

    ; Shifter base HIGH byte ($07) is the same for both screen pages
    ; ($70000 and $78000), so we write it ONCE here and only update
    ; the MID byte per VBL in .after_copy below (saves ~20 cyc/VBL).
    move.b  #(UFW_SCREEN_A >> 16), VIDEO_BASE_ADDR_HIGH.w

    ; Pin IRQ state for the duration of .vbl_loop:
    ;   SR = $2300: supervisor mode, IPL=3. Blocks levels 1-3 (HBL
    ;   at IPL 2), allows VBL at IPL 4 and MFP at IPL 6. The only
    ;   MFP source enabled is Timer-B (IERA/IMRA bit 0), so the
    ;   m68k sees VBL + Timer-B IRQs.
    move.w  #$2300, sr

    ; --- Per-VBL loop ---
.vbl_loop:
    ; CPU-halt wait for the next vsync. The m68k `stop #$2300` halts
    ; until an IRQ at level > 3 fires (VBL at IPL=4, MFP at IPL=6).
    ; Because Timer-B (MFP) can be re-enabled for audio/IKBD work,
    ; we can't assume the next wake is the VBL -- the userfw_vbl
    ; handler clears UFW_VBL_FLAG, but the dummy MFP handlers do
    ; not. After each wake we check the flag; if it's still set the
    ; wake came from a non-VBL IRQ and we `stop` again.
    move.w  #-1, UFW_VBL_FLAG
.wait_vbl:
    stop    #$2300
    tst.w   UFW_VBL_FLAG
    bne.s   .wait_vbl
    STUDY_POINT STUDY_POINT_WAKE

    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark
    endc

    ; DMA sound (see AUDIO_BUFFER_ADDR): where the chip plays now, by
    ; AUDIO_DMA_POS_UNIT bytes, for the RP; then the mirror copied into the
    ; ring from where the last copy ended up to the profile's lead
    ; (UFW_DMA_LEAD) ahead of the chip. The counter moves while it is read:
    ; its middle byte is read again. A frontier out of step (the first VBL,
    ; a long stall) copies the lead. About 0.35 ms at 12,517 Hz, 0.7 at
    ; 25,033 Hz; Timer-B, off on this path, takes 2.1 ms at 5,585 Hz.
    tst.w   UFW_AUDIO_DMA
    beq     .dma_done
.dma_pos:
    moveq   #0, d0
    move.b  DMA_SND_COUNT_MID.w, d0
    move.b  DMA_SND_COUNT_LOW.w, d1
    cmp.b   DMA_SND_COUNT_MID.w, d0
    bne.s   .dma_pos
    lsl.w   #8, d0
    move.b  d1, d0
    sub.w   #(UFW_DMA_RING & $FFFF), d0
    and.w   #(AUDIO_DMA_RING_BYTES - AUDIO_DMA_POS_UNIT), d0
    move.w  d0, d1
    lsr.w   #4, d1                        ; / AUDIO_DMA_POS_UNIT
    lea     DMA_POS_WINDOW, a1
    tst.b   (a1, d1.w)
    add.w   UFW_DMA_LEAD, d0
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d0 ; copy up to here
    move.w  UFW_DMA_FRONT, d1             ; from where the last copy ended
    move.w  d0, UFW_DMA_FRONT
    move.w  d0, d2
    sub.w   d1, d2
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d2
    cmp.w   #(AUDIO_DMA_RING_BYTES / 2), d2
    bls.s   .dma_count
    move.w  d0, d1
    sub.w   UFW_DMA_LEAD, d1
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d1
    move.w  UFW_DMA_LEAD, d2
.dma_count:
    lsr.w   #3, d2
    subq.w  #1, d2
    bmi.s   .dma_done
    lea     AUDIO_BUFFER_ADDR, a2
    lea     UFW_DMA_RING, a3
.dma_copy:
    move.l  0(a2, d1.w), 0(a3, d1.w)
    move.l  4(a2, d1.w), 4(a3, d1.w)
    addq.w  #8, d1
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d1
    dbf     d2, .dma_copy
.dma_done:

    ; Publish RP-supplied palette to the shifter. 16 words
    ; from PALETTE_ADDR -> $FFFF8240..$FFFF825E via two MOVEMs.
    ; Cost: 76 (load) + 72 (store) + 16 (lea) = ~164 cyc / VBL =
    ; ~20 us. Apps that don't want RP-driven palette can leave the
    ; cart slot zero (= all-black screen, since the m68k still
    ; publishes it every frame) -- swap the load EA below for
    ; their own palette source if needed.
    lea     PALETTE_ADDR, a5
    movem.l (a5), d0-d7
    movem.l d0-d7, PALETTE_BASE.w

    ; Blit only a frame the RP has finished publishing, and only once
    ; (see FB_FRAME_COUNTER_ADDR), and no sooner than the profile's VBLs a
    ; frame after the last one (PROFILE_25FPS: every second VBL, on every
    ; machine; a frame late by a VBL delays only the next one). Nothing
    ; new: no blit, no flip, no ack.
    move.w  UFW_VBL_COUNT, d1
    sub.w   UFW_FRAME_VBL, d1
    cmp.w   UFW_VBLS_A_FRAME, d1
    blo     .input_check
    move.w  FB_FRAME_COUNTER_ADDR, d0
    cmp.w   UFW_LAST_FRAME, d0
    beq     .input_check
    move.w  d0, UFW_LAST_FRAME
    move.w  UFW_VBL_COUNT, UFW_FRAME_VBL
    STUDY_POINT STUDY_POINT_COPY

    ; The copy: the blitter when there is one and the RP asks for it, or
    ; leaves it to us on the DMA sound path (BLIT_MODE_ADDR), else
    ; FBDRV_INLINE. The blitter reads the cart framebuffer as MOVEM does, a
    ; chunk of FBDRV_ITER_BYTES per line going forward, and writes each
    ; chunk where MOVEM's predecrement store would: one line per chunk,
    ; from the page's last chunk back to its first (BLT_DST_Y_INC), then
    ; the tail in natural order. Either way A5 ends at the page start, as
    ; .after_copy expects.
    movea.l UFW_SCREEN_PAGE, a5
    tst.w   UFW_HAS_BLITTER
    beq     .copy_cpu
    move.w  BLIT_MODE_ADDR, d1
    cmp.b   #BLIT_MODE_BLITTER, d1
    beq.s   .copy_blitter
    tst.b   d1
    bne     .copy_cpu
    tst.w   UFW_AUDIO_DMA
    beq     .copy_cpu
.copy_blitter:
    lea     BLT_BASE.w, a1
    move.w  #2, BLT_SRC_X_INC-BLT_BASE(a1)
    move.w  #2, BLT_SRC_Y_INC-BLT_BASE(a1)
    move.l  #FRAMEBUFFER_ADDR, BLT_SRC_ADDR-BLT_BASE(a1)
    moveq   #-1, d2
    move.w  d2, BLT_ENDMASK1-BLT_BASE(a1)
    move.w  d2, BLT_ENDMASK2-BLT_BASE(a1)
    move.w  d2, BLT_ENDMASK3-BLT_BASE(a1)
    move.w  #2, BLT_DST_X_INC-BLT_BASE(a1)
    move.w  #BLT_DST_Y_INC, BLT_DST_Y_INC_REG-BLT_BASE(a1)
    lea     (FBDRV_MAIN_BYTES - FBDRV_ITER_BYTES)(a5), a2
    move.l  a2, BLT_DST_ADDR-BLT_BASE(a1)
    move.w  #FBDRV_ITER_BYTES / 2, BLT_X_COUNT-BLT_BASE(a1)
    move.w  #BLT_HOP_OP_COPY, BLT_HOP-BLT_BASE(a1)
    clr.b   BLT_SKEW-BLT_BASE(a1)
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w
    endc

    ; A piece of chunks at a time, in hog mode (see BLT_BASE); between
    ; pieces the interrupts run.
    lsr.w   #8, d1
    bne.s   .blit_pieces
    moveq   #BLIT_PIECE_DEFAULT, d1
.blit_pieces:
    move.w  #FBDRV_MAIN_ITERS, d2
.blit_hog:
    move.w  d1, d3
    cmp.w   d2, d3
    bls.s   .blit_hog_piece
    move.w  d2, d3
.blit_hog_piece:
    move.w  d3, BLT_Y_COUNT-BLT_BASE(a1)
    move.b  #BLT_BUSY_HOG, BLT_CTRL-BLT_BASE(a1)
    nop
    sub.w   d3, d2
    bne.s   .blit_hog

    ifne    FBDRV_TAIL_BYTES
    lea     FBDRV_TAIL_DISP(a5), a2
    move.l  a2, BLT_DST_ADDR-BLT_BASE(a1)
    move.w  #FBDRV_TAIL_BYTES / 2, BLT_X_COUNT-BLT_BASE(a1)
    move.w  #1, BLT_Y_COUNT-BLT_BASE(a1)
    move.b  #BLT_BUSY_HOG, BLT_CTRL-BLT_BASE(a1)
    nop
    endc
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_DONE, PALETTE_IDX0.w
    endc
    bra     .after_copy

    ; A5 = END of the screen page chunk-covered region. FBDRV_INLINE
    ; uses predec MOVEM (`movem.l list, -(a5)`) and walks A5 backwards
    ; from page_end down to page_start as it stores chunks in reverse
    ; order. After FBDRV_MAIN_ITERS iters A5 ends at the page START,
    ; which is the value .after_copy below expects in A5.
.copy_cpu:
    lea     (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)(a5), a5

    ; The 68000's copy via the FBDRV_INLINE macro (defined in the
    ; constants block): every machine without a blitter, and those with
    ; one on the YM sound path or when the RP asks for it.
    ;
    ; FBDRV_INLINE clobbers D0-D7, A1-A4, A6. A0 and A7 (SP) are
    ; PRESERVED (not in the MOVEM list): A0 holds the Timer-B
    ; handler's dedicated audio buffer pointer (initialised at boot,
    ; advances + wraps inside the handler), A7 keeps the supervisor
    ; SP valid so IRQs can fire safely. A6 is the macro's own src
    ; pointer (overwritten at macro entry) so no save is needed.
    ; D0-D7 / A1-A4 are scratch and not consumed after.
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = white (blit in flight)
    endc
    FBDRV_INLINE                      ; inline cart->ST screen copy
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_DONE, PALETTE_IDX0.w     ; border = green (copy done)
    endc

.after_copy:
    STUDY_POINT STUDY_POINT_COPIED

    ; Flip the video base to the just-written page. A5 still holds
    ; UFW_SCREEN_PAGE (preserved by FBDRV_INLINE). Only the MID byte
    ; of the screen base differs between the two pages -- HIGH was
    ; written once at boot (constant $07 for both $70000 / $78000).
    ;
    ; UFW_SCREEN_PAGE is a 32-bit address stored big-endian, so byte
    ; +2 of the longword is exactly the MID byte (bits 8..15) we need
    ; to write to VIDEO_BASE_ADDR_MID. Read it straight from memory
    ; instead of recomputing via lsr/move chain from A5.
    move.b  UFW_SCREEN_PAGE+2, VIDEO_BASE_ADDR_MID.w

    ; Toggle UFW_SCREEN_PAGE between SCREEN_A and SCREEN_B for the
    ; next frame.
    move.l  a5, d0
    eor.l   #UFW_SCREEN_XOR, d0
    move.l  d0, UFW_SCREEN_PAGE

    ; Frame-sync ack: one cart-bus read tells the RP the blit
    ; is finished and the cart FB is free to overwrite. Emitted every
    ; VBL (the FB is free here -- blit done, page flipped). The RP's
    ; commemul ring captures the read; fb_publish() on the RP blocks
    ; until it sees this before running the next chunky-to-planar.
    tst.b   VBLSYNC_ADDR

.input_check:
    ; IKBD commands from the RP (IKBD_OUT_ADDR): when their generation
    ; changes, send the bytes one per VBL, only when the ACIA can take one
    ; (no waiting), a 0 as a VBL with no byte; then report the generation.
    ; A new generation before the end starts over. Nothing is sent while the
    ; RP rewrites the block (busy bit), and a byte only if the generation is
    ; still the one read before it.
    move.w  IKBD_OUT_GEN, d0
    btst    #IKBD_OUT_BUSY_BIT, d0
    bne.s   .ikbd_out_done
    cmp.w   UFW_IKBD_OUT_GEN, d0
    beq.s   .ikbd_out_next
    move.w  d0, UFW_IKBD_OUT_GEN
    clr.w   UFW_IKBD_OUT_STEP
.ikbd_out_next:
    move.w  UFW_IKBD_OUT_STEP, d1
    bmi.s   .ikbd_out_done                ; all sent and reported
    move.w  IKBD_OUT_LEN, d2
    cmp.w   #IKBD_OUT_MAX, d2
    bls.s   .ikbd_out_len
    moveq   #IKBD_OUT_MAX, d2
.ikbd_out_len:
    cmp.w   d2, d1
    bhs.s   .ikbd_out_report
    btst    #1, ACIA_KBD_STATUS.w         ; can the ACIA take a byte?
    beq.s   .ikbd_out_done                ; not yet: next VBL
    lea     IKBD_OUT_BYTES, a1
    move.b  (a1, d1.w), d2
    cmp.w   IKBD_OUT_GEN, d0              ; still the same commands?
    bne.s   .ikbd_out_done
    tst.b   d2
    beq.s   .ikbd_out_sent                ; 0: nothing this VBL
    move.b  d2, ACIA_KBD_DATA.w
.ikbd_out_sent:
    addq.w  #1, UFW_IKBD_OUT_STEP
    bra.s   .ikbd_out_done
.ikbd_out_report:
    move.w  #-1, UFW_IKBD_OUT_STEP
    and.w   #$FF, d0
    lea     IKBD_OUT_WINDOW, a1
    tst.b   (a1, d0.w)
.ikbd_out_done:

    ; ESC detection: the RP-side IKBD demux writes
    ; CMD_BOOT_GEM into CMD_MAGIC_SENTINEL_ADDR on ESC press. CMD_RESET:
    ; the RP is about to reboot into Booster. Any other
    ; sentinel value (NOP, future commands) leaves the loop running.
    STUDY_POINT STUDY_POINT_IDLE
    move.l  CMD_MAGIC_SENTINEL_ADDR, d0
    cmp.l   #CMD_RESET, d0
    beq     .cold_reset
    cmp.l   #CMD_BOOT_GEM, d0
    bne     .vbl_loop

    ; --- ESC pressed: restore IRQ state and return to TOS ---------
    ;
    ; Mask interrupts before touching MFP / vectors.
    ori.w   #$0700, sr

    ; The DMA sound chip stops; the Microwire stays as TOS sets it.
    tst.w   UFW_AUDIO_DMA
    beq.s   .exit_sound
    clr.b   DMA_SND_CTRL.w
.exit_sound:

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
    move.b  28(a5), MFP_VR.w             ; restore TOS's S=1 / vector base

    ; Restore the 6 vectors we overwrote.
    move.l  0(a5), VEC_HBL.w
    move.l  4(a5), VEC_TIMERD.w
    move.l  8(a5), VEC_TIMERC.w
    move.l  12(a5), VEC_ACIA.w
    move.l  16(a5), VEC_TIMERB.w
    move.l  20(a5), VEC_TIMERA.w

    ; Restore TOS's VBL vector ($70 save from UFW_VBL_VEC_SAVE).
    move.l  UFW_VBL_VEC_SAVE, VBL_VECTOR.w

    ; Give TOS its mouse and joysticks back (see IKBD_CMD_RESET_HDR).
    IKBD_SEND IKBD_CMD_JOY_EVENTS
    IKBD_SEND IKBD_CMD_MOUSE_REL
    move.b  #ACIA_MIDI_CTRL_TOS, ACIA_MIDI_CTRL.w

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

    ; --- CMD_RESET: cold-reset the ST, from RAM -------------------
    ;
    ; The RP reboots into Booster about 100 ms after it asks for this,
    ; and from then on the cartridge answers nothing useful: nothing may
    ; run from it. Mask every interrupt (userfw's handlers live in the
    ; cartridge), copy userfw_reset_stub to UFW_RESET_STUB and run it
    ; there. TOS's cold boot reinitialises the shifter, MFP, IKBD, YM
    ; and every vector userfw took, so nothing is restored by hand.
.cold_reset:
    move.w  #$2700, sr
    tst.w   UFW_AUDIO_DMA
    beq.s   .reset_sound
    clr.b   DMA_SND_CTRL.w                ; nothing plays from a ring TOS reuses
.reset_sound:
    lea     userfw_reset_stub(pc), a1
    lea     UFW_RESET_STUB, a2
    moveq   #((userfw_reset_stub_end - userfw_reset_stub) / 2) - 1, d0
.copy_reset_stub:
    move.w  (a1)+, (a2)+
    dbf     d0, .copy_reset_stub
    jmp     UFW_RESET_STUB

; -------------------------------------------------------------------
; userfw_vbl -- VBL interrupt handler. Three jobs:
;   1. Tell the RP how many IKBD bytes have been read so far, low byte
;      (IKBD_COUNT_WINDOW): the RP checks it got every one, and uses the
;      report as a clock among the IKBD bytes.
;   2. Point A0 at the next audio slice (AUDIO_BUFFER_ADDR + slice *
;      AUDIO_SLICE_BYTES) and tell the RP which slice plays now.
;      Timer-B consumes samples from the slice's start on the next IRQ.
;      Moving A0 once per VBL eliminates the explicit `cmpa.l + bcs.s`
;      wrap in the Timer-B hot path, so that handler stays a few
;      moves + rte. A0 is dedicated to audio (excluded from the
;      FBDRV_INLINE MOVEM list and from the ACIA handler), so it's
;      safe to overwrite here from IRQ context.
;   3. Clear UFW_VBL_FLAG so .vbl_loop's `stop`-then-check wait can
;      distinguish a VBL wake from a Timer-B (or other MFP) wake.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. The ACIA IRQ ($118) is userfw's own (userfw_acia_irq), so
; GEMDOS's keyboard buffer is no longer filled; the keys go to the RP.
userfw_vbl:
    ; Move Timer-B to the next audio slice and tell the RP which one: a ROM3
    ; read at AUDIO_SLICE_WINDOW + the slice. The RP writes only the slices
    ; after it. Timer-B (IPL 6) can interrupt this handler (IPL 4), and it
    ; reads a sample through A0: masked here, it never sees A0 anywhere but
    ; on a slice. This handler interrupts the blit between a MOVEM load and
    ; its store, when every register but A0 and A7 holds pixels: D0 is saved.
    ; About 30 us per VBL; a Timer-B sample waits at most that long.
    ; userfw_acia_irq cannot run in the middle of the count report either,
    ; so every byte it counted was forwarded before the report.
    move.w  #$2700, sr
    addq.w  #1, UFW_VBL_COUNT
    ifne    TIME_STUDY
    movem.l d0-d2/a1, -(sp)
    moveq   #STUDY_POINT_VBL, d2
    bsr     study_report
    movem.l (sp)+, d0-d2/a1
    endc
    move.l  d0, -(sp)
    moveq   #0, d0
    move.b  UFW_IKBD_COUNT+1, d0
    movea.l #IKBD_COUNT_WINDOW, a0
    tst.b   (a0, d0.w)
    move.w  UFW_AUDIO_SLICE, d0
    addq.w  #1, d0
    and.w   #AUDIO_SLICES-1, d0
    move.w  d0, UFW_AUDIO_SLICE
    movea.l #AUDIO_SLICE_WINDOW, a0
    tst.b   (a0, d0.w)
    lsl.w   #8, d0                        ; x AUDIO_SLICE_BYTES: an immediate
    lsl.w   #AUDIO_SLICE_SHIFT-8, d0      ; shifts 8 bits at most
    movea.l #AUDIO_BUFFER_ADDR, a0
    adda.w  d0, a0
    move.l  (sp)+, d0
    clr.w   UFW_VBL_FLAG
    rte

; -------------------------------------------------------------------
; userfw_timerb_audio -- Timer-B IRQ handler. Fires at the profile's rate
; (Timer-B in /4 delay mode, TBDR = the profile's count: 5,585 or 21,943 Hz).
;
; Dual-channel Ghostbusters-LUT mode: each sample in the cart buffer
; is 2 bytes = (vA, vB), pre-resolved at build time by running the
; raw G1.SAM bytes through the demo's 64-entry SAMPLE1 LUT (top 6
; bits of each PCM byte index a (chA, chB) pair). Per fire:
;   1. Write vA to YM ch A vol (reg 8 latched on entry).
;   2. Latch reg 9 (ch B vol).
;   3. Write vB to YM ch B vol.
;   4. Re-latch reg 8 so the next fire writes ch A immediately.
;
; A0 is a DEDICATED cart audio-buffer cursor (userfw_vbl points it at
; the next audio slice each VBL; postinc walks 2 bytes/fire).
;
; MFP is in auto-EOI mode (VR S=0) so the in-service bit clears
; automatically on each IACK cycle.
;
; Cycle budget per fire:
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vA -> ch A vol
;   move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; 12 cyc -- latch reg 9
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vB -> ch B vol
;   move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; 12 cyc -- re-latch reg 8
;   rte                                    ; 20 cyc
;   ---                                    ; 68 cyc/IRQ
; Plus ~44 cyc of IRQ entry and exit: ~112 cyc per fire, 112 fires
; per PAL VBL = ~12.5 k cyc = ~1.6 ms = ~8% of the frame.
userfw_timerb_audio:
    move.b  (a0)+, YM_DATA.w               ; vA -> ch A vol
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; latch ch B vol reg
    move.b  (a0)+, YM_DATA.w               ; vB -> ch B vol
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; back to ch A vol reg for next fire
    rte

; -------------------------------------------------------------------
; userfw_acia_irq -- keyboard ACIA receive interrupt (MFP GPIP4, $118).
; Forwards the byte the ACIA holds with one cart-bus read, counts it
; (UFW_IKBD_COUNT, see userfw_vbl) and returns. One byte per interrupt:
; the 6850 holds a single byte (a second one overruns it, reported below),
; reading it raises the ACIA's IRQ line, and the next byte lowers it again,
; an edge the MFP sees. The handler is kept short because it is paid for
; every byte: at line speed 16 times a VBL, when the full-screen blit
; leaves almost no slack. It can fire between a MOVEM load and its store
; in FBDRV_INLINE: it saves D0 and A1 and never touches A0 (the audio
; cursor). Auto-EOI: no in-service bit to clear.
userfw_acia_irq:
    move.l  d0, -(sp)
    move.l  a1, -(sp)
    moveq   #0, d0
    move.b  ACIA_KBD_STATUS.w, d0
    btst    #0, d0                        ; a byte waiting?
    beq.s   .acia_done
    btst    #5, d0                        ; overrun: bytes were lost before it
    beq.s   .acia_read
    tst.b   IKBD_OVERRUN_ADDR
.acia_read:
    move.b  ACIA_KBD_DATA.w, d0           ; clears RX-ready and the overrun
    lea     IKBD_WINDOW_BASE, a1
    tst.b   (a1, d0.w)                    ; forward it
    addq.w  #1, UFW_IKBD_COUNT
.acia_done:
    move.l  (sp)+, a1
    move.l  (sp)+, d0
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134, Timer-C $114, Timer-D $110; ACIA
; $118 and Timer-B $120 too, until their own handlers go in). Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
userfw_dummy_irq:
    rte

    ifne    TIME_STUDY
; -------------------------------------------------------------------
; userfw_timera_tick -- the stopwatch's wrap (TIME_STUDY): every 256
; ticks of Timer-A, 1.04 ms.
userfw_timera_tick:
    addq.w  #1, UFW_SW_WRAPS
    rte

; study_report -- tells the RP point D2 and the stopwatch's low 16 bits
; (TIME_STUDY): three ROM3 reads, with the interrupts masked so that no other
; report (the VBL's) falls between them. Clobbers D0, D1, A1.
study_report:
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    move.w  UFW_SW_WRAPS, d1
    moveq   #0, d0
    move.b  MFP_TADR.w, d0
    neg.b   d0                            ; ticks into this wrap
    btst    #5, MFP_IPRA.w                ; a wrap not counted yet?
    beq.s   .sr_ok
    tst.b   d0
    bmi.s   .sr_ok                        ; it came after the read
    addq.w  #1, d1
.sr_ok:
    lsl.w   #8, d1
    or.w    d1, d0
    lea     STUDY_POINT_WINDOW, a1
    tst.b   (a1, d2.w)
    move.w  d0, d1
    lsr.w   #8, d1
    lea     STUDY_HI_WINDOW, a1
    tst.b   (a1, d1.w)
    and.w   #$FF, d0
    lea     STUDY_LO_WINDOW, a1
    tst.b   (a1, d0.w)
    move.w  (sp)+, sr
    rts
    endc

; -------------------------------------------------------------------
; userfw_reset_stub -- copied to UFW_RESET_STUB and run there by
; .cold_reset: position-independent, and it reads nothing from the
; cartridge. Waits UFW_PRE_RESET_WAIT (a couple of seconds on an 8 MHz
; ST), clears TOS's memory-valid magics so it takes this for a power-on,
; and jumps through the reset vector.
userfw_reset_stub:
    move.l  #UFW_PRE_RESET_WAIT, d0
.wait:
    subq.l  #1, d0
    bne.s   .wait
    clr.l   $420.w                    ; memvalid
    clr.l   $43A.w                    ; memval2
    clr.l   $51A.w                    ; memval3
    movea.l $4.w, a0
    jmp     (a0)
userfw_reset_stub_end:

; The Microwire commands userfw sends on the DMA path (see MW_DATA):
; %10 then a 3-bit command and its value.
userfw_mw_cmds:
    dc.w    $04E8                         ; master volume 0 dB
    dc.w    $0554                         ; left 0 dB
    dc.w    $0514                         ; right 0 dB
    dc.w    $0486                         ; treble flat
    dc.w    $0446                         ; bass flat
    dc.w    $0401                         ; mix: the YM with the DMA sound
userfw_mw_cmds_end:

; The NOP tail. This module is the last in the cartridge image, and
; firmware.py strips trailing zero bytes from it: the last word must be
; harmless to drop or to prefetch past. An app that includes
; inc/sidecart_functions.s here must keep the tail after it, so the
; senders' wait loop is never the image's last code (see main.s).
    even
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
userfw_end:
