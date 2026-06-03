/**
 * File: demo_sprites.c
 * Description: Story 5.4 -- multisprite swarm demo (STUB).
 *
 * Current state: placeholder. Renders a "TODO" screen so the
 * dispatcher state machine is exercisable end-to-end while the
 * runtime-configurable sprite count + render loop lands in
 * Story 5.4 follow-up tasks.
 */

#include "demo.h"

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"

extern const struct FB_FONT font6x8;

static void sprites_init(void) {
  /* nothing yet */
}

static void sprites_teardown(void) {
  /* nothing yet */
}

static void sprites_render_frame(void) {
  fb_chunked_clear(15);

  font_set_font(&font6x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  font_move(64, 72);
  font_print("Multi-sprite swarm");
  font_move(64, 88);
  font_print("(Epic 5 Story 5.4 -- TODO)");
  font_move(48, 168);
  font_print("Press ESC to return to menu");

  fb_publish();
}

const demo_module_t demo_sprites = {
    .name = "sprites",
    .init = sprites_init,
    .render_frame = sprites_render_frame,
    .handle_key = NULL,
    .teardown = sprites_teardown,
};
