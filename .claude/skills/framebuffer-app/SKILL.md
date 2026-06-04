---
name: framebuffer-app
description: >-
  Use when building, modifying, or debugging an app on this
  md-framebuffer-template -- a template for sub-20-ms audiovisual
  SidecarTridge Multi-device microfirmware apps for the Atari ST (games,
  demos, console/computer emulations) where the speed of drawing a
  colourful 320x200 screen matters; you draw a 320x200 16-colour
  framebuffer on the RP2040 in the cartridge and the firmware blits it to
  the ST each VBL. Covers the chunked framebuffer, the fb_* / palette /
  audio / ikbd APIs, stripping the bundled demos to start a fresh app,
  the main-loop pattern, and the build / RAM constraints. Triggers on
  tasks like "make a framebuffer app", "draw X on the Atari screen", "add
  audio/input", "strip the demos", or "why does my app show garbage".
---

# Building a framebuffer app on md-framebuffer-template

This template is for **sub-20-millisecond audiovisual SidecarTridge
Multi-device microfirmware apps** for the Atari ST / STE / MegaST(E) —
games, demos, and console/computer emulations where the speed of putting
a colourful 320×200 screen up matters. You draw a 320×200 16-colour
framebuffer in the Pico's RAM and the firmware blits it to the ST each
VBL (50 Hz), with keyboard input and YM audio for free.

**You develop 100% on the RP2040 side — the framework does the heavy
lifting:** a dual (page-flipped) framebuffer on the Atari ST side
(tear-free, managed for you); real 50 Hz locked to the ST's vertical
blank; **~19 ms of compute every VBL** to draw your frame; chunked
drawing on the RP2040 (one byte per pixel) with the chunked → ST planar
conversion done for you in **~1 ms per VBL** (split across both cores);
**~6 kHz, 6-bit sampled sound** out the YM2149; and Atari ST keyboard
handled on the RP2040 with decoded scancodes delivered to your app.

## The model (read this first)

The app runs on the RP2040 in the cartridge. You **draw into one
byte-per-pixel buffer** (`fb_chunked_buffer`, 320×200, low nibble =
palette index 0–15) and call **`fb_publish()` once per frame**. The
firmware does the chunky→planar conversion and a tear-free, VBL-synced
hand-off to the ST at 50 Hz. No m68k assembly, no bus timing, no
double-buffering. `fb_publish()` blocks on the ST's VBL, so one call per
loop paces the app to 50 Hz.

`README.md` is the human guide; `CLAUDE.md` is the architecture
deep-dive; `examples/hello_text/` is a minimal working app.

## Starting a fresh app

**Quick path:** `examples/hello_text/apply.sh` does all of the below — it
backs up `rp/` to `rp.bak`, deletes the demo/menu files, and installs a
minimal `emul.c` + `CMakeLists.txt`. The manual steps:

1. **Delete the demos**: `rp/src/demo_*.c` (5 files), `rp/src/include/demo.h`,
   and the asset headers (`sidecart_logo.h`, `sidecart_text.h`,
   `solid3d.h`, `sprites_data.h`, `cojo_texture.h`, `cojo_font.h`,
   `diego_sprite.h`, `uridium_surface.h`).
2. **`rp/src/CMakeLists.txt`**: remove the `demo_*.c` lines from
   `target_sources(...)`.
3. **`rp/src/emul.c`**: replace the demo-dispatcher block (the
   `demo_dispatcher_init()` call and the `demo_dispatcher_*` calls in the
   `while (true)` loop) with your own init + render loop. Keep everything
   above it (the boot sequence) and the `fb_pump_rom3()` / `ikbd_pump()` /
   `audio_render_frame()` calls. Drop `#include "demo.h"`.
4. **`desc/app.json`**: set your `uuid` (must match the UUID passed to
   `build.sh`).

`examples/hello_text/emul.c` is a ready-made stripped `emul.c` to copy.

## The API (keep these modules; they are your API)

```c
// pixels: fb_chunked.h
extern uint8_t fb_chunked_buffer[FB_CHUNKED_W * FB_CHUNKED_H];  // 320*200
fb_chunked_clear(color);                       // fill whole buffer
fb_chunked_plot(x, y, color);                  // bounds-checked pixel
// rects + sprites: fb_blit.h
fb_fill_rect(x, y, w, h, color);
fb_blit(&bitmap, x, y);                        // opaque (FB_BITMAP{w,h,data})
fb_blit_key(&bitmap, x, y, key);               // key index = transparent
// text: fb_font.h  (font8x8 defined in fb.c; no printf -- format numbers yourself)
font_set_font(&font8x8); font_set_color(0); font_align(FONT_ALIGN_LEFT);
font_move(x, y); font_print("TEXT");
// palette: palette.h  (16 entries, channels 0..7; idx 0 white, 15 black by default)
palette_set_entry(2, PALETTE_RGB(7, 0, 0));    // or palette_set(entries[16])
// publish: fb.h
fb_publish();                                  // once per frame, after drawing
// input: ikbd.h
ikbd_key_event_t k; while (ikbd_pop_key(&k)) { if (k.is_press) ... }  // k.scancode
// audio: audio.h
audio_play_loop(data, bytes);                  // loop a baked-in buffer, OR
audio_set_fill_callback(cb);                   // cb(buf,bytes) per VBL, live
audio_render_frame();                          // call every loop iteration
```

**SD card:** the microSD is already mounted at boot
(`sdcard_initFilesystem()` in `emul_start()`). Read/write with standard
FatFs — `#include "ff.h"`, then `f_open` / `f_read` / `f_write` /
`f_close` on absolute paths. Helpers in `sdcard.h`: `sdcard_isMounted()`,
`sdcard_ensureFolder()`, `sdcard_dirExist()`. Keep file I/O **out of the
per-frame loop** (SPI reads cost ms and blow the ~19 ms budget) — load at
startup or stream small chunks like `audio.c` does for `.YMS`.

**Per-app config** (persistent settings, editable from Booster without
recompiling): defaults live in `rp/src/aconfig.c` `defaultEntries[]` as
`{"KEY", SETTINGS_TYPE_STRING|INT|BOOL, "default"}`, with the key macro in
`aconfig.h`. Read: `settings_find_entry(aconfig_getContext(), "KEY")` →
`->value` (always a string; `atoi()` for ints). Write:
`settings_put_string` / `_integer` / `_bool(aconfig_getContext(), "KEY",
v)` then `settings_save(aconfig_getContext(), true)`. (This is how the SD
folder name `ACONFIG_PARAM_FOLDER` is supplied.)

Main loop shape (in `emul_start()`):

```c
while (true) {
    fb_pump_rom3(); ikbd_pump();          // keep: input + VBL sync
    ikbd_key_event_t k; while (ikbd_pop_key(&k)) { /* handle */ }
    /* clear, draw into fb_chunked_buffer */
    fb_publish();                          // tear-free 50 Hz
    audio_render_frame();                  // keep: audio
}
```

## Critical constraints (get these wrong and the ST shows garbage)

- **RAM is the binding limit.** `.bss` + heap must not grow past
  `0x20030000` (the m68k cart-ROM mirror). A large `static` array can
  push past it and silently corrupt the m68k code → the ST runs garbage
  / looks "rolled back". The framebuffer buffers already use ~96 KB.
  Big lookup tables / textures should stay `const` (flash), not RAM.
- **Build:** `./build.sh <board> <build_type> <uuid>`. Build with the
  dev UUID `44444444-4444-4444-8444-444444444444` (the placeholder in
  CLAUDE.md keys the app to the wrong identity → it jumps to Booster).
  The user usually runs the build themselves; only build the **Atari/m68k
  target** if you change `target/atarist/` asm. RP-only C changes don't
  need it.
- **Optimization:** the global build is `MinSizeRel` (`-Os`). For hot
  per-pixel loops add `#pragma GCC optimize("O3")` at the top of that
  *compute-only* `.c` file and `__not_in_flash_func()` on the function —
  never on bus/PIO/timing code. The demo sources are the reference for
  the full toolbox (LUTs, the SIO interpolator, `fb_core1_dispatch`
  dual-core).
- **Never** edit the `pico-sdk/`, `pico-extras/`, `fatfs-sdk/`
  submodules — the build re-pins them. Don't add features to `main.c`
  (use `emul.c`).
- **Palette:** index 0 = white (text/border), 15 = black (background);
  `PALETTE_RGB(r,g,b)` channels are 0..7. Re-publishing the palette each
  frame is cheap (colour-cycling).
- **Commits:** no AI attribution / `Co-Authored-By` trailers.

## Verifying without hardware

You usually can't flash. Validate fixed-point / addressing math by
mirroring the exact C in a small offline script and diffing against the
reference, the way the demos were validated (e.g. interpolator configs,
rasteriser hole-checks). Then the user flashes and confirms.
