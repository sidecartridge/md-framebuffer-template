/**
 * File: emul.c
 * Description: Template microfirmware boot path. Brings up the cartridge
 *              bus emulator, the 32 KB color framebuffer, the ROM3
 *              command channel, the SD card, and the SELECT button, then
 *              hands off to a chandler-drained main loop.
 *
 * Story 1.2.12 stripped the previous u8g2-backed terminal subsystem
 * (term.c, display.c, display_term.h). The cart-side UI is now driven
 * entirely by fb.c writing into the 32 KB low-res framebuffer at
 * $FA8300, which the m68k userfw VBL loop blits to ST screen each
 * frame. Apps extend this template by:
 *   - drawing into `fb_screen.framebuffer` via fb_font / fb_draw, and
 *   - registering ROM3 callbacks with chandler_addCB().
 */

#include "emul.h"

#include <stdint.h>

#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "debug.h"
#include "fb.h"
#include "ff.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"

/* No sleep -- tight loop. Story 1.2.13's dirty-frame handshake means
 * the m68k only blits cart->ST when fb_render_frame() actually publishes
 * a new counter value. The RP can rewrite the framebuffer at whatever
 * rate it wants; the m68k decides per VBL whether the new content is
 * worth copying. Apps that need a fixed cadence can add their own
 * sleep_ms / sleep_until call here. */

void emul_start() {
  // RP2040 RAM is undefined at power-on; firmware.py only emits the
  // bytes up to the last non-zero in BOOT.BIN (padded to 64 KB), so
  // without an explicit erase the framebuffer region at $FA8300+ would
  // be whatever was sitting in RAM and the m68k blit would copy that
  // noise to the ST screen. Zero the whole 64 KB shared region first
  // so every byte the m68k can see is deterministic.
  ERASE_FIRMWARE_IN_RAM();

  // Copy the cartridge image into the now-zeroed region.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialise the cartridge ROM4 read engine. ROM4 reads are served
  // entirely by chained DMAs feeding the PIO TX FIFO -- no CPU/IRQ
  // involvement. Without this engine the cartridge image is unreadable
  // from the m68k, so a failure here is fatal.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Initialise the 32 KB low-res framebuffer (320x200, 4 bpp). Sets
  // up `fb_screen` for the font/draw primitives, clears the FB to
  // black (0xFF -> palette index 15 on the default TOS palette), and
  // renders the boot pattern. The m68k VBL loop in userfw.s blits
  // this to an off-screen ST page each frame and flips the video base.
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and
  // the command handler that polls the ring, parses the protocol, and
  // dispatches each command to the registered callbacks. Apps add
  // their own callbacks here via `chandler_addCB(cb)` -- the template
  // ships with none registered.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();

  // SD card -- best-effort. Apps that need persistent storage can
  // ignore the failure path or treat it as fatal. The folder name is
  // taken from per-app config (ACONFIG_PARAM_FOLDER) so apps can be
  // reconfigured from Booster without recompiling.
  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/test";
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }

  // Cartridge SELECT button -- apps can poll select_isPressed().
  select_configure();

  // Main loop:
  //   1. Drain the ROM3 command ring -> dispatch to registered chandler
  //      callbacks (apps register theirs above via chandler_addCB).
  //   2. Re-render the cart framebuffer. The m68k VBL loop in userfw.s
  //      blits this into an ST screen page once per vertical retrace,
  //      so any change here becomes visible within ~20 ms.
  //
  // The 1.2.15 smoke-test version of fb_render_frame redraws the whole
  // 32 KB framebuffer every tick (clear + text + scrolling marquee).
  // No tearing handshake yet -- if a m68k blit catches mid-write, the
  // ST will show half-old / half-new bands and we'll know 1.2.13 is
  // actually needed.
  DPRINTF("Entering main loop\n");
  while (true) {
    chandler_loop();
    fb_render_frame();
  }
}
