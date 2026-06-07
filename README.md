# md-framebuffer-template

A template for building **sub-20-millisecond audiovisual SidecarTridge
Multi-device microfirmware apps** for the Atari ST / STE / MegaST(E) —
games, demos, and console/computer emulations where the speed of putting
a colourful 320×200 screen up matters. Your app runs on the Raspberry Pi
Pico (RP2040) in the cartridge: you draw into a **320×200, 16-colour
framebuffer** in the Pico's RAM, and the firmware blits it to the ST
screen every VBL (50 Hz) for you. You also get keyboard input and YM
audio out of the box.

The headline idea: **you write into one byte-per-pixel buffer, call
`fb_publish()` once a frame, and the picture appears on the ST — tear-free
at 50 Hz.** No m68k assembly, no bus timing, no double-buffering to manage.

### You develop 100% on the RP2040 side — the framework does the heavy lifting

- **Dual (page-flipped) framebuffer on the Atari ST side** — tear-free
  display, fully managed for you.
- **Real 50 Hz**, locked to the ST's vertical blank.
- **~19 ms of compute every VBL** for your app to draw its frame.
- **Chunked drawing on the RP2040** — you write one byte per pixel; the
  framework does the chunked → Atari ST planar conversion for you.
- **~1 ms per VBL** for that chunky→planar conversion (split across both
  cores), so it barely eats into your frame budget.
- **~6 kHz, 6-bit sampled sound** out the YM2149.
- **Atari ST keyboard handled on the RP2040** — decoded scancodes
  delivered straight to your app.

> This repo ships with a 4-demo showcase + an animated menu.
> This guide is about **starting your own app**: what to remove, the API
> you keep, and a minimal example. For the build toolchain and flashing,
> see the official docs:
> <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

---

## 1. Build

```bash
# ./build.sh <board> <build_type> <app_uuid>
#   board:      pico | pico_w | sidecartos_16mb
#   build_type: debug | release
#   app_uuid:   UUID4 identifying your app (must match desc/app.json)
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
```

Output is `dist/<APP_UUID>-<VERSION>.uf2` — drag it onto the Pico in
BOOTSEL mode (or use the official tooling). Requires the ARM GNU Toolchain
14.2 (`PICO_TOOLCHAIN_PATH`) and, only if you change m68k code, the
`atarist-toolkit-docker` (`stcmd`). A pure framebuffer app never touches
the m68k side.

### Jumping to the Booster configurator

**Hold the cartridge SELECT button while powering on** (or while resetting
the Pico) to jump straight into the **Booster** app — the SidecarTridge
configurator menu — *before* this app runs. `main.c` reads SELECT at boot
and, if it's held, calls `reset_jump_to_booster()` immediately:

```c
select_configure();
if (select_detectPush()) {
    reset_jump_to_booster();   // never returns
}
```

This is the recovery / reconfiguration escape hatch: it works even if the
app's saved config is wrong or the app misbehaves. Power on with SELECT
released to run the app normally.

---

## 2. Starting fresh — strip the demos

The template includes a boot menu and four demos as worked examples.
For your own app, remove them and wire your code into the main loop.

**The quick way:** `examples/hello_text/apply.sh` does all of this for you
— it backs up `rp/` to `rp.bak`, deletes the demo/menu files below, and
drops in a minimal `emul.c` + `CMakeLists.txt`. Run it, then build:

```bash
examples/hello_text/apply.sh
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
# revert anytime with:  rm -rf rp && mv rp.bak rp
```

The rest of this section is what `apply.sh` automates, for when you'd
rather strip the demos by hand.

### Files to **delete** (demo / menu only)

```
rp/src/demo_menu.c            rp/src/include/demo.h
rp/src/demo_parallax.c        rp/src/include/sidecart_logo.h
rp/src/demo_3d.c              rp/src/include/sidecart_text.h
rp/src/demo_sprites.c         rp/src/include/solid3d.h
rp/src/demo_cojorotozoom.c    rp/src/include/sprites_data.h
                              rp/src/include/cojo_texture.h
                              rp/src/include/cojo_font.h
                              rp/src/include/diego_sprite.h
                              rp/src/include/uridium_surface.h
```

Keep `tools/png_to_texture.py` and `tools/wav_to_ym4.py` — they convert
your *own* image/audio assets into headers.

### Files to **change**

- **`rp/src/CMakeLists.txt`** — remove the five `demo_*.c` entries from
  `target_sources(...)`. (You can also drop `hardware_interp` from
  `target_link_libraries` unless you use the SIO interpolator.)
- **`rp/src/emul.c`** — the main loop currently drives the demo
  dispatcher; replace that with your own init + render (see §4).
- **`desc/app.json`** — set your app's `uuid` (must match the UUID you
  pass to `build.sh`) and name/description.

### Files to **keep** — this is your API

| Module | What it gives you |
| --- | --- |
| `fb.c/.h` | framebuffer init + `fb_publish()` (the one call per frame) |
| `fb_chunked.c/.h` | `fb_chunked_buffer` (the pixels you draw into) + clear/plot + dual-core helper |
| `fb_blit.c/.h` | rectangles, opaque + colour-keyed sprite blits |
| `fb_font.c/.h` + `font8x8.h` | text |
| `palette.c/.h` | the 16-colour palette |
| `audio.c/.h` | YM audio (loop a buffer or stream via callback) |
| `ikbd.c/.h` | keyboard events |

Everything else (`main.c`, `commemul`, `romemul`, `ikbd`, `sdcard`,
`select`, `reset`, `gconfig`/`aconfig`, `cart_shared.h`, `constants.h`,
`settings/`) is plumbing — leave it alone.

---

## 3. The framebuffer API

### The pixel buffer

You draw into one global byte array — **one byte per pixel, the low
nibble is the palette index (0–15)**:

```c
#include "fb_chunked.h"
extern uint8_t fb_chunked_buffer[FB_CHUNKED_W * FB_CHUNKED_H]; // 320 * 200
```

`fb_chunked_buffer[y * FB_CHUNKED_W + x] = colour_index;` sets a pixel.
After drawing a frame, call `fb_publish()` (from `fb.h`) once — it does
the chunky→planar conversion and the tear-free, VBL-synced hand-off to
the ST.

### Clearing & plotting (`fb_chunked.h`)

```c
void fb_chunked_clear(uint8_t color);                 // fill whole buffer
static inline void fb_chunked_plot(unsigned x, unsigned y, uint8_t color); // bounds-checked
```

### Rectangles & sprites (`fb_blit.h`)

```c
void fb_fill_rect(int x, int y, int w, int h, uint8_t color);     // clipped
void fb_blit(const struct FB_BITMAP *bm, int x, int y);           // opaque
void fb_blit_key(const struct FB_BITMAP *bm, int x, int y, uint8_t key); // key = transparent

struct FB_BITMAP { uint16_t width; uint16_t height; const uint8_t *data; };
// data = width*height bytes, row-major, one palette index per byte.
```

### Text (`fb_font.h`)

```c
extern const struct FB_FONT font8x8;     // defined in fb.c
font_set_font(&font8x8);
font_set_color(0);                       // palette index 0 (white by default)
font_align(FONT_ALIGN_LEFT);             // or _CENTER / _RIGHT
font_move(x, y);
font_print("HELLO");                     // no printf; format numbers yourself
```

### Palette (`palette.h`)

16 entries; each colour is `PALETTE_RGB(r, g, b)` with `r/g/b` in 0–7
(Atari ST 3-bit channels). Index 0 defaults to white, 15 to black.

```c
void palette_init(void);                              // default palette
void palette_set(const uint16_t entries[16]);         // bulk replace
void palette_set_entry(uint8_t idx, uint16_t color);  // one entry
// e.g. palette_set_entry(2, PALETTE_RGB(7,0,0));      // idx 2 = bright red
```

Re-publishing the palette every frame is cheap — that's how the menu does
its colour-cycling.

### Publishing (`fb.h`)

```c
void fb_publish(void);            // call once per frame, after drawing
uint32_t fb_last_convert_us(void);// c2p cost of the last publish (debug)
```

`fb_publish()` blocks until the ST has finished blitting the previous
frame, so calling it once per loop naturally paces your app to 50 Hz.

---

## 4. Your first app — moving text

This whole section is a ready-to-build app in **`examples/hello_text/`**.
From a fresh checkout, `apply.sh` backs up `rp/` to `rp.bak` and swaps in
the stripped app:

```bash
examples/hello_text/apply.sh    # backup rp/ -> rp.bak, strip demos, install
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
# revert any time:  rm -rf rp && mv rp.bak rp
```

Here it is — replace the demo block in `rp/src/emul.c`'s main loop with
this. It bounces a string around the screen and prints a `DRAW`/`C2P`
microsecond readout (the same debug numbers the demos show):

```c
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "ikbd.h"
#include "audio.h"
#include "pico/time.h"

// fb_font has no printf -- tiny uint32 -> string helper for the readout.
static const char *u32str(uint32_t n, char *buf, int sz) {
    char *p = buf + sz; *--p = '\0';
    if (!n) *--p = '0';
    else while (n) { *--p = (char)('0' + n % 10); n /= 10; }
    return p;
}

// ... inside emul_start(), after fb_init / audio_init / etc. ...

palette_init();
font_set_font(&font8x8);

int x = 100, y = 90, dx = 2, dy = 1;
uint32_t prev_draw_us = 0, prev_cv_us = 0;   // previous frame's timings
char num[11];

while (true) {
    fb_pump_rom3();   // keyboard + VBL sync plumbing -- keep this
    ikbd_pump();

    // (optional) read keys
    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
        if (k.is_press && k.scancode == 0x01) {  // ESC scancode
            // ... exit / change state ...
        }
    }

    uint32_t t0 = time_us_32();       // start of this frame's drawing

    // --- draw one frame ---
    fb_chunked_clear(15);            // clear to palette index 15 (black)
    font_set_color(0);               // white
    font_move((unsigned)x, (unsigned)y);
    font_print("HELLO ATARI ST");

    // DRAW + C2P microsecond readout (previous frame's numbers)
    font_move(8, 6);
    font_print("DRAW "); font_print(u32str(prev_draw_us, num, sizeof num));
    font_print(" C2P "); font_print(u32str(prev_cv_us, num, sizeof num));
    font_print(" US");

    // --- animate ---
    x += dx; y += dy;
    if (x < 0 || x > 320 - 14*8) dx = -dx;   // "HELLO ATARI ST" = 14 chars
    if (y < 0 || y > 200 - 8)    dy = -dy;

    uint32_t draw_us = time_us_32() - t0;   // drawing cost, before publish
    fb_publish();        // push to the ST, paces to 50 Hz
    prev_draw_us = draw_us;
    prev_cv_us = fb_last_convert_us();       // c2p cost of that publish
    audio_render_frame();
}
```

That's a complete app: clear → draw → animate → `fb_publish()`. `DRAW` is
the time spent drawing the frame; `C2P` is the chunky→planar cost of
`fb_publish()`. Swap `font_print` for `fb_blit`/`fb_fill_rect` to draw
your own graphics.

> Tip: keep `fb_pump_rom3()` + `ikbd_pump()` at the top of the loop and
> `audio_render_frame()` at the bottom — those keep input and audio alive.

---

## 5. Audio (`audio.h`)

The firmware streams a 1 KB cart buffer to the YM2149 every VBL. You
supply the bytes one of two ways.

### Loop a baked-in buffer

Convert a `.wav`/`.sam` to a header with `tools/wav_to_ym4.py`, then:

```c
#include "audio_sample.h"   // generates audio_sample_data[]
audio_init();               // once at boot
audio_play_loop(audio_sample_data, sizeof(audio_sample_data));
// ... then call audio_render_frame() once per main-loop iteration.
```

### Generate audio live (callback)

For dynamic sound, install a fill callback. The library calls it once per
VBL with the exact byte count the m68k will consume (224 = 112 stereo
samples at ~5,585 Hz):

```c
static void my_fill(uint8_t *buf, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) buf[i] = next_sample_byte();
}
audio_set_fill_callback(my_fill);   // pass NULL for silence
```

Either way, **`audio_render_frame()` must be called each loop iteration**
(it self-paces to ~50 Hz). There's also `audio_play_yms_file(path)` to
stream a `.YMS` file from SD — see §6.

---

## 6. SD card (`sdcard.h` + FatFs)

The cartridge has a microSD slot, and **the template already mounts it for
you at boot** — so reading and writing files is just standard
[FatFs](http://elm-chan.org/fsp/) (`f_open` / `f_read` / `f_write` /
`f_close` / `f_opendir` …). Good for level data, save games, bitmaps,
streamed audio, anything that won't fit in flash.

### It's mounted at boot

`emul_start()` already does this (keep it; it's part of the boot block):

```c
FATFS fsys;
const char *folder = "/myapp";   // your app's working directory on the card
if (sdcard_initFilesystem(&fsys, folder) != SDCARD_INIT_OK) {
    // No card / unreadable. Decide: continue without SD, or treat as fatal.
}
```

`sdcard_initFilesystem()` mounts the card and creates `folder` if missing.
In the stock template the folder name comes from per-app config
(`ACONFIG_PARAM_FOLDER`, default `/test`) so it can be changed from Booster
without recompiling — hard-code your own string if you prefer.

### Then just use FatFs

```c
#include "ff.h"

FIL f;
if (f_open(&f, "/myapp/level1.dat", FA_READ) == FR_OK) {
    UINT n;
    f_read(&f, buf, sizeof(buf), &n);   // n = bytes actually read
    f_close(&f);
}
```

Writing is the same with `FA_WRITE | FA_CREATE_ALWAYS` and `f_write`. Note
**paths are absolute** (`/myapp/...`) — there's no per-app current
directory. Handy helpers in `sdcard.h`: `sdcard_isMounted()`,
`sdcard_dirExist(path)`, `sdcard_ensureFolder(path)` (mkdir-if-missing),
and `sdcard_getMountedInfo(&total_mb, &free_mb)`.

> Keep file I/O **out of the per-frame path** — SPI reads take many
> milliseconds and will blow the ~19 ms VBL budget. Load at startup, or
> stream a little per frame the way `audio.c` reads a `.YMS` in chunks.

---

## 7. Per-app config (`aconfig`)

Each app gets a small key-value store in flash, **editable from the
Booster app without recompiling** — handy for things like a working
folder, a difficulty level, or a default mode. (It's how the SD folder
name above is supplied.)

Declare your defaults in `rp/src/aconfig.c` (`defaultEntries[]`) and the
key name in `rp/src/include/aconfig.h`:

```c
// aconfig.h
#define ACONFIG_PARAM_SPEED "SPEED"

// aconfig.c -- defaultEntries[]
{ACONFIG_PARAM_SPEED, SETTINGS_TYPE_INT, "3"},   // type: _STRING | _INT | _BOOL
```

Read it anywhere after boot (values are always stored as strings):

```c
#include "aconfig.h"
#include "settings.h"

SettingsConfigEntry *e = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_SPEED);
int speed = e ? atoi(e->value) : 3;     // fall back to a default if unset
```

Write + persist (e.g. to save progress):

```c
settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_SPEED, speed);  // or _string / _bool
settings_save(aconfig_getContext(), true);   // true = disable IRQs during the flash write
```

---

## 8. The main loop, in one picture

```
emul_start():
    fb_init(&fb_mode_320x200);   // brings up the framebuffer + Core 1
    audio_init();
    ... mount SD, configure SELECT button ...
    <your init: palette, fonts, state>

    while (true):
        fb_pump_rom3();          // ROM3 ring -> IKBD + VBL frame-sync
        ikbd_pump();             // decode key events
        while ikbd_pop_key(&k):  <handle key>
        <draw your frame into fb_chunked_buffer>
        fb_publish();            // tear-free 50 Hz hand-off to the ST
        audio_render_frame();    // refill the YM buffer
```

You own the `<...>` lines; the rest is the template's plumbing.

---

## Going faster

If a frame gets heavy, the demos are the reference for the RP2040
optimization toolbox: per-file `#pragma GCC optimize("O3")`,
`__not_in_flash_func()` on hot
loops, fixed-point + sin/cos LUTs, the SIO interpolator for texture
addressing, and a dual-core band split via `fb_core1_dispatch()`.

## More docs

- `CLAUDE.md` — architecture deep-dive (the framebuffer pipeline, shared
  region, IKBD/audio internals). The reference for AI-assisted work.
- `programming.md` — shared-region table + budget rules.
- Official build/usage docs:
  <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## License

GPL v3.0 — see [LICENSE](LICENSE).
