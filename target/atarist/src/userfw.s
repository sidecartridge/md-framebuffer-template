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
; IRQ ownership: TOS's HBL ($68), Timer-A/C/D ($134/$114/$110)
; handlers are stubbed to single-rte dummies and their MFP IERA/IERB
; bits are cleared so they cannot fire. The custom VBL handler at $70,
; Timer-B ($120, audio) and the keyboard ACIA ($118) stay active.
;
; IKBD bytes are forwarded by the keyboard ACIA's receive interrupt
; (userfw_acia_irq, MFP GPIP4): each byte as it arrives, emitted via a
; cart-bus read at IKBD_WINDOW_BASE + byte ($FB8200..$FB82FF). The RP
; captures the read via the commemul PIO+DMA ring (no per-read CPU
; overhead) and runs the IKBD demux from its main loop.
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

; Scratch word in TOS's _dskbufp ($4C6..$4C9). userfw does no disk
; I/O, so the slot is fair game while userfw owns the machine.
; .vbl_loop arms this to -1 then `stop`s; userfw_vbl clears it. The
; m68k re-stops on any non-VBL IRQ (Timer-B etc.) and only exits the
; wait when the VBL handler has cleared the flag.
UFW_VBL_FLAG          equ $4C6               ; word: cleared by userfw_vbl, polled after each `stop`

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
    movea.l #UFW_FB_SRC, a6
    rept    FBDRV_MAIN_ITERS
    movem.l (a6)+, d0-d7/a1-a4
    movem.l d0-d7/a1-a4, -(a5)
    endr
    ;
    ; Tail: copy the last FBDRV_TAIL_BYTES bytes of the blitted
    ; region that the chunked main loop can't reach (FB_COPY_LINES *
    ; 160 isn't a multiple of FBDRV_ITER_BYTES=48). A6 is at
    ; UFW_FB_SRC + FBDRV_MAIN_BYTES after the REPT; A5 is back at
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

; FB dirty-frame counter (lives in cart shared region at $FA400C). The
; RP fills the framebuffer and then writes a new value here as the
; LAST step of the frame. If this matches D4 (last seen) we skip the
; cart->ST blit + video flip entirely.
FB_FRAME_COUNTER      equ $00FA400C

; RP→m68k command sentinel at $FA4000. The RP IKBD demux writes
; CMD_BOOT_GEM here when it decodes an ESC keypress; userfw's main
; loop polls and exits back to GEM on match. Must agree
; with main.s's CMD_MAGIC_SENTINEL_ADDR / CMD_BOOT_GEM equs.
CMD_MAGIC_SENTINEL    equ $00FA4000
CMD_BOOT_GEM          equ 2

; 16-entry ST palette slot. 32 bytes of palette words
; published by the RP; .vbl_loop applies them to PALETTE_BASE each
; frame via a MOVEM-load + MOVEM-store. Mirrors main.s PALETTE_ADDR.
PALETTE_ADDR          equ $00FA4040
PALETTE_SIZE          equ 32

; Screen pages live just below TOS RAM top (TT-style 256 KB ST RAM
; assumption -- screens land at $70000/$78000, matching md-sprites-demo).
UFW_SCREEN_A          equ $00070000
UFW_SCREEN_B          equ $00078000
UFW_SCREEN_XOR        equ (UFW_SCREEN_A ^ UFW_SCREEN_B)

UFW_FB_SRC            equ $00FA8300           ; FRAMEBUFFER_ADDR

; --- YM2149 sound chip (single-channel A 4-bit DAC) ----------------
;
; PSG access: write a register number to $FFFF8800 (latch), then
; write data to $FFFF8802. Reg 8 = ch A volume (low 4 bits). We
; configure ch A as a "fake DAC": tone enabled, period = 0 (DC
; clamp above the audio band so the volume register is the only
; thing driving the output). Reg 8 stays latched after boot, so the
; Timer-B handler just writes a single byte to YM_DATA per fire.
YM_SELECT             equ $FFFF8800
YM_DATA               equ $FFFF8802
YM_REG_MIXER          equ 7                  ; tone+noise enables
YM_REG_CHA_VOL        equ 8                  ; channel A volume (low 4 bits)
YM_REG_CHB_VOL        equ 9                  ; channel B volume (low 4 bits)
YM_MIXER_DAC_CHA      equ $FE                ; tone A enabled, all other tones/noise off, ports out
YM_MIXER_DAC_AB       equ $FC                ; tones A AND B enabled, tone C off, all noise off, ports out (Ghostbusters dual-channel fake DAC)

; Cart-shared audio sample buffer (mirrors AUDIO_BUFFER_ADDR /
; AUDIO_BUFFER_SIZE in main.s and CART_AUDIO_BUFFER_OFFSET in
; rp/src/include/cart_shared.h). 256 bytes of YM ch A volume
; nibbles, filled by the RP and read by the Timer-B handler.
AUDIO_BUFFER_ADDR     equ $00FA4100
AUDIO_BUFFER_SIZE     equ 1024
AUDIO_BUFFER_END      equ (AUDIO_BUFFER_ADDR + AUDIO_BUFFER_SIZE)

; Number of 320-px lines the cart->ST blit covers per frame. Full ST
; low-res is 200; copying fewer leaves the bottom band of the
; destination ST page untouched (useful for a status row or to bound
; the blitter cost).
FB_COPY_LINES         equ 200         ; M68k copies all 200 lines (32000 bytes = 666 chunks * 48 B + 32-byte tail). Full screen blitted.
FB_ROW_BYTES          equ 160                 ; 320 px * 4 bpp / 8

; --- IKBD ownership ------------------------------------------------

; Keyboard ACIA at $FFFFFC00/02. MIDI ACIA at $FFFFFC04/06 is not
; touched. Status bit 0 = RX-data-ready; bit 1 = TX-empty.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02

; IKBD commands. While userfw runs the stream must be keyboard scancodes
; only: a mouse packet ($F8-$FB dx dy) or a joystick event ($FE/$FF state)
; has its header dropped by the RP's demux and its payload decoded as keys,
; so a nudge of the mouse (dx = +1 is $01) reads as an ESC press.
;
; For a while after a reset the IKBD undoes a mouse-off command, and TOS
; resets it moments before this cartridge code runs: on a cold boot the $12
; was sometimes lost and the mouse stayed on (seen on a Mega ST). So userfw
; resets the IKBD itself, waits for the reset's answer ($F0, $F1 on a later
; IKBD release), throws it away, lets IKBD_SETTLE_ITERS pass and only then
; turns the mouse and joysticks off. On the way out, TOS gets back what it
; set up: joystick events, then the relative mouse. The order matters: $14
; also turns the mouse off.
IKBD_CMD_RESET_HDR    equ $80    ; reset command header (followed by $01)
IKBD_CMD_RESET_RUN    equ $01    ; reset + self-test; answers $F0 (or $F1)
IKBD_CMD_MOUSE_REL    equ $08    ; relative mouse reporting (TOS's mode)
IKBD_CMD_MOUSE_OFF    equ $12    ; disable mouse reporting
IKBD_CMD_JOY_EVENTS   equ $14    ; joystick event reporting (TOS's mode)
IKBD_CMD_JOY_OFF      equ $1A    ; disable joysticks

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
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-A ack = bit 5)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_VR                equ $FFFFFA17          ; vector register (high nibble = vector base, bit 3 = S: 1=software EOI, 0=auto-EOI)
MFP_TACR              equ $FFFFFA19          ; Timer-A control register (cleared at boot for safety)
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register (delay-mode + prescaler)
MFP_TBDR              equ $FFFFFA21          ; Timer-B data register (8-bit countdown)

; Timer-B audio rate. MFP master clock = 2.4576 MHz. We pick a /4
; prescaler with count 110:
;   f = 2.4576 MHz / (4 * 110) = 5,585.45 Hz
; (~10.9% slower than STE-low's 6,258 Hz). The count was raised from
; 98 -> 110 to free ~1500 cyc/VBL for the FB_COPY_LINES=200 macro;
; sample.h is still generated at the older 6,269 Hz rate, so the
; jingle plays back ~11% lower pitch (about 2 semitones down) -- a
; modest but audible detune. Regenerate sample.h at 5585 Hz via
; wav_to_ym4.py if exact pitch matters. PAL VBL = 49.92 Hz so
; ~111.71 samples/VBL. At 2 bytes per sample (dual-ghost LUT) that's
; ~223 bytes/VBL in the cart buffer (audio.c's AUDIO_BYTES_PER_VBL
; = 224 matches this).
TIMERB_PRESCALER      equ 1                  ; /4 (delay mode)
TIMERB_COUNT          equ 110                ; ~5,585 Hz (~112 samples/PAL VBL)

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
IKBD_WINDOW_BASE      equ $FB8200

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
IKBD_OVERRUN_ADDR     equ $FB8500
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
VBLSYNC_ADDR          equ $FB8400

; Hello (rp/src/include/st_session.h): the ST and the RP reboot
; independently and the RP keeps its state across an ST reset, the command
; sentinel included, so after an exit to GEM the next boot would read
; CMD_BOOT_GEM and leave at once. userfw says hello at every boot with three
; ROM3 reads, the TOS version's two bytes then the machine; on the hello the
; RP writes CMD_NOP to the sentinel and starts its session over. Windows and
; the machine byte: see cart_shared.h and st_session.h.
ST_HELLO_WINDOW       equ $FB8800     ; + machine: family in bits 7..4, model in 3..0
ST_TOS_HI_WINDOW      equ $FB8900     ; + TOS version, high byte
ST_TOS_LO_WINDOW      equ $FB8A00     ; + TOS version, low byte
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

    ; --- IKBD ownership setup ------------------------------------
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

    ; Keyboard scancodes only from here on (see IKBD_CMD_MOUSE_OFF): reset
    ; the IKBD, wait for its answer, let it settle, then mouse and joysticks
    ; off. Interrupts are masked, so nothing else reads the ACIA meanwhile;
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
    IKBD_SEND IKBD_CMD_MOUSE_OFF
    IKBD_SEND IKBD_CMD_JOY_OFF

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

    ; --- Timer-B setup (audio @ ~6.27 kHz, STE-low-like) ---------
    ; Install our handler at $120 (overrides the dummy installed
    ; above). Load count -> TBDR, then prescaler -> TBCR starts
    ; the countdown. Enable + unmask Timer-B at the MFP. SR is
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
    move.b  #TIMERB_COUNT, MFP_TBDR.w
    move.b  #TIMERB_PRESCALER, MFP_TBCR.w

    ; Initialise A0 to the audio buffer base for the Timer-B handler.
    ; A0 is NOT in the FBDRV_INLINE MOVEM list and no other code in
    ; userfw touches it after this point, so the handler can rely on
    ; A0 holding a valid cart-buffer pointer at all times -- saves
    ; the push/pop around it in the hot IRQ path (-24 cyc/fire).
    movea.l #AUDIO_BUFFER_ADDR, a0

    bset    #0, MFP_IERA.w                ; Timer-B IRQ enable (IERA bit 0)
    bset    #0, MFP_IMRA.w                ; Timer-B IRQ unmask (IMRA bit 0)

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
    move.w  #-1, UFW_VBL_FLAG.w
.wait_vbl:
    stop    #$2300
    tst.w   UFW_VBL_FLAG.w
    bne.s   .wait_vbl

    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark
    endc

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

    ; A5 = END of the screen page chunk-covered region. FBDRV_INLINE
    ; uses predec MOVEM (`movem.l list, -(a5)`) and walks A5 backwards
    ; from page_end down to page_start as it stores chunks in reverse
    ; order. After FBDRV_MAIN_ITERS iters A5 ends at the page START,
    ; which is the value .after_copy below expects in A5.
    movea.l UFW_SCREEN_PAGE, a5
    lea     (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)(a5), a5

    ; Pure 68000 CPU copy via the FBDRV_INLINE macro (defined in
    ; the constants block). Same code path on plain ST / STE /
    ; MegaSTE / TT / Falcon -- no _MCH cookie dispatch, no STE
    ; blitter.
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
    ; ESC detection: the RP-side IKBD demux writes
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

    ; Give TOS its mouse and joysticks back (see IKBD_CMD_MOUSE_OFF).
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

; -------------------------------------------------------------------
; userfw_vbl -- VBL interrupt handler. Two jobs:
;   1. Reset A0 to AUDIO_BUFFER_ADDR. This is the cart-buffer base,
;      and Timer-B will start consuming samples from offset 0 on
;      the next IRQ. Pinning A0 = base once per VBL eliminates the
;      explicit `cmpa.l + bcs.s` wrap in the Timer-B hot path, so
;      that handler shrinks to a single `move.b (a0)+, YM_DATA.w`
;      + rte. A0 is dedicated to audio (excluded from the
;      FBDRV_INLINE MOVEM list and from the ACIA handler), so it's
;      safe to overwrite here from IRQ context.
;   2. Clear UFW_VBL_FLAG so .vbl_loop's `stop`-then-check wait can
;      distinguish a VBL wake from a Timer-B (or other MFP) wake.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. The ACIA IRQ ($118) is userfw's own (userfw_acia_irq), so
; GEMDOS's keyboard buffer is no longer filled; the keys go to the RP.
userfw_vbl:
    movea.l #AUDIO_BUFFER_ADDR, a0
    clr.w   UFW_VBL_FLAG.w
    rte

; -------------------------------------------------------------------
; userfw_timerb_audio -- Timer-B IRQ handler. Fires at ~12.5 kHz
; when Timer-B is running in /4 delay mode with TBDR=49.
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
; A0 is a DEDICATED cart audio-buffer cursor (userfw_vbl resets it
; to AUDIO_BUFFER_ADDR each VBL; postinc walks 2 bytes/fire).
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
; At 12,539 Hz: 68 * 251 = ~17.1 k cyc/VBL = 2.1 ms = 10.7% CPU.
; Combined with FB_COPY_LINES=100 macro (~8.8 ms / VBL = 44%),
; total VBL load ~55%, leaving ~9.1 ms slack.
userfw_timerb_audio:
    move.b  (a0)+, YM_DATA.w               ; vA -> ch A vol
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; latch ch B vol reg
    move.b  (a0)+, YM_DATA.w               ; vB -> ch B vol
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; back to ch A vol reg for next fire
    rte

; -------------------------------------------------------------------
; userfw_acia_irq -- keyboard ACIA receive interrupt (MFP GPIP4, $118).
; Forwards every byte the ACIA holds, one cart-bus read each, and only
; then returns: the MFP sees an edge only when the ACIA's IRQ line goes
; down, so a byte left behind would never interrupt again. Each pass
; takes several microseconds (the ACIA sits on the slow E-clock bus), so
; RX-ready has cleared before it is tested again. It can fire between a
; MOVEM load and its store in FBDRV_INLINE: it saves D0 and A1 and never
; touches A0 (the audio cursor). Auto-EOI: no in-service bit to clear.
userfw_acia_irq:
    movem.l d0/a1, -(sp)
    lea     IKBD_WINDOW_BASE, a1
.acia_next:
    move.b  ACIA_KBD_STATUS.w, d0
    btst    #0, d0                        ; a byte waiting?
    beq.s   .acia_done
    btst    #5, d0                        ; overrun: bytes were lost before it
    beq.s   .acia_read
    tst.b   IKBD_OVERRUN_ADDR
.acia_read:
    moveq   #0, d0
    move.b  ACIA_KBD_DATA.w, d0           ; clears RX-ready and the overrun
    tst.b   (a1, d0.w)                    ; forward it
    bra.s   .acia_next
.acia_done:
    movem.l (sp)+, d0/a1
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134, Timer-C $114, Timer-D $110; ACIA
; $118 and Timer-B $120 too, until their own handlers go in). Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
userfw_dummy_irq:
    rte

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
