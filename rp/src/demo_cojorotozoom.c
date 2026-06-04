/**
 * File: demo_cojorotozoom.c
 * Description: "cojorotozoom" colour rotozoomer.
 *
 * A classic demoscene rotozoom of the C23CUT.PNG logo, over a
 * scrolling rusted girder grid (ported from the Uridium demo):
 *
 *   1. rusted girder grid  (scrolls left + up)
 *   2. the logo rotozoom on top -- the texture's black (idx 15) is
 *      TRANSPARENT, so the grid shows through the logo's empty areas
 *      while the emblem spins/zooms over it.
 *   3. a right-to-left bitmap scroll-text (FONT.PNG glyph font, 5-shade
 *      pink ramp idx 6..10; black = see-through), overlaid on the
 *      rotozoom with a sinusoidal zoom pulse (0.25x..8x) sampled about
 *      the screen centre.
 *   4. the "DIEGO" NEOchrome portrait (diego_sprite.h) flown around the
 *      screen on top of everything via a Lissajous path, colour-keyed,
 *      with a 0.25x..2x scale pulse.
 *
 * The rotozoom uses the standard incremental trick -- per row we
 * compute a start texture coord and constant per-pixel deltas, so the
 * inner loop is just two adds + a masked sample, no per-pixel
 * multiplies. All fixed-point (8.8), no FPU. Texture + palette come
 * from cojo_texture.h (logo idx 1..5, scroller idx 6..10, grid idx
 * 11..14); the palette is installed at init and default restored on
 * teardown.
 * Loops until ESC. On-screen DRAW + C2P microsecond readouts.
 */

#include <stdint.h>

#include "cojo_font.h"
#include "cojo_texture.h"
#include "demo.h"
#include "diego_sprite.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "hardware/interp.h"
#include "palette.h"
#include "pico/platform.h"
#include "pico/time.h"

extern const struct FB_FONT font8x8;

/* The global build is MinSizeRel (-Os), which leaves the per-pixel
 * rotozoom/scroller loops un-unrolled with weak register allocation --
 * the main reason the micro-opts above showed little gain. Force -O3
 * for this whole translation unit. It's safe to do here precisely
 * because this file is pure compute (texture sampling + framebuffer
 * writes) -- none of the timing-sensitive cart-bus / PIO code that a
 * global -O3 destabilised lives here. */
#pragma GCC optimize("O3")

/* The rotozoom uses INTERP0 as a hardware texel-address generator (see
 * roto_render_frame). The lane shifts/masks below hard-code a 256-wide,
 * 64-tall power-of-two texture, so guard those assumptions. */
_Static_assert(COJO_TEX_W == 256 && COJO_TEX_WMASK == 255 &&
                   COJO_TEX_HMASK == 63,
               "interp rotozoom config assumes a 256x64 power-of-two texture");

/* sin/cos table: 256 steps over 2*pi, scaled to +-256 (= +-1.0 in
 * 8.8 fixed point). cos(i) = sin256[(i + 64) & 255]. */
static const int16_t sin256[256] = {
    0,    6,    13,   19,   25,   31,   38,   44,   50,   56,   62,   68,
    74,   80,   86,   92,   98,   104,  109,  115,  121,  126,  132,  137,
    142,  147,  152,  157,  162,  167,  172,  177,  181,  185,  190,  194,
    198,  202,  206,  209,  213,  216,  220,  223,  226,  229,  231,  234,
    237,  239,  241,  243,  245,  247,  248,  250,  251,  252,  253,  254,
    255,  255,  256,  256,  256,  256,  256,  255,  255,  254,  253,  252,
    251,  250,  248,  247,  245,  243,  241,  239,  237,  234,  231,  229,
    226,  223,  220,  216,  213,  209,  206,  202,  198,  194,  190,  185,
    181,  177,  172,  167,  162,  157,  152,  147,  142,  137,  132,  126,
    121,  115,  109,  104,  98,   92,   86,   80,   74,   68,   62,   56,
    50,   44,   38,   31,   25,   19,   13,   6,    0,    -6,   -13,  -19,
    -25,  -31,  -38,  -44,  -50,  -56,  -62,  -68,  -74,  -80,  -86,  -92,
    -98,  -104, -109, -115, -121, -126, -132, -137, -142, -147, -152, -157,
    -162, -167, -172, -177, -181, -185, -190, -194, -198, -202, -206, -209,
    -213, -216, -220, -223, -226, -229, -231, -234, -237, -239, -241, -243,
    -245, -247, -248, -250, -251, -252, -253, -254, -255, -255, -256, -256,
    -256, -256, -256, -255, -255, -254, -253, -252, -251, -250, -248, -247,
    -245, -243, -241, -239, -237, -234, -231, -229, -226, -223, -220, -216,
    -213, -209, -206, -202, -198, -194, -190, -185, -181, -177, -172, -167,
    -162, -157, -152, -147, -142, -137, -132, -126, -121, -115, -109, -104,
    -98,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -50,  -44,  -38,  -31,
    -25,  -19,  -13,  -6,
};

/* HUD text colour. idx 0 is reserved white in the texture's palette
 * (cojo_texture.h), kept out of the image so the readout stays
 * legible over it. */
#define COL_TEXT 0

/* Background grid (palette idx 11..14 from cojo_texture.h), same
 * effect as the Uridium demo. */
#define GRID_CELL 32
#define GRID_SPEED 3     /* vertical beams scroll left */
#define GRID_VSPEED 1    /* horizontal beams scroll up */
#define COL_BEAM_HI 11   /* girder lit edge   (grid highlight grey) */
#define COL_BEAM_BODY 12 /* girder body       */
#define COL_BEAM_LO 13   /* girder shadow     */
#define COL_BOLT 14      /* bolt head (light grey) */

/* Rotation / zoom / pan advance at DIFFERENT rates so they don't stay
 * locked -- you see the logo at many rotation+zoom combinations rather
 * than always the same zoom for a given angle. */
#define ROT_SPEED 1
#define ZOOM_DRIFT                                    \
  5 /* zoom phase = f + (f>>5) ~ 1.03/frame: ~1/frame \
     * but slowly slides vs the rotation (full cycle  \
     * ~160 s) so they never lock. */
#define PAN_SPEED 2

/* DIEGO portrait sprite drifts in a Lissajous path over the scene AND
 * pulses in scale. AX/AY = pixel amplitude about screen centre;
 * FX != FY so the path never retraces a simple loop. The scale
 * animates sinusoidally between 0.25x and 2x (8.8 fixed point). */
#define DIEGO_AX 110
#define DIEGO_AY 44
#define DIEGO_FX 2
#define DIEGO_FY 3
#define DIEGO_ZMIN 64   /* 0.25 * 256 */
#define DIEGO_ZMAX 512  /* 2.00 * 256 */
#define DIEGO_ZMID ((DIEGO_ZMIN + DIEGO_ZMAX) / 2)
#define DIEGO_ZHALF ((DIEGO_ZMAX - DIEGO_ZMIN) / 2)
#define DIEGO_ZSPEED 3  /* scale-pulse phase advance / frame */

static uint32_t s_frame;
static uint32_t s_prev_draw_us;
static uint32_t s_prev_cv_us;

static const char *__not_in_flash_func(u32str)(uint32_t n, char *buf,
                                               int buf_sz) {
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

/* --- Scrolling industrial background (ported from the Uridium demo) - */

static void __not_in_flash_func(draw_bolt)(int cx, int cy) {
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      fb_chunked_plot((unsigned)(cx + dx), (unsigned)(cy + dy),
                      (dx == 0 && dy == 0) ? COL_BOLT : COL_BEAM_LO);
    }
  }
}

static void __not_in_flash_func(draw_grid)(int go, int vo) {
  int x0 = (GRID_CELL - go) % GRID_CELL;
  int y0 = (GRID_CELL - vo) % GRID_CELL;

  /* Vertical beams: walk a column pointer straight down the buffer
   * (p += W) instead of recomputing y*W per pixel, and hoist the
   * left/right edge guards out of the inner loop (they're per-column). */
  for (int x = x0; x < FB_CHUNKED_W; x += GRID_CELL) {
    int left = (x - 1) >= 0;
    int right = (x + 1) < FB_CHUNKED_W;
    uint8_t *p = fb_chunked_buffer + x;
    for (int y = 0; y < FB_CHUNKED_H; y++, p += FB_CHUNKED_W) {
      p[0] = ((y & 7) < 2) ? COL_BEAM_LO : COL_BEAM_BODY;
      if (left) {
        p[-1] = COL_BEAM_HI;
      }
      if (right) {
        p[1] = COL_BEAM_LO;
      }
    }
  }

  /* Horizontal beams: the three row offsets are constant along a line,
   * so resolve them once per line instead of per pixel inside
   * fb_chunked_plot. */
  for (int y = y0; y < FB_CHUNKED_H; y += GRID_CELL) {
    uint8_t *row = fb_chunked_buffer + y * FB_CHUNKED_W;
    uint8_t *up = (y - 1) >= 0 ? row - FB_CHUNKED_W : NULL;
    uint8_t *dn = (y + 1) < FB_CHUNKED_H ? row + FB_CHUNKED_W : NULL;
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      row[x] = ((x & 7) < 2) ? COL_BEAM_LO : COL_BEAM_BODY;
      if (up) {
        up[x] = COL_BEAM_HI;
      }
      if (dn) {
        dn[x] = COL_BEAM_LO;
      }
    }
  }

  for (int y = y0; y < FB_CHUNKED_H; y += GRID_CELL) {
    for (int x = x0; x < FB_CHUNKED_W; x += GRID_CELL) {
      draw_bolt(x, y);
    }
  }
}

/* --- Right-to-left scroll-text overlay (FONT.PNG bitmap font) ------- */

#define SCROLL_SRC_SPEED 2 /* scroll speed in source text-px / frame */

/* Pulsing zoom: scale animates sinusoidally between 0.25x and 8x (8.8
 * fixed point). The text is sampled destination-driven (like the
 * rotozoom) about the screen centre, so it grows/shrinks as it scrolls. */
#define ZOOM_MIN_FIX 64   /* 0.25 * 256 */
#define ZOOM_MAX_FIX 2048 /* 8.00 * 256 */
#define ZOOM_MID_FIX ((ZOOM_MIN_FIX + ZOOM_MAX_FIX) / 2)
#define ZOOM_HALF_FIX ((ZOOM_MAX_FIX - ZOOM_MIN_FIX) / 2)
#define ZOOM_SPEED 2 /* zoom-pulse phase advance / frame */

#define SCROLL_X_ANCHOR (FB_CHUNKED_W / 2) /* zoom centre x (screen) */
#define SCROLL_Y_CENTER (FB_CHUNKED_H / 2) /* zoom centre y (screen) */
#define SCROLL_V_CENTER \
  (COJO_FONT_CELL_H / 2) /* glyph-row mapped to y centre */

static const char SCROLL_MSG[] =
    "HI GUYS THIS IS CANAL 23 BACK FROM THE EIGHTIES!      ";

static uint32_t s_scroll_uf; /* scroll position, source px in 8.8 */
static int s_zoom_phase;

/* Draw the looping message as a single horizontal strip, sampled per
 * screen pixel at the current pulsing zoom. The strip is the glyph
 * cells concatenated (32 src px each, 31 inked + 1 separator) and wraps
 * mod its length, so the trailing spaces are the loop gap. Only inked,
 * non-transparent texels are written, so the rotozoom shows through. */
static void __not_in_flash_func(draw_scroller)(void) {
  const int n = (int)(sizeof(SCROLL_MSG) - 1); /* drop the NUL */
  const int strip = n * COJO_FONT_CELL_W;      /* source strip width (px) */
  const long mod = (long)strip << 8;           /* strip width in 8.8 */

  int s_fix =
      ZOOM_MID_FIX + ((sin256[s_zoom_phase & 255] * ZOOM_HALF_FIX) >> 8);
  int inv = 65536 / s_fix; /* 8.8 source-px per screen-px */
  uint32_t scroll = (uint32_t)(s_scroll_uf % (uint32_t)mod);

  /* Restrict the row scan to the band's on-screen extent (rows where the
   * glyph maps to vv in [0,CELL_H)); the per-row vv guard below is the
   * exact backstop. */
  int y_lo = SCROLL_Y_CENTER - (SCROLL_V_CENTER * 256) / inv;
  int y_hi = SCROLL_Y_CENTER +
             ((COJO_FONT_CELL_H - 1 - SCROLL_V_CENTER) * 256) / inv + 1;
  if (y_lo < 0) {
    y_lo = 0;
  }
  if (y_hi > FB_CHUNKED_H) {
    y_hi = FB_CHUNKED_H;
  }

  for (int y = y_lo; y < y_hi; y++) {
    int vv = ((SCROLL_V_CENTER << 8) + (y - SCROLL_Y_CENTER) * inv) >> 8;
    if (vv < 0 || vv >= COJO_FONT_CELL_H) {
      continue;
    }
    /* source u at x=0, normalised into [0, mod) */
    long u0 = (long)scroll + (long)(0 - SCROLL_X_ANCHOR) * inv;
    u0 %= mod;
    if (u0 < 0) {
      u0 += mod;
    }
    uint32_t u_fix = (uint32_t)u0;
    uint8_t *dst = fb_chunked_buffer + y * FB_CHUNKED_W;
    int last_ci = -1;                     /* cached glyph cell across pixels */
    const uint8_t *grow = cojo_font_data; /* font row base of current glyph */
    int blank = 1;
    for (int x = 0; x < FB_CHUNKED_W; x++) {
      uint32_t up = u_fix >> 8;              /* source px */
      int ci = (int)(up / COJO_FONT_CELL_W); /* char index (>>5) */
      if (ci != last_ci) {
        /* Only recompute the glyph cell when the source column enters a
         * new character -- this hoists the /10 and %10 cell-locate
         * divisions out of the per-pixel path (the big win at high zoom,
         * where each char spans many screen pixels). */
        last_ci = ci;
        int c = (unsigned char)SCROLL_MSG[ci];
        int gidx =
            (c >= 32 && c < 128) ? cojo_font_ascii[c - 32] : COJO_FONT_BLANK;
        blank = (gidx == COJO_FONT_BLANK);
        if (!blank) {
          int gx0 = (gidx % COJO_FONT_COLS) * COJO_FONT_CELL_W;
          int gy = (gidx / COJO_FONT_COLS) * COJO_FONT_CELL_H + vv;
          grow = cojo_font_data + gy * COJO_FONT_SHEET_W + gx0;
        }
      }
      if (!blank) {
        int cell_col = (int)(up & (COJO_FONT_CELL_W - 1)); /* &31 */
        if (cell_col < COJO_FONT_GLYPH_W) {                /* skip separator */
          uint8_t pix = grow[cell_col];
          if (pix != COJO_FONT_TRANSPARENT) {
            dst[x] = pix;
          }
        }
      }
      u_fix += (uint32_t)inv;
      if (u_fix >= (uint32_t)mod) {
        u_fix -= (uint32_t)mod;
      }
    }
  }
  s_scroll_uf += (uint32_t)SCROLL_SRC_SPEED << 8;
  s_zoom_phase += ZOOM_SPEED;
}

/* Draw the DIEGO sprite centred at (cx,cy), scaled by s_fix (8.8), with
 * colour-key transparency. Destination-driven nearest sample with the
 * source column advanced incrementally (no per-pixel multiply). */
static void __not_in_flash_func(blit_diego)(int cx, int cy, int s_fix) {
  const int w = diego_sprite.width;
  const int h = diego_sprite.height;
  int inv = 65536 / s_fix;             /* 8.8 source-px per screen-px */
  int half_w = (w * s_fix) >> 9;       /* half the scaled width  (px) */
  int half_h = (h * s_fix) >> 9;       /* half the scaled height (px) */
  int x0 = cx - half_w, x1 = cx + half_w;
  int y0 = cy - half_h, y1 = cy + half_h;
  if (x0 < 0) {
    x0 = 0;
  }
  if (x1 > FB_CHUNKED_W) {
    x1 = FB_CHUNKED_W;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (y1 > FB_CHUNKED_H) {
    y1 = FB_CHUNKED_H;
  }
  for (int y = y0; y < y1; y++) {
    int sv = (((y - cy) * inv) >> 8) + (h >> 1);
    if (sv < 0 || sv >= h) {
      continue;
    }
    const uint8_t *srow = diego_sprite_data + sv * w;
    uint8_t *drow = fb_chunked_buffer + y * FB_CHUNKED_W;
    int su_fix = (x0 - cx) * inv + ((w >> 1) << 8);
    for (int x = x0; x < x1; x++) {
      int su = su_fix >> 8;
      if ((unsigned)su < (unsigned)w) {
        uint8_t p = srow[su];
        if (p != DIEGO_KEY) {
          drow[x] = p;
        }
      }
      su_fix += inv;
    }
  }
}

static void __not_in_flash_func(roto_init)(void) {
  palette_set(cojo_palette);
  s_frame = 0;
  s_prev_draw_us = 0;
  s_prev_cv_us = 0;
  s_scroll_uf = 0;
  s_zoom_phase = 0;
}

static void __not_in_flash_func(roto_teardown)(void) {
  palette_init(); /* restore the template default palette */
  s_frame = 0;
}

static void __not_in_flash_func(roto_render_frame)(void) {
  uint32_t t0 = time_us_32();
  uint32_t f = s_frame++;

  /* Animated rotation, pulsing zoom, slow circular pan -- each at its
   * own rate (see ROT/ZOOM/PAN_SPEED) so they stay out of lockstep. */
  int ang = (int)(f * ROT_SPEED) & 255;
  int cos_a = sin256[(ang + 64) & 255]; /* +-256 */
  int sin_a = sin256[ang];              /* +-256 */
  /* 8.8 zoom that pulses between extremes. z is texels-per-screen-
   * pixel: small z = magnified (texture huge), large z = many tiles
   * (texture tiny). Range ~64..4096 (8.8) = ~4x in .. ~0.06x out
   * (about 20 tiles across when smallest). */
  uint32_t zphase = f + (f >> ZOOM_DRIFT);
  int z = 2080 + ((2016 * sin256[zphase & 255u]) >> 8);

  /* Per-pixel / per-row texture-coord deltas (8.8 texels). */
  int dux = (cos_a * z) >> 8;
  int duy = (-sin_a * z) >> 8;
  int dvx = (sin_a * z) >> 8;
  int dvy = (cos_a * z) >> 8;

  /* Texture-space centre with a slow circular pan, in 8.8. */
  int cu = ((COJO_TEX_W / 2) << 8) + (sin256[(f * PAN_SPEED) & 255u] << 2);
  int cv =
      ((COJO_TEX_H / 2) << 8) + (sin256[((f * PAN_SPEED) + 64u) & 255u] << 2);

  /* Scrolling industrial background (shows through the logo's empty
   * areas). Clear to black, then the rust girder framework. */
  fb_chunked_clear(COJO_TEX_TRANSPARENT);
  draw_grid((int)((f * GRID_SPEED) % GRID_CELL),
            (int)((f * GRID_VSPEED) % GRID_CELL));

  /* Texture coord at screen (0,0): centre minus the rotated offset to
   * the screen centre (160,100). */
  int u_row = cu - 160 * dux - 100 * duy;
  int v_row = cv - 160 * dvx - 100 * dvy;

  /* RP2040 interpolator as the texel-address generator -- the canonical
   * "affine texture mapping" use of INTERP0. With ACCUM0=u, ACCUM1=v
   * (8.8) and BASE2 = texture base:
   *   lane 0 (shift 8, mask 0..7)  -> (u >> 8) & 255       == &WMASK
   *   lane 1 (shift 0, mask 8..13) -> ((v >> 8) & 63) << 8 == (&HMASK)*W
   *   PEEK_FULL = BASE2 + lane0 + lane1 = the texel byte address.
   * interp_add_accumulator advances u/v by the per-pixel deltas. This
   * collapses the ~6 shift/mask/add address-gen instructions + 2 coord
   * adds into ~3 single-cycle SIO accesses per pixel. The generated
   * address is validated byte-identical to the plain C expression
   *   cojo_texture_data[((v>>8)&HMASK)*COJO_TEX_W + ((u>>8)&WMASK)].
   * Re-armed every frame so nothing else can leave INTERP0 misconfigured. */
  interp_config c0 = interp_default_config();
  interp_config_set_shift(&c0, 8);
  interp_config_set_mask(&c0, 0, 7);
  interp_set_config(interp0, 0, &c0);
  interp_config c1 = interp_default_config();
  interp_config_set_shift(&c1, 0);
  interp_config_set_mask(&c1, 8, 13);
  interp_set_config(interp0, 1, &c1);
  interp0->base[2] = (uintptr_t)cojo_texture_data;

  for (int sy = 0; sy < FB_CHUNKED_H; sy++) {
    interp0->accum[0] = (uint32_t)u_row;
    interp0->accum[1] = (uint32_t)v_row;
    uint8_t *row = fb_chunked_buffer + sy * FB_CHUNKED_W;
    for (int sx = 0; sx < FB_CHUNKED_W; sx++) {
      uint8_t texel = *(const uint8_t *)interp_peek_full_result(interp0);
      if (texel != COJO_TEX_TRANSPARENT) {
        row[sx] = texel; /* logo over the background; black = see-through */
      }
      interp_add_accumulator(interp0, 0, (uint32_t)dux);
      interp_add_accumulator(interp0, 1, (uint32_t)dvx);
    }
    u_row += duy;
    v_row += dvy;
  }

  /* Scroll-text overlay along the bottom (drawn over the rotozoom). */
  draw_scroller();

  /* DIEGO portrait flying around on top of everything: Lissajous drift
   * about screen centre + a 0.25x..2x scale pulse. */
  int hcx = FB_CHUNKED_W / 2 + ((DIEGO_AX * sin256[(f * DIEGO_FX) & 255u]) >> 8);
  int hcy = FB_CHUNKED_H / 2 + ((DIEGO_AY * sin256[(f * DIEGO_FY) & 255u]) >> 8);
  int hs = DIEGO_ZMID + ((sin256[(f * DIEGO_ZSPEED) & 255u] * DIEGO_ZHALF) >> 8);
  blit_diego(hcx, hcy, hs);

  /* HUD. */
  char num[11];
  font_set_font(&font8x8);
  font_set_color(COL_TEXT);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  font_move(8, 6);
  font_print("COJOROTOZOOM   ESC=MENU");
  if (g_show_timing) {
    font_move(8, 16);
    font_print("DRAW ");
    font_print(u32str(s_prev_draw_us, num, sizeof(num)));
    font_print(" uS");
    font_move(8, 26);
    font_print("C2P  ");
    font_print(u32str(s_prev_cv_us, num, sizeof(num)));
    font_print(" uS");
  }

  uint32_t draw_us = time_us_32() - t0;
  fb_publish();
  s_prev_draw_us = draw_us;
  s_prev_cv_us = fb_last_convert_us();
}

const demo_module_t demo_cojorotozoom = {
    .name = "cojorotozoom",
    .init = roto_init,
    .render_frame = roto_render_frame,
    .handle_key = NULL,
    .teardown = roto_teardown,
};
