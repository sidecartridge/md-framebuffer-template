# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `programming.md` (full shared-region table and budget rules), `README.md` (high-level region/userfw overview), `AGENTS.md` (overlapping playbook + troubleshooting table), `CHANGELOG.md` (fastest way to spot breaking changes between template versions — e.g. v1.1.0's `init_romemul` signature change, v1.2.0's region rearrangement, v1.2.1's seed-must-advance invariant).

## What this repo is

Template for a **Sidecartridge Multi-device microfirmware app** targeting Atari ST / STE / MegaST(E). Each "app" is a UF2 image that runs on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot, emulating a ROM cartridge for the Atari, while also handling SD card I/O and per-app config. The template ships a **32 KB color framebuffer** in the cartridge that the m68k blits to ST screen every VBL — the primary extension point is "draw into the cart FB, the m68k blits it for you". Public build/usage docs are at <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

Network plumbing (WiFi / lwIP / mbedTLS / httpc) was deliberately stripped in Epic 1 / Story 1.1 — apps that need it bring it back from `md-microfirmware-template` upstream.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`):
1. Copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. `gen_fbdrv.py` emits `target/atarist/src/fbdrv.s` (the MOVEM-loop cart→ST screen copy) before vasm runs. Enforces a **16 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 16384`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of the `<build_type>` argument. A full `Release` previously caused breakage (memory/over-optimization). The legacy line is left commented in `rp/build.sh`. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on PR.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release).
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, manual interaction over the serial debug console.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Framebuffer pipeline (the headline feature)

End-to-end, every visible pixel on the ST goes through this path each VBL:

1. **RP draws into a chunked off-screen buffer** at RP `0x20000000+` (`fb_chunked_buffer`, 320×200 bytes = 64 KB, one byte per pixel, palette index in low nibble). `fb_font.c` writes text; `fb_blit.c` writes rectangles / bitmaps / sprites with color-key.
2. **RP publishes via chunky-to-planar conversion** into the cart FB at `$FA8300` (RP `0x20030000 + 0x8300`). The conversion uses a multiplication-based bit transpose in hand-written Thumb assembly (`fb_chunked_asm.S` — `fb_c2p_half`). For each 4-pixel uint32 group, plane K's 4-bit nibble = `((q >> K) & 0x01010101) * 0x80402010 >> 28`. The work is split between **Core 0 (top 100 rows) and Core 1 (bottom 100 rows)**, synced via the inter-core FIFO. Conversion time: ~1 ms / frame, content-independent.
3. **m68k blits cart FB → ST screen page** inside `userfw.s`'s VBL loop. Two paths, picked once per boot by walking the `_MCH` cookie:
   - Plain ST (`D7=0`): `jsr fbdrv` — the unrolled MOVEM-loop in `target/atarist/src/fbdrv.s` (generated by `gen_fbdrv.py`).
   - STE-class (`D7=1`, includes MegaSTE / TT / Falcon): **inline** STE blitter in HOG mode, set up directly in `userfw.s`. Do **not** factor this into a cart-ROM subroutine — `jsr`-then-`rts` after HOG completion fetches garbage from the cart bus and bombs the m68k with "Illegal Instruction" (4 bombs). See `.claude/projects/.../memory/project_blitter_inline.md`.
4. **m68k flips video base** to the page it just wrote. `userfw.s` toggles `A4` between `$70000` and `$78000` every frame, so the display always reads from a stable page while the next blit fills the other.
5. **PALETTE_IDX0 (`$FFFF8240`) doubles as a timing tape-measure** — `userfw.s` writes it at three points (post-vsync, blit-start, blit-end) so the ST border shows the blit cost as colored bands. White band = blit in flight (≈4 ms STE, ≈10 ms ST). Green/blue band = slack until next vsync.

The number of lines the STE blitter copies is parameterized by `FB_COPY_LINES` (default 192, full screen would be 200). Leaving a few rows untouched lets an app keep a static status bar in the destination ST page that the FB blit never overwrites.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge header + boot dispatcher. Lives at `$FA0000` (ROM4 cartridge region). `pre_auto` (CA_INIT bit 27) runs in supervisor mode, copies `start_rom_code` below ST screen memory, jumps there, checks resolution (high-res falls to GEM with a warning), then **`jmp USERFW` directly** — no terminal command dispatch in the default path. The old `check_commands` / `rom_function` indirection is still defined (for apps that want CMD_START-style handoff from the RP) but unused.
- **No Atari RAM**: the cartridge code deliberately uses none of the ST's RAM (no `.bss`, no heap). Data lives in registers or the shared region. Adding naive BSS to `userfw.s` will silently corrupt system variables — keep state in `APP_FREE` / `SHARED_VARIABLES`, the cart code itself (which is **read-only** from m68k — writes bus-error), or registers. The `userfw.s` VBL loop runs entirely out of registers (D6 = saved screen base, D7 = MCH flag, A4 = hidden screen).
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (2 KB budget), `userfw.s` at offset `0x0800` (6 KB budget), and the generated `fbdrv.s` at offset `0x2000` (~8 KB MOVEM-loop). Total cart code ≤ 16 KB (`CARTRIDGE_CODE_SIZE`). `main.s` exposes the userfw entry as `USERFW equ (ROM4_ADDR + $800)`. The default `userfw.s` runs the FB blit VBL loop and exits on ESC; replace its body with your own per-frame logic, or extend it.
- Adding more m68k modules: add a new `.text_<name>` section in `userfw.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `main.s`, and add the `.o` target to `target/atarist/Makefile`.
- Built via `stcmd make release` (m68k assembler in Docker). A 64 KB padded copy of the BOOT.BIN is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 16 KB | m68k header + main.s (≤ 2 KB) + userfw.s (≤ 6 KB) + generated fbdrv.s (≤ 8 KB). Read-only from m68k. |
| `$FA4000` | `CMD_MAGIC_SENTINEL_ADDR` | 4 B | RP→m68k command word (`CMD_NOP`, `CMD_RESET`, `CMD_BOOT_GEM`, `CMD_START`). |
| `$FA4004` | `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, reserved, 60 × 4 B indexed shared variables | 256 B | fixed-offset metadata block until `$FA4100`. |
| `$FA4100` | `APP_FREE_ADDR` | ~16.5 KB | contiguous arena for app buffers, ends at FRAMEBUFFER. |
| `$FA8300` | `FRAMEBUFFER_ADDR` | 32000 B | 320×200×4bpp color framebuffer. Top of the region so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block. Read by `fbdrv` / STE blitter every VBL. |

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. The trimmed template version is ~97 lines: erase + copy firmware to RAM, init romemul, init fb, init commemul / chandler, mount SD, configure SELECT, then loop `{chandler_loop(); fb_render_frame(); sleep_ms(N);}`. Apps add foreground work inside that loop.
- `fb.c` / `fb.h` — **owns the 32 KB planar framebuffer at `$FA8300` AND the demo render loop.** `fb_init(&fb_mode_320x200)` populates `fb_screen` (pointer + width + height + bpp), calls `fb_chunked_init()` (which launches Core 1), builds the demo sprite, clears the planar FB to 0xFF (= palette index 15 = black on default TOS palette), and renders the initial frame. `fb_render_static()` draws the title + ESC hint; `fb_render_frame()` clears the chunked buffer + composes static + dynamic content + calls `fb_chunky_to_planar` to publish.
- `fb_chunked.c` / `fb_chunked.h` — **chunked off-screen buffer + dual-core publish dispatcher.** `fb_chunked_buffer[320 * 200]` is the byte-per-pixel render target apps draw into. `fb_chunked_init()` launches Core 1 with `fb_c2p_core1_loop` which parks on the inter-core FIFO waiting for each frame's bottom-half dst pointer. `fb_chunky_to_planar(planar)` pushes the bottom-half dst, runs the top half on Core 0, then waits for Core 1's completion signal.
- `fb_chunked_asm.S` — **hand-written Thumb assembly worker.** `fb_c2p_half(dst, src, src_end)` processes a chunked range using a multiplication-based bit transpose (`((q & 0x01010101) * 0x80402010) >> 28` packs a 4-pixel plane bit into a nibble in one MUL). Both cores execute this same code on their respective halves. No LUTs in scratch; only two 32-bit constants in registers.
- `fb_font.c` / `fb_font.h` — text primitives. `font_set_font(&font6x8)`, `font_set_color(0)` for palette-index-0 (white on default TOS), `font_move(x, y)`, `font_print(str)`. `render_text` writes single bytes into `fb_chunked_buffer` (no LUT, no plane math) so the cart-bus byte-swap is irrelevant at the font layer.
- `fb_blit.c` / `fb_blit.h` — bitmap / sprite primitives. `fb_fill_rect(x, y, w, h, color)`, `fb_blit(bm, dst_x, dst_y)` (opaque memcpy per row), `fb_blit_key(bm, dst_x, dst_y, key)` (color-key transparent sprite). All target the chunked buffer.
- `font6x8.h` — 6×8 ASCII font data + the `FB_FONT font6x8` instance definition. Include from exactly one .c file (currently `fb.c`) to avoid multiple-definition link errors.
- `romemul.c` / `romemul.pio` — PIO programs and the runtime that emulates the cartridge ROM/RAM bus to the Atari (driven by `READ_*` / `WRITE_*` GPIOs defined in `include/constants.h`).
- `commemul.c` / `commemul.pio`, `chandler.c` — **polled command channel; the primary extension point for adding new commands.** `commemul` is a PIO+DMA ring that captures every ROM3 access into a 32 KB ring buffer with no IRQs; `chandler_loop()` drains the ring, parses each command via `tprotocol_parse`, and dispatches to callbacks registered with `chandler_addCB(cb)`. Callback signature: `void cb(TransmissionProtocol *protocol, uint16_t *payloadPtr)` — `payloadPtr` is past the random-token prefix; read parameters with the `TPROTO_GET_*` macros. After dispatch, `chandler` writes the random-token reply into shared memory so the m68k side detects the ack. The template registers no callbacks by default.
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store). The global-config defaults in `gconfig.c` mirror the Booster app and include WiFi parameters even though this template no longer brings up networking — **do not trim them**.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `select.c`, `reset.c`, `tprotocol.c` — SELECT-button handling, soft reset/jump-to-booster, transport protocol primitives.

The `display.c` / `term.c` / `u8g2/` subsystem from upstream `md-microfirmware-template` was deleted in Story 1.2.12 — the framebuffer template's UI is just "draw into `fb_screen`".

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 128 K | Normal RAM |
| `ROM_IN_RAM` | `0x20020000` | 128 K | ROM data mirrored to RAM for fast bus access |

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). The PIO bus emulation runs hot — Core 0 also overclocks to 225 MHz at `VREG_VOLTAGE_1_10`. **Core 1 is owned by `fb_c2p_core1_loop`** (the chunky-to-planar bottom-half worker, see `fb_chunked.c`); apps that want Core 1 for other purposes need to replace that worker or refactor the conversion pipeline.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy` — both wired up via CMake when the binaries are on `PATH`).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
