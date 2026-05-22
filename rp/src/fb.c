/**
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and brings up the
 *              single 32 KB cartridge framebuffer the m68k reads each
 *              VBL (single-FB design, Story 1.2 Q1).
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CHANDLER_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <string.h>

#include "chandler.h"
#include "debug.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "font6x8.h"            /* defines `font6x8` (FB_FONT instance) */
#include "pico/time.h"          /* time_us_32 for the timing overlay */

/* Story 2.3 demo sprite. Multi-colour 16x16 ball with a transparent
 * background (key = 0xFF; transparent corners give it a rounded look
 * against the black background). Generated once at fb_init so apps
 * can see what a typical sprite blob looks like in code -- real apps
 * will source pixel data from an asset file. */
#define DEMO_SPRITE_W 16
#define DEMO_SPRITE_H 16
#define DEMO_SPRITE_KEY 0xFFu

static uint8_t demo_sprite_data[DEMO_SPRITE_W * DEMO_SPRITE_H];
static const struct FB_BITMAP demo_sprite = {
    DEMO_SPRITE_W, DEMO_SPRITE_H, demo_sprite_data};

static void fb_build_demo_sprite(void) {
  /* Concentric rings: inner core, middle band, outer rim, transparent
   * outside. Centred on (7.5, 7.5); use integer distance-squared
   * comparisons so we don't pull in float math. */
  for (int y = 0; y < DEMO_SPRITE_H; y++) {
    for (int x = 0; x < DEMO_SPRITE_W; x++) {
      int dx = 2 * x - 15; /* doubled so 0.5 offset is integer */
      int dy = 2 * y - 15;
      int r_sq_x4 = dx * dx + dy * dy; /* 4 * actual r_sq */
      uint8_t c;
      if (r_sq_x4 <= 36)        c = 9;       /* core  (r <= 3)   */
      else if (r_sq_x4 <= 144)  c = 13;      /* band  (r <= 6)   */
      else if (r_sq_x4 <= 196)  c = 1;       /* rim   (r <= 7)   */
      else                      c = DEMO_SPRITE_KEY; /* transparent */
      demo_sprite_data[y * DEMO_SPRITE_W + x] = c;
    }
  }
}

const struct FB_MODE fb_mode_320x200 = {320, 200, 4};

struct FB_SCREEN fb_screen;

/* RP-incremented dirty-frame counter at $FA400C. The m68k userfw reads
 * this each VBL and only blits cart->ST screen when the value differs
 * from what it saw last iteration. Set in fb_init, bumped at the end of
 * fb_render_frame after all FB writes commit. */
static volatile uint32_t *fb_frame_counter;

int fb_init(const struct FB_MODE *mode) {
  if (mode == NULL) {
    return -1;
  }
  fb_screen.framebuffer =
      (unsigned int *)((unsigned int)&__rom_in_ram_start__ +
                       CHANDLER_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;
  fb_frame_counter =
      (volatile uint32_t *)((uint8_t *)&__rom_in_ram_start__ +
                            CHANDLER_FB_FRAME_COUNTER_OFFSET);

  /* Launch Core 1 with the chunky-to-planar bottom-half worker. */
  fb_chunked_init();

  /* Build the demo sprite once. */
  fb_build_demo_sprite();

  /* Story 2.1: paint the chunked buffer + run the conversion once so
   * the cart framebuffer is in a deterministic state before the main
   * loop spins up. fb_render_static() is left defined for now but no
   * longer called -- its planar writes would be overwritten by the
   * conversion every frame. Story 2.2 will port it to chunked and
   * reactivate it. */
  fb_render_frame();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CHANDLER_FRAMEBUFFER_SIZE);
  return 0;
}

void fb_clear(void) {
  /* 0xFF on a 4 bpp Atari ST low-res screen with the default TOS
   * palette renders as solid black (all four bitplanes set -> palette
   * index 15). 0x00 would be palette index 0 = white, which is hard
   * to distinguish from "framebuffer never touched". */
  memset(fb_screen.framebuffer, 0xFF, CHANDLER_FRAMEBUFFER_SIZE);
}

/* Story 2.2 smoke test: full template UI rendered via the chunked path.
 *
 * fb_font's render_text now writes directly into fb_chunked_buffer (one
 * byte per pixel, palette index in low nibble). fb_render_frame composes
 * the whole frame from scratch each iteration (chunked clear → static UI
 * → dynamic UI), then runs fb_chunky_to_planar once to publish into the
 * cart FB. Per-frame work is dominated by the conversion, not the text
 * writes, so the surgical-erase optimisation from Story 1.2.15 no longer
 * applies. */

static uint32_t fb_frame_tick = 0;

/* Story 2.4 instrumentation. The previous frame's measurements get
 * rendered as part of the current frame -- there's no way to render
 * timings *for* a frame *into* that same frame. Stale-by-one is fine
 * for a visual indicator. */
static uint32_t last_convert_us = 0;
static uint32_t last_frame_us = 0;

static const char fb_marquee[] =
    "*** md-framebuffer-template -- RP2040 draws into a chunked buffer, "
    "chunky-to-planar publishes every frame, m68k blits to ST screen "
    "each VBL -- press ESC to return to GEM ***   ";

/* Write the decimal representation of `n` into the END of `buf` and
 * return a pointer to the first character. `buf` must be at least 11
 * bytes (10 digits + NUL). Replaces an `snprintf(buf, "%lu", n)` call
 * so we don't pull newlib's printf into the binary. */
static const char *fb_fmt_uint(uint32_t n, char *buf, size_t buf_sz) {
  char *p = buf + buf_sz;
  *--p = '\0';
  if (n == 0) {
    *--p = '0';
  } else {
    while (n > 0) {
      *--p = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return p;
}

void fb_render_static(void) {
  font_set_font(&font6x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);

  font_move(160, 40);
  font_print("md-framebuffer-template");
  font_move(160, 56);
  font_print("Sidecartridge Multi-device");
  font_move(160, 168);
  font_print("Press ESC to return to GEM");
}

void fb_render_frame(void) {
  uint32_t t_frame_start = time_us_32();

  /* Black background (palette index 15 on default TOS low-res). */
  fb_chunked_clear(15);

  /* Static parts: title + ESC hint. */
  fb_render_static();

  /* Frame counter at top-left. */
  font_set_font(&font6x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  font_move(8, 8);
  font_print("frame: ");
  char digits[11];
  font_print(fb_fmt_uint(fb_frame_tick, digits, sizeof(digits)));

  /* Timing overlay: previous frame's total + chunky-to-planar µs. */
  font_move(8, 16);
  font_print("fr us: ");
  font_print(fb_fmt_uint(last_frame_us, digits, sizeof(digits)));
  font_move(160, 16);
  font_print("cv us: ");
  font_print(fb_fmt_uint(last_convert_us, digits, sizeof(digits)));

  /* Marquee row -- scroll one column per frame. */
  const int marquee_len = (int)(sizeof(fb_marquee) - 1);
  const int glyph_w = 6;
  const int cols = FB_CHUNKED_W / glyph_w + 2; /* one over each edge */
  const int step = (int)(fb_frame_tick % (uint32_t)marquee_len);
  for (int i = 0; i < cols; i++) {
    int src = (step + i) % marquee_len;
    char ch[2] = {fb_marquee[src], 0};
    font_move((unsigned)(i * glyph_w), 184);
    font_print(ch);
  }

  /* Story 2.3: demo sprite bouncing inside the area between the static
   * text rows. Two independent linear ping-pongs at different periods
   * give a Lissajous-style trajectory without needing trig tables. */
  unsigned int t = fb_frame_tick / 2u; /* slow down vs RP loop speed */
  int x_range = FB_CHUNKED_W - DEMO_SPRITE_W;
  int y_low = 72;                                   /* below title */
  int y_high = 160 - DEMO_SPRITE_H;                 /* above ESC hint */
  int y_range = y_high - y_low;
  unsigned int sx_cycle = t % (unsigned)(2 * x_range);
  int sx = (int)((sx_cycle < (unsigned)x_range) ? sx_cycle
                                                : 2 * x_range - sx_cycle);
  unsigned int sy_cycle = (t / 3u) % (unsigned)(2 * y_range);
  int sy = y_low + (int)((sy_cycle < (unsigned)y_range)
                             ? sy_cycle
                             : 2 * y_range - sy_cycle);
  fb_blit_key(&demo_sprite, sx, sy, DEMO_SPRITE_KEY);

  /* Publish: transpose chunked -> planar into the cart framebuffer. */
  uint32_t t_conv_start = time_us_32();
  fb_chunky_to_planar((uint16_t *)fb_screen.framebuffer);
  last_convert_us = time_us_32() - t_conv_start;

  fb_frame_tick++;

  /* Publish the new frame counter as the LAST write of this frame.
   * The memory barrier ensures every preceding FB write has committed
   * to the RP2040 bus before the m68k can observe the new counter
   * value -- otherwise the m68k VBL loop could read counter=N and blit
   * a half-finished frame. */
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;

  last_frame_us = time_us_32() - t_frame_start;
}
