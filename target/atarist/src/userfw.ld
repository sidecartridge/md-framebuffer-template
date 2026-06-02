/* userfw.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR. Each module's .text section is placed at a fixed
 * offset within the image and main.s references that offset via the
 * matching `equ` (e.g. USERFW equ $FA0800 ↔ .text_userfw at 0x000800).
 *
 * Budget (must add up to <= CARTRIDGE_CODE_SIZE = 16 KB; the build
 * script asserts this against BOOT.BIN):
 *
 *   .text_main      0x0000..0x07FF  ( 2 KB)  m68k boot + dispatch
 *   .text_userfw    0x0800..0x3FFF  (14 KB)  user firmware module
 *                                            (includes the inlined
 *                                            cart->ST framebuffer
 *                                            copy via FBDRV_INLINE)
 *
 * fbdrv note: previously there was a third section .text_fbdrv at
 * offset 0x2000 holding a standalone MOVEM-loop copy routine that
 * .copy_st jsr'd into. The jsr was isolated as the source of a
 * cart-bus instability (Epic 4 Story 4.7 follow-up), so the routine
 * was inlined as a macro (FBDRV_INLINE) in userfw.s and the section
 * was removed. userfw's budget grew from 6 KB to 14 KB to absorb it.
 *
 * The template is now a pure 68000 CPU copy: no STE blitter path,
 * no _MCH dispatch -- same FBDRV_INLINE runs identically on plain
 * ST / STE / MegaSTE / TT / Falcon.
 *
 * Adding a new module is a two-step change:
 *   1. Pick an offset within the remaining cartridge budget and add a
 *      new ".text_<name> 0x????? : { <name>.o(.text) }" section here.
 *   2. Mirror the offset on the m68k side with `equ (ROM4_ADDR + $????)`
 *      in main.s, and add the .o target to target/atarist/Makefile.
 */
SECTIONS
{
    .text_main 0x000000 : {
        main.o(.text)
    }

    .text_userfw 0x000800 : {
        userfw.o(.text)
    }
}
