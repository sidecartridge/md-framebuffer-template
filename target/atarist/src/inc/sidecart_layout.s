; SidecarTridge Multi-device: the cartridge window and the ROM3 signalling
; windows, as the ST sees them.
;
; Included by main.s and userfw.s, and by every module that talks to the RP:
; never hard-code an address in the window, use these names. The RP side is
; rp/src/include/cart_shared.h; tests/host/test_layout.py checks that the two
; agree.
;
;   $FA0000  CARTRIDGE                  m68k header + code (max 16 KB),
;                                       userfw's inline MOVEM blit included
;   $FA4000  CMD_MAGIC_SENTINEL_ADDR    4 B   RP->ST command word
;   $FA4004  RANDOM_TOKEN_ADDR          4 B   (legacy: the senders' handshake)
;   $FA4008  RANDOM_TOKEN_SEED_ADDR     4 B   (legacy: the senders' handshake)
;   $FA400C  FB_FRAME_COUNTER_ADDR      4 B   bumped as the last write of a publish
;   $FA4010  SHARED_VARIABLES           240 B (60 x 4-byte slots, the app's)
;   $FA4018  IKBD_OUT_ADDR              16 B  (slots 2..5: IKBD commands from the RP)
;   $FA4028  AUDIO_OUT_ADDR             2 B   (slot 6: the RP keeps the ST to the YM)
;   $FA402C  BLIT_MODE_ADDR             2 B   (slot 7: the CPU or the blitter copies the screen)
;   $FA4030  PROFILE_ADDR               2 B   (slot 8: the app's profile, read once at boot)
;   $FA4040  PALETTE_ADDR               32 B  (slots 12..19: 16 palette words)
;   $FA4100  AUDIO_BUFFER_ADDR          4096 B (YM volume pairs, or DMA samples)
;   $FA5100  BOOT_STATUS_ADDR           2 B   read once in pre_auto: 0 = start
;   $FA5102  BOOT_MESSAGE_ADDR          126 B why the RP refused; printed before GEM
;   $FA5180  APP_FREE_ADDR              ~12.4 KB for the app, up to the framebuffer
;   $FA8300  FRAMEBUFFER_ADDR           32000 B (320x200 4bpp, flush at the top)
;   $FAFFFF  end of the window

ROM4_ADDR               equ $FA0000
CARTRIDGE_CODE_SIZE     equ $4000       ; 16 KB max for the cartridge header + code
SHARED_BLOCK_ADDR       equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)          ; $FA4000
CMD_MAGIC_SENTINEL_ADDR equ SHARED_BLOCK_ADDR                          ; $FA4000
RANDOM_TOKEN_ADDR       equ (CMD_MAGIC_SENTINEL_ADDR + 4)              ; $FA4004
RANDOM_TOKEN_SEED_ADDR  equ (RANDOM_TOKEN_ADDR + 4)                    ; $FA4008
; Bumped by the RP as the last write of every publish; userfw blits only when
; its low word has changed since the last blit.
FB_FRAME_COUNTER_ADDR   equ (RANDOM_TOKEN_SEED_ADDR + 4)               ; $FA400C
SHARED_VARIABLES        equ (FB_FRAME_COUNTER_ADDR + 4)                ; $FA4010

; Commands for the IKBD from the RP (its input modes, rp/src/ikbd.c): a generation
; word, a length word and up to IKBD_OUT_MAX command bytes. Whenever the
; generation changes, userfw sends the bytes to the IKBD, one per VBL (a 0
; sends nothing that VBL), then reports the generation's low byte at
; IKBD_OUT_WINDOW. Bit 15 of the generation is set while the RP rewrites
; the block. Slots 2..5 of SHARED_VARIABLES.
IKBD_OUT_ADDR           equ (SHARED_VARIABLES + (2 * 4))               ; $FA4018
IKBD_OUT_GEN            equ IKBD_OUT_ADDR                              ; word
IKBD_OUT_LEN            equ (IKBD_OUT_ADDR + 2)                        ; word
IKBD_OUT_BYTES          equ (IKBD_OUT_ADDR + 4)                        ; IKBD_OUT_MAX bytes
IKBD_OUT_SIZE           equ 16
IKBD_OUT_MAX            equ (IKBD_OUT_SIZE - 4)
IKBD_OUT_BUSY_BIT       equ 15

; 16-entry ST palette, published by the RP and applied by userfw every VBL.
; Slots 12..19 of SHARED_VARIABLES.
PALETTE_ADDR            equ (SHARED_BLOCK_ADDR + $40)                  ; $FA4040
PALETTE_SIZE            equ 32                                         ; 16 words

; Audio, two ways. On an STE or a Mega STE the DMA sound chip plays 8-bit
; samples at the profile's rate from a ring in ST RAM (userfw's UFW_DMA_RING), which
; userfw fills every VBL from the audio buffer, its mirror: the ST reports
; where the chip plays (DMA_POS_WINDOW), copies the mirror from its last copy
; to AUDIO_DMA_LEAD bytes ahead of that, and the RP writes the mirror further
; ahead still. Elsewhere Timer-B plays (vA, vB) YM volume pairs, one pair per
; interrupt, from the first AUDIO_SLICES slices of the buffer, one VBL each:
; the VBL handler points A0 at the next slice and tells the RP which one
; (AUDIO_SLICE_WINDOW); the RP writes the slices ahead of it.
AUDIO_BUFFER_ADDR       equ (SHARED_BLOCK_ADDR + $100)                 ; $FA4100
AUDIO_BUFFER_SIZE       equ 4096
AUDIO_BUFFER_END        equ (AUDIO_BUFFER_ADDR + AUDIO_BUFFER_SIZE)    ; $FA5100
AUDIO_SLICES            equ 4
AUDIO_SLICE_BYTES       equ 1024        ; a VBL is 224 of them at 5,585 Hz, 878 at 21,943
AUDIO_SLICE_SHIFT       equ 10          ; log2(AUDIO_SLICE_BYTES)
AUDIO_DMA_RING_BYTES    equ AUDIO_BUFFER_SIZE   ; 16 VBLs at 12,517 Hz, 8 at 25,033
AUDIO_DMA_POS_UNIT      equ 16          ; the chip's position is reported by 16 bytes

; The app's profile (rp/src/include/profile.h), a word the RP writes before
; the ST boots and userfw reads once: how often the ST takes a new frame
; (every VBL, or every second one), Timer-B's count on the YM path (/4:
; 614,400 Hz / count), the DMA chip's mode and lead (the bytes userfw keeps
; copied ahead of it: 3 VBLs, so a missed VBL still finds samples). Slot 8.
PROFILE_ADDR            equ (SHARED_VARIABLES + (8 * 4))               ; $FA4030
PROFILE_50FPS           equ 0           ; 50 fps, YM 5,585 Hz, DMA 12,517 Hz
PROFILE_25FPS           equ 1           ; 25 fps, YM 21,943 Hz, DMA 25,033 Hz
PROFILE_50FPS_VBLS      equ 1
PROFILE_50FPS_TIMERB_COUNT equ 110
PROFILE_50FPS_DMA_MODE  equ $81         ; mono, 12,517 Hz
PROFILE_50FPS_DMA_LEAD  equ 768
PROFILE_25FPS_VBLS      equ 2
PROFILE_25FPS_TIMERB_COUNT equ 28
PROFILE_25FPS_DMA_MODE  equ $82         ; mono, 25,033 Hz
PROFILE_25FPS_DMA_LEAD  equ 1536

; Which way the ST plays the sound, as the RP asks (a word, read once at
; boot): AUDIO_OUT_AUTO takes the DMA chip when the machine has one,
; AUDIO_OUT_YM keeps to the YM. Slot 6 of SHARED_VARIABLES.
AUDIO_OUT_ADDR          equ (SHARED_VARIABLES + (6 * 4))               ; $FA4028
AUDIO_OUT_AUTO          equ 0
AUDIO_OUT_YM            equ 1

; Who copies the cart framebuffer to the screen page, as the RP asks (a
; word, read every VBL): the low byte is the mode, the high byte the chunks
; of 48 bytes the blitter copies at a time (0: BLIT_PIECE_DEFAULT). Without
; a blitter the CPU copies whatever the mode. Slot 7 of SHARED_VARIABLES.
BLIT_MODE_ADDR          equ (SHARED_VARIABLES + (7 * 4))               ; $FA402C
BLIT_MODE_AUTO          equ 0           ; the blitter on the DMA sound path, else the CPU
BLIT_MODE_CPU           equ 1           ; the 68000's MOVEM loop
BLIT_MODE_BLITTER       equ 2           ; the blitter, whatever the sound path
BLIT_PIECE_DEFAULT      equ 40          ; about 1 ms: an IKBD byte waits less than its 1.28 ms

; Boot block. The RP can refuse to start the app (st_session_veto_boot()): a
; non-zero status makes pre_auto print the NUL-terminated message and return
; to GEM.
BOOT_STATUS_ADDR        equ AUDIO_BUFFER_END                           ; $FA5100
BOOT_MESSAGE_ADDR       equ (BOOT_STATUS_ADDR + 2)                     ; $FA5102
BOOT_MESSAGE_SIZE       equ 126

; The app's own buffers, up to the framebuffer.
APP_FREE_ADDR           equ (BOOT_MESSAGE_ADDR + BOOT_MESSAGE_SIZE)    ; $FA5180

; 320x200 low resolution, 4 bitplanes, at the top of the window so that an
; overrun walks off its end rather than over the block above.
FRAMEBUFFER_SIZE        equ 32000
FRAMEBUFFER_ADDR        equ (ROM4_ADDR + $10000 - FRAMEBUFFER_SIZE)    ; $FA8300
; Lines of it the blit copies each frame (the RP lays the framebuffer out
; for exactly this many: CART_FB_BLIT_LINES), and the bytes of one line.
FB_COPY_LINES           equ 200
FB_ROW_BYTES            equ 160                                        ; 320 px * 4 bpp / 8

; User firmware entry point: userfw.s at offset $0800 of the image
; (target/atarist/src/userfw.ld).
USERFW                  equ (ROM4_ADDR + $800)                         ; $FA0800

; Values of the command word at CMD_MAGIC_SENTINEL_ADDR.
CMD_NOP                 equ 0           ; nothing to do
CMD_RESET               equ 1           ; cold-reset the ST (the RP goes to Booster)
CMD_BOOT_GEM            equ 2           ; leave for GEM
CMD_TERMINAL            equ 3           ; (legacy: the terminal of the upstream template)
CMD_START               equ 4           ; hand control to the user firmware (USERFW)

; The ST tells the RP things by reading ROM3 addresses: the high byte of the
; address says what the read means, the low byte carries a value.
ROMCMD_START_ADDR       equ $FB0000
IKBD_WINDOW_BASE        equ (ROMCMD_START_ADDR + $8200)  ; + an IKBD byte
IKBD_COUNT_WINDOW       equ (ROMCMD_START_ADDR + $8300)  ; + IKBD bytes read so far, low byte (every VBL)
VBLSYNC_ADDR            equ (ROMCMD_START_ADDR + $8400)  ; blit done: the framebuffer is free
IKBD_OVERRUN_ADDR       equ (ROMCMD_START_ADDR + $8500)  ; the keyboard ACIA overran
AUDIO_SLICE_WINDOW      equ (ROMCMD_START_ADDR + $8600)  ; + the audio slice playing from this VBL
IKBD_OUT_WINDOW         equ (ROMCMD_START_ADDR + $8700)  ; + low byte of the IKBD command generation sent
ST_HELLO_WINDOW         equ (ROMCMD_START_ADDR + $8800)  ; + the machine: hello, a new session
ST_TOS_HI_WINDOW        equ (ROMCMD_START_ADDR + $8900)  ; + TOS version, high byte
ST_TOS_LO_WINDOW        equ (ROMCMD_START_ADDR + $8A00)  ; + TOS version, low byte
STUDY_POINT_WINDOW      equ (ROMCMD_START_ADDR + $8B00)  ; + a point of the loop (TIME_STUDY), then
STUDY_HI_WINDOW         equ (ROMCMD_START_ADDR + $8C00)  ; + its stopwatch ticks, high byte, and
STUDY_LO_WINDOW         equ (ROMCMD_START_ADDR + $8F00)  ; + low byte
DMA_POS_WINDOW          equ (ROMCMD_START_ADDR + $8D00)  ; + where the DMA chip plays, / AUDIO_DMA_POS_UNIT
ST_FEATURES_WINDOW      equ (ROMCMD_START_ADDR + $8E00)  ; + ST_FEATURE_* bits, once per boot
ST_FEATURE_BLITTER      equ 1
