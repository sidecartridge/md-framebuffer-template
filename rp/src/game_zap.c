/**
 * File: game_zap.c
 * Description: Zap, the mouse game (games.h): enemies and gems wander the
 *              arena; the left button zaps an enemy under the crosshair, the
 *              right one takes a gem, for sixty seconds.
 */

#include <stdbool.h>
#include <stdint.h>

#include "art_crosshair.h"
#include "art_enemy.h"
#include "art_gem.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_font.h"
#include "game_kit.h"
#include "games.h"
#include "ikbd.h"
#include "pico/time.h"

#define ENEMIES 4
#define GEMS 3
#define ROUND_S 60
#define SC_D 0x20

static kit_actor_t enemies[ENEMIES], gems[GEMS], aim; /* aim: the crosshair */
static int score, best, seconds_left = ROUND_S;
static bool playing, hud_dirty;
static uint8_t buttons_shown = 0xFF; /* the buttons the HUD shows held */
static uint32_t round_end_us, frame, draw_us;

static void wander(kit_actor_t *a) {
  kit_place(a, 16, &aim, 48);
  a->dx = (kit_rnd() & 1) ? 1 : -1;
  a->dy = (kit_rnd() & 1) ? 1 : -1;
}

void zap_start(void) {
  kit_start();
  ikbd_set_input_mode(IKBD_INPUT_MOUSE);
  aim = (kit_actor_t){KIT_COLS * KIT_TILE / 2, KIT_HUD_H + KIT_ROWS * KIT_TILE / 2};
  for (int i = 0; i < ENEMIES; i++) wander(&enemies[i]);
  for (int i = 0; i < GEMS; i++) wander(&gems[i]);
  playing = false;
  hud_dirty = true;
}

void zap_key(const ikbd_key_event_t *k) {
  if (k->is_press && k->scancode == SC_D) kit_show_timing = !kit_show_timing;
}

/* The target under the crosshair's centre, or NULL. */
static kit_actor_t *under_aim(kit_actor_t *list, int n) {
  for (int i = 0; i < n; i++) {
    if (aim.x >= list[i].x && aim.x < list[i].x + 16 && aim.y >= list[i].y &&
        aim.y < list[i].y + 16) {
      return &list[i];
    }
  }
  return NULL;
}

/* A click: the right button on the right target scores and sends it
 * elsewhere, the wrong one costs points. */
static void click(bool left) {
  kit_actor_t *enemy_hit = under_aim(enemies, ENEMIES);
  kit_actor_t *gem_hit = under_aim(gems, GEMS);
  kit_actor_t *good = left ? enemy_hit : gem_hit;
  if (good) {
    score += 10;
    kit_sfx(left, left ? 1500 : 800, left ? 100 : 1600, 120);
    wander(good);
  } else if (enemy_hit || gem_hit) {
    score = score >= 5 ? score - 5 : 0;
    kit_sfx(false, 150, 100, 200); /* a low buzz */
  } else {
    kit_sfx(false, 2000, 1800, 20); /* a click on the floor */
  }
  hud_dirty = true;
}

static void draw_hud(uint8_t buttons) {
  kit_hud_clear();
  kit_print(0, 0, "SCORE");
  kit_print(48, 0, kit_num((uint32_t)score));
  kit_print(96, 0, "TIME");
  kit_print(136, 0, kit_num((uint32_t)seconds_left));
  kit_print(168, 0, "BEST");
  kit_print(208, 0, kit_num((uint32_t)best));
  /* The buttons held now, lit: the mouse test. */
  static const struct {
    uint8_t bit;
    int x;
    const char *name;
  } lamps[] = {{IKBD_MOUSE_LEFT, 288, "L"}, {IKBD_MOUSE_RIGHT, 304, "R"}};
  for (int i = 0; i < 2; i++) {
    bool on = buttons & lamps[i].bit;
    fb_fill_rect(lamps[i].x, 0, 8, KIT_HUD_H, on ? KIT_WHITE : KIT_BLACK);
    font_set_color(on ? KIT_BLACK : KIT_WHITE);
    kit_print(lamps[i].x, 0, lamps[i].name);
  }
  font_set_color(KIT_WHITE);
  buttons_shown = buttons;
  hud_dirty = false;
}

void zap_frame(void) {
  /* The mouse: the movement since the last frame moves the crosshair; a
   * click is a press since the last frame, never missed. */
  ikbd_mouse_t m;
  ikbd_read_mouse(&m);
  aim.x += m.dx;
  aim.y += m.dy;
  aim.x = aim.x < KIT_MIN_X ? KIT_MIN_X : aim.x >= KIT_MAX_X ? KIT_MAX_X - 1 : aim.x;
  aim.y = aim.y < KIT_MIN_Y ? KIT_MIN_Y : aim.y >= KIT_MAX_Y ? KIT_MAX_Y - 1 : aim.y;

  if (!playing) {
    if (m.pressed) { /* either button starts a round */
      playing = true;
      score = 0;
      round_end_us = time_us_32() + ROUND_S * 1000000u;
    }
  } else {
    for (int i = 0; i < ENEMIES; i++) kit_move(&enemies[i], 16);
    for (int i = 0; i < GEMS; i++) kit_move(&gems[i], 16);
    if (m.pressed & IKBD_MOUSE_LEFT) click(true);
    if (m.pressed & IKBD_MOUSE_RIGHT) click(false);
    /* Time, not frames: a mouse moved fast costs the ST frames. */
    int32_t left_us = (int32_t)(round_end_us - time_us_32());
    int s = left_us > 0 ? (int)((left_us + 999999) / 1000000) : 0;
    if (s != seconds_left) hud_dirty = true;
    seconds_left = s;
    if (s == 0) {
      playing = false;
      if (score > best) best = score;
      kit_sfx(true, 400, 60, 500);
    }
  }
  frame++;

  uint32_t t0 = time_us_32();
  kit_begin_frame();
  for (int i = 0; i < GEMS; i++) {
    kit_sprite(&gem[(frame / 16 + i) & 1], gems[i].x, gems[i].y);
  }
  for (int i = 0; i < ENEMIES; i++) {
    kit_sprite(&enemy[(frame / 12 + i) & 1], enemies[i].x, enemies[i].y);
  }
  if (!playing) {
    kit_label(108, 76, "CLICK TO PLAY");
    kit_label(28, 100, "LEFT ZAPS REDS, RIGHT TAKES GEMS");
  }
  kit_sprite(&crosshair, aim.x - 8, aim.y - 8);
  if (hud_dirty || m.buttons != buttons_shown) draw_hud(m.buttons);
  kit_readout(draw_us, "");
  draw_us = time_us_32() - t0;
  fb_publish();
}
