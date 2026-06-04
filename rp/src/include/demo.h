/**
 * File: demo.h
 * Description: Demo module interface + dispatcher.
 *
 * Each demo (`demo_parallax`, `demo_3d`, `demo_sprites`) is a
 * `demo_module_t` providing a small set of lifecycle functions. The
 * dispatcher in demo_menu.c owns the boot-menu UI, the demo-selection
 * state machine, and the ESC routing:
 *
 *   - In MENU state: numeric keys 1/2/3 start a demo; ESC exits to
 *     GEM via the cart CMD_BOOT_GEM sentinel.
 *   - In ACTIVE state: ESC tears down the demo and returns to the
 *     menu; all other keys are forwarded to the demo via its
 *     `handle_key` callback.
 *
 * Demos draw into `fb_chunked_buffer` via the existing fb_font /
 * fb_blit / fb_chunked_clear primitives and call `fb_publish()` at
 * the end of `render_frame()` to push the planar conversion + frame
 * counter update to the m68k.
 *
 * The dispatcher takes ownership of the ESC key by calling
 * `ikbd_set_esc_auto_exit(false)` during demo_dispatcher_init().
 */

#ifndef DEMO_H_INCLUDED
#define DEMO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "ikbd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /* Short human-readable name (for DPRINTF / debugging). */
  const char *name;

  /* Called when transitioning MENU -> ACTIVE for this demo. */
  void (*init)(void);

  /* Called once per main-loop iteration while ACTIVE. Demo draws
   * into fb_chunked_buffer and ends with `fb_publish()`. */
  void (*render_frame)(void);

  /* Called for each ikbd_pop_key event while ACTIVE, except ESC
   * (which the dispatcher consumes for "back to menu"). May be
   * NULL if the demo ignores keys. */
  void (*handle_key)(const ikbd_key_event_t *k);

  /* Called once when the user ESCs back to the menu. May be NULL. */
  void (*teardown)(void);
} demo_module_t;

/* The demos. Each lives in its own .c file. */
extern const demo_module_t demo_parallax;
extern const demo_module_t demo_3d;
extern const demo_module_t demo_sprites;
extern const demo_module_t demo_cojorotozoom;

/* Dispatcher entry points. Call init() once at boot; call
 * handle_key() for every popped IKBD event and render_frame() once
 * per main-loop iteration. */
void demo_dispatcher_init(void);
void demo_dispatcher_handle_key(const ikbd_key_event_t *k);
void demo_dispatcher_render_frame(void);

/* Shared DRAW/C2P timing readout toggle (the hidden 'D' key, handled by
 * the dispatcher in any state). The menu and every demo gate their µs
 * readout on this. Defined in demo_menu.c; starts ON. */
extern bool g_show_timing;

#ifdef __cplusplus
}
#endif

#endif /* DEMO_H_INCLUDED */
