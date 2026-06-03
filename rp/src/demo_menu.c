/**
 * File: demo_menu.c
 * Description: Boot menu + demo dispatcher (Epic 5 Story 5.1).
 *
 * Owns the boot menu UI (rendered into the same chunked framebuffer
 * the demos use) and the MENU <-> ACTIVE state machine.
 *
 * ESC routing:
 *   - MENU + ESC = exit to GEM (CMD_BOOT_GEM via cart sentinel).
 *   - ACTIVE + ESC = teardown current demo, return to MENU.
 *
 * The dispatcher opts out of ikbd.c's built-in ESC auto-exit during
 * init so it can own the key end-to-end.
 */

#include "demo.h"

#include <stdint.h>

#include "cart_shared.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ikbd.h"
#include "memfunc.h"

/* font8x8 is defined in fb.c (the canonical single point that
 * includes the heavy font8x8.h glyph data). Demos just use it. */
extern const struct FB_FONT font8x8;

typedef enum {
  DEMO_STATE_MENU,
  DEMO_STATE_ACTIVE,
  DEMO_STATE_EXITING,
} demo_state_t;

static demo_state_t s_state;
static const demo_module_t *s_active;

#define MENU_ITEM_COUNT 3
static const demo_module_t *const s_menu[MENU_ITEM_COUNT] = {
    &demo_parallax,
    &demo_3d,
    &demo_sprites,
};

/* IKBD scancodes for the menu hotkeys. ESC = $01 already used as
 * the back/exit key; 1/2/3 = $02/$03/$04 on the unshifted top row. */
#define IKBD_SC_ESC 0x01u
#define IKBD_SC_1   0x02u
#define IKBD_SC_2   0x03u
#define IKBD_SC_3   0x04u

static void render_menu(void) {
  /* Background = palette idx 15 (black per the default palette);
   * text foreground = idx 0 (white). */
  fb_chunked_clear(15);

  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  /* Title block. */
  font_move(60, 24);
  font_print("md-framebuffer-template");
  font_move(80, 36);
  font_print("Epic 5 -- chunky demos");

  /* Menu items. */
  font_move(80, 76);
  font_print("1.  Uridium scroll");
  font_move(80, 96);
  font_print("2.  3D scene");
  font_move(80, 116);
  font_print("3.  Multi-sprite swarm");

  /* Footer hint. At 8px/char the 38-char line is 304px wide, so it
   * starts at x=8 to clear the right edge (8..312 of 320). */
  font_move(8, 168);
  font_print("Press 1-3 to start, ESC to exit to GEM");

  fb_publish();
}

static void exit_to_gem(void) {
  volatile uint32_t *sentinel =
      (volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                            CART_CMD_SENTINEL_OFFSET);
  *sentinel = cart_asM68kLong(CART_CMD_BOOT_GEM);
  s_state = DEMO_STATE_EXITING;
}

void demo_dispatcher_init(void) {
  s_state = DEMO_STATE_MENU;
  s_active = NULL;
  /* Take ownership of the ESC key from ikbd.c -- the dispatcher
   * routes it to "back to menu" while a demo is active and uses it
   * to exit to GEM only from the menu. */
  ikbd_set_esc_auto_exit(false);
  DPRINTF("demo_dispatcher_init: MENU state, ESC owned by dispatcher\n");
}

void demo_dispatcher_handle_key(const ikbd_key_event_t *k) {
  if (!k->is_press) {
    /* Forward releases to the active demo (it may track held keys);
     * the menu only cares about presses. */
    if (s_state == DEMO_STATE_ACTIVE && s_active && s_active->handle_key) {
      s_active->handle_key(k);
    }
    return;
  }

  if (s_state == DEMO_STATE_MENU) {
    switch (k->scancode) {
      case IKBD_SC_ESC:
        DPRINTF("dispatcher: ESC from menu -> exit to GEM\n");
        exit_to_gem();
        break;
      case IKBD_SC_1:
      case IKBD_SC_2:
      case IKBD_SC_3: {
        unsigned idx = (unsigned)(k->scancode - IKBD_SC_1);
        if (idx < MENU_ITEM_COUNT) {
          s_active = s_menu[idx];
          s_state = DEMO_STATE_ACTIVE;
          DPRINTF("dispatcher: starting demo '%s'\n",
                  s_active->name ? s_active->name : "(unnamed)");
          if (s_active->init) {
            s_active->init();
          }
        }
        break;
      }
      default:
        break;
    }
    return;
  }

  if (s_state == DEMO_STATE_ACTIVE) {
    if (k->scancode == IKBD_SC_ESC) {
      DPRINTF("dispatcher: ESC from demo '%s' -> back to menu\n",
              s_active && s_active->name ? s_active->name : "(unnamed)");
      if (s_active && s_active->teardown) {
        s_active->teardown();
      }
      s_active = NULL;
      s_state = DEMO_STATE_MENU;
      return;
    }
    if (s_active && s_active->handle_key) {
      s_active->handle_key(k);
    }
  }
}

void demo_dispatcher_render_frame(void) {
  switch (s_state) {
    case DEMO_STATE_MENU:
      render_menu();
      break;
    case DEMO_STATE_ACTIVE:
      if (s_active && s_active->render_frame) {
        s_active->render_frame();
      }
      break;
    case DEMO_STATE_EXITING:
      /* Don't touch the framebuffer; the m68k will see CMD_BOOT_GEM
       * on its next VBL poll and exit cleanly back to GEM. */
      break;
  }
}
