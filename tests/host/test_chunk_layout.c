// units: rp/src/fb_chunked.c
/* The framebuffer's chunk layout, end to end: what the app draws in the
 * chunky buffer is what the ST shows.
 *
 * fb_chunky_to_planar() transposes the chunky buffer to the ST's planes on
 * both cores (core 1 is a thread here) and publishes it to the cartridge
 * framebuffer with the 48-byte chunks in reverse order, because the m68k's
 * blit stores each chunk with a predecrementing MOVEM: chunk k read from the
 * cartridge lands at CART_FB_CHUNK_COVERED - 48 (k + 1) in the screen page,
 * and the tail is copied as it is (cart_shared.h, "Framebuffer chunk layout",
 * and FBDRV_INLINE in userfw.s). The ST's blitter, when it copies instead,
 * must put every chunk where that MOVEM would. This test runs the RP's side,
 * models both copies from the constants of cart_shared.h, decodes the ST's
 * planes and compares every pixel with what was drawn.
 *
 * The two workers are Thumb assembly on the RP (fb_chunked_asm.S); here they
 * are the C they stand for: the ST's low-resolution planar format, and a
 * chunk-reversed copy. */
#include <stdbool.h>
#include <string.h>

#include "cart_shared.h"
#include "fb_chunked.h"
#include "test.h"

#define W FB_CHUNKED_W
#define H FB_CHUNKED_H

/* Pixels [src, src_end) to planar words at dst: per 16 pixels, one word per
 * plane (0..3), the leftmost pixel in bit 15. */
void fb_c2p_half(uint16_t *dst, const uint8_t *src, const uint8_t *src_end) {
  for (; src < src_end; src += 16, dst += 4) {
    for (int plane = 0; plane < 4; plane++) {
      uint16_t word = 0;
      for (int i = 0; i < 16; i++) {
        word |= (uint16_t)(((src[i] >> plane) & 1u) << (15 - i));
      }
      dst[plane] = word;
    }
  }
}

/* count 48-byte chunks to dst, in order, from src_last backwards. */
void fb_chunk_reverse_copy48(uint8_t *dst, const uint8_t *src_last,
                             uint32_t count) {
  for (uint32_t k = 0; k < count; k++) {
    memcpy(dst + 48u * k, src_last - 48u * k, 48u);
  }
}

/* The cartridge framebuffer, as 16-bit words (the ST reads the RP's words
 * with their bytes swapped, which leaves their values as they are). */
static uint16_t cart[CART_FRAMEBUFFER_SIZE / 2];
static uint16_t screen[CART_FRAMEBUFFER_SIZE / 2];

/* FBDRV_INLINE: CART_FB_CHUNK_COUNT MOVEM pairs, each reading one chunk
 * forward and storing it below the previous one, then the tail in place. */
static void m68k_blit(void) {
  const unsigned chunk = CART_FB_CHUNK_BYTES / 2;
  for (unsigned k = 0; k < CART_FB_CHUNK_COUNT; k++) {
    unsigned to = (CART_FB_CHUNK_COVERED / 2) - chunk * (k + 1);
    memcpy(&screen[to], &cart[chunk * k], CART_FB_CHUNK_BYTES);
  }
  memcpy(&screen[CART_FB_CHUNK_COVERED / 2], &cart[CART_FB_CHUNK_COVERED / 2],
         CART_FB_CHUNK_TAIL);
}

/* The blitter as userfw drives it (HOP source, OP source, all endmasks
 * set): word by word, adding the X increment after each word of a line but
 * the last and the Y increment after the last; its addresses carry on from
 * one start to the next. Addresses are byte offsets into the cartridge
 * framebuffer and the screen page. */
typedef struct {
  int32_t src, dst;
  int32_t src_x_inc, src_y_inc, dst_x_inc, dst_y_inc;
  uint32_t x_count;
} blitter_t;

static void blitter_run(blitter_t *b, uint32_t y_count) {
  for (uint32_t y = 0; y < y_count; y++) {
    for (uint32_t x = 0; x < b->x_count; x++) {
      screen[b->dst / 2] = cart[b->src / 2];
      bool last = x + 1u == b->x_count;
      b->src += last ? b->src_y_inc : b->src_x_inc;
      b->dst += last ? b->dst_y_inc : b->dst_x_inc;
    }
  }
}

/* userfw's blitter copy: a chunk per line, the destination back a chunk per
 * line from the last one (BLT_DST_Y_INC), started again every `piece` lines,
 * then the tail in place. */
static void blitter_blit(uint32_t piece) {
  blitter_t b = {
      .src = 0,
      .dst = CART_FB_CHUNK_COVERED - CART_FB_CHUNK_BYTES,
      .src_x_inc = 2,
      .src_y_inc = 2,
      .dst_x_inc = 2,
      .dst_y_inc = -(2 * CART_FB_CHUNK_BYTES - 2),
      .x_count = CART_FB_CHUNK_BYTES / 2,
  };
  for (uint32_t left = CART_FB_CHUNK_COUNT; left > 0;) {
    uint32_t n = left < piece ? left : piece;
    blitter_run(&b, n);
    left -= n;
  }
  b.dst = CART_FB_CHUNK_COVERED;
  b.x_count = CART_FB_CHUNK_TAIL / 2;
  blitter_run(&b, 1);
}

static uint8_t st_pixel(int x, int y) {
  const uint16_t *group = &screen[y * 80 + (x / 16) * 4];
  int bit = 15 - (x % 16);
  return (uint8_t)(((group[0] >> bit) & 1) | (((group[1] >> bit) & 1) << 1) |
                   (((group[2] >> bit) & 1) << 2) |
                   (((group[3] >> bit) & 1) << 3));
}

static int shows_what_was_drawn(void) {
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (st_pixel(x, y) != (fb_chunked_buffer[y * W + x] & 0x0Fu)) {
        fprintf(stderr, "pixel (%d, %d): ST shows %u, drawn %u\n", x, y,
                st_pixel(x, y), fb_chunked_buffer[y * W + x] & 0x0Fu);
        return 0;
      }
    }
  }
  return 1;
}

/* Every copy of the frame in `cart` shows it: the MOVEM loop, and the
 * blitter in one run and in pieces of the ST's default, of one chunk, of an
 * odd size and of the largest the RP can ask for. The screen starts as
 * garbage each time: every byte must be copied. */
static int every_copy_shows_what_was_drawn(void) {
  static const uint32_t pieces[] = {CART_FB_CHUNK_COUNT, 40, 1, 7, 255};
  memset(screen, 0x5A, sizeof(screen));
  m68k_blit();
  if (!shows_what_was_drawn()) return 0;
  for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
    memset(screen, 0xA5, sizeof(screen));
    blitter_blit(pieces[i]);
    if (!shows_what_was_drawn()) {
      fprintf(stderr, "blitter, pieces of %u chunks\n", (unsigned)pieces[i]);
      return 0;
    }
  }
  return 1;
}

int main(void) {
  /* The blit covers the whole framebuffer, as userfw's FBDRV_INLINE does
   * with FB_COPY_LINES = 200 (tests/host/test_layout.py checks the two
   * agree). */
  CHECK_EQ(CART_FB_BLIT_BYTES, CART_FRAMEBUFFER_SIZE);
  CHECK_EQ(CART_FB_CHUNK_COVERED + CART_FB_CHUNK_TAIL, CART_FB_BLIT_BYTES);

  fb_chunked_init(); /* core 1 */

  /* A pattern with every colour in every position, a gradient, a single
   * pixel at each corner, and the last line, which is the tail. */
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      fb_chunked_buffer[y * W + x] = (uint8_t)((x * 7 + y * 3) & 0x0F);
    }
  }
  fb_chunky_to_planar(cart);
  CHECK(every_copy_shows_what_was_drawn());

  fb_chunked_clear(0);
  fb_chunked_buffer[0] = 1;
  fb_chunked_buffer[W - 1] = 2;
  fb_chunked_buffer[(H - 1) * W] = 3;
  fb_chunked_buffer[H * W - 1] = 4;
  for (int x = 0; x < W; x++) fb_chunked_buffer[(H - 1) * W + x] = (uint8_t)(x & 15);
  fb_chunky_to_planar(cart);
  CHECK(every_copy_shows_what_was_drawn());

  /* The high nibble of a chunky byte is not a colour: the ST sees only the
   * palette index in the low nibble. */
  for (int i = 0; i < W * H; i++) fb_chunked_buffer[i] = (uint8_t)(0xA0 | (i % 11));
  fb_chunky_to_planar(cart);
  CHECK(every_copy_shows_what_was_drawn());

  TEST_END();
}
