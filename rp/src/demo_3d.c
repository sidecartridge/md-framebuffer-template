/**
 * File: demo_3d.c
 * Description: Story 5.3 -- 3D filled-flat-shaded demo (STUB).
 *
 * Current state: placeholder. Renders a "TODO" screen so the
 * dispatcher state machine is exercisable end-to-end while the
 * filled-3D + optional raster effect implementation lands in
 * Story 5.3 follow-up tasks.
 */

#include "demo.h"

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"

extern const struct FB_FONT font8x8;

static void demo_3d_init(void) {
  /* nothing yet */
}

static void demo_3d_teardown(void) {
  /* nothing yet */
}

static void demo_3d_render_frame(void) {
  fb_chunked_clear(15);

  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  font_move(64, 72);
  font_print("3D scene demo");
  font_move(64, 88);
  font_print("(Epic 5 Story 5.3 -- TODO)");
  font_move(48, 168);
  font_print("Press ESC to return to menu");

  fb_publish();
}

const demo_module_t demo_3d = {
    .name = "3d",
    .init = demo_3d_init,
    .render_frame = demo_3d_render_frame,
    .handle_key = NULL,
    .teardown = demo_3d_teardown,
};
