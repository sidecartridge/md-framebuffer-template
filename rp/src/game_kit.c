/**
 * File: game_kit.c
 * Description: What the two games share; see game_kit.h.
 */

#include "game_kit.h"

#include <stdlib.h>

#include "art_tiles.h"
#include "audio.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

bool kit_show_timing;

/* What this frame drew over the floor, restored by the next one. */
typedef struct {
  int x, y, w, h;
} rect_t;
#define MAX_DRAWN 48
static rect_t drawn[MAX_DRAWN];
static int n_drawn;
static uint32_t rng = 0x2545F491u;

static void sound_cb(int8_t *buf, uint32_t samples);

uint32_t kit_rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

const char *kit_num(uint32_t n) {
  static char buf[11];
  char *p = buf + sizeof(buf);
  *--p = '\0';
  do {
    *--p = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  return p;
}

/* --- the screen --- */

static const struct FB_BITMAP *tile_at(int col, int row) {
  if (col == 0 || row == 0 || col == KIT_COLS - 1 || row == KIT_ROWS - 1) {
    return &tiles[2]; /* wall */
  }
  return &tiles[(col * 7 + row * 3) % 5 == 0]; /* floor, two kinds */
}

/* The tiles under a rectangle of the arena, drawn again. */
static void restore(rect_t r) {
  for (int row = (r.y - KIT_HUD_H) / KIT_TILE;
       row <= (r.y + r.h - 1 - KIT_HUD_H) / KIT_TILE; row++) {
    for (int col = r.x / KIT_TILE; col <= (r.x + r.w - 1) / KIT_TILE; col++) {
      fb_blit(tile_at(col, row), col * KIT_TILE, KIT_HUD_H + row * KIT_TILE);
    }
  }
}

static void remember(int x, int y, int w, int h) {
  if (n_drawn < MAX_DRAWN) drawn[n_drawn++] = (rect_t){x, y, w, h};
}

void kit_draw_arena(void) {
  restore((rect_t){0, KIT_HUD_H, KIT_COLS * KIT_TILE, KIT_ROWS * KIT_TILE});
  n_drawn = 0;
}

void kit_begin_frame(void) {
  for (int i = 0; i < n_drawn; i++) restore(drawn[i]);
  n_drawn = 0;
}

void kit_sprite(const struct FB_BITMAP *bm, int x, int y) {
  fb_blit_key(bm, x, y, KIT_KEY);
  remember(x, y, bm->width, bm->height);
}

void kit_print(int x, int y, const char *s) {
  font_move((unsigned)x, (unsigned)y);
  font_print(s);
}

void kit_label(int x, int y, const char *s) {
  int w = 0;
  while (s[w]) w++;
  fb_fill_rect(x - 2, y - 2, w * 8 + 4, 12, KIT_BLACK);
  kit_print(x, y, s);
  remember(x - 2, y - 2, w * 8 + 4, 12);
}

void kit_hud_clear(void) {
  fb_fill_rect(0, 0, FB_CHUNKED_W, KIT_HUD_H, KIT_BLACK);
}

static char *append(char *p, const char *end, const char *s) {
  while (*s && p < end) *p++ = *s++;
  return p;
}

void kit_readout(uint32_t draw_us, const char *extra) {
  if (!kit_show_timing) return;
  char line[40], *p = line, *end = line + sizeof(line) - 1;
  p = append(p, end, "DRAW ");
  p = append(p, end, kit_num(draw_us));
  p = append(p, end, " C2P ");
  p = append(p, end, kit_num(fb_last_convert_us()));
  p = append(p, end, extra);
  *p = '\0';
  kit_label(8, KIT_MAX_Y + 4, line);
}

void kit_start(void) {
  palette_set(tiles_palette);
  font_set_font(&font8x8);
  font_set_color(KIT_WHITE);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  audio_set_pcm_callback(sound_cb, AUDIO_DMA_RATE_HZ);
  kit_draw_arena();
}

/* --- actors --- */

void kit_place(kit_actor_t *a, int size, const kit_actor_t *avoid, int away) {
  do {
    a->x = KIT_MIN_X + (int)(kit_rnd() % (unsigned)(KIT_MAX_X - KIT_MIN_X - size));
    a->y = KIT_MIN_Y + (int)(kit_rnd() % (unsigned)(KIT_MAX_Y - KIT_MIN_Y - size));
  } while (abs(a->x - avoid->x) < away && abs(a->y - avoid->y) < away);
}

void kit_move(kit_actor_t *a, int size) {
  a->x += a->dx;
  a->y += a->dy;
  if (a->x < KIT_MIN_X) a->dx = abs(a->dx);
  if (a->x > KIT_MAX_X - size) a->dx = -abs(a->dx);
  if (a->y < KIT_MIN_Y) a->dy = abs(a->dy);
  if (a->y > KIT_MAX_Y - size) a->dy = -abs(a->dy);
}

bool kit_touch(const kit_actor_t *a, int as, const kit_actor_t *b, int bs) {
  const int m = 3; /* a few pixels of mercy */
  return a->x + m < b->x + bs && b->x + m < a->x + as &&
         a->y + m < b->y + bs && b->y + m < a->y + as;
}

/* --- sound --- */

/* A two-voice square-wave tune over a 16-step loop (Hz, 0 a rest) and the
 * effects, computed sample by sample as the sound plays: signed 8-bit PCM
 * at the DMA chip's rate, which the YM plays too. */
static const uint16_t lead[16] = {659, 0, 587, 523, 440, 0, 523, 587,
                                  659, 784, 659, 587, 523, 0, 440, 0};
static const uint16_t bass[16] = {110, 110, 0, 110, 87, 87, 0, 87,
                                  131, 131, 0, 131, 98, 98, 0, 98};
#define STEP (AUDIO_DMA_RATE_HZ / 7) /* samples a step: about 140 ms */
#define PHASE_PER_HZ (UINT32_MAX / AUDIO_DMA_RATE_HZ)
static uint32_t tune_pos, lead_phase, bass_phase, sfx_phase, sfx_inc;
static uint32_t noise = 1;
static int32_t sfx_slide; /* added to sfx_inc every sample */
static int sfx_left;      /* samples of the effect still to play */
static bool sfx_noise;

void kit_sfx(bool is_noise, int from_hz, int to_hz, int ms) {
  sfx_left = ms * (int)AUDIO_DMA_RATE_HZ / 1000;
  sfx_noise = is_noise;
  sfx_inc = (uint32_t)from_hz * PHASE_PER_HZ;
  sfx_slide = (to_hz - from_hz) * (int32_t)PHASE_PER_HZ / sfx_left;
}

static void sound_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    uint32_t step = tune_pos / STEP;
    int v = 0;
    if (lead[step] && tune_pos % STEP < STEP * 7 / 8) { /* staccato */
      lead_phase += lead[step] * PHASE_PER_HZ;
      v += (lead_phase >> 31) ? 18 : -18;
    }
    if (bass[step]) {
      bass_phase += bass[step] * PHASE_PER_HZ;
      v += (bass_phase >> 31) ? 22 : -22;
    }
    if (++tune_pos == 16 * STEP) tune_pos = 0;
    if (sfx_left > 0) {
      sfx_left--;
      uint32_t before = sfx_phase;
      sfx_phase += sfx_inc;
      sfx_inc += (uint32_t)sfx_slide;
      if (sfx_noise && sfx_phase < before) { /* a new noise level */
        noise = noise * 1103515245u + 12345u;
      }
      bool high = sfx_noise ? (noise >> 30) & 1 : sfx_phase >> 31;
      v += high ? 48 : -48;
    }
    buf[i] = (int8_t)v;
  }
}
