/**
 * File: demo_parallax.c
 * Description: "Uridium-style" parallax demo.
 *
 * Five layers drawn into the chunked byte-per-pixel buffer (low
 * nibble = palette index), back to front:
 *
 *   Layer 1 (slow)  : starfield -- two depth classes scrolling at
 *                     different speeds for parallax.
 *   Layer 2 (medium): a rusted industrial girder framework over the
 *                     stars (embossed beams + bolted joints; the open
 *                     cells let the stars show through).
 *   Layer 3 (med-fast): a drifting diagonal grey lattice (diamond grid
 *                     in two grey tones) over the girders.
 *   Layer 4 (fast)  : the Uridium "zinc" metal surface (a 3344x136
 *                     scroll strip from the C64 art, see
 *                     uridium_surface.h). Black pixels are transparent
 *                     so the lattice + girders + stars show through.
 *   Layer 5 (player): the starship; up/down arrows fly it vertically.
 *
 * The demo installs its own 16-colour palette at init (greys for the
 * zinc surface + blues/orange for the ship) and restores the default
 * palette on teardown. Loops until ESC (the dispatcher owns ESC).
 * Two on-screen readouts show the full-screen draw cost and the c2p
 * cost in microseconds.
 */

#include "demo.h"

#include <stdint.h>

#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "uridium_surface.h"

/* Per-file -O3: the global build is MinSizeRel (-Os); this file is pure
 * compute (per-pixel parallax/sprite drawing), so optimise it for
 * speed. No cart-bus / PIO timing code lives here. */
#pragma GCC optimize("O3")

extern const struct FB_FONT font8x8;

/* Palette indices (this demo's own palette, installed at init). */
#define COL_BG         15  /* space black                     */
#define COL_STAR_DIM    3  /* far star (blue)                 */
#define COL_STAR_MID    5  /* mid star (pale cyan)            */
#define COL_STAR_HI     0  /* near star (white)               */
#define COL_TEXT        0  /* HUD text (white)                */

/* This demo's palette. idx 1/2 are the zinc greys the surface LUT
 * maps to; 0=white, 3/4=ship blue ramp, 5=star cyan, 7=ship engine,
 * 15=space black. Restored to the template default on teardown. */
static const uint16_t s_uridium_palette[PALETTE_ENTRIES] = {
    PALETTE_RGB(7, 7, 7),  /* 0  white  - text / cockpit / hi-star / surface hi */
    PALETTE_RGB(2, 2, 2),  /* 1  dark grey - surface shadow + ship outline */
    PALETTE_RGB(5, 5, 5),  /* 2  light grey - zinc surface */
    PALETTE_RGB(1, 3, 6),  /* 3  blue - ship body / dim star */
    PALETTE_RGB(3, 5, 7),  /* 4  light blue - ship highlight */
    PALETTE_RGB(5, 6, 7),  /* 5  pale cyan - mid star */
    PALETTE_RGB(7, 4, 1),  /* 6  bright rust - girder highlight edge */
    PALETTE_RGB(7, 4, 0),  /* 7  orange - ship engine */
    PALETTE_RGB(5, 2, 0),  /* 8  rust brown - girder body */
    PALETTE_RGB(2, 1, 0),  /* 9  dark rust - girder shadow */
    PALETTE_RGB(4, 4, 4),  /* 10 mid grey - diagonal lattice */
    PALETTE_RGB(0, 0, 0),  /* 11 */
    PALETTE_RGB(0, 0, 0),  /* 12 */
    PALETTE_RGB(0, 0, 0),  /* 13 */
    PALETTE_RGB(0, 0, 0),  /* 14 */
    PALETTE_RGB(0, 0, 0),  /* 15 black - space */
};

/* Starfield. */
#define STAR_COUNT      96
#define STAR_FIELD_W    320
#define STAR_SPEED_FAR   1
#define STAR_SPEED_NEAR  2

/* Middle layer: a rusted industrial girder framework (between the
 * stars and the surface). Each beam is drawn embossed (highlight /
 * body / shadow) for a 3D look, with corrosion mottling along its
 * length and a bolted plate at every joint. Cells stay open so the
 * stars show through; the metal surface in turn covers most of it,
 * leaving the framework visible in the star margins + surface gaps. */
#define GRID_CELL       32
#define GRID_SPEED       3  /* vertical beams scroll left  (right->left) */
#define GRID_VSPEED      1  /* horizontal beams scroll up  (down->up)    */
#define COL_BEAM_HI      6  /* bright rust - lit edge (top/left)  */
#define COL_BEAM_BODY    8  /* rust brown - beam body             */
#define COL_BEAM_LO      9  /* dark rust - shadow edge / pitting  */
#define COL_BOLT         2  /* light grey metal - bolt head       */

/* Diagonal grey lattice (between the rust girders and the surface).
 * Two crossing families of 45-degree lines form a drifting diamond
 * grid in two grey tones; only the line pixels are written so the
 * rust girders + stars still show in the open cells. SPACING is a
 * power of two so the test is a bitwise AND. */
#define DIAG_SPACING    16
#define DIAG_MASK       (DIAG_SPACING - 1)
#define DIAG_SPEED       2
#define COL_DIAG_A       2  /* light grey - "/" lines */
#define COL_DIAG_B      10  /* mid grey   - "\" lines */

/* Surface scroll. Sits as a band SURF_Y..SURF_Y+H, scrolling left. */
#define SURF_Y          32
#define SURF_SPEED       4

/* Ship control: up/down arrows fly the ship vertically. IKBD has no
 * auto-repeat, so we latch held state on press/release and move while
 * held. */
#define IKBD_SC_UP      0x48u
#define IKBD_SC_DOWN    0x50u
#define SHIP_X          46
#define SHIP_SPEED       2
#define SHIP_Y_MIN       4
#define SHIP_Y_MAX     (FB_CHUNKED_H - SHIP_H - 4)

/* Player ship sprite: 28x16, palette indices, key 0xFF transparent. */
#define SHIP_W 28
#define SHIP_H 16
#define SHIP_KEY 0xFFu
static const uint8_t ship_data[SHIP_W * SHIP_H] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x01, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x07, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x03, 0x03, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const struct FB_BITMAP ship_bmp = {SHIP_W, SHIP_H, ship_data};

static uint16_t s_star_x[STAR_COUNT];
static uint8_t  s_star_y[STAR_COUNT];
static uint8_t  s_star_kind[STAR_COUNT];
static uint32_t s_frame;
static uint8_t  s_built;

static int     s_ship_y;       /* current ship top-left y */
static uint8_t s_up_held;      /* up arrow currently held */
static uint8_t s_down_held;    /* down arrow currently held */

static uint32_t s_prev_draw_us;
static uint32_t s_prev_cv_us;

static uint32_t s_rng = 0x1234567u;
static uint32_t rng(void) {
  s_rng = s_rng * 1664525u + 1013904223u;
  return s_rng;
}

static const char *u32str(uint32_t n, char *buf, int buf_sz) {
  char *p = buf + buf_sz;
  *--p = '\0';
  if (n == 0) {
    *--p = '0';
  } else {
    while (n) {
      *--p = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return p;
}

static void build_stars(void) {
  s_rng = 0x1234567u;
  for (int i = 0; i < STAR_COUNT; i++) {
    s_star_x[i] = (uint16_t)(rng() % STAR_FIELD_W);
    s_star_y[i] = (uint8_t)(rng() % FB_CHUNKED_H);
    s_star_kind[i] = (uint8_t)(rng() & 1u);
  }
}

static void parallax_init(void) {
  if (!s_built) {
    build_stars();
    s_built = 1;
  }
  palette_set(s_uridium_palette);
  s_frame = 0;
  s_prev_draw_us = 0;
  s_prev_cv_us = 0;
  s_ship_y = 92;
  s_up_held = 0;
  s_down_held = 0;
}

static void parallax_teardown(void) {
  palette_init();  /* restore the template default palette */
  s_frame = 0;
}

/* Latch up/down arrow held state. IKBD sends one press + one release
 * (no auto-repeat), so we track held and move per-frame in render. */
static void parallax_handle_key(const ikbd_key_event_t *k) {
  if (k->scancode == IKBD_SC_UP) {
    s_up_held = k->is_press;
  } else if (k->scancode == IKBD_SC_DOWN) {
    s_down_held = k->is_press;
  }
}

/* Draw a 3x3 bolted plate (dark rivet plate + bright bolt head)
 * centred on (cx, cy). fb_chunked_plot clips at the edges. */
static void __not_in_flash_func(draw_bolt)(int cx, int cy) {
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      fb_chunked_plot((unsigned)(cx + dx), (unsigned)(cy + dy),
                      (dx == 0 && dy == 0) ? COL_BOLT : COL_BEAM_LO);
    }
  }
}

/* Draw the rusted girder framework. Vertical beams scroll left by
 * `go`; horizontal beams scroll up by `vo`. Each beam is an embossed
 * 3px strip (lit edge / mottled body / shadow edge); joints get a
 * bolted plate. */
static void __not_in_flash_func(draw_grid)(int go, int vo) {
  int x0 = (GRID_CELL - go) % GRID_CELL;  /* first vertical beam   */
  int y0 = (GRID_CELL - vo) % GRID_CELL;  /* first horizontal beam */

  /* Vertical girders (scroll left): columns x-1 (lit) | x (body) | x+1
   * (shadow), mottled per row so the rust looks corroded. */
  for (int x = x0; x < FB_CHUNKED_W; x += GRID_CELL) {
    for (int y = 0; y < FB_CHUNKED_H; y++) {
      uint8_t body = ((y & 7) < 2) ? COL_BEAM_LO : COL_BEAM_BODY;
      fb_chunked_buffer[y * FB_CHUNKED_W + x] = body;
      fb_chunked_plot((unsigned)(x - 1), (unsigned)y, COL_BEAM_HI);
      fb_chunked_plot((unsigned)(x + 1), (unsigned)y, COL_BEAM_LO);
    }
  }
  /* Horizontal girders (scroll up): rows y-1 (lit) | y (body) | y+1
   * (shadow). Drawn second so crossings read as horizontal-over. */
  for (int y = y0; y < FB_CHUNKED_H; y += GRID_CELL) {
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      uint8_t body = ((x & 7) < 2) ? COL_BEAM_LO : COL_BEAM_BODY;
      fb_chunked_buffer[y * FB_CHUNKED_W + x] = body;
      fb_chunked_plot((unsigned)x, (unsigned)(y - 1), COL_BEAM_HI);
      fb_chunked_plot((unsigned)x, (unsigned)(y + 1), COL_BEAM_LO);
    }
  }
  /* Bolted joints at every intersection. */
  for (int y = y0; y < FB_CHUNKED_H; y += GRID_CELL) {
    for (int x = x0; x < FB_CHUNKED_W; x += GRID_CELL) {
      draw_bolt(x, y);
    }
  }
}

/* Draw the diagonal grey lattice, translating toward the bottom-left
 * (right->left and up->down). Rigid translation by (-o,+o): the "\"
 * family ((x-y) lines) sweeps down-left; the "/" family ((x+y) lines)
 * runs parallel to the motion so it stays put while the diamond
 * crossings flow along it. Only line pixels are written (cells stay
 * open). The +4096 keeps the "\" term non-negative before the mask
 * (multiple of the spacing -> same residue). */
static void __not_in_flash_func(draw_diag)(uint32_t f) {
  int o = (int)((2u * (uint32_t)DIAG_SPEED * f) & (uint32_t)DIAG_MASK);
  for (int y = 0; y < FB_CHUNKED_H; y++) {
    uint8_t *row = fb_chunked_buffer + y * FB_CHUNKED_W;
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      if (((x - y + o + 4096) & DIAG_MASK) == 0) {
        row[x] = COL_DIAG_B;
      } else if (((x + y) & DIAG_MASK) == 0) {
        row[x] = COL_DIAG_A;
      }
    }
  }
}

/* Draw the scrolling surface band. For each screen column the source
 * column wraps over the 3344-wide strip; black (code 0) is left
 * transparent so the starfield underneath shows through. */
static void __not_in_flash_func(draw_surface)(int xoff) {
  for (int x = 0; x < FB_CHUNKED_W; x++) {
    int sc = x + xoff;
    if (sc >= URIDIUM_SURFACE_W) sc -= URIDIUM_SURFACE_W;
    uint32_t byte_off = (uint32_t)(sc >> 2);
    int shift = (sc & 3) * 2;
    const uint8_t *col = uridium_surface_data + byte_off;
    uint8_t *dst = fb_chunked_buffer + (SURF_Y * FB_CHUNKED_W) + x;
    for (int r = 0; r < URIDIUM_SURFACE_H; r++) {
      uint8_t code = (uint8_t)((col[r * URIDIUM_SURFACE_STRIDE] >> shift) & 3u);
      if (code != URIDIUM_CODE_TRANSPARENT) {
        dst[r * FB_CHUNKED_W] = uridium_surface_lut[code];
      }
    }
  }
}

static void __not_in_flash_func(parallax_render_frame)(void) {
  uint32_t t0 = time_us_32();
  uint32_t f = s_frame++;

  /* Layer 1: starfield over black. */
  fb_chunked_clear(COL_BG);
  for (int i = 0; i < STAR_COUNT; i++) {
    int speed = s_star_kind[i] ? STAR_SPEED_NEAR : STAR_SPEED_FAR;
    int x = (int)(((uint32_t)s_star_x[i] + STAR_FIELD_W -
                   (f * (uint32_t)speed) % STAR_FIELD_W) %
                  STAR_FIELD_W);
    uint8_t c = s_star_kind[i] ? COL_STAR_HI
                               : ((i & 1) ? COL_STAR_MID : COL_STAR_DIM);
    fb_chunked_plot((unsigned)x, s_star_y[i], c);
  }

  /* Layer 2: rusted girder framework -- scrolls left + up. */
  draw_grid((int)((f * GRID_SPEED) % GRID_CELL),
            (int)((f * GRID_VSPEED) % GRID_CELL));

  /* Layer 3: diagonal grey lattice -- drifts left + down. */
  draw_diag(f);

  /* Layer 4: scrolling Uridium surface over everything below (fast).
   * Black pixels are transparent, so the lattice, girders and stars
   * show through the surface gaps. */
  int xoff = (int)((f * SURF_SPEED) % URIDIUM_SURFACE_W);
  draw_surface(xoff);

  /* Layer 5: the player ship. Up/down arrows fly it vertically. */
  if (s_up_held) s_ship_y -= SHIP_SPEED;
  if (s_down_held) s_ship_y += SHIP_SPEED;
  if (s_ship_y < SHIP_Y_MIN) s_ship_y = SHIP_Y_MIN;
  if (s_ship_y > SHIP_Y_MAX) s_ship_y = SHIP_Y_MAX;
  fb_blit_key(&ship_bmp, SHIP_X, s_ship_y, SHIP_KEY);

  /* HUD: label + previous frame's timings (us). */
  char num[11];
  font_set_font(&font8x8);
  font_set_color(COL_TEXT);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  font_move(8, 6);
  font_print("URIDIUM  UP/DN=FLY  ESC=MENU");
  if (g_show_timing) {
    font_move(8, 16);
    font_print("DRAW ");
    font_print(u32str(s_prev_draw_us, num, sizeof(num)));
    font_print(" US");
    font_move(8, 26);
    font_print("C2P  ");
    font_print(u32str(s_prev_cv_us, num, sizeof(num)));
    font_print(" US");
  }

  uint32_t draw_us = time_us_32() - t0;
  fb_publish();
  s_prev_draw_us = draw_us;
  s_prev_cv_us = fb_last_convert_us();
}

const demo_module_t demo_parallax = {
    .name = "uridium",
    .init = parallax_init,
    .render_frame = parallax_render_frame,
    .handle_key = parallax_handle_key,
    .teardown = parallax_teardown,
};
