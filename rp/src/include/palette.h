/**
 * File: palette.h
 * Description: 16-entry ST hardware-palette publisher.
 *
 * The RP writes a 16-word palette into the cart shared-region slot
 * at CART_PALETTE_OFFSET; the m68k VBL handler in userfw.s reads
 * the slot every frame and applies it to the shifter's palette
 * registers at $FFFF8240..$FFFF825E. uint16_t writes are
 * transparent across the cart-bus byte-swap, so the value stored
 * here is exactly what the m68k sees.
 *
 * Palette word format (standard ST 9-bit colour):
 *     0000.0RRR.0GGG.0BBB
 *
 * Use `PALETTE_RGB(r, g, b)` to build a word from three 0..7
 * channel values. STE 4-bits-per-channel extends this with a 4th
 * (low) bit in positions 3 / 7 / 11; the ST-only macro keeps
 * those bits zero so palettes are forward-compatible.
 *
 * The default palette (palette_init) follows the framebuffer
 * scaffolding's existing conventions:
 *
 *   - idx 0  = white   -- matches `font_set_color(0)` -> readable
 *                         text against any background. Also drives
 *                         the shifter border colour (no separate
 *                         border register on plain ST).
 *   - idx 15 = black   -- matches `clear-to-0xFF = background`
 *                         used by fb_init() / fb_render_frame().
 *   - idx 1..14        -- a mid palette of blue / warm / green /
 *                         earth shades intended to cover the
 *                         three demos with a single shared set.
 */

#ifndef PALETTE_H_INCLUDED
#define PALETTE_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PALETTE_ENTRIES 16

/* Build an ST palette word from three 0..7 channel values. Each
 * extra bit is masked off so callers can pass e.g. 0..15 by
 * accident without polluting the unused-bit positions. */
#define PALETTE_RGB(r, g, b)                                                  \
  ((uint16_t)(((((uint16_t)(r)) & 7u) << 8) |                                 \
              ((((uint16_t)(g)) & 7u) << 4) |                                 \
              (((uint16_t)(b)) & 7u)))

/* Publish the default palette to the cart slot. Idempotent. Call
 * once during boot after ERASE_FIRMWARE_IN_RAM. */
void palette_init(void);

/* Bulk overwrite the cart-slot palette. `entries` must point to
 * exactly PALETTE_ENTRIES uint16_t values. */
void palette_set(const uint16_t entries[PALETTE_ENTRIES]);

/* Overwrite a single palette entry. */
void palette_set_entry(uint8_t idx, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* PALETTE_H_INCLUDED */
