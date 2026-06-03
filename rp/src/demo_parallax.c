/**
 * File: demo_parallax.c
 * Description: Story 5.2 -- parallax scroll demo (STUB).
 *
 * Current state: placeholder. Renders a "TODO" screen so the
 * dispatcher state machine is exercisable end-to-end while the real
 * 3-layer scroll implementation lands in Story 5.2 follow-up tasks.
 */

#include "demo.h"

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"

extern const struct FB_FONT font8x8;

static void parallax_init(void) {
  /* nothing yet */
}

static void parallax_teardown(void) {
  /* nothing yet */
}

static void parallax_render_frame(void) {
  fb_chunked_clear(15);

  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  font_move(64, 72);
  font_print("Parallax scroll demo");
  font_move(64, 88);
  font_print("(Epic 5 Story 5.2 -- TODO)");
  font_move(48, 168);
  font_print("Press ESC to return to menu");

  fb_publish();
}

const demo_module_t demo_parallax = {
    .name = "parallax",
    .init = parallax_init,
    .render_frame = parallax_render_frame,
    .handle_key = NULL,
    .teardown = parallax_teardown,
};
