/**
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and brings up the
 *              single 32 KB cartridge framebuffer the m68k reads each
 *              VBL (single-FB design).
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CART_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "font8x8.h"            /* defines `font8x8` (FB_FONT instance) */
#include "ikbd.h"
#include "pico/time.h"          /* time_us_32 for the timing overlay */
#include "profile.h"
#include "st_session.h"

/* Blit-done ack. The m68k blits only a frame it has not blitted yet (the
 * frame counter's low word changed; see FB_FRAME_COUNTER_ADDR in userfw.s) and
 * then does a cart-bus read at $FB8400 (VBLSYNC_ADDR); the commemul ring
 * captures it with low-16 = 0x84xx. fb_pump_rom3 routes ROM3 samples to
 * the IKBD demux, the ST's hello and this detector. fb_publish() waits for
 * the ack of the frame it published last before overwriting the cart FB:
 * after it, the m68k reads nothing until the counter changes again, so the
 * publish can happen at any moment. The timeout keeps the RP from hanging
 * when the m68k is not running (e.g. before it boots, or in GEM). */
#define FB_VBLSYNC_HIBYTE   0x8400u
#define FB_VBLSYNC_HIMASK   0xFF00u
/* Safety net only: must comfortably exceed the worst-case latency from
 * "counter bumped" to "m68k VBLSYNC" (up to ~2 VBLs = 40 ms when c2p
 * overruns the slack and the m68k skips a frame). Firing early would
 * let the next c2p race the blit -- so keep it generous; it should
 * never fire while the m68k is actually running. */
#define FB_VSYNC_TIMEOUT_US 60000u

static volatile uint32_t s_vbl_seen;
static uint32_t s_vbl_published;
/* Publishes that gave up waiting for the ack: expected until the ST runs
 * userfw, never while it does. Readable over SWD by its symbol. */
uint32_t fbAckTimeouts = 0;

/* Demo sprite. Multi-colour 16x16 ball with a transparent
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
                       CART_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;
  fb_frame_counter =
      (volatile uint32_t *)((uint8_t *)&__rom_in_ram_start__ +
                            CART_FB_FRAME_COUNTER_OFFSET);

  /* Zero the dirty-frame counter so the m68k userfw VBL loop sees a
   * clean baseline. The full shared region is already zeroed by
   * emul_start's ERASE_FIRMWARE_IN_RAM, so this is defensive against
   * future callers that might re-init fb without erasing. (Used to
   * live in chandler_init; relocated when chandler was removed.) */
  *fb_frame_counter = 0;

  /* The app's profile (profile.h): userfw reads it once, at its boot, for
   * the frames a VBL and the sound's rate. */
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_PROFILE_OFFSET)) = PROFILE_ID;

  /* Launch Core 1 with the chunky-to-planar bottom-half worker. */
  fb_chunked_init();

  /* Build the demo sprite once. */
  fb_build_demo_sprite();

  /* Paint the chunked buffer + run the conversion once so
   * the cart framebuffer is in a deterministic state before the main
   * loop spins up. fb_render_static() is left defined for now but no
   * longer called -- its planar writes would be overwritten by the
   * conversion every frame. */
  fb_render_frame();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CART_FRAMEBUFFER_SIZE);
  return 0;
}

void fb_clear(void) {
  /* 0xFF on a 4 bpp Atari ST low-res screen with the default TOS
   * palette renders as solid black (all four bitplanes set -> palette
   * index 15). 0x00 would be palette index 0 = white, which is hard
   * to distinguish from "framebuffer never touched". */
  memset(fb_screen.framebuffer, 0xFF, CART_FRAMEBUFFER_SIZE);
}

/* Full template UI rendered via the chunked path.
 *
 * fb_font's render_text now writes directly into fb_chunked_buffer (one
 * byte per pixel, palette index in low nibble). fb_render_frame composes
 * the whole frame from scratch each iteration (chunked clear → static UI
 * → dynamic UI), then runs fb_chunky_to_planar once to publish into the
 * cart FB. Per-frame work is dominated by the conversion, not the text
 * writes, so the surgical-erase optimisation no longer applies. */

static uint32_t fb_frame_tick = 0;

/* Timing instrumentation. The previous frame's measurements get
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
  font_set_font(&font8x8);
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
  font_set_font(&font8x8);
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

  /* Demo sprite bouncing inside the area between the static
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

  fb_publish();

  last_frame_us = time_us_32() - t_frame_start;
}

#if defined(_DEBUG) && (_DEBUG != 0)
/* The ST's stopwatch, when userfw.s reports it (TIME_STUDY): points of its
 * loop with the stopwatch's low 16 bits (MFP Timer-A /10: 10 / 2.4576 us a
 * tick). fbStopwatch keeps the latest FB_STOPWATCH_POINTS of them as point
 * << 16 | ticks and fbStopwatchCount counts them all: tools/dev/swd.py
 * stopwatch reads them. From them, the slack of each frame the ST copied:
 * how long before the VBL that may start the next frame (the profile's VBLs
 * a frame after the one before the copy) the copy ended. A histogram in
 * FB_SLACK_BUCKET_US steps (the last bucket holds the rest), the smallest
 * slack seen, and the copies that ended after that VBL (the next frame
 * waited a VBL more). Readable over SWD. */
#define FB_STOPWATCH_POINTS 128u
#define FB_STOPWATCH_POINT_COPY 1u
#define FB_STOPWATCH_POINT_COPIED 2u
#define FB_STOPWATCH_POINT_VBL 4u
#define FB_STOPWATCH_TICK_NS 4069u /* 10 / 2.4576 MHz */
#define FB_SLACK_BUCKETS 40u
#define FB_SLACK_BUCKET_US 100u
/* A VBL of the PAL ST (32.084988 MHz / 4 / (512 x 313)). */
#define FB_VBL_PERIOD_US 19979u
volatile uint32_t fbStopwatch[FB_STOPWATCH_POINTS];
volatile uint32_t fbStopwatchCount = 0;
volatile uint32_t fbSlackHist[FB_SLACK_BUCKETS];
volatile uint32_t fbSlackMinUs = UINT32_MAX;
volatile uint32_t fbSlackLate = 0;
volatile uint32_t fbSlackReports = 0;
static int s_sw_point = -1;
static int s_sw_high = -1;
static bool s_sw_vbl_seen, s_sw_copying;
static uint16_t s_sw_vbl, s_sw_copy_vbl; /* the last VBL, the VBL before the copy */

static void fb_stopwatch_sample(uint16_t sample) {
  uint8_t value = (uint8_t)sample;
  switch (sample & CART_ROM3_WINDOW_MASK) {
    case CART_ROM3_STUDY_POINT_WINDOW:
      s_sw_point = value;
      s_sw_high = -1;
      return;
    case CART_ROM3_STUDY_HI_WINDOW:
      if (s_sw_point >= 0) s_sw_high = value;
      return;
    case CART_ROM3_STUDY_LO_WINDOW:
      break;
    default:
      return;
  }
  if (s_sw_point < 0 || s_sw_high < 0) return;
  uint32_t point = (uint32_t)s_sw_point;
  uint16_t ticks = (uint16_t)((s_sw_high << 8) | value);
  s_sw_point = s_sw_high = -1;
  fbStopwatch[fbStopwatchCount % FB_STOPWATCH_POINTS] = (point << 16) | ticks;
  fbStopwatchCount++;
  if (point == FB_STOPWATCH_POINT_VBL) {
    s_sw_vbl = ticks;
    s_sw_vbl_seen = true;
  } else if (point == FB_STOPWATCH_POINT_COPY && s_sw_vbl_seen) {
    s_sw_copy_vbl = s_sw_vbl;
    s_sw_copying = true;
  } else if (point == FB_STOPWATCH_POINT_COPIED && s_sw_copying) {
    s_sw_copying = false;
    uint32_t ended_us =
        (uint16_t)(ticks - s_sw_copy_vbl) * FB_STOPWATCH_TICK_NS / 1000u;
    uint32_t deadline_us = PROFILE_VBLS_A_FRAME * FB_VBL_PERIOD_US;
    fbSlackReports++;
    if (ended_us >= deadline_us) {
      fbSlackLate++;
      return;
    }
    uint32_t slack_us = deadline_us - ended_us;
    if (slack_us < fbSlackMinUs) fbSlackMinUs = slack_us;
    uint32_t bucket = slack_us / FB_SLACK_BUCKET_US;
    if (bucket >= FB_SLACK_BUCKETS) bucket = FB_SLACK_BUCKETS - 1u;
    fbSlackHist[bucket]++;
  }
}
#else
#define fb_stopwatch_sample(sample) ((void)0)
#endif

/* ROM3 ring dispatch: route each captured cart-bus read to the IKBD
 * demux, the ST's hello, the VBL frame-sync detector and (debug builds) the
 * ST's stopwatch. */
static void fb_rom3_dispatch(uint16_t sample) {
  ikbd_consume_rom3_sample(sample);
  st_session_consume_rom3_sample(sample);
  if ((sample & FB_VBLSYNC_HIMASK) == FB_VBLSYNC_HIBYTE) {
    s_vbl_seen++;
  }
  fb_stopwatch_sample(sample);
}

void fb_pump_rom3(void) { commemul_poll(fb_rom3_dispatch); }

void fb_set_copy_mode(uint8_t mode, uint8_t piece) {
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_BLIT_MODE_OFFSET)) =
      (uint16_t)(mode | (piece << 8));
}

void fb_publish(void) {
  /* 1. Transpose chunked -> planar SCRATCH (RP RAM, dual-core). This is
   *    the slow part (~1 ms) but it does NOT touch the cart FB, so it
   *    runs unsynchronized and overlaps the m68k's blit of the previous
   *    frame. */
  uint32_t t0 = time_us_32();
  fb_transpose();
  uint32_t transpose_us = time_us_32() - t0;

  /* 2. Block until the m68k has finished blitting the previous frame
   *    (its VBLSYNC ack) so the cart FB is free to overwrite. Drain the
   *    ROM3 ring meanwhile so IKBD / ESC stay live; the timeout is a
   *    safety net for "m68k not running" (e.g. at boot). */
  uint32_t t_wait = time_us_32();
  while (s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    if (time_us_32() - t_wait > FB_VSYNC_TIMEOUT_US) {
      fbAckTimeouts++;
      break;
    }
  }
  s_vbl_published = s_vbl_seen;

  /* 3. Publish: fast chunk-reversed memcpy scratch -> cart FB. This is
   *    the only cart-FB write and it's short (~120 us), so it fits in
   *    the m68k's post-blit slack -- no overrun, no tearing, full 50 Hz.
   *    Mark the frame ready as the last write (barrier first). */
  uint32_t t1 = time_us_32();
  fb_planar_publish((uint16_t *)fb_screen.framebuffer);
  last_convert_us = transpose_us + (time_us_32() - t1);

  fb_frame_tick++;
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;
}

uint32_t fb_last_convert_us(void) { return last_convert_us; }
