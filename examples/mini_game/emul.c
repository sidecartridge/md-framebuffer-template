/**
 * File: examples/mini_game/emul.c
 * Description: A firmware that boots straight into a game: the template's
 *              boot sequence (rp/src/emul.c without its demo menu), then a
 *              main loop that runs Arena, the joystick game
 *              (rp/src/game_arena.c). Zap, the mouse game, runs the same
 *              way: swap arena_* for zap_* below.
 *
 * See examples/mini_game/README.md for the apply + build steps.
 */

#include "emul.h"

#include <stdint.h>

#include "aconfig.h"
#include "audio.h"
#include "commemul.h"
#include "debug.h"
#include "devhooks.h"
#include "fb.h"
#include "ff.h"
#include "games.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "st_session.h"
#include "target_firmware.h"

void emul_start() {
  /* --- boot sequence (the template's emul.c) --- */
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
  ikbd_init();
  if (init_romemul(false) < 0) {
    panic("init_romemul failed");
  }
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }
  palette_init();
  audio_init();

  /* SD card -- best-effort (the game does not use it). */
  static FATFS fsys; /* ~600 bytes: off core 0's stack */
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/test";
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }

  /* SELECT (configured in main()): a short press restarts the RP, a press
   * held 10 s is a factory reset. select_poll() in the loop runs them. */
  select_setResetCallback(reset_device);
  select_setLongResetCallback(reset_deviceAndEraseFlash);

  /* --- the game --- */
  arena_start(); /* palette, joystick mode, the tune; a new round */

  DPRINTF("Entering main loop\n");
  while (true) {
    fb_pump_rom3();  /* ROM3 ring -> IKBD demux + VBL frame-sync */
    devhooks_poll(); /* debug builds: keys from the host over SWD */
    ikbd_pump();
    select_poll();
    if (st_session_consume_boot()) { /* the ST rebooted: a new game */
      arena_start();
    }
    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) { /* ESC press + release leaves to GEM */
      arena_key(&k);
    }
    arena_frame(); /* ends with fb_publish(): paced to the ST's 50 Hz */
    audio_render_frame();
  }
}
