/**
 * File: demo_games.c
 * Description: The two games (games.h) as entries of the demo menu. The
 *              dispatcher keeps ESC (back to the menu) and D (the timing
 *              readout, which the games then follow); leaving a game gives
 *              the menu back its keyboard mode and its music.
 */

#include "demo.h"
#include "game_kit.h"
#include "games.h"
#include "ikbd.h"

static void games_leave(void) {
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  demo_menu_music();
}

static void arena_menu_frame(void) {
  kit_show_timing = g_show_timing;
  arena_frame();
}

static void zap_menu_frame(void) {
  kit_show_timing = g_show_timing;
  zap_frame();
}

const demo_module_t demo_arena = {
    .name = "arena",
    .init = arena_start,
    .render_frame = arena_menu_frame,
    .handle_key = arena_key,
    .teardown = games_leave,
};

const demo_module_t demo_zap = {
    .name = "zap",
    .init = zap_start,
    .render_frame = zap_menu_frame,
    .handle_key = zap_key,
    .teardown = games_leave,
};
