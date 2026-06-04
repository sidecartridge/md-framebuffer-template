; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000

; Shared 64 KB region layout (must match rp/src/include/cart_shared.h).
;
;   $FA0000  CARTRIDGE			m68k header + code (max 16 KB)
;					Includes the unrolled MOVEM block
;					(fbdrv.s) at offset $2000.
;   $FA4000  CMD_MAGIC_SENTINEL_ADDR	4 B
;   $FA4004  RANDOM_TOKEN_ADDR		4 B  (legacy / unused since Epic 3.8)
;   $FA4008  RANDOM_TOKEN_SEED_ADDR	4 B  (legacy / unused since Epic 3.8)
;   $FA400C  FB_FRAME_COUNTER_ADDR	4 B
;   $FA4010  SHARED_VARIABLES		240 B (60 x 4-byte slots, app-free).
;   $FA4100  APP_FREE_ADDR	      ~16.5 KB free arena, ends at FRAMEBUFFER
;   $FA8300  FRAMEBUFFER_ADDR	      32000 B (320x200 4bpp, flush at top)
;   $FAFFFF  end of region

CARTRIDGE_CODE_SIZE	equ $4000	; 16 KB max for cartridge header + code + fbdrv
SHARED_BLOCK_ADDR	equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)		; $FA4000
CMD_MAGIC_SENTINEL_ADDR	equ SHARED_BLOCK_ADDR				; $FA4000

; 16-entry ST palette slot (Epic 5). 32 bytes of 16-bit palette
; words published by the RP, applied to $FFFF8240..$FFFF825E by
; userfw_vbl_loop. Slot 12 of SHARED_VARIABLES (offset +$30).
PALETTE_ADDR		equ (SHARED_BLOCK_ADDR + $40)			; $FA4040
PALETTE_SIZE		equ 32						; 16 words

FRAMEBUFFER_SIZE	equ 32000	; 320x200 low-res (4bpp) framebuffer
FRAMEBUFFER_ADDR	equ (ROM4_ADDR + $10000 - FRAMEBUFFER_SIZE)	; $FA8300

; Audio sample buffer (256 bytes). YM ch A 4-bit DAC nibbles, one
; byte per sample. Read sequentially by the m68k Timer-B IRQ handler
; in userfw.s; filled by RP-side audio.c with log-LUT-mapped samples.
AUDIO_BUFFER_ADDR	equ (SHARED_BLOCK_ADDR + $100)			; $FA4100
AUDIO_BUFFER_SIZE	equ 1024
AUDIO_BUFFER_END	equ (AUDIO_BUFFER_ADDR + AUDIO_BUFFER_SIZE)	; $FA4500

; APP_FREE starts after the audio buffer.
APP_FREE_ADDR		equ AUDIO_BUFFER_END				; $FA4500
FBDRV_ADDR		equ (ROM4_ADDR + $2000)				; $FA2000 (MOVEM loop cart->ST screen copy)

; Transitional: the pre-Story-1.2 boot UI fills only the first 8000 bytes
; of the framebuffer with a 1bpp u8g2 image, and the .print_loop_low
; copy loop below expands that mono buffer to fit the 32000-byte ST
; screen. Story 1.2.6+ replaces that loop with the native 4bpp fbdrv
; copy and this constant goes away.
MONO_UI_BUFFER_SIZE	equ 8000

; User firmware entry point. The cartridge image places userfw.s at
; offset $0800 of BOOT.BIN via target/atarist/src/userfw.ld; main.s
; gets the first 2 KB ($0000..$07FF), userfw gets the next 6 KB
; ($0800..$1FFF), and fbdrv.s occupies the rest of the 16 KB cart
; budget ($2000..$3FFF). The CARTRIDGE_CODE_SIZE = 16 KB cap covers all.
USERFW			equ (ROM4_ADDR + $800)				; $FA0800

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code
PRE_RESET_WAIT		equ $FFFFF

; If 1, the display will not use the framebuffer and will write directly to the
; display memory. This is useful to reduce the memory usage in the rp2040
; When not using the framebuffer, the endianness swap must be done in the atari ST
DISPLAY_BYPASS_FRAMEBUFFER 	equ 1

CMD_NOP				equ 0		; No operation command
CMD_RESET			equ 1		; Reset command
CMD_BOOT_GEM		equ 2		; Boot GEM command
CMD_TERMINAL		equ 3		; Terminal command
CMD_START			equ 4		; Hand control to the user firmware (USERFW)

_conterm			equ $484	; Conterm device number


; Constants needed for the commands
RANDOM_TOKEN_ADDR:        equ (CMD_MAGIC_SENTINEL_ADDR + 4)  ; $FA4004
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4)        ; $FA4008
; $FA400C: Framebuffer dirty-frame counter. RP increments it after
; every fb_render_frame() (with a memory barrier); userfw.s reads it
; each VBL and skips the cart->ST blit + video flip when unchanged.
FB_FRAME_COUNTER_ADDR:    equ (RANDOM_TOKEN_SEED_ADDR + 4)   ; $FA400C
RANDOM_TOKEN_POST_WAIT:   equ $1                             ; Wait cycles after the RNG is ready
COMMAND_TIMEOUT           equ $0000FFFF                      ; Timeout for the command
COMMAND_WRITE_TIMEOUT     equ COMMAND_TIMEOUT                ; Timeout for write commands

SHARED_VARIABLES:         equ (FB_FRAME_COUNTER_ADDR + 4)    ; $FA4010 (60 indexed 4-byte slots, app-free)

ROMCMD_START_ADDR:        equ $FB0000					  ; We are going to use ROM3 address
CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings
; App commands for the terminal
APP_TERMINAL 				equ $0 ; The terminal app

; App terminal commands
APP_TERMINAL_START   		equ $0 ; Start terminal command
APP_TERMINAL_KEYSTROKE 		equ $1 ; Keystroke command

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer
; _p_cookies ($5a0) lives in inc/sidecart_functions.s (also used by
; that file's detect_hw).


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Vsync wait
vsync_wait          macro
					move.w #37,-(sp)
					trap #14
					addq.l #2,sp
                    endm    

; XBIOS GetRez
; Return the current screen resolution in D0
get_rez				macro
					move.w #4,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; Check the left or right shift key. If pressed, exit.
check_shift_keys	macro
					move.w #-1, -(sp)			; Read all key status
					move.w #$b, -(sp)			; BIOS Get shift key status
					trap #13
					addq.l #4,sp

					btst #1,d0					; Left shift skip and boot GEM
					bne boot_gem

					btst #0,d0					; Right shift skip and boot GEM
					bne boot_gem

					endm

; Check the keys pressed
check_keys			macro

					gemdos	Cconis,2		; Check if a key is pressed
					tst.l d0
					beq .\@no_key

					gemdos	Cnecin,2		; Read the key pressed

					cmp.b #27, d0		; Check if the key is ESC
					beq .\@esc_key	; If it is, send terminal command

					move.l d0, d3
					send_sync APP_TERMINAL_KEYSTROKE, 4

					bra .\@no_key
.\@esc_key:
					send_sync APP_TERMINAL_START, 0

.\@no_key:

					endm

check_commands		macro
					move.l CMD_MAGIC_SENTINEL_ADDR, d6	; Store in the D6 register the remote command value
					cmp.l #CMD_TERMINAL, d6		; Check if the command is a terminal command
					bne.s .\@check_reset

					; Check the keys for the terminal emulation
					check_keys
					bra .\@bypass
.\@check_reset:
					cmp.l #CMD_RESET, d6		; Check if the command is a reset
					beq .reset					; If it is, reset the computer
					cmp.l #CMD_BOOT_GEM, d6		; Check if the command is to boot GEM
					beq boot_gem				; If it is, boot GEM
					cmp.l #CMD_START, d6		; Check if the command hands over to USERFW
					beq rom_function			; If it is, jump to the user firmware dispatcher

					; If we are here, the command is a NOP
					; If the command is a NOP, check the shift keys to bypass the command
					; check_shift_keys
					check_keys
.\@bypass:
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $4000 (16 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "TERM",0
    even

pre_auto:
; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; We assume the screen memory address is in D0 after the get_screen_base call
	move.l d0, a6				; Save the screen memory address in A6

; Enable bconin to return shift key status
	or.b #%1000, _conterm.w

; Get the resolution of the screen. High-res (640x400 mono) is not
; supported by the framebuffer template; bail to GEM with a message
; mirroring md-sprites-demo's lowres_only branch.
	get_rez
	cmp.w #2, d0
	beq .highres_unsupported

; Story 1.2: the old mono boot-UI loop (.print_loop_low, which read the
; first 8 KB of the cartridge framebuffer and expanded it 1bpp -> 4bpp
; into the ST screen) is gone. With u8g2 removed there's nothing left
; to render in mono, and the expander mis-mapped any 4bpp content
; written to the cart FB (40 cart bytes -> 1 ST row, so rows 0..4 of
; a 4bpp image landed on ST rows 0, 4, 8, 12, 16). Boot straight into
; the user firmware: userfw owns the VBL loop and runs fbdrv (or, on
; STE-class machines, an inline blitter copy) which copies the cart FB
; to ST screen verbatim with the correct 4bpp planar interpretation.
	jmp USERFW

.highres_unsupported:
	print .highres_unsupported_txt
	bra boot_gem

.highres_unsupported_txt:
	dc.b "High resolution (640x400) not supported.",$d,$a
	dc.b "Switch to low or medium res and reboot.",$d,$a
	dc.b 0
	even

.reset:
    move.l #PRE_RESET_WAIT, d6
.wait_me:
    subq.l #1, d6           ; Decrement the outer loop
    bne.s .wait_me          ; Wait for the timeout

	clr.l $420.w			; Invalidate memory system variables
	clr.l $43A.w
	clr.l $51A.w
	move.l $4.w, a0			; Now we can safely jump to the reset vector
	jmp (a0)
	nop

boot_gem:
	; If we get here, continue loading GEM
    rts

; Dispatcher for the user firmware module. Reached on CMD_START via the
; sentinel poll in check_commands. The cartridge image places userfw.s
; at offset $0800 (USERFW = $FA0800) through target/atarist/src/userfw.ld;
; main.s simply hands control over with a one-way jmp. Apps that want
; to chain multiple modules can change this to a sequence of jsr / jmp
; the same way md-drives-emulator's rom_function dispatches into
; GEMDRIVE/FLOPPYEMUL/ACSIEMUL/RTCEMUL.
rom_function:
    jmp USERFW

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"


end_rom_code:
end_pre_auto:
	even
	dc.l 0