// units: rp/src/fb_blit.c
/* fb_fill_rect, fb_blit, fb_blit_key and their band variants against a
 * pixel-by-pixel reference: inside, across each edge and corner, fully
 * outside, larger than the screen, and clipped to bands. The keyed row
 * routine is Thumb assembly on the RP (fb_blit_asm.S); here it is the C it
 * stands for. The sanitizers catch any write outside the buffer. */
#include <string.h>

#include "fb_blit.h"
#include "fb_chunked.h"
#include "test.h"

#define W FB_CHUNKED_W
#define H FB_CHUNKED_H
#define BG 0xEEu
#define KEY 0x55u

uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE];
static uint8_t expect[FB_CHUNKED_SIZE];

void fb_blit_key_row(uint8_t *dst, const uint8_t *src, uint32_t cw,
                     uint8_t key) {
  for (uint32_t i = 0; i < cw; i++) {
    if (src[i] != key) dst[i] = src[i];
  }
}

static void reset(void) {
  memset(fb_chunked_buffer, BG, sizeof fb_chunked_buffer);
  memset(expect, BG, sizeof expect);
}

static void ref_fill(int x, int y, int w, int h, uint8_t color) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      if (xx >= 0 && xx < W && yy >= 0 && yy < H) expect[yy * W + xx] = color;
    }
  }
}

static void ref_blit(const struct FB_BITMAP *bm, int x, int y, int y0, int y1,
                     int keyed) {
  for (int r = 0; r < bm->height; r++) {
    for (int c = 0; c < bm->width; c++) {
      int xx = x + c, yy = y + r;
      uint8_t p = bm->data[r * bm->width + c];
      if (xx < 0 || xx >= W || yy < y0 || yy >= y1 || yy < 0 || yy >= H)
        continue;
      if (keyed && p == KEY) continue;
      expect[yy * W + xx] = p;
    }
  }
}

static int matches(void) {
  return memcmp(fb_chunked_buffer, expect, sizeof expect) == 0;
}

/* Positions that put a w x h shape inside, across every edge and corner,
 * just outside every edge, and far outside. */
static int positions(int w, int h, int (*out)[2]) {
  const int xs[] = {-w - 40, -w, -w + 1, -3, 0, 17, W - w, W - 3, W - 1, W,
                    W + 40};
  const int ys[] = {-h - 40, -h, -h + 1, -3, 0, 23, H - h, H - 3, H - 1, H,
                    H + 40};
  int n = 0;
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
    for (unsigned j = 0; j < sizeof ys / sizeof ys[0]; j++) {
      out[n][0] = xs[i];
      out[n][1] = ys[j];
      n++;
    }
  }
  return n;
}

static uint8_t bitmap_data[400 * 260];

static struct FB_BITMAP make_bitmap(int w, int h) {
  for (int i = 0; i < w * h; i++) {
    uint8_t p = (uint8_t)((i * 7 + i / w * 13) & 0x0F);
    bitmap_data[i] = (i % 5 == 0) ? KEY : p; /* some transparent pixels */
  }
  struct FB_BITMAP bm = {(uint16_t)w, (uint16_t)h, bitmap_data};
  return bm;
}

int main(void) {
  static int pos[121][2];
  const int sizes[][2] = {{1, 1}, {16, 16}, {37, 5}, {320, 200}, {400, 260}};

  /* fb_fill_rect: every size and position. */
  for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    int n = positions(sizes[s][0], sizes[s][1], pos);
    for (int i = 0; i < n; i++) {
      reset();
      fb_fill_rect(pos[i][0], pos[i][1], sizes[s][0], sizes[s][1], 3);
      ref_fill(pos[i][0], pos[i][1], sizes[s][0], sizes[s][1], 3);
      CHECK(matches());
    }
  }
  /* Empty and negative sizes draw nothing. */
  const int empty[][2] = {{0, 10}, {10, 0}, {-5, 10}, {10, -5}};
  for (unsigned e = 0; e < sizeof empty / sizeof empty[0]; e++) {
    reset();
    fb_fill_rect(10, 10, empty[e][0], empty[e][1], 3);
    CHECK(matches());
  }

  /* fb_blit and fb_blit_key: every size and position. */
  for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    struct FB_BITMAP bm = make_bitmap(sizes[s][0], sizes[s][1]);
    int n = positions(sizes[s][0], sizes[s][1], pos);
    for (int i = 0; i < n; i++) {
      reset();
      fb_blit(&bm, pos[i][0], pos[i][1]);
      ref_blit(&bm, pos[i][0], pos[i][1], 0, H, 0);
      CHECK(matches());
      reset();
      fb_blit_key(&bm, pos[i][0], pos[i][1], KEY);
      ref_blit(&bm, pos[i][0], pos[i][1], 0, H, 1);
      CHECK(matches());
    }
  }

  /* The band variants draw only rows y0 <= y < y1, so two cores can each
   * draw one band of the same frame. */
  const int bands[][2] = {{0, 100}, {100, 200}, {37, 150}, {0, 200}, {50, 50}};
  for (unsigned b = 0; b < sizeof bands / sizeof bands[0]; b++) {
    struct FB_BITMAP bm = make_bitmap(37, 61);
    int n = positions(37, 61, pos);
    for (int i = 0; i < n; i++) {
      reset();
      fb_blit_band(&bm, pos[i][0], pos[i][1], bands[b][0], bands[b][1]);
      ref_blit(&bm, pos[i][0], pos[i][1], bands[b][0], bands[b][1], 0);
      CHECK(matches());
      reset();
      fb_blit_key_band(&bm, pos[i][0], pos[i][1], KEY, bands[b][0],
                       bands[b][1]);
      ref_blit(&bm, pos[i][0], pos[i][1], bands[b][0], bands[b][1], 1);
      CHECK(matches());
    }
  }

  /* No bitmap, or one without data, draws nothing. */
  reset();
  fb_blit(NULL, 0, 0);
  struct FB_BITMAP nodata = {8, 8, NULL};
  fb_blit(&nodata, 0, 0);
  fb_blit_key(&nodata, 0, 0, KEY);
  CHECK(matches());

  TEST_END();
}
