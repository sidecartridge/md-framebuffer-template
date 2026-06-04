/**
 * File: fb_blit.h
 * Description: Bitmap / sprite blit primitives for the chunked
 *              framebuffer.
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

/** Band-clipped variants: as above but clip vertically to the half-open
 *  row window [band_y0, band_y1) instead of the full screen. Used for
 *  per-core dual-core rendering -- two cores each draw a disjoint band of
 *  the same buffer with no overlap. The full-screen forms
 *  above are just these with band = [0, FB_CHUNKED_H). */
void __not_in_flash_func(fb_blit_band)(const struct FB_BITMAP *bm, int dst_x,
                                       int dst_y, int band_y0, int band_y1);
void __not_in_flash_func(fb_blit_key_band)(const struct FB_BITMAP *bm,
                                           int dst_x, int dst_y, uint8_t key,
                                           int band_y0, int band_y1);

#ifdef __cplusplus
}
#endif

#endif /* FB_BLIT_H */
