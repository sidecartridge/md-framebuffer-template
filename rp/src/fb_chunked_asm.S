@ File: fb_chunked_asm.S
@
@ Hand-written Thumb assembly: fb_chunky_to_planar.
@ Multiplication-based bit transpose, no LUT.
@
@ Per 4-pixel group (uint32 q, palette index in low nibble of each byte):
@
@   nibble_K = (((q >> K) & 0x01010101) * 0x80402010) >> 28
@
@   Where bit 0 of `q & 0x01010101` is bit K of pixel 0 (lowest byte of
@   q), bit 8 is bit K of pixel 1, etc. Multiplying by 0x80402010
@   ("comb constant") scatters the four bits into the top 4 bits of the
@   result (positions 28-31), in order pixel 0 -> bit 31, ..., pixel 3
@   -> bit 28. >> 28 brings them to bits 0-3 of the nibble, with pixel
@   0 at the MSB (matching the Atari ST shifter's leftmost-pixel-is-
@   MSB convention).
@
@ Then nibble << position drops it at the right place inside the
@ 16-bit plane word for group g (g = 0..3): position = 12 - 4*g for
@ planes 0/2 (low half of the accumulator pair), 28 - 4*g for planes
@ 1/3 (high half).
@
@ Two accumulators:
@   r4 = P01 = (plane1 << 16) | plane0
@   r5 = P23 = (plane3 << 16) | plane2
@ Written as two STR uint32 at end of block. LE storage + cart-bus
@ byte-swap at word level = m68k sees 4 BE 16-bit plane words.
@
@ Loop control via end-pointer comparison in r8 (callee-saved).

        .cpu cortex-m0plus
        .thumb
        .syntax unified

        .section .time_critical.fb_c2p_half, "ax", %progbits
        .thumb_func
        .global fb_c2p_half
        .type fb_c2p_half, %function

@ void fb_c2p_half(uint16_t *dst, const uint8_t *src, const uint8_t *src_end);
@   r0 = dst (advances by 8 per block)
@   r1 = src (advances by 16 per block)
@   r2 = src_end (loop terminator, copied to r8 at entry so r2 can
@                 hold the multiplier constant during the inner loop)
@
@ Processes pixels in [src, src_end). Caller passes the start/end of
@ a chunked-buffer range and the corresponding planar destination;
@ the function makes no assumption about the FULL buffer size, so it
@ can be invoked twice (one half per core) for the Core 0 / Core 1
@ parallel pattern.
@
@ Register allocation (during inner loop):
@   r0 = dst (advances by 8 per block)
@   r1 = src (advances by 16 per block)
@   r2 = multiplier constant 0x80402010
@   r3 = mask constant 0x01010101
@   r4 = P01 accumulator (plane 0 in low 16, plane 1 in high 16)
@   r5 = P23 accumulator (plane 2 in low 16, plane 3 in high 16)
@   r6 = nibble scratch
@   r7 = q (4-pixel chunked uint32)
@   r8 = src_end (callee-saved high reg)

@ Inner macro: extract one plane's 4-bit nibble from q (held in r7) and
@ OR it into the accumulator at the appropriate bit position.
@   K     = plane bit index (0..3)
@   shift = bit position inside the destination accumulator
@   acc   = r4 (P01) or r5 (P23)
        .macro NIBBLE K, shift, acc
        lsrs    r6, r7, #\K              @ extract bit K to LSB of each byte
        ands    r6, r3                   @ keep only bit K positions (r3 = mask)
        muls    r6, r2                   @ comb -> bits 28..31 of result (r2 = mul)
        lsrs    r6, r6, #28              @ bring to nibble at bits 0..3
        .if \shift > 0
        lsls    r6, r6, #\shift          @ place nibble in plane word
        .endif
        orrs    \acc, r6
        .endm

fb_c2p_half:
        push    {r4, r5, r6, r7, lr}
        mov     r4, r8                   @ stash r8 (callee-saved high reg)
        push    {r4}

        mov     r8, r2                   @ src_end -> r8

        ldr     r2, .Lmul                @ multiplier constant
        ldr     r3, .Lmask               @ AND mask constant

        @ Entry guard: empty / inverted range (src >= src_end) skips
        @ all work. The loop's own end check is at the BOTTOM (work-
        @ then-compare), so without this guard a zero-length call would
        @ write 8 bytes past dst and read 16 bytes past src before the
        @ comparison saw the exhaustion. BCC short-forwards into the
        @ loop; the unconditional B reaches .Ldone (out of conditional-
        @ branch range from here, but well within +-2 KB B range).
        cmp     r1, r8
        bcc     1f
        b       .Ldone
1:

.Lblock_loop:
        movs    r4, #0                   @ P01 = 0
        movs    r5, #0                   @ P23 = 0

        @ Group 0: pixels 0..3 -> nibble at plane-word bits 12..15
        ldr     r7, [r1, #0]
        NIBBLE 0, 12, r4
        NIBBLE 1, 28, r4
        NIBBLE 2, 12, r5
        NIBBLE 3, 28, r5

        @ Group 1: pixels 4..7 -> bits 8..11
        ldr     r7, [r1, #4]
        NIBBLE 0,  8, r4
        NIBBLE 1, 24, r4
        NIBBLE 2,  8, r5
        NIBBLE 3, 24, r5

        @ Group 2: pixels 8..11 -> bits 4..7
        ldr     r7, [r1, #8]
        NIBBLE 0,  4, r4
        NIBBLE 1, 20, r4
        NIBBLE 2,  4, r5
        NIBBLE 3, 20, r5

        @ Group 3: pixels 12..15 -> bits 0..3 (no LSL for shift == 0)
        ldr     r7, [r1, #12]
        NIBBLE 0,  0, r4
        NIBBLE 1, 16, r4
        NIBBLE 2,  0, r5
        NIBBLE 3, 16, r5

        @ Emit 4 plane words as 2 uint32 stores.
        str     r4, [r0, #0]             @ plane 0 + plane 1
        str     r5, [r0, #4]             @ plane 2 + plane 3

        adds    r1, r1, #16              @ src += 16
        adds    r0, r0, #8               @ dst += 8

        cmp     r1, r8
        bcs     .Ldone
        b       .Lblock_loop
.Ldone:

        pop     {r4}                     @ restore r8
        mov     r8, r4
        pop     {r4, r5, r6, r7, pc}

        .align 2
.Lmul:         .word 0x80402010
.Lmask:        .word 0x01010101

        .size   fb_c2p_half, .-fb_c2p_half

@ ---------------------------------------------------------------------------
@ void fb_chunk_reverse_copy48(uint8_t *dst, const uint8_t *src_last,
@                              uint32_t count);
@
@ The chunk-reversed publish from fb_planar_publish. Copies
@ `count` 48-byte (12-word) chunks to `dst` (forward) from `src_last`
@ (which points at the LAST chunk) in REVERSE chunk order: dst chunk k
@ gets src chunk (count-1-k). Each chunk's 48 bytes are copied forward.
@
@ Why asm: GCC at -O3 can't prove the `uint8_t*` pointers are word-aligned
@ and emits a `bl memcpy` per chunk (666/frame); forcing word copies gets
@ it to 12 ldr + 12 str/chunk but never ldm/stm. We know cart FB ($FA8300)
@ and `fb_planar_scratch` are 4-byte aligned and chunks are 48 = 12 words,
@ so ldmia/stmia (5+5+2 regs) is safe and ~halves the memory ops. Placed
@ in .time_critical (RAM) like the c2p worker -- it's on the per-frame
@ publish path that runs in the m68k's post-blit slack.
@
@   r0 = dst, r1 = src_last, r2 = count.  r3-r7 = chunk words.
        .section .time_critical.fb_chunk_reverse_copy48, "ax", %progbits
        .thumb_func
        .global fb_chunk_reverse_copy48
        .type fb_chunk_reverse_copy48, %function
fb_chunk_reverse_copy48:
        push    {r4, r5, r6, r7, lr}
        cmp     r2, #0
        beq     2f
1:
        ldmia   r1!, {r3, r4, r5, r6, r7}   @ words 0..4  (20 B), src += 20
        stmia   r0!, {r3, r4, r5, r6, r7}   @ dst += 20
        ldmia   r1!, {r3, r4, r5, r6, r7}   @ words 5..9  (20 B), src += 20
        stmia   r0!, {r3, r4, r5, r6, r7}   @ dst += 20
        ldmia   r1!, {r3, r4}               @ words 10..11 (8 B) -> 48 B
        stmia   r0!, {r3, r4}
        subs    r1, r1, #96                 @ src back to previous chunk start
        subs    r2, r2, #1
        bne     1b
2:
        pop     {r4, r5, r6, r7, pc}
        .size   fb_chunk_reverse_copy48, .-fb_chunk_reverse_copy48
