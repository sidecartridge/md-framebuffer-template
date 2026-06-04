# Example: hello_text

The smallest "real app" on this template — bounces **"HELLO ATARI ST"**
around the 320×200 colour screen (with the baked-in jingle playing) and
prints a **`DRAW` / `C2P` microsecond readout** in the top-left, the same
debug numbers the demos show (`DRAW` = time drawing the frame, `C2P` =
the chunky→planar cost of `fb_publish()`). It's the template with the
demos stripped out, so it shows the minimal shape: clear → draw → animate
→ `fb_publish()`.

## What's here

- **`emul.c`** — a drop-in replacement for `rp/src/emul.c`: the template's
  boot sequence, then a tiny render loop. Everything above the
  `--- app state ---` marker is unchanged from the template; below it is
  the whole app.
- **`CMakeLists.txt`** — a copy of `rp/src/CMakeLists.txt` with the five
  `demo_*.c` sources and the `hardware_interp` link removed.
- **`apply.sh`** — backs up `rp/` to `rp.bak`, then customizes `rp/` to
  build this example.

## Build it (the easy way)

```bash
examples/hello_text/apply.sh    # backs up rp/ -> rp.bak, then customizes rp/
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
# flash dist/<uuid>-<version>.uf2 to the Pico
```

`apply.sh` is reversible — it refuses to clobber an existing `rp.bak`, and
to undo everything:

```bash
rm -rf rp && mv rp.bak rp
```

You should see "HELLO ATARI ST" bouncing around the screen at 50 Hz.

<details>
<summary>Or do it by hand</summary>

1. Delete the demo files:
   ```bash
   rm rp/src/demo_*.c \
      rp/src/include/{demo,sidecart_logo,sidecart_text,solid3d,sprites_data,cojo_texture,cojo_font,diego_sprite,uridium_surface}.h
   ```
2. Replace the build config and the app:
   ```bash
   cp examples/hello_text/CMakeLists.txt rp/src/CMakeLists.txt
   cp examples/hello_text/emul.c         rp/src/emul.c
   ```
   (Or just remove the five `demo_*.c` lines from `rp/src/CMakeLists.txt`.)
3. `./build.sh pico_w release <uuid>`.
</details>

## Make it yours

- Replace the loop body with your own drawing — `fb_blit`, `fb_fill_rect`,
  direct `fb_chunked_buffer[y * FB_CHUNKED_W + x] = idx`, palette changes.
- Swap the audio (`audio_play_loop` / `audio_set_fill_callback`), or delete
  the audio lines for silence.
- Full API in the repo `README.md`; architecture in `CLAUDE.md`; the
  `framebuffer-app` Claude skill (`.claude/skills/`) can drive the work.
