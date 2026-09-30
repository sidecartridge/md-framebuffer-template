/**
 * File: games.h
 * Description: Two small games, written against the public APIs only
 *              (fb_*, font_*, palette_*, ikbd_*, audio_*): the reference for
 *              starting a game. Each is a menu entry of the demo firmware
 *              (demo_games.c) and runs on its own in examples/mini_game.
 *
 *   Arena (game_arena.c), the joystick: steer with joystick 1 or the cursor
 *   keys, fire (or space) shoots the way you last moved. Grab the gems, shoot
 *   the enemies, stay away from them: three lives.
 *
 *   Zap (game_zap.c), the mouse: enemies and gems wander the arena; the left
 *   button zaps an enemy under the crosshair, the right one takes a gem. The
 *   wrong button costs points. Sixty seconds a round; the HUD lights L and R
 *   while the buttons are held.
 *
 * For each: start() sets up the screen, the input mode and the sound and
 * starts a round; key() takes every key event (ikbd_pop_key()); frame()
 * plays and draws one frame and ends with fb_publish(). D toggles the timing
 * readout (game_kit.h).
 */

#ifndef GAMES_H
#define GAMES_H

#include "ikbd.h"

void arena_start(void);
void arena_key(const ikbd_key_event_t *k);
void arena_frame(void);

void zap_start(void);
void zap_key(const ikbd_key_event_t *k);
void zap_frame(void);

#endif /* GAMES_H */
