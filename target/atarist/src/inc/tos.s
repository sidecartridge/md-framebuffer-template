; SidecarTridge Multi-device - An extension to Atari ST computers through the cartridge port
; Copyright (C) 2023-24 GOODDATA LABS SL
;

; TOS macros, definitions and constants 

; System calls
syscall	macro
	move.w	#\2,-(sp)               ; Push syscall identifier
	trap	#\1                     ; Do the call

	ifnc	'','\3'                 ; If a stack rewind parameter is passed
	ifgt	\3-8                    ;
	lea	\3(sp),sp                   ; Rewind the stack
	elseif                          ;
	addq.l	#\3,sp                  ; Rewind using addq
	endc                            ;
	endc                            ;
	endm

; GEMDOS system call
gemdos	macro
	syscall	1,\1,\2
	endm

Pterm0	macro
	clr.w	-(sp)
	trap	#1
	endm

EnterSuper	macro
	clr.l	-(sp)
	move.w	#Super,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,savestack
	endm

ExitSuper	macro
	move.l	savestack,-(sp)
	move.w	#Super,-(sp)
	trap	#1
	addq.l	#6,sp
	endm


print	macro	; Print a string
	pea	\1
	gemdos	Cconws,6
	endm

pchar	macro	; Print a character
	move.l	#(\1)!(Cconout<<16),-(sp)
	trap	#1
	addq.l	#4,sp
	endm

; Print a character from a register d0.b
pchar_reg	macro			; Print a character
			andi.l #$00FF, d0		; Clear d0 to use it for calculations
			move.w	d0,-(sp)
			gemdos	Cconout,4
			endm

; Macro to print a number between 0 and 99
; Number must be in d0.b
print_num	macro

	andi.l #$00FF, d0		; Clear d0 to use it for calculations

	; Extract tens digit
	divu.w	#$A, d0			; Divide by 10, quotient in MSB.w, remainder in LSB.w
	andi.l #$00FF00FF,d0 	; Keep only the quotient and remainder
	addi.l #$00300030,d0 	; Convert to ASCII both
	swap 	d0				; Swap to get the remainder
	move.w d0, -(sp)		; Push remainder
	swap	d0				; Swap to get the quotient
	move.w  d0, -(sp)		; Push tens digit
	move.w  #2, -(sp)		; Push 2 bytes to print
	trap #1					; Print tens digit
	addq.l	#4, sp			; Rewind stack

	; Extract ones digit
	; We have already pushed into the stack the remainder of the previous division
	move.w  #2, -(sp)		; Push 2 bytes to print
	trap #1					; Print ones digit
	addq.l	#4, sp			; Rewind stack

	endm

; Macro to print an hex number between 0 and 255
; Number must be in d0.l
; Number of nibbles to print must be in d1.l
print_hex 	macro
    movem.l d0-d7, -(sp)     ; Push all registers
    move.l  d0, d6           ; Copy D0 to D6 for manipulation
    rol.l #8, d6             ; Shift right by 8 bits to get the next byte
;    moveq   #7, d1           ; Counter for 8 nibbles (32 bits / 4 bits per nibble)

.\@print_next_nibble:
    move.l  d6, d3           ; Copy D2 to D3 to extract the nibble
    btst    #0, d1           ;
    beq.s   .\@print_low_nibble ; If the counter is even, print the low nibble
.\@print_high_nibble:
    lsr.l   #4, d3           ; Shift right by 4 bits to get the high nibble
    bra.s .\@print_nibble
.\@print_low_nibble:
    rol.l #8, d6             ; Shift right by 8 bits to get the next byte
.\@print_nibble:
    andi.l  #$0F, d3         ; Mask off all but the lower nibble
    cmpi.l  #$0A, d3         ; Compare with 10 to determine if it's A-F
    blt.s   .\@digit            ; If less than 10, it's a digit
    addi.l  #$37, d3         ; Convert to ASCII ('A' - 'F')
    bra.s   .\@print_char
.\@digit:
    addi.l  #$30, d3        ; Convert to ASCII ('0' - '9')
.\@print_char:
    move.w  d3, -(sp)        ; Push the character to print
    move.w  #2, -(sp)		; Push 2 bytes to print
    trap    #1               ; Print char
    addq.l  #4, sp           ; Rewind stack

    dbra    d1, .\@print_next_nibble  ; Decrement d1 and branch if not yet zero

    movem.l  (sp)+, d0-d7     ; Pop all registers

    endm


pchar2	macro
	movem.l	d0-d2/a0-a2,-(sp)
	pchar	\1
	pchar	\2
	movem.l	(sp)+,d0-d2/a0-a2
	endm

crlf	macro
	movem.l	d0-d2/a0-a2,-(sp)
	pchar	$0d
	pchar	$0a
	movem.l	(sp)+,d0-d2/a0-a2
	endm

bell	macro	; Ring the bell
	pchar	$07
	endm

ask	macro	; Ask: print a message, wait for a key and print its character
	pea	\1
	gemdos	Cconws
	gemdos	Cconin,8
	endm

asksil	macro	; Ask silently: print a message and wait for a key
	pea	\1
	gemdos	Cconws
;	gemdos	Cconis,8
	gemdos  Crawcin,8
	endm


; GEMDOS calls
Cconin		EQU	1
Cconout		EQU	2
Cauxout		EQU	4
Crawio		EQU	6
Crawcin		EQU	7
Cnecin		EQU	8
Cconws		EQU	9
Cconrs		EQU	10
Cconis		EQU	11
Dsetdrv		EQU	14
Cauxis		EQU	18
Dgetdrv		EQU	25
Fsetdta		EQU	26
Super		EQU	32
Tsetdate	EQU	43
Tsettime	EQU	45
Fgetdta		EQU	47
Sversion	EQU	48
Ptermres	EQU	49
Dfree		EQU	54
Dcreate		EQU	57
Ddelete		EQU	58
Dsetpath	EQU	59
Fcreate		EQU	60
Fopen		EQU	61
Fclose		EQU	62
Fread		EQU	63
Fwrite		EQU	64
Fdelete		EQU	65
Fseek		EQU	66
Fattrib		EQU	67
Fdup		EQU	69
Fforce		EQU	70
Dgetpath	EQU	71
Malloc		EQU	72
Mfree		EQU	73
Mshrink		EQU	74
Pexec		EQU	75
Pterm		EQU	76
Fsfirst		EQU	78
Fsnext		EQU	79
Frename		EQU	86
Fdatime		EQU	87

; BIOS system call
bios	macro
	syscall	13,\1,\2
	endm

Bconin		EQU	2
Bconout		EQU	3
Rwabs		EQU	4
Setexc		EQU	5
Getbpb		EQU	7
Mediach		EQU	9
Drvmap		EQU	10

; XBIOS system calls
xbios	macro
	syscall	14,\1,\2
	endm

Setcolor	EQU	7
Floprd		EQU	8
Flopwr		EQU	9
Flopfmt		EQU	10
Random		EQU	17

; System variables
flock		EQU	$43e                              ; Floppy semaphore
vbclock		EQU	$462                            ; vblank counter
bootdev		EQU	$446                            ; Boot device
hz200		EQU	$4ba                              ; 200Hz timer
nflops		EQU	$4a6                             ; Number of mounted floppies
drvbits		EQU	$4c2                            ; Mounted drives
sysbase		EQU	$4f2                            ; OSHEADER pointer
pun_ptr		EQU	$516                            ; PUN_INFO table
phystop		EQU	$42e                            ; Top of physical RAM
memtop		EQU	$436                             ; Top of TOS RAM
dskbufp		EQU	$4c6                            ; Disk buffers
bufl		EQU	$4b2                               ; Buffer lists

; Exception vectors
gemdos.vector	EQU	$84
bios.vector		EQU	$b4
xbios.vector	EQU	$b8
getbpb.vector	EQU	$472
rwabs.vector	EQU	$476
mediach.vector	EQU	$47e

; GEMDOS error codes
; From emuTOS
E_OK		EQU	0          ; OK, no error
ERR			EQU	-1          ; basic, fundamental error
EBADRQ		EQU	-5       ; bad rEQUest
ESECNF		EQU	-8       ; sector not found
EWRITF		EQU	-10      ; write fault
EREADF		EQU	-11      ; read fault
EWRPRO		EQU	-13      ; write protect
E_CHNG		EQU	-14      ; media change
EUNDEV		EQU	-15      ; Unknown device
EBADSF		EQU	-16      ; bad sectors on format
EOTHER		EQU	-17      ; insert other disk
EINVFN		EQU	-32      ; invalid file name
EFILNF		EQU	-33      ; file not found
EPTHNF		EQU	-34      ; path not found
EACCDN		EQU	-36      ; access denied
EBADF		EQU	-37       ; bad file descriptor
EDRIVE		EQU	-46      ; invalid drive specification
ECWD		EQU	-47        ; current dir cannot be deleted
ENSAME		EQU	-48      ; not the same drive
ENMFIL		EQU	-49      ; no more files
ERANGE		EQU	-64      ; seek out of range
EPLFMT		EQU	-66      ; invalid program load format

; DMA port hardware registers
dma			EQU	$ffff8604
dmadata		EQU	dma
dmactrl		EQU	dma+2
dmahigh		EQU	dma+5
dmamid		EQU	dma+7
dmalow		EQU	dma+9
gpip		EQU	$fffffa01


; Structures

; PD/BASEPAGE size
pd...		EQU	256
