/**
 * File: game_arena.c
 * Description: Arena, the joystick game (games.h): steer with joystick 1 or
 *              the cursor keys, grab the gems, shoot the enemies, stay away
 *              from them.
 */

#include <stdbool.h>
#include <stdint.h>

#include "art_bolt.h"
#include "art_enemy.h"
#include "art_gem.h"
#include "art_player.h"
#include "fb.h"
#include "game_kit.h"
#include "games.h"
#include "ikbd.h"
#include "pico/time.h"

#define START_ENEMIES 3
#define MAX_ENEMIES 32
#define START_LIVES 3
#define HURT_FRAMES 100 /* 2 s blinking, untouchable, after a hit */
#define SHOT_AWAY 150   /* an enemy shot comes back 3 s later */

/* IKBD scancodes. */
#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_SPACE 0x39
#define SC_D 0x20

static kit_actor_t hero, gem_at, shot, enemies[MAX_ENEMIES];
static int n_enemies, enemy_size = 16, score, best, lives, hurt, gems;
static int face_x = 1, face_y; /* where the hero last moved: where it shoots */
static bool shooting, game_over, hud_dirty, fire_key;
static bool frozen, redraw_each_second;
static uint32_t frame, frames_shown, draw_us;
static bool held[128]; /* keys down now, by scancode */

#if defined(_DEBUG) && (_DEBUG != 0)
/* Debug builds: knobs to vary the scene from the keyboard or a script
 * (tools/dev/swd.py key): keypad +/- enemies, S their size (16 / 32), F
 * freezes everything, R redraws the whole screen once, A every second. */
#define KNOBS 1
static uint8_t big_pixels[ENEMY_COUNT][32 * 32];
static struct FB_BITMAP big[ENEMY_COUNT]; /* the enemy, pixels doubled */
#endif

static void add_enemy(void) {
  if (n_enemies == MAX_ENEMIES) return;
  kit_actor_t *e = &enemies[n_enemies++];
  int speed = 1 + (score >= 100) + (score >= 250);
  kit_place(e, enemy_size, &hero, 64);
  e->dx = (kit_rnd() & 1) ? speed : -speed;
  e->dy = (kit_rnd() & 1) ? 1 : -1;
  e->away = 0;
}

static void new_round(void) {
  score = gems = hurt = 0;
  lives = START_LIVES;
  shooting = game_over = false;
  hero = (kit_actor_t){(KIT_COLS * KIT_TILE - 16) / 2,
                       KIT_HUD_H + (KIT_ROWS * KIT_TILE - 16) / 2};
  n_enemies = 0;
  for (int i = 0; i < START_ENEMIES; i++) add_enemy();
  kit_place(&gem_at, 16, &hero, 48);
  kit_draw_arena();
  hud_dirty = true;
}

void arena_start(void) {
  kit_start();
  ikbd_set_input_mode(IKBD_INPUT_JOYSTICKS); /* stick 1; port 0 ignored */
#ifdef KNOBS
  for (int f = 0; f < ENEMY_COUNT; f++) {
    for (int i = 0; i < 32 * 32; i++) {
      big_pixels[f][i] = enemy[f].data[(i / 64) * 16 + (i % 32) / 2];
    }
    big[f] = (struct FB_BITMAP){32, 32, big_pixels[f]};
  }
#endif
  new_round();
}

void arena_key(const ikbd_key_event_t *k) {
  /* The IKBD reports a press and a release, never a repeat: the game keeps
   * which keys are down itself. */
  held[k->scancode] = k->is_press;
  if (!k->is_press) return;
  if (k->scancode == SC_SPACE) fire_key = true;
  if (k->scancode == SC_D) kit_show_timing = !kit_show_timing;
#ifdef KNOBS
  if (k->scancode == 0x4E) add_enemy();                  /* keypad + */
  if (k->scancode == 0x4A && n_enemies > 1) n_enemies--; /* keypad - */
  if (k->scancode == 0x1F) enemy_size = 48 - enemy_size; /* S */
  if (k->scancode == 0x21) frozen = !frozen;             /* F */
  if (k->scancode == 0x13) kit_draw_arena();             /* R */
  if (k->scancode == 0x1E) redraw_each_second = !redraw_each_second; /* A */
#endif
}

/* One step of the game: movement, shots, collisions. */
static void update(int dx, int dy, bool fire) {
  if (game_over) {
    if (fire) new_round();
    return;
  }
  hero.x += dx * 2;
  hero.y += dy * 2;
  hero.x = hero.x < KIT_MIN_X ? KIT_MIN_X
           : hero.x > KIT_MAX_X - 16 ? KIT_MAX_X - 16 : hero.x;
  hero.y = hero.y < KIT_MIN_Y ? KIT_MIN_Y
           : hero.y > KIT_MAX_Y - 16 ? KIT_MAX_Y - 16 : hero.y;
  if (dx || dy) {
    face_x = dx;
    face_y = dy;
  }
  if (fire && !shooting) { /* one shot on screen at a time */
    shooting = true;
    shot = (kit_actor_t){hero.x + 4, hero.y + 4, face_x * 5, face_y * 5, 0};
    kit_sfx(false, 1200, 400, 60);
  }
  if (shooting) {
    shot.x += shot.dx;
    shot.y += shot.dy;
    shooting = shot.x >= KIT_MIN_X && shot.x <= KIT_MAX_X - 8 &&
               shot.y >= KIT_MIN_Y && shot.y <= KIT_MAX_Y - 8;
  }
  for (int i = 0; i < n_enemies; i++) {
    kit_actor_t *e = &enemies[i];
    if (e->away) { /* shot: back somewhere else when the time is up */
      if (--e->away == 0) kit_place(e, enemy_size, &hero, 64);
      continue;
    }
    kit_move(e, enemy_size);
    if (shooting && kit_touch(&shot, 8, e, enemy_size)) {
      shooting = false;
      e->away = SHOT_AWAY;
      score += 5;
      hud_dirty = true;
      kit_sfx(true, 1500, 100, 150);
    } else if (!hurt && !game_over && kit_touch(&hero, 16, e, enemy_size)) {
      hurt = HURT_FRAMES;
      hud_dirty = true;
      kit_sfx(true, 3000, 200, 400);
      if (--lives == 0) {
        game_over = true;
        hurt = 0; /* the hero stays visible */
      }
    }
  }
  if (hurt) hurt--;
  if (kit_touch(&hero, 16, &gem_at, 16)) {
    score += 10;
    hud_dirty = true;
    kit_sfx(false, 800, 1600, 120);
    kit_place(&gem_at, 16, &hero, 48);
    if (++gems % 5 == 0) add_enemy();
  }
  if (score > best) best = score;
}

static void draw_hud(void) {
  kit_hud_clear();
  kit_print(0, 0, "SCORE");
  kit_print(48, 0, kit_num((uint32_t)score));
  kit_print(128, 0, "LIVES");
  kit_print(176, 0, kit_num((uint32_t)lives));
  kit_print(224, 0, "BEST");
  kit_print(264, 0, kit_num((uint32_t)best));
  hud_dirty = false;
}

void arena_frame(void) {
  /* Input: joystick 1 or the keys held; fire is a press since the last
   * frame (the joystick's latch, or a space press), never missed. */
  ikbd_joystick_t j;
  ikbd_read_joystick(1, &j);
  int dx = ((j.state & IKBD_JOY_RIGHT) || held[SC_RIGHT]) -
           ((j.state & IKBD_JOY_LEFT) || held[SC_LEFT]);
  int dy = ((j.state & IKBD_JOY_DOWN) || held[SC_DOWN]) -
           ((j.state & IKBD_JOY_UP) || held[SC_UP]);
  bool fire = (j.pressed & IKBD_JOY_FIRE) || fire_key;
  fire_key = false;
  if (!frozen) {
    update(dx, dy, fire);
    frame++; /* the animations' clock */
  }
  if (redraw_each_second && ++frames_shown % 50 == 0) kit_draw_arena();

  uint32_t t0 = time_us_32();
  kit_begin_frame();
  kit_sprite(&gem[(frame / 16) & 1], gem_at.x, gem_at.y);
  for (int i = 0; i < n_enemies; i++) {
    if (enemies[i].away) continue;
    const struct FB_BITMAP *bm = &enemy[(frame / 12 + i) & 1];
#ifdef KNOBS
    if (enemy_size == 32) bm = &big[(frame / 12 + i) & 1];
#endif
    kit_sprite(bm, enemies[i].x, enemies[i].y);
  }
  if (shooting) kit_sprite(&bolt, shot.x, shot.y);
  if (!(hurt & 4)) kit_sprite(&player, hero.x, hero.y); /* blinks if hurt */
  if (game_over) {
    kit_label(124, 92, "GAME OVER");
    kit_label(116, 108, "FIRE TO PLAY");
  }
  if (hud_dirty) draw_hud();
  char extra[24] = "";
#ifdef KNOBS
  const char *n = kit_num((uint32_t)n_enemies);
  int p = 0;
  extra[p++] = ' ';
  extra[p++] = 'E';
  while (*n) extra[p++] = *n++;
  for (const char *s = enemy_size == 32 ? " BIG" : ""; *s;) extra[p++] = *s++;
  for (const char *s = frozen ? " FROZEN" : ""; *s;) extra[p++] = *s++;
  extra[p] = '\0';
#endif
  kit_readout(draw_us, extra);
  draw_us = time_us_32() - t0;
  fb_publish();
}
