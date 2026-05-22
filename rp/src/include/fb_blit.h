/**
 * File: fb_blit.h
 * Description: Bitmap / sprite blit primitives for the chunked
 *              framebuffer (Epic 2 Story 2.3).
 *
 * All blits target `fb_chunked_buffer` (one byte per pixel, palette
 * index in low nibble). Bitmaps are flat `uint8_t` arrays in
 * row-major order (row stride == width).
 *
 * Pattern ported from md-sprites-demo / pico-vga-6bit-demo: the
 * chunked layout makes bitmap blits a trivial memcpy per row,
 * optionally with a per-pixel color-key test for sprite transparency.
 */

#ifndef FB_BLIT_H
#define FB_BLIT_H

#include <stdint.h>

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Static bitmap descriptor. `data` must be `width * height` bytes,
 *  row-major. Each byte is a palette index (low nibble used). */
struct FB_BITMAP {
  uint16_t width;
  uint16_t height;
  const uint8_t *data;
};

/** Fill a rectangle in the chunked buffer with a single palette index.
 *  Clipped to the chunked buffer bounds. Negative x/y allowed. */
void __not_in_flash_func(fb_fill_rect)(int x, int y, int w, int h,
                                       uint8_t color);

/** Opaque bitmap blit: copies every source pixel to the chunked
 *  buffer. Clipped to bounds. Negative dst_x/dst_y allowed. */
void __not_in_flash_func(fb_blit)(const struct FB_BITMAP *bm, int dst_x,
                                  int dst_y);

/** Transparent bitmap blit: pixels equal to `key` are skipped, all
 *  others copied. Clipped to bounds. Used for sprites with a
 *  transparent color. */
void __not_in_flash_func(fb_blit_key)(const struct FB_BITMAP *bm, int dst_x,
                                      int dst_y, uint8_t key);

#ifdef __cplusplus
}
#endif

#endif /* FB_BLIT_H */
