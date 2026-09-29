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
;   $FA4040  PALETTE_ADDR               32 B  (slots 12..19: 16 palette words)
;   $FA4100  AUDIO_BUFFER_ADDR          1024 B (YM volume pairs)
;   $FA4500  BOOT_STATUS_ADDR           2 B   read once in pre_auto: 0 = start
;   $FA4502  BOOT_MESSAGE_ADDR          126 B why the RP refused; printed before GEM
;   $FA4580  APP_FREE_ADDR              ~15.4 KB for the app, up to the framebuffer
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

; 16-entry ST palette, published by the RP and applied by userfw every VBL.
; Slots 12..19 of SHARED_VARIABLES.
PALETTE_ADDR            equ (SHARED_BLOCK_ADDR + $40)                  ; $FA4040
PALETTE_SIZE            equ 32                                         ; 16 words

; Audio: (vA, vB) YM volume pairs, one pair per Timer-B interrupt, in
; AUDIO_SLICES slices of one VBL each. The VBL handler points A0 at the next
; slice and tells the RP which one (AUDIO_SLICE_WINDOW); the RP writes the
; slices ahead of it, never the one playing.
AUDIO_BUFFER_ADDR       equ (SHARED_BLOCK_ADDR + $100)                 ; $FA4100
AUDIO_BUFFER_SIZE       equ 1024
AUDIO_BUFFER_END        equ (AUDIO_BUFFER_ADDR + AUDIO_BUFFER_SIZE)    ; $FA4500
AUDIO_SLICES            equ 4
AUDIO_SLICE_BYTES       equ 256         ; one VBL is 224 of them at ~5,585 Hz
AUDIO_SLICE_SHIFT       equ 8           ; log2(AUDIO_SLICE_BYTES)

; Boot block. The RP can refuse to start the app (st_session_veto_boot()): a
; non-zero status makes pre_auto print the NUL-terminated message and return
; to GEM.
BOOT_STATUS_ADDR        equ AUDIO_BUFFER_END                           ; $FA4500
BOOT_MESSAGE_ADDR       equ (BOOT_STATUS_ADDR + 2)                     ; $FA4502
BOOT_MESSAGE_SIZE       equ 126

; The app's own buffers, up to the framebuffer.
APP_FREE_ADDR           equ (BOOT_MESSAGE_ADDR + BOOT_MESSAGE_SIZE)    ; $FA4580

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
VBLSYNC_ADDR            equ (ROMCMD_START_ADDR + $8400)  ; blit done: the framebuffer is free
IKBD_OVERRUN_ADDR       equ (ROMCMD_START_ADDR + $8500)  ; the keyboard ACIA overran
AUDIO_SLICE_WINDOW      equ (ROMCMD_START_ADDR + $8600)  ; + the audio slice playing from this VBL
ST_HELLO_WINDOW         equ (ROMCMD_START_ADDR + $8800)  ; + the machine: hello, a new session
ST_TOS_HI_WINDOW        equ (ROMCMD_START_ADDR + $8900)  ; + TOS version, high byte
ST_TOS_LO_WINDOW        equ (ROMCMD_START_ADDR + $8A00)  ; + TOS version, low byte
