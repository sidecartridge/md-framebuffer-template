/**
 * File: game_kit.h
 * Description: What the two games (games.h) share, on the public APIs only:
 *              the screen they play on, sprites drawn over it and restored,
 *              the HUD, a tune and sound effects, random numbers.
 *
 * The screen is a HUD line on top, then an arena of 20 x 12 tiles of 16 x 16
 * pixels with a wall around it. Each frame draws again only what moved:
 * kit_begin_frame() puts the floor back under what the last frame drew with
 * kit_sprite() and kit_label(), then the game draws its sprites where they
 * are now. The rest of the screen stays as it was, so a frame costs a few
 * tile blits per sprite instead of the whole arena.
 *
 * The art is indexed PNGs in rp/src/assets sharing one 16-colour palette
 * (art_tiles.h's), converted by tools/png_to_bitmap.py; colour 15 is the
 * sprites' transparent key.
 */

#ifndef GAME_KIT_H
#define GAME_KIT_H

#include <stdbool.h>
#include <stdint.h>

#include "fb_blit.h"

#define KIT_TILE 16
#define KIT_HUD_H 8
#define KIT_COLS 20
#define KIT_ROWS 12
/* The floor inside the walls: where the actors move. */
#define KIT_MIN_X KIT_TILE
#define KIT_MIN_Y (KIT_HUD_H + KIT_TILE)
#define KIT_MAX_X ((KIT_COLS - 1) * KIT_TILE)
#define KIT_MAX_Y (KIT_HUD_H + (KIT_ROWS - 1) * KIT_TILE)

/* Colours of the shared palette. */
#define KIT_BLACK 0
#define KIT_WHITE 7
#define KIT_KEY 15 /* transparent in the sprites */

/* Something that moves: a position (its top-left corner), a speed in pixels
 * per frame, and how many frames it stays away when it has been hit. */
typedef struct {
  int x, y, dx, dy;
  int away;
} kit_actor_t;

/* Sets the palette, the font and the sound (the tune starts), and draws
 * the whole arena. A game calls it when it starts. */
void kit_start(void);

/* Draws the whole arena again (a new round); nothing is left to restore. */
void kit_draw_arena(void);

/* Puts the floor back under what the last frame drew. First thing of a
 * frame's drawing. */
void kit_begin_frame(void);

/* Draws a sprite (colour KIT_KEY transparent), restored next frame. */
void kit_sprite(const struct FB_BITMAP *bm, int x, int y);

/* White text on a black box, restored next frame. */
void kit_label(int x, int y, const char *s);

/* The HUD line: kit_hud_clear() blanks it, kit_print() writes on it (or
 * anywhere; the font is 8 x 8, white). */
void kit_hud_clear(void);
void kit_print(int x, int y, const char *s);

/* `n` in decimal, in a buffer the next call reuses. */
const char *kit_num(uint32_t n);

/* An actor somewhere on the floor, at least `away` pixels from `avoid`, and
 * one step of one, bouncing off the walls. `size` is its width and height. */
void kit_place(kit_actor_t *a, int size, const kit_actor_t *avoid, int away);
void kit_move(kit_actor_t *a, int size);

/* Two actors' boxes overlap, by more than a few pixels of mercy. */
bool kit_touch(const kit_actor_t *a, int as, const kit_actor_t *b, int bs);

/* A random number (xorshift32). */
uint32_t kit_rnd(void);

/* A sound effect over the tune: a square wave, or noise, sliding from one
 * pitch to the other. A new one replaces the one playing. */
void kit_sfx(bool noise, int from_hz, int to_hz, int ms);

/* The timing readout: when kit_show_timing is set, kit_readout() writes the
 * previous frame's drawing time and the last publish's conversion time (in
 * microseconds), then `extra`, on the bottom wall. The games toggle it with
 * the D key. */
extern bool kit_show_timing;
void kit_readout(uint32_t draw_us, const char *extra);

#endif /* GAME_KIT_H */
