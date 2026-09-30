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
- **Sampled sound**: 8-bit at 12.5 kHz through the DMA sound chip on an
  STE or a Mega STE, ~6 kHz 6-bit out the YM2149 elsewhere — the same
  asset plays on both. Or trade frames for sound: the **25 fps profile**
  plays 25 kHz and ~22 kHz (see "Two profiles" in §5).
- **Atari ST keyboard, mouse and joysticks handled on the RP2040** —
  decoded scancodes, mouse movement and stick states delivered straight to
  your app.

> This repo ships with a 4-demo showcase, an input test and an animated menu.
> This guide is about **starting your own app**: what to remove, the API
> you keep, and a minimal example. For the build toolchain and flashing,
> see the official docs:
> <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

---

## 1. Build

```bash
# ./build.sh <board> <build_type> <app_uuid>
#   board:      pico_w
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

The template includes a boot menu with four demos, an input test and two
small games as worked examples. For your own app, remove the menu and wire
your code into the main loop.

**The quick way:** `examples/hello_text/apply.sh` does all of this for you
— it backs up `rp/` to `rp.bak`, deletes the demo/menu files below, and
drops in a minimal `emul.c` + `CMakeLists.txt`. (For a game,
`examples/mini_game/apply.sh` does the same and boots into one of the
games: see "Starting a game" below.) Run it, then build:

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
rp/src/demo_input.c           rp/src/include/cojo_texture.h
rp/src/demo_games.c           rp/src/include/cojo_font.h
                              rp/src/include/diego_sprite.h
                              rp/src/include/uridium_surface.h
```

Keep `tools/png_to_bitmap.py`, `tools/png_to_texture.py` and
`tools/wav_to_ym4.py` — they convert your *own* images and sounds into
headers. The games (`rp/src/game_*.c`) can stay or go.

### Files to **change**

- **`rp/src/CMakeLists.txt`** — remove the seven `demo_*.c` entries from
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
| `ikbd.c/.h` | keyboard events, the mouse and the joysticks (input modes) |

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
void fb_set_copy_mode(uint8_t mode, uint8_t piece); // who copies on the ST
```

`fb_publish()` blocks until the ST has finished blitting the previous
frame, so calling it once per loop naturally paces your app to 50 Hz.

On the ST the 68000 copies each frame to the screen, or, on an STE or a
Mega STE, the blitter, which gives the ST about 1.4 ms of every frame
back. Where the YM plays the sound (a plain ST, a Mega ST fitted with a
blitter) the 68000 copies: the blitter holds the CPU off, Timer-B's samples
come late and the sound gets rough. `fb_set_copy_mode(CART_BLIT_MODE_BLITTER, 0)`
takes the blitter anyway, for an app without sound (about 3 ms more on a
Mega ST); `CART_BLIT_MODE_CPU` keeps the 68000.

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

### Starting a game

**`examples/mini_game/`** boots straight into one of the two games that
come with the template, which are also entries 6 and 7 of the demo menu:
`rp/src/game_arena.c` (the joystick: grab gems, shoot enemies) and
`rp/src/game_zap.c` (the mouse: the left button zaps, the right one takes).
They show what a game needs beyond the section above:

- an input mode, keys held down (the IKBD sends no repeats), and the
  joystick's and the mouse's press latches;
- sprites drawn again only where they moved, over a tiled arena
  (`game_kit.c`);
- sprites and tiles drawn as indexed PNGs and converted by
  `tools/png_to_bitmap.py` into `FB_BITMAP`s and a palette;
- a tune and sound effects computed as they play, through
  `audio_set_pcm_callback()`.

`examples/mini_game/README.md` walks through them.

---

## 5. Audio (`audio.h`)

On an STE or a Mega STE the sound goes through the DMA sound chip (8-bit,
12,517 Hz, no interrupt per sample); on a plain ST or Mega ST it goes
through the YM2149 (~6-bit, 5,585 Hz). Those are the 50 fps profile's
rates; the 25 fps profile doubles and quadruples them (below). The template picks the output at
every ST boot, and the RP writes the samples ahead of what the ST plays
from a timer interrupt, so the sound never tears and never repeats,
whatever your frame rate. Your audio is YM volume pairs or 8-bit PCM, and
either plays on both outputs: the RP converts at runtime.

### Loop a baked-in buffer

Convert a `.wav`/`.sam` to a header with `tools/wav_to_ym4.py`, then:

```c
#include "audio_sample.h"   // generates audio_sample_data[]
audio_init();               // once at boot
audio_play_loop(audio_sample_data, sizeof(audio_sample_data));
// ... then call audio_render_frame() once per main-loop iteration.
```

### 8-bit PCM

For a recording at its best on the DMA chip, convert it to signed 8-bit
PCM at 12,517 Hz (`tools/wav_to_ym4.py --mode pcm --target-rate 12517
--header-output my_sound.h my_sound.wav`), or generate PCM live at any
rate; on the YM the RP plays its upper 6 bits through the same table:

```c
#include "my_sound.h"       // audio_sample_data[], AUDIO_SAMPLE_RATE_HZ
audio_play_pcm_loop(audio_sample_data, audio_sample_count, AUDIO_SAMPLE_RATE_HZ);

static void my_pcm(int8_t *buf, uint32_t samples) { /* ... */ }
audio_set_pcm_callback(my_pcm, AUDIO_DMA_RATE_HZ);
```

`audio_prefer_ym(true)` keeps an STE on the YM from its next boot;
`audio_uses_dma()` says which output plays.

### Generate audio live (callback)

For dynamic sound in YM pairs, install a fill callback. The library asks it
for pairs at 5,585 Hz, whatever the output's rate (it resamples them):

```c
static void my_fill(uint8_t *buf, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) buf[i] = next_sample_byte();
}
audio_set_fill_callback(my_fill);   // pass NULL for silence
```

Either way, **`audio_render_frame()` must be called each loop iteration**:
it tops up a small FIFO (4 VBLs of samples, 3 on the YM at 25 fps) that the interrupt plays from.
Your loop may take up to 80 ms between two calls without the sound
noticing; a sample reaches the speaker at most 120 ms after your callback
made it on the YM, about 175 ms on the DMA chip. There's also `audio_play_yms_file(path)` to stream a `.YMS` file
from SD — see §7.

### Two profiles: 50 fps, or 25 fps with rich sound

The ST's time goes to two things: copying your frame to its screen (17.6 ms
for a full screen) and, on a plain ST or Mega ST, playing the sound
sample by sample through Timer-B (about 19 µs a sample). You choose the
trade at compile time with `APP_PROFILE` (`rp/src/include/profile.h`):

| | `PROFILE_50FPS` (default) | `PROFILE_25FPS` |
|---|---|---|
| Frames | every VBL: 50 fps | every second VBL: 25 fps, on every machine |
| YM (plain ST, Mega ST) | 5,585 Hz | 21,943 Hz |
| DMA chip (STE, Mega STE) | 12,517 Hz | 25,033 Hz |
| Good for | arcade games, full-screen effects | story games, laserdisc-style |

```bash
APP_PROFILE=PROFILE_25FPS ./build.sh pico_w release 44444444-4444-4444-8444-444444444444
```

The same ST code serves both: the RP tells the ST the profile at boot.
`fb_publish()` then paces your loop at the profile's frame rate. Measured on
a Mega ST at 25 fps, the full copy and 22 kHz sound leave about 5.5 ms of
each 40 ms frame free, and a mouse moved flat out costs no frame. Your
sources keep their rates: `AUDIO_DMA_RATE_HZ` follows the profile, and YM
pairs and PCM at other rates are resampled. If your game steps its state
once a frame, it moves half as fast at 25 fps: scale by
`PROFILE_VBLS_A_FRAME`, or count time (`time_us_32()`).

---

## 6. Keyboard, mouse and joysticks (`ikbd.h`)

The ST forwards every byte its keyboard processor (the IKBD) sends, and the
RP decodes them: keys arrive as events, the mouse and the joysticks as state
you read once a frame.

### Pick an input mode

Port 0 takes the mouse or joystick 0, never both, so the app picks what the
IKBD reports. The keyboard works in every mode.

| Mode | Port 0 | Port 1 |
| --- | --- | --- |
| `IKBD_INPUT_KEYBOARD` (the default) | — | — |
| `IKBD_INPUT_MOUSE` | mouse | — |
| `IKBD_INPUT_MOUSE_JOY1` | mouse | joystick |
| `IKBD_INPUT_JOYSTICKS` | joystick | joystick |

`ikbd_set_input_mode()` takes a few VBLs: `ikbd_get_live_input_mode()` says
when the ST has sent the IKBD its commands. The mode survives an ST reset
and a keyboard plugged back in; keys held when the keyboard went away are
released.

```c
ikbd_set_input_mode(IKBD_INPUT_MOUSE_JOY1);   // once

// once per frame:
ikbd_mouse_t m;
ikbd_read_mouse(&m);                  // movement since the last read
px += m.dx; py += m.dy;
if (m.pressed & IKBD_MOUSE_LEFT) { /* clicked, even between two reads */ }

ikbd_joystick_t j;
ikbd_read_joystick(1, &j);            // port 1
if (j.state & IKBD_JOY_LEFT) ship_x--;
if (j.pressed & IKBD_JOY_FIRE) { /* fire pressed since the last read */ }
```

What the hardware decides:

- Stick 0's fire is the left mouse button and stick 1's fire the right one:
  the same wires. In `IKBD_INPUT_MOUSE_JOY1` the IKBD reports stick 1's
  fire only as the right button; `ikbd_read_joystick(1)` shows it as fire.
- A mouse left in port 0 in joystick mode moves stick 0.
- The IKBD has no auto-repeat: one press and one release per key.
- `ikbd_send_commands()` sends other IKBD commands, once the mode is live.

Menu entry 5 (the input test) shows all of it live, with the counters.

### What it costs the ST

Every IKBD byte is an interrupt on the ST, 30-40 µs. The full-screen blit
and the audio leave the ST about 0.27 ms of each VBL, so while the mouse
moves fast (up to 9 bytes a VBL: the mouse modes set a threshold of 4 counts
per packet) some blits end after the next VBL and that frame waits one: on a
Mega ST, 50 frames a second at rest, 30-40 while the mouse moves flat out.
The sound stays clean. On an STE or a Mega STE the DMA chip plays the sound
and the blitter copies the frame, and the mouse costs no frames; nor does
it in the 25 fps profile (§5). The ST's stopwatch (`TIME_STUDY` in
`userfw.s`, `tools/dev/swd.py stopwatch`) measures the slack your app
leaves (`tools/dev/README.md`); blitting fewer lines (`FB_COPY_LINES`,
about 88 µs per line) buys more.

---

## 7. SD card (`sdcard.h` + FatFs)

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

## 8. Per-app config (`aconfig`)

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

## 9. The main loop, in one picture

```
emul_start():
    fb_init(&fb_mode_320x200);   // brings up the framebuffer + Core 1
    audio_init();
    ... mount SD, configure SELECT button ...
    <your init: palette, fonts, state>

    while (true):
        fb_pump_rom3();          // ROM3 ring -> IKBD + VBL frame-sync
        ikbd_pump();             // decode keys, mouse, joysticks
        while ikbd_pop_key(&k):  <handle key>
        ikbd_read_mouse(&m);     <move things>   (in a mouse mode)
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

## Testing and debugging

- **Host tests**: `make -C tests/host test` runs the template's pure logic on your computer in a
  second or two: the blits' clipping, the framebuffer layout end to end (what you draw is what
  the ST shows), the ST and RP copies of every shared constant, the audio converter and slices,
  and the IKBD decoder against a simulated keyboard losing bytes. CI runs
  them on every pull request. Add yours as `tests/host/test_*.c` (its first line names the
  firmware sources it compiles) or `test_*.py`.
- **A constant both sides share** (a new block in the cartridge window, a new ROM3 signal) goes
  in `target/atarist/src/inc/sidecart_layout.s` and `rp/src/include/cart_shared.h`, with a row in
  `tests/host/test_layout.py`.
- **With a Raspberry Pi Debug Probe** on the RP's SWD and debug UART, `tools/dev/` builds, flashes
  and verifies from the host, captures the console, reads counters while the app runs, grabs the
  screen as a PNG and types on the ST's keyboard: see `tools/dev/README.md`.

## More docs

- `CLAUDE.md` — architecture deep-dive (the framebuffer pipeline, shared
  region, IKBD/audio internals). The reference for AI-assisted work.
- `programming.md` — shared-region table + budget rules.
- Official build/usage docs:
  <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## License

GPL v3.0 — see [LICENSE](LICENSE).
