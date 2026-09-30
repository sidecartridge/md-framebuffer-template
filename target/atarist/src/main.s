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

; The cartridge window's layout, the command values and the ROM3 signalling
; windows, shared with userfw.s (must match rp/src/include/cart_shared.h).
	include inc/sidecart_layout.s

FBDRV_ADDR		equ (ROM4_ADDR + $2000)				; $FA2000 (MOVEM loop cart->ST screen copy)

; Left over from the removed mono boot UI, which filled the first 8000
; bytes of the framebuffer with a 1bpp u8g2 image that the
; .print_loop_low copy loop expanded to the 32000-byte ST screen. The
; native 4bpp copy replaced it; nothing references this constant.
MONO_UI_BUFFER_SIZE	equ 8000

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code
PRE_RESET_WAIT		equ $FFFFF

; If 1, the display will not use the framebuffer and will write directly to the
; display memory. This is useful to reduce the memory usage in the rp2040
; When not using the framebuffer, the endianness swap must be done in the atari ST
DISPLAY_BYPASS_FRAMEBUFFER 	equ 1

_conterm			equ $484	; Conterm device number


; Constants needed for the commands
RANDOM_TOKEN_POST_WAIT:   equ $1                             ; Wait cycles after the RNG is ready
COMMAND_TIMEOUT           equ $0000FFFF                      ; Timeout for the command
COMMAND_WRITE_TIMEOUT     equ COMMAND_TIMEOUT                ; Timeout for write commands

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

; The old mono boot-UI loop (.print_loop_low, which read the
; first 8 KB of the cartridge framebuffer and expanded it 1bpp -> 4bpp
; into the ST screen) is gone. With u8g2 removed there's nothing left
; to render in mono, and the expander mis-mapped any 4bpp content
; written to the cart FB (40 cart bytes -> 1 ST row, so rows 0..4 of
; a 4bpp image landed on ST rows 0, 4, 8, 12, 16). Boot straight into
; the user firmware: userfw owns the VBL loop and runs the FBDRV_INLINE
; copy, which copies the cart FB to ST screen verbatim with the correct 4bpp planar interpretation.
; Unless the RP refused to start the app: then print its reason and return
; to GEM, as for high resolution.
	tst.w BOOT_STATUS_ADDR
	bne.s .boot_vetoed
	jmp USERFW

.boot_vetoed:
	print BOOT_MESSAGE_ADDR
	print .crlf_txt
	bra boot_gem

.crlf_txt:
	dc.b $d,$a,0
	even

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

; The NOP tail. The senders' wait loop must never be the last code of a module:
; firmware.py strips trailing zero bytes from the image, pre_auto relocates
; start_rom_code..end_rom_code in whole longwords, and both the write sender
; (its code size includes 4 bytes past the loop) and the 68000's prefetch read
; past the loop's last word. Every module that includes sidecart_functions.s
; ends like this.
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
main_end:

end_rom_code:
end_pre_auto:
	even
	dc.l 0