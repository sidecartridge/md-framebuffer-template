/* userfw.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR. Each module's .text section is placed at a fixed
 * offset within the image and main.s references that offset via the
 * matching `equ` (e.g. USERFW equ $FA1000 ↔ .text_userfw at 0x000800).
 *
 * Budget (must add up to <= CARTRIDGE_CODE_SIZE = 8 KB; the build
 * script asserts this against BOOT.BIN):
 *
 *   .text_main    0x0000..0x07FF  ( 2 KB)  m68k boot + dispatch
 *   .text_userfw  0x0800..0x1FFF  ( 6 KB)  user firmware module
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
