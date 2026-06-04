/**
 * File: demo_3d.c
 * Description: filled flat-shaded rotating 3D solid.
 *
 * A classic demoscene vector object: a geodesic sphere (icosahedron
 * subdivided, vertex/face counts in solid3d.h) tumbling in space, each
 * facet flat-shaded by a directional light. Pure integer fixed-point (no
 * FPU): rotation via a sin/cos table, perspective projection (one
 * reciprocal-multiply per vertex), backface culling, then direct
 * triangle fill (no z-buffer / painter's sort -- a convex solid's front
 * faces tile the silhouette exactly once back faces are culled). Faces
 * are scanline-filled straight into the chunked framebuffer via an
 * incrementally-walked edge DDA, with a canonical top->bottom vertex
 * order so shared edges meet seam-free.
 * 16 Atari ST colours (a shaded ramp + gradient backdrop); 320x200.
 * On-screen DRAW + C2P readouts; ESC returns to the menu (dispatcher).
 */

#include "demo.h"

#include <stdint.h>
#include <string.h>

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "solid3d.h"

#pragma GCC optimize("O3")

extern const struct FB_FONT font8x8;

/* sin/cos: 256 steps over 2*pi, scaled to +-256. cos(i)=sin256[i+64]. */
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
#define SIN(a) (sin256[(a) & 255])
#define COS(a) (sin256[((a) + 64) & 255])

/* Geometry (geodesic-sphere verts + triangle faces) is in solid3d.h. */

/* Palette: 0/1 = backdrop gradient, 2..13 = 12-step shaded ramp
 * (dark->bright), 14 = white (HUD), 15 = black. */
/* "Boing Ball"-inspired: magenta grid backdrop (idx 0 = grid line,
 * idx 1 = cell) + a dark-red -> red -> white shade ramp (the ball reads
 * red with white highlights). */
static const uint16_t solid_palette[PALETTE_ENTRIES] = {
    /* idx 0 == ST border colour, so the grey backdrop is also the border. */
    PALETTE_RGB(5, 5, 5), PALETTE_RGB(7, 2, 7),  /* grey cell+border / magenta line */
    PALETTE_RGB(2, 0, 0), PALETTE_RGB(3, 0, 0),  PALETTE_RGB(4, 1, 1),
    PALETTE_RGB(5, 1, 1), PALETTE_RGB(6, 2, 2),  PALETTE_RGB(7, 2, 2),
    PALETTE_RGB(7, 3, 3), PALETTE_RGB(7, 4, 4),  PALETTE_RGB(7, 5, 5),
    PALETTE_RGB(7, 6, 6), PALETTE_RGB(7, 7, 7),  PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7), PALETTE_RGB(0, 0, 0),
};

#define CXp (FB_CHUNKED_W / 2)
#define CYp (FB_CHUNKED_H / 2)
#define FOV 150   /* base projection scale (pixels) -- smaller object */
#define ZOOM_AMP 110 /* +- zoom pulse on FOV: ranges 40..260 (tiny ball
                      * far out -> nearly fills the screen zoomed in) */
#define ZOOM_SPEED 2 /* zoom-sine phase advance / frame */
#define BOUNCE_SPEED 160        /* horizontal bounce, 8.8 px/frame (slow) */
#define BOUNCE_LIMIT (105 << 8) /* +- travel from centre, 8.8 */
/* Vertical gravity bounce off the floor (classic Boing arc): the ball
 * rises from the floor, decelerates under GRAVITY, falls back and on
 * touching the floor relaunches upward at a fixed BOUNCE_VY so every
 * bounce reaches the same apex (no integer energy drift). s_bally is the
 * height ABOVE the floor (>= 0); s_ballvy is +up. All 8.8 fixed point. */
#define GRAVITY 80     /* downward accel / frame, 8.8 */
#define BOUNCE_VY 2560 /* up-launch speed at floor, 8.8 (apex ~160 px, fast) */
/* The floor is pinned to the bottom row of the physical screen and the
 * vertical zoom is anchored there (not at the screen centre): the ball's
 * projected silhouette half-height is ~0.41*fov, so seating its centre at
 * (bottom - ball_r) keeps its lowest pixel on the bottom row for every
 * zoom level. The backdrop grid is anchored to the same floor. */
#define FLOOR_SCREEN (FB_CHUNKED_H - 1)   /* floor = bottom row */
#define BALL_R(fov) (((fov) * 106) >> 8)  /* silhouette half-height (px) */
#define ZDIST 1100 /* camera distance, 8.8 (keeps z + ZDIST > 0) */
#define RAMP_LO 2
#define RAMP_N 12 /* shade indices RAMP_LO .. RAMP_LO+RAMP_N-1 */
#define GRID_STEP 14 /* backdrop grid cell size (px) -- smaller cells */
#define COL_TEXT 14

/* Directional light. Slightly lateral/behind so the ball gets a clear
 * light->dark falloff across its visible face. */
#define LX 190
#define LY 120
#define LZ 40
/* Per-vertex intensity = (vertex . light) >> SHADE_SHIFT + SHADE_BIAS,
 * in 8.8 of the ramp index (0 .. RAMP_N<<8). Interpolated (Gouraud) and
 * ordered-dithered across each triangle so facets blend smoothly. */
#define SHADE_SHIFT 6
#define SHADE_BIAS (6 << 8)

static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint8_t s_ax, s_ay, s_az, s_zoom;
static int s_ballx, s_balldir;              /* horizontal bounce (8.8) + dir */
static int s_bally, s_ballvy;               /* vertical bounce pos + vel (8.8) */
static int s_tz[SOLID_NV];                  /* transformed z (8.8) */
static int s_px[SOLID_NV], s_py[SOLID_NV];  /* projected screen coords */
static int s_tx[SOLID_NV], s_ty[SOLID_NV];  /* transformed x,y (8.8) */
static uint32_t s_prev_draw_us, s_prev_cv_us;

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

/* Fill one scanline [xl,xr] with the facet's two bracketing ramp colours,
 * ordered-dithered by the Bayer cell. Pure table-compare + store -- no
 * divide, no per-pixel accumulate. */
static inline void __not_in_flash_func(fill_span)(int y, int xl, int xr,
                                                  int frac, uint8_t col_lo,
                                                  uint8_t col_hi) {
  if (xl > xr) {
    int t = xl;
    xl = xr;
    xr = t;
  }
  if (xl < 0) xl = 0;
  if (xr > FB_CHUNKED_W - 1) xr = FB_CHUNKED_W - 1;
  const uint8_t *brow = bayer4[y & 3];
  uint8_t *p = fb_chunked_buffer + (size_t)y * FB_CHUNKED_W + xl;
  for (int x = xl; x <= xr; x++) *p++ = (frac > brow[x & 3]) ? col_hi : col_lo;
}

/* Flat-shaded, ordered-dithered triangle fill. The whole facet is one
 * intensity `iv` (8.8 of the ramp index): the two bracketing ramp colours
 * and the 4-bit dither fraction are computed once, so the inner loop is
 * just compare+store. The two active edges of each half are walked
 * incrementally -- one divide per edge for a 16.16 slope, then an add per
 * scanline -- instead of re-dividing every row, so the rasteriser costs
 * ~3 divides per triangle instead of ~2 per scanline. Vertices are walked
 * canonical top->bottom so a shared edge rounds identically in both
 * adjacent faces => seam-free (verified: 0 interior holes across the
 * tumble). */
static void __not_in_flash_func(fill_tri)(const int *px, const int *py,
                                          int iv) {
  int o0 = 0, o1 = 1, o2 = 2; /* sort the 3 vertices top->bottom by y */
  if (py[o0] > py[o1]) { int t = o0; o0 = o1; o1 = t; }
  if (py[o1] > py[o2]) { int t = o1; o1 = o2; o2 = t; }
  if (py[o0] > py[o1]) { int t = o0; o0 = o1; o1 = t; }
  int ax = px[o0], ay = py[o0];
  int bx = px[o1], by = py[o1];
  int cx = px[o2], cy = py[o2];
  if (cy == ay) return; /* zero height */

  /* dither setup, constant across the whole facet */
  int base = iv >> 8, frac = (iv >> 4) & 15;
  int lo = base, hi = base + 1;
  if (lo < 0) lo = 0; else if (lo > RAMP_N - 1) lo = RAMP_N - 1;
  if (hi < 0) hi = 0; else if (hi > RAMP_N - 1) hi = RAMP_N - 1;
  uint8_t col_lo = (uint8_t)(RAMP_LO + lo), col_hi = (uint8_t)(RAMP_LO + hi);

  /* long edge a->c spans the full height (one divide for its slope) */
  int dxac = ((cx - ax) << 16) / (cy - ay);
  int xac = ax << 16;

  if (by > ay) { /* top half: short edge a->b */
    int dxab = ((bx - ax) << 16) / (by - ay);
    int xab = ax << 16;
    for (int y = ay; y < by; y++) {
      if (y >= 0 && y < FB_CHUNKED_H)
        fill_span(y, xac >> 16, xab >> 16, frac, col_lo, col_hi);
      xac += dxac;
      xab += dxab;
    }
  }
  if (cy > by) { /* bottom half: short edge b->c */
    int dxbc = ((cx - bx) << 16) / (cy - by);
    int xbc = bx << 16;
    for (int y = by; y < cy; y++) {
      if (y >= 0 && y < FB_CHUNKED_H)
        fill_span(y, xac >> 16, xbc >> 16, frac, col_lo, col_hi);
      xac += dxac;
      xbc += dxbc;
    }
  }
}

static void __not_in_flash_func(solid_init)(void) {
  palette_set(solid_palette);
  s_ax = 0;
  s_ay = 0;
  s_az = 0;
  s_zoom = 0;
  s_ballx = 0;
  s_balldir = 1;
  s_bally = 0; /* start on the floor, launching upward */
  s_ballvy = BOUNCE_VY;
  s_prev_draw_us = 0;
  s_prev_cv_us = 0;
  font_set_font(&font8x8);
  font_set_border(0, 0);
}

static void __not_in_flash_func(solid_teardown)(void) {
  palette_init();
}

static void __not_in_flash_func(solid_render_frame)(void) {
  uint32_t t0 = time_us_32();

  /* Combined rotation matrix Rz*Ry*Rx, elements 8.8. */
  int cx = COS(s_ax), sx = SIN(s_ax);
  int cy = COS(s_ay), sy = SIN(s_ay);
  int cz = COS(s_az), sz = SIN(s_az);
  int m00 = (cy * cz) >> 8;
  int m01 = (((sx * sy >> 8) * cz) >> 8) - ((cx * sz) >> 8);
  int m02 = (((cx * sy >> 8) * cz) >> 8) + ((sx * sz) >> 8);
  int m10 = (cy * sz) >> 8;
  int m11 = (((sx * sy >> 8) * sz) >> 8) + ((cx * cz) >> 8);
  int m12 = (((cx * sy >> 8) * sz) >> 8) - ((sx * cz) >> 8);
  int m20 = -sy;
  int m21 = (sx * cy) >> 8;
  int m22 = (cx * cy) >> 8;

  /* Pulsing zoom + horizontal bounce: animate the projection scale and
   * the on-screen centre. */
  int fov = FOV + ((sin256[s_zoom] * ZOOM_AMP) >> 8);
  int cxc = CXp + (s_ballx >> 8);
  /* Seat the ball's centre so its bottom pixel lands on the floor row
   * (== screen bottom) at rest, then lift it by the bounce height. The
   * floor stays pinned to the screen bottom at every zoom level. */
  int cyc = (FLOOR_SCREEN - BALL_R(fov)) - (s_bally >> 8);

  for (int i = 0; i < SOLID_NV; i++) {
    int x = solid_vx[i][0], y = solid_vx[i][1], z = solid_vx[i][2];
    int tx = (m00 * x + m01 * y + m02 * z) >> 8;
    int ty = (m10 * x + m11 * y + m12 * z) >> 8;
    int tz = (m20 * x + m21 * y + m22 * z) >> 8;
    int zd = tz + ZDIST;
    /* One reciprocal (fov/zd in 16.16) shared by x and y, so each vertex
     * costs a single divide + two single-cycle multiplies instead of two
     * divides. zd >= ZDIST - radius (~680) so no divide-by-zero/overflow. */
    int inv = (fov << 16) / zd;
    s_tx[i] = tx;
    s_ty[i] = ty;
    s_tz[i] = tz;
    s_px[i] = cxc + ((tx * inv + 0x8000) >> 16);
    s_py[i] = cyc - ((ty * inv + 0x8000) >> 16);
  }

  /* Boing-style grid backdrop: magenta lines (idx 1) on a grey
   * background (idx 0, == border). The cell size scales with the same
   * fov as the ball (grid zooms in/out in sync) and is centred on the
   * screen so it zooms about the middle.
   *
   * The cell size is kept in 8.8 fixed point and each line is positioned
   * as centre + k*step (rounded to the nearest pixel) rather than on an
   * integer grid stride. So as the step changes fractionally each frame,
   * the outer lines cross a pixel boundary on their own frame instead of
   * the whole grid snapping when an integer stride ticks -- the zoom
   * reads smooth every frame. Ball overdraws it. */
  int gstep = (GRID_STEP * fov << 8) / FOV; /* 8.8 cell size */
  if (gstep < (4 << 8)) gstep = 4 << 8;
  uint8_t *fb = fb_chunked_buffer;
  memset(fb, 0, (size_t)FB_CHUNKED_W * FB_CHUNKED_H);
  for (int k = 0;; k++) { /* horizontal lines, anchored at the floor */
    int yk = FLOOR_SCREEN - ((k * gstep + 128) >> 8);
    if (yk < 0) break;
    memset(fb + (size_t)yk * FB_CHUNKED_W, 1, FB_CHUNKED_W);
  }
  for (int k = 0;; k++) { /* vertical lines, symmetric about CXp */
    int off = (k * gstep + 128) >> 8;
    int xp = CXp + off, xn = CXp - off;
    if (xp >= 0 && xp < FB_CHUNKED_W) {
      uint8_t *p = fb + xp;
      for (int y = 0; y < FB_CHUNKED_H; y++, p += FB_CHUNKED_W) *p = 1;
    }
    if (k && xn >= 0 && xn < FB_CHUNKED_W) {
      uint8_t *p = fb + xn;
      for (int y = 0; y < FB_CHUNKED_H; y++, p += FB_CHUNKED_W) *p = 1;
    }
    if (xp >= FB_CHUNKED_W && xn < 0) break;
  }

  /* Backface-cull, then flat-fill each visible triangle. For a convex
   * solid the front faces tile the silhouette exactly, so no painter's
   * sort or z-buffer is needed once back faces are culled. */
  for (int f = 0; f < SOLID_NF; f++) {
    const uint16_t *fv = solid_face[f];
    int e1x = s_tx[fv[1]] - s_tx[fv[0]], e1y = s_ty[fv[1]] - s_ty[fv[0]],
        e1z = s_tz[fv[1]] - s_tz[fv[0]];
    int e2x = s_tx[fv[2]] - s_tx[fv[0]], e2y = s_ty[fv[2]] - s_ty[fv[0]],
        e2z = s_tz[fv[2]] - s_tz[fv[0]];
    int fcx = e1y * e2z - e1z * e2y; /* outward normal (full precision) */
    int fcy = e1z * e2x - e1x * e2z;
    int fcz = e1x * e2y - e1y * e2x;
    int ccx = (s_tx[fv[0]] + s_tx[fv[1]] + s_tx[fv[2]]) / 3;
    int ccy = (s_ty[fv[0]] + s_ty[fv[1]] + s_ty[fv[2]]) / 3;
    int ccz = (s_tz[fv[0]] + s_tz[fv[1]] + s_tz[fv[2]]) / 3;
    /* normal . (face_centre - camera): >= 0 => faces away => cull. */
    if ((fcx >> 4) * ccx + (fcy >> 4) * ccy + (fcz >> 4) * (ccz + ZDIST) >= 0) {
      continue;
    }
    /* Flat per-facet intensity (facet stays a distinct tone so rotation
     * reads), 8.8 of the ramp; the dither in fill_tri only softens the
     * step between facets, it does not blur the facet itself. */
    int fiv = ((ccx * LX + ccy * LY + ccz * LZ) >> SHADE_SHIFT) + SHADE_BIAS;
    if (fiv < 0) fiv = 0;
    if (fiv > (RAMP_N << 8) - 1) fiv = (RAMP_N << 8) - 1;
    int px[3], py[3];
    for (int k = 0; k < 3; k++) {
      px[k] = s_px[fv[k]];
      py[k] = s_py[fv[k]];
    }
    fill_tri(px, py, fiv);
  }

  /* HUD. */
  char num[11];
  font_set_color(COL_TEXT);
  font_align(FONT_ALIGN_LEFT);
  font_move(8, 6);
  font_print("3D SOLID   ESC=MENU");
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
  font_move(8, 36);
  font_print("FACES ");
  font_print(u32str(SOLID_NF, num, sizeof(num)));

  uint32_t draw_us = time_us_32() - t0;
  fb_publish();
  s_prev_draw_us = draw_us;
  s_prev_cv_us = fb_last_convert_us();

  s_ax += 1;
  s_ay += 2;
  s_az += 1;
  s_zoom += ZOOM_SPEED;

  /* Slow horizontal bounce: travel until a limit, then reverse. */
  s_ballx += BOUNCE_SPEED * s_balldir;
  if (s_ballx > BOUNCE_LIMIT) {
    s_ballx = BOUNCE_LIMIT;
    s_balldir = -1;
  } else if (s_ballx < -BOUNCE_LIMIT) {
    s_ballx = -BOUNCE_LIMIT;
    s_balldir = 1;
  }

  /* Vertical gravity bounce: rise, decelerate, fall; on touching the
   * floor (height 0) relaunch upward at a fixed speed (constant apex). */
  s_ballvy -= GRAVITY;
  s_bally += s_ballvy;
  if (s_bally <= 0) {
    s_bally = 0;
    s_ballvy = BOUNCE_VY;
  }
}

const demo_module_t demo_3d = {
    .name = "3d",
    .init = solid_init,
    .render_frame = solid_render_frame,
    .handle_key = NULL,
    .teardown = solid_teardown,
};
