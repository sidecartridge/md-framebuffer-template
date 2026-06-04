/**
 * File: examples/hello_text/emul.c
 * Description: Minimal "hello text" framebuffer app -- a stripped emul.c
 *              with the demo dispatcher removed. Drop this over
 *              rp/src/emul.c (and delete the demo files + their
 *              CMakeLists entries) to build a working app that bounces a
 *              string around the 320x200 colour framebuffer.
 *
 * See examples/hello_text/README.md for the apply + build steps, and the
 * repo README.md for the full API. Everything above the "app state"
 * marker is the template's boot sequence, kept verbatim.
 */

#include "emul.h"

#include <stdint.h>

#include "aconfig.h"
#include "audio.h"
#include "audio_sample.h"
#include "commemul.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

#define MSG "HELLO ATARI ST"
#define MSG_W (14 * 8) /* 14 chars * 8 px wide */

/* uint32 -> decimal string (fb_font has no printf). Writes into the tail
 * of `buf` and returns a pointer to the first digit. */
static const char *u32str(uint32_t n, char *buf, int buf_sz) {
  char *p = buf + buf_sz;
  *--p = '\0';
  if (n == 0) {
    *--p = '0';
  } else {
    while (n) {
      *--p = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return p;
}

void emul_start() {
  /* --- boot sequence (identical to the template's emul.c) --- */
  ERASE_FIRMWARE_IN_RAM();
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

  /* SD card -- best-effort (only needed if you stream audio/data). */
  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/test";
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }

  /* Loop the baked-in jingle. Delete these two lines for a silent app. */
  audio_play_loop(audio_sample_data, (uint32_t)sizeof(audio_sample_data));

  select_configure();

  /* --- app state --- */
  font_set_font(&font8x8);
  int x = 100, y = 90, dx = 2, dy = 1;
  uint32_t prev_draw_us = 0, prev_cv_us = 0; /* previous frame's timings */
  char num[11];

  /* --- main loop --- */
  DPRINTF("Entering main loop\n");
  while (true) {
    fb_pump_rom3(); /* ROM3 ring -> IKBD demux + VBL frame-sync */
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      /* ESC is scancode 0x01 -- handle exit / state changes here. */
      (void)k;
    }

    uint32_t t0 = time_us_32(); /* mark the start of this frame's drawing */

    /* draw one frame */
    fb_chunked_clear(15); /* palette index 15 = black */
    font_set_color(0);    /* palette index 0 = white */
    font_move((unsigned)x, (unsigned)y);
    font_print(MSG);

    /* DRAW + C2P microsecond readout (previous frame's numbers), the same
     * debug readout the demos show. DRAW = time spent drawing this frame;
     * C2P = the chunky->planar cost of the last fb_publish(). */
    font_move(8, 6);
    font_print("DRAW ");
    font_print(u32str(prev_draw_us, num, sizeof(num)));
    font_print(" C2P ");
    font_print(u32str(prev_cv_us, num, sizeof(num)));
    font_print(" US");

    /* animate -- bounce off the screen edges */
    x += dx;
    y += dy;
    if (x < 0 || x > FB_CHUNKED_W - MSG_W) dx = -dx;
    if (y < 0 || y > FB_CHUNKED_H - 8) dy = -dy;

    uint32_t draw_us = time_us_32() - t0; /* drawing cost, before publish */
    fb_publish();                         /* tear-free 50 Hz hand-off */
    prev_draw_us = draw_us;
    prev_cv_us = fb_last_convert_us();     /* c2p cost of that publish */
    audio_render_frame();
  }
}
