/**
 * File: fb_blit.c
 * Description: Implementations of fb_fill_rect / fb_blit / fb_blit_key.
 *
 * Story 2.3 first cut: plain C with per-row clipping + memcpy for the
 * opaque blit. Story 2.4 can vectorise / unroll if needed.
 */

#include "fb_blit.h"

#include <string.h>

#include "fb_chunked.h"

/* Per-file -O3: the global build is MinSizeRel (-Os); these blit
 * primitives are pure per-pixel compute on the draw path. No cart-bus /
 * PIO timing code here. */
#pragma GCC optimize("O3")

/* Hand-written Thumb worker (fb_blit_asm.S): one colour-keyed row, copy
 * cw bytes src->dst skipping bytes == key. Unrolled x4 to amortise the
 * per-pixel loop control that dominates the -O3 C version. */
extern void fb_blit_key_row(uint8_t *dst, const uint8_t *src, uint32_t cw,
                            uint8_t key);

/* Intersect [start, start + len) with [0, max) and return the clipped
 * length and the (positive) offset into the source. */
static inline int clip_axis(int start, int len, int max, int *src_off) {
  if (start < 0) {
    *src_off = -start;
    len += start; /* shrink by the negative portion */
    start = 0;
  } else {
    *src_off = 0;
  }
  if (start + len > max) {
    len = max - start;
  }
  if (len < 0) len = 0;
  return len;
}

void __not_in_flash_func(fb_fill_rect)(int x, int y, int w, int h,
                                       uint8_t color) {
  int sox, soy; /* clip offsets unused for fill */
  int cw = clip_axis(x, w, FB_CHUNKED_W, &sox);
  int ch = clip_axis(y, h, FB_CHUNKED_H, &soy);
  if (cw <= 0 || ch <= 0) return;

  int cx = x < 0 ? 0 : x;
  int cy = y < 0 ? 0 : y;
  uint8_t *line = fb_chunked_buffer + (size_t)cy * FB_CHUNKED_W + cx;
  for (int row = 0; row < ch; row++) {
    memset(line, color, (size_t)cw);
    line += FB_CHUNKED_W;
  }
}

void __not_in_flash_func(fb_blit)(const struct FB_BITMAP *bm, int dst_x,
                                  int dst_y) {
  if (!bm || !bm->data) return;
  int sox, soy;
  int cw = clip_axis(dst_x, bm->width, FB_CHUNKED_W, &sox);
  int ch = clip_axis(dst_y, bm->height, FB_CHUNKED_H, &soy);
  if (cw <= 0 || ch <= 0) return;

  int cx = dst_x < 0 ? 0 : dst_x;
  int cy = dst_y < 0 ? 0 : dst_y;
  uint8_t *dst = fb_chunked_buffer + (size_t)cy * FB_CHUNKED_W + cx;
  const uint8_t *src = bm->data + (size_t)soy * bm->width + sox;
  for (int row = 0; row < ch; row++) {
    memcpy(dst, src, (size_t)cw);
    dst += FB_CHUNKED_W;
    src += bm->width;
  }
}

void __not_in_flash_func(fb_blit_key)(const struct FB_BITMAP *bm, int dst_x,
                                      int dst_y, uint8_t key) {
  if (!bm || !bm->data) return;
  int sox, soy;
  int cw = clip_axis(dst_x, bm->width, FB_CHUNKED_W, &sox);
  int ch = clip_axis(dst_y, bm->height, FB_CHUNKED_H, &soy);
  if (cw <= 0 || ch <= 0) return;

  int cx = dst_x < 0 ? 0 : dst_x;
  int cy = dst_y < 0 ? 0 : dst_y;
  uint8_t *dst = fb_chunked_buffer + (size_t)cy * FB_CHUNKED_W + cx;
  const uint8_t *src = bm->data + (size_t)soy * bm->width + sox;
  for (int row = 0; row < ch; row++) {
    fb_blit_key_row(dst, src, (uint32_t)cw, key);
    dst += FB_CHUNKED_W;
    src += bm->width;
  }
}
