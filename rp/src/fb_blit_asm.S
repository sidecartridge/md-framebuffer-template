@ File: fb_blit_asm.S
@
@ Hand-written Thumb: fb_blit_key_row -- one row of a colour-keyed
@ (transparent) sprite blit. Copies cw
@ bytes src->dst, skipping bytes equal to `key`.
@
@ Why asm: the C inner loop at -O3 spends ~11 cyc/pixel, of which ~5 are
@ pure loop control (one index increment + compare + branch per pixel).
@ Unrolling x4 amortises that control over four pixels (~7.5 cyc/pixel).
@ It stays byte-wise on purpose -- sprite widths/positions are arbitrary
@ (the loserboy frames are 51 px wide), so source and destination rarely
@ share word alignment; a word-wide path would help only sporadically and
@ can't be hardware-validated here. Placed in .time_critical (RAM): it is
@ on the per-frame sprite path (the swarm blits ~120 sprites/frame).
@
@ void fb_blit_key_row(uint8_t *dst, const uint8_t *src, uint32_t cw,
@                      uint8_t key);
@   r0 = dst, r1 = src, r2 = cw, r3 = key.  r4 = counter, r5 = pixel byte.

        .cpu cortex-m0plus
        .thumb
        .syntax unified

        .section .time_critical.fb_blit_key_row, "ax", %progbits
        .thumb_func
        .global fb_blit_key_row
        .type fb_blit_key_row, %function
fb_blit_key_row:
        push    {r4, r5, lr}
        lsrs    r4, r2, #2             @ r4 = cw / 4  (full 4-pixel groups)
        beq     .Lrem
.Lgrp:
        ldrb    r5, [r1, #0]           @ pixel 0
        cmp     r5, r3
        beq     1f
        strb    r5, [r0, #0]
1:      ldrb    r5, [r1, #1]           @ pixel 1
        cmp     r5, r3
        beq     2f
        strb    r5, [r0, #1]
2:      ldrb    r5, [r1, #2]           @ pixel 2
        cmp     r5, r3
        beq     3f
        strb    r5, [r0, #2]
3:      ldrb    r5, [r1, #3]           @ pixel 3
        cmp     r5, r3
        beq     4f
        strb    r5, [r0, #3]
4:      adds    r1, r1, #4
        adds    r0, r0, #4
        subs    r4, r4, #1
        bne     .Lgrp
.Lrem:
        movs    r4, #3
        ands    r2, r4                 @ r2 = cw & 3  (leftover 0..3 pixels)
        beq     .Ldone
.Lremloop:
        ldrb    r5, [r1]
        adds    r1, r1, #1
        cmp     r5, r3
        beq     5f
        strb    r5, [r0]
5:      adds    r0, r0, #1
        subs    r2, r2, #1
        bne     .Lremloop
.Ldone:
        pop     {r4, r5, pc}
        .size   fb_blit_key_row, .-fb_blit_key_row
