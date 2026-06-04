/**
 * File: emul.c
 * Description: Template microfirmware boot path. Brings up the cartridge
 *              bus emulator, the 32 KB color framebuffer, the ROM3
 *              command-capture ring, the SD card, and the SELECT
 *              button, then enters a tight main loop that drains the
 *              ROM3 ring into the IKBD demux and re-renders the
 *              framebuffer.
 *
 * The previous u8g2-backed terminal subsystem was stripped
 * (term.c, display.c, display_term.h). The cart-side UI is now driven
 * entirely by fb.c writing into the 32 KB low-res framebuffer at
 * $FA8300, which the m68k userfw VBL loop blits to ST screen each
 * frame. Apps extend this template by drawing into
 * `fb_screen.framebuffer` via fb_font / fb_blit.
 */

#include "emul.h"

#include <stdint.h>

#include "aconfig.h"
#include "audio.h"
#include "audio_sample.h"
#include "commemul.h"
#include "debug.h"
#include "demo.h"
#include "fb.h"
#include "ff.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"

/* No sleep -- tight loop. The dirty-frame handshake means
 * the m68k only blits cart->ST when fb_publish() actually advances
 * the frame counter. The RP can rewrite the framebuffer at whatever
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

  // Reset the IKBD ring (init order before commemul_init is fine since
  // the producer side runs from the main loop, not from an IRQ).
  ikbd_init();

  // Initialise the cartridge ROM4 read engine. ROM4 reads are served
  // entirely by chained DMAs feeding the PIO TX FIFO -- no CPU/IRQ
  // involvement. IKBD ingest is on ROM3 + commemul ring (see main
  // loop below).
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 cart-bus capture (PIO + 32 KB DMA ring) BEFORE
  // fb_init: fb_init's first fb_publish() drains the ROM3 ring while it
  // waits for the m68k's VBL ack, so the ring must already exist. (At
  // boot the m68k may not be emitting acks yet; the first publish just
  // times out after ~33 ms and proceeds.) The main loop drains the
  // ring via fb_pump_rom3() -> IKBD demux + VBL frame-sync.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }

  // Initialise the 32 KB low-res framebuffer (320x200, 4 bpp). Sets
  // up `fb_screen` for the font/draw primitives, clears the FB to
  // black (0xFF -> palette index 15 on the default TOS palette), and
  // renders the boot pattern. The m68k VBL loop in userfw.s blits
  // this to an off-screen ST page each frame and flips the video base.
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }

  // Publish the default 16-colour palette to the cart shared-region
  // slot. The m68k VBL handler applies it to the shifter palette
  // registers ($FFFF8240..) every frame. Apps that want a different
  // palette call palette_set() / palette_set_entry() at any time;
  // the next VBL picks it up.
  palette_init();

  // Initialise the cart audio buffer producer (see audio.h). The
  // m68k Timer-B IRQ in userfw.s consumes the buffer at ~5,585 Hz
  // (2 B/sample dual-channel mode). audio_init() leaves the buffer
  // silent until a callback is installed; the playback source is
  // chosen below, after the SD card has had a chance to mount.
  audio_init();

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

  // Audio source selection. Try to stream a
  // .YMS file from the app folder first; on any failure (no SD,
  // file missing, bad header, rate mismatch) fall back to the
  // baked-in Ghostbusters G1 jingle so the demo still has audio.
  // Apps swap their own audio_play_yms_file path or replace this
  // block with audio_set_fill_callback() / audio_play_loop().
  if (audio_play_yms_file("DEMO.YMS") < 0) {
    audio_play_loop(audio_sample_data,
                    (uint32_t)sizeof(audio_sample_data));
  }

  // Cartridge SELECT button -- apps can poll select_isPressed().
  select_configure();

  // Bring up the demo dispatcher. demo_dispatcher_init takes
  // ownership of the ESC key from ikbd.c (ESC now means "back to
  // menu" inside a demo and "exit to GEM" only when the menu is on
  // screen). The first dispatcher render paints the boot menu over
  // whatever fb_init left in the framebuffer.
  demo_dispatcher_init();

  // Main loop:
  //   1. Drain the ROM3 commemul ring straight into the IKBD raw-byte
  //      ring (one PIO sample per IKBD byte the m68k Timer-B handler
  //      forwarded; the filter inside ikbd_consume_rom3_sample picks
  //      out the $FB8200..$FB82FF window).
  //   2. Run the IKBD demux on whatever bytes arrived.
  //   3. Forward decoded key events to the dispatcher (which routes
  //      to the menu or the active demo).
  //   4. Re-render the cart framebuffer via the dispatcher (menu UI
  //      or active demo's render_frame). The m68k VBL loop in
  //      userfw.s blits this into an ST screen page once per VBL.
  DPRINTF("Entering main loop\n");
  while (true) {
    fb_pump_rom3();  /* drains ROM3 ring -> IKBD demux + VBL frame-sync */
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      demo_dispatcher_handle_key(&k);
    }

    demo_dispatcher_render_frame();
    audio_render_frame();
  }
}
