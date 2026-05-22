/**
 * File: fb_draw.h
 * Description: Pixel-mask table used by the text renderer to poke
 *              individual pixels into the 4-bitplane framebuffer.
 *
 * Ported from md-sprites-demo's `rp/src/include/vga/draw.h`. Only the
 * text-rendering primitives survive — sprites, tiles, color-index
 * helpers, RGB6 packing, and the ST-planar conversion all stay behind
 * because Story 1.2 only ports text rendering (Q5 / Q2).
 *
 * Pico SDK note: `pixel_masks_flat` is parked in `scratch_x` so it
 * doesn't compete with hot ROM/FB paths on the AHB; if the bank
 * disappears in the future, the section attribute on the array's
 * definition can be removed without affecting correctness.
 */

#ifndef FB_DRAW_H
#define FB_DRAW_H

#include <stdint.h>

#include "pico/stdlib.h"

#define FB_PIXEL_MASK_TABLE_SIZE 256
#define FB_NUM_BITPLANES 4
#define FB_GROUP_PIXELS 4
#define FB_BLOCK_PIXELS 16

#ifdef __cplusplus
extern "C" {
#endif

/* Precomputed per-pixel masks. Index layout: (palette_index << 4) |
 * pixel_x (0..15). palette_index 0xF acts as a clear mask covering all
 * bitplanes. */
extern uint64_t pixel_masks_flat[FB_PIXEL_MASK_TABLE_SIZE];

void __not_in_flash_func(init_pixel_masks)(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_DRAW_H */
