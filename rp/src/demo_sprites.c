/**
 * File: demo_sprites.c
 * Description: multi-sprite swarm demo.
 *
 * Ported from the "pico-vga-6bit-demo" by Ricardo Massaro
 * (https://github.com/moefh/pico-vga-6bit-demo, MIT License,
 * Copyright (c) 2021 Ricardo Massaro): a swarm of bouncing "loserboy"
 * characters walking over a tiled background, occasionally shouting a
 * message, with an FPS counter.
 *
 * Adapted to this template: the VGA driver, double-buffering and the
 * word-packed software blitter are dropped; sprites are byte-per-pixel
 * (sprites_data.h) drawn into our chunked framebuffer with fb_blit /
 * fb_blit_key, text uses our fb_font, the palette is reduced to 16
 * Atari ST colours, and the screen is 320x200. ESC returns to the menu
 * (handled by the dispatcher).
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
#include "sprites_data.h"

#pragma GCC optimize("O3")

extern const struct FB_FONT font8x8;

#define MAX_SPRITES 200      /* ceiling for the swarm array */
#define ADD_INTERVAL_US 3000000u /* add one sprite every 3 s */
#define FRAME_BUDGET_US 19500    /* stop growing once DRAW+C2P exceeds this */
#define COL_TEXT 3               /* white in sprites_palette */

/* loserboy frame layout (from the original demo). */
#define LB_STAND_FRAME 10
#define LB_MIRROR_START 11
#define LB_WALK_DELAY 4
static const unsigned int lb_walk_cycle[] = {
    5, 6, 7, 8, 9, 8, 7, 6, 5, 0, 1, 2, 3, 4, 3, 2, 1, 0,
};

/* 5x4 background tile map (indexes into tiles_frames[]). */
static const uint8_t bg_map[20] = {
    0, 0, 0, 0, 0,  //
    0, 0, 1, 0, 0,  //
    0, 1, 0, 1, 0,  //
    0, 0, 0, 0, 0,  //
};

static const char *lb_messages[] = {
    "I'LL GET YOU!", "COME BACK HERE!", "AYEEEEE!",
    "YOU CANT ESCAPE!", "TAKE THIS!",
};

struct CHARACTER {
  const struct FB_BITMAP *sprite;
  int message_index;
  int x, y, dx, dy;
  int frame;
  int message_frame;
};

static struct CHARACTER s_chars[MAX_SPRITES];
static uint32_t s_rng = 0x1234567u;
static uint32_t s_prev_draw_us;
static uint32_t s_prev_cv_us;
static int s_active;         /* sprites currently shown (starts at 1) */
static uint32_t s_add_at_us; /* absolute time of the next sprite add */
static int s_capped;         /* latched once DRAW+C2P exceeds the budget */

static uint32_t __not_in_flash_func(rng)(void) {
  uint32_t x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rng = x;
  return x;
}

/* hand-rolled unsigned->decimal (fb_font has no printf). */
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

static void __not_in_flash_func(init_characters)(void) {
  for (int i = 0; i < MAX_SPRITES; i++) {
    struct CHARACTER *ch = &s_chars[i];
    ch->x = (int)(rng() % (FB_CHUNKED_W - SPRITES_LB_W));
    ch->y = (int)(rng() % (FB_CHUNKED_H - SPRITES_LB_H));
    ch->dx = (int)(1 + rng() % 3) * ((rng() & 1) ? -1 : 1);
    ch->dy = (int)(1 + rng() % 2) * ((rng() & 1) ? -1 : 1);
    ch->frame = i + i * LB_WALK_DELAY;
    ch->sprite = &loserboy_frames[LB_STAND_FRAME];
    ch->message_index = -1;
    ch->message_frame = -1;
  }
}

static void __not_in_flash_func(move_character)(struct CHARACTER *ch) {
  if (ch->message_frame-- < 0) {
    ch->message_index = -1;
    ch->message_frame = (int)(600 + rng() % 1200);
  } else if (ch->message_frame == 180) {
    ch->message_index = (int)(rng() % count_of(lb_messages));
  }

  if (ch->message_frame > 1500) {
    ch->sprite =
        &loserboy_frames[LB_STAND_FRAME + ((ch->dx < 0) ? LB_MIRROR_START : 0)];
  } else {
    ch->x += ch->dx;
    if (ch->x < -SPRITES_LB_W / 2) ch->dx = (int)(1 + rng() % 3);
    if (ch->x >= FB_CHUNKED_W - SPRITES_LB_W / 2)
      ch->dx = -(int)(1 + rng() % 3);

    ch->y += ch->dy;
    if (ch->y < -SPRITES_LB_H / 2) ch->dy = (int)(1 + rng() % 2);
    if (ch->y >= FB_CHUNKED_H - SPRITES_LB_H / 2)
      ch->dy = -(int)(1 + rng() % 2);

    ch->frame++;
    if (ch->frame / LB_WALK_DELAY >= (int)count_of(lb_walk_cycle)) ch->frame = 0;
    int frame_num = (int)lb_walk_cycle[ch->frame / LB_WALK_DELAY];
    ch->sprite =
        &loserboy_frames[frame_num + ((ch->dx < 0) ? LB_MIRROR_START : 0)];
  }
}

static void __not_in_flash_func(sprites_init)(void) {
  palette_set(sprites_palette);
  s_rng = time_us_32() | 1u;
  s_prev_draw_us = 0;
  s_prev_cv_us = 0;
  font_set_font(&font8x8);
  font_set_border(0, 0);
  init_characters();
  s_active = 1;
  s_capped = 0;
  s_add_at_us = time_us_32() + ADD_INTERVAL_US;
}

static void __not_in_flash_func(sprites_teardown)(void) {
  palette_init(); /* restore the template default palette */
}

/* Render the background tiles + all sprites, clipped to rows [y0, y1).
 * Called on BOTH cores with disjoint bands (dual-core): the
 * tiles fully cover each band (so they double as the per-band clear) and
 * the bands never overlap, so the two cores never write the same pixel.
 * Reads only -- positions were settled by move_character on Core 0 before
 * dispatch, so there is no shared-write race. */
static void __not_in_flash_func(render_band)(int y0, int y1) {
  for (int ty = 0; ty < 4; ty++) {
    for (int tx = 0; tx < 5; tx++) {
      fb_blit_band(&tiles_frames[bg_map[ty * 5 + tx]], tx * SPRITES_TILE_W,
                   ty * SPRITES_TILE_H, y0, y1);
    }
  }
  for (int i = 0; i < s_active; i++) {
    struct CHARACTER *ch = &s_chars[i];
    fb_blit_key_band(ch->sprite, ch->x, ch->y, SPRITES_KEY, y0, y1);
  }
}

/* Core 1 job: the bottom half-screen band. */
static void __not_in_flash_func(render_bottom_band_job)(void *arg) {
  (void)arg;
  render_band(FB_CHUNKED_H / 2, FB_CHUNKED_H);
}

static void __not_in_flash_func(sprites_render_frame)(void) {
  uint32_t t0 = time_us_32();

  /* Grow the swarm by one sprite every ADD_INTERVAL_US, until a frame's
   * DRAW+C2P no longer fits the budget (s_capped latches below) or the
   * array is full. */
  if (!s_capped && s_active < MAX_SPRITES && t0 >= s_add_at_us) {
    s_active++;
    s_add_at_us = t0 + ADD_INTERVAL_US;
  }

  for (int i = 0; i < s_active; i++) {
    move_character(&s_chars[i]);
  }

  /* Find the last active speech bubble (logic only; the bubble is printed
   * on Core 0 after the bands join, below). */
  int msg_index = -1, msg_x = 0, msg_y = 0;
  for (int i = 0; i < s_active; i++) {
    struct CHARACTER *ch = &s_chars[i];
    if (ch->message_index >= 0) {
      msg_x = ch->x + SPRITES_LB_W / 2;
      msg_y = ch->y - 10;
      msg_index = ch->message_index;
    }
  }

  /* Dual-core: Core 1 draws the bottom half-screen band while Core 0
   * draws the top. Disjoint bands => no overlap, no race. */
  fb_core1_dispatch(render_bottom_band_job, NULL);
  render_band(0, FB_CHUNKED_H / 2);
  fb_core1_wait();

  /* Speech bubble + HUD drawn on top, on Core 0 (both bands now done). */
  font_set_color(COL_TEXT);
  if (msg_index >= 0) {
    font_align(FONT_ALIGN_CENTER);
    font_move(msg_x, msg_y);
    font_print(lb_messages[msg_index]);
  }

  /* HUD: sprite count + previous frame's DRAW / C2P microseconds. */
  char num[11];
  font_align(FONT_ALIGN_LEFT);
  font_move(8, 6);
  font_print("SPRITES ");
  font_print(u32str((uint32_t)s_active, num, sizeof(num)));
  font_print(s_capped ? " MAX   ESC=MENU" : "   ESC=MENU");
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

  /* Latch the cap once this frame's DRAW+C2P blew the budget, so we
   * stop adding sprites (the current count stays on screen). */
  if (draw_us + s_prev_cv_us > FRAME_BUDGET_US) {
    s_capped = 1;
  }
}

const demo_module_t demo_sprites = {
    .name = "sprites",
    .init = sprites_init,
    .render_frame = sprites_render_frame,
    .handle_key = NULL,
    .teardown = sprites_teardown,
};
