; User firmware module
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; This module is the m68k-side "user firmware" that main.s hands control
; to once the RP signals CMD_START via the cartridge sentinel. The
; cartridge image places this file at offset $0800 (USERFW = $FA0800);
; main.s reaches it through `rom_function: jmp USERFW`.
;
; Replace this body with whatever your app needs to run on the Atari ST
; side: read shared variables, send commands via send_sync, render
; directly to screen RAM, etc. The shared-region symbols defined in
; main.s (RANDOM_TOKEN_ADDR, SHARED_VARIABLES, APP_FREE_ADDR, ...) are
; available here too via the include.
;
; Demo: this stub prints "Example firmware load..." to the GEMDOS
; console (which lands on the Atari ST screen) and returns. The
; cartridge is reached after GEMDOS init (CA_INIT bit 27 set in
; main.s's header), so the Cconws trap is safe to use here.

	section text

; GEMDOS Cconws: print null-terminated string to console.
;   trap #1, function 9 (.w on stack), string ptr (.l on stack).
GEMDOS_Cconws		equ 9

userfw:
	lea	hello_msg(pc), a0	; address of message
	move.l	a0, -(sp)		; push string pointer
	move.w	#GEMDOS_Cconws, -(sp)	; push function code
	trap	#1			; call GEMDOS
	addq.l	#6, sp			; clean up arguments
	rts

hello_msg:
	; ESC E = VT52 clear screen + home cursor
	dc.b	27,"E"
	dc.b	"Example firmware load..."
	dc.b	0,255
	even
	dc.l 0
		