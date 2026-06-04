# Example: hello_text

The smallest "real app" on this template — bounces **"HELLO ATARI ST"**
around the 320×200 colour screen (with the baked-in jingle playing). It's
the template with the demos stripped out, so it shows the minimal shape:
clear → draw → animate → `fb_publish()`.

## What's here

- **`emul.c`** — a complete, drop-in replacement for `rp/src/emul.c`: the
  template's boot sequence, then a tiny render loop. Everything above the
  `--- app state ---` marker is unchanged from the template; below it is
  the whole app.

## Build it

From a fresh checkout of the template:

1. **Delete the demo files:**
   ```bash
   rm rp/src/demo_*.c \
      rp/src/include/{demo,sidecart_logo,sidecart_text,solid3d,sprites_data,cojo_texture,cojo_font,diego_sprite,uridium_surface}.h
   ```
2. **Drop the demos from the build** — in `rp/src/CMakeLists.txt`, remove
   the five `demo_*.c` lines from `target_sources(...)` (you can also drop
   `hardware_interp` from `target_link_libraries`).
3. **Use this app's `emul.c`:**
   ```bash
   cp examples/hello_text/emul.c rp/src/emul.c
   ```
4. **Build** (the dev UUID, or your own from `desc/app.json`):
   ```bash
   ./build.sh pico_w release 44444444-4444-4444-8444-444444444444
   ```
5. **Flash** `dist/<uuid>-<version>.uf2` to the Pico.

You should see the string bouncing around the screen at 50 Hz.

## Make it yours

- Replace the loop body with your own drawing — `fb_blit`, `fb_fill_rect`,
  direct `fb_chunked_buffer[y * FB_CHUNKED_W + x] = idx`, palette changes.
- Swap the audio (`audio_play_loop` / `audio_set_fill_callback`), or delete
  the two `audio_play_loop` / `audio_render_frame`-adjacent lines for
  silence.
- Full API in the repo `README.md`; architecture in `CLAUDE.md`; the
  `framebuffer-app` Claude skill (`.claude/skills/`) can drive the work.
