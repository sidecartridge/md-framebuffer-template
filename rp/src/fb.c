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
#include "fb_draw.h"
#include "fb_font.h"
#include "font6x8.h"            /* defines `font6x8` (FB_FONT instance) */

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

  /* Build the per-pixel mask LUT the text/draw primitives use to poke
   * individual pixels into the 4-bitplane framebuffer. Pixel masks are
   * the only piece of fb_draw.c that survived the trim. */
  init_pixel_masks();

  /* Clear the whole FB to black ONCE at boot. RP RAM is 0x00 after
   * ERASE_FIRMWARE_IN_RAM, which on the default TOS low-res palette
   * is white -- drawing white text on top would be invisible. The
   * dynamic redraw later only re-erases the rows it rewrites; the
   * rest of the FB keeps this 0xFF background untouched. */
  fb_clear();

  /* Static parts of the boot UI -- never touched again after this,
   * so the m68k blits them consistently every frame regardless of
   * whatever the dynamic redraw is doing concurrently. */
  fb_render_static();
  /* Initial dynamic frame so the counter/marquee are populated before
   * the main loop kicks in. */
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

/* Story 1.2.15 smoke test.
 *
 * The default TOS low-res palette has index 0 = white and index 15 =
 * black, so font_set_color(0) draws white glyphs against the 0xFF
 * cleared background.
 *
 * Render is split:
 *   - fb_render_static() draws the title and ESC hint once at boot.
 *     The m68k blits this content every frame from then on.
 *   - fb_render_frame() runs every main-loop iteration and only
 *     touches two 8-row bands (frame-counter row at y=8 and marquee
 *     row at y=184). Per-frame RP write is ~2.5 KB instead of the
 *     full 32 KB clear, which shrinks the window in which the m68k
 *     blit can race the RP redraw. */

static uint32_t fb_frame_tick = 0;

static const char fb_marquee[] =
    "*** md-framebuffer-template -- RP2040 draws into cart RAM, m68k blits "
    "to ST screen every VBL -- press ESC to return to GEM ***   ";

#define FB_GLYPH_H        8
#define FB_ROW_BYTES      160
#define FB_COUNTER_Y      8
#define FB_MARQUEE_Y      184

/* Write the decimal representation of `n` into the END of `buf` and
 * return a pointer to the first character. `buf` must be at least 11
 * bytes (10 digits + NUL). This replaces a `snprintf(buf, "%lu", n)`
 * call so we don't pull newlib's printf into the binary. */
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

/* Erase one 8-row band (one text line tall) of the framebuffer back
 * to 0xFF (= palette index 15 = black on the default TOS palette). */
static inline void fb_erase_row_band(unsigned int y) {
  uint8_t *fb = (uint8_t *)fb_screen.framebuffer;
  memset(fb + (size_t)y * FB_ROW_BYTES, 0xFF,
         FB_GLYPH_H * FB_ROW_BYTES);
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
  font_set_font(&font6x8);
  font_set_color(0);
  font_set_border(0, 0);

  /* Frame counter row -- erase 8 rows then redraw. The text width
   * grows as the counter grows (1 -> 10 -> 100 -> ...) so we have
   * to erase the whole row, not just the previous text extent. */
  fb_erase_row_band(FB_COUNTER_Y);
  font_align(FONT_ALIGN_LEFT);
  font_move(8, FB_COUNTER_Y);
  font_print("frame: ");
  char digits[11];
  font_print(fb_fmt_uint(fb_frame_tick, digits, sizeof(digits)));

  /* Marquee row -- erase then slide the marquee text by one column. */
  fb_erase_row_band(FB_MARQUEE_Y);
  const int marquee_len = (int)(sizeof(fb_marquee) - 1);
  const int glyph_w = 6;
  const int cols = 320 / glyph_w + 2; /* one over each edge */
  const int step = (int)(fb_frame_tick % (uint32_t)marquee_len);
  for (int i = 0; i < cols; i++) {
    int src = (step + i) % marquee_len;
    char ch[2] = {fb_marquee[src], 0};
    font_move((unsigned)(i * glyph_w), FB_MARQUEE_Y);
    font_print(ch);
  }

  fb_frame_tick++;

  /* Publish the new frame counter as the LAST write of this frame.
   * The memory barrier ensures every preceding FB write has committed
   * to the RP2040 bus before the m68k can observe the new counter
   * value -- otherwise the m68k VBL loop could read counter=N and blit
   * a half-finished frame. */
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;
}
