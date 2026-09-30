# Example: mini_game

A firmware that boots straight into a game, with the demo menu stripped
out. It is the place to start a game of your own.

Two small games come with the template, in `rp/src`. Both are entries 6
and 7 of the demo menu, and this example runs either of them on its own:

- **Arena** (`game_arena.c`), the joystick game. Steer with joystick 1 or
  the cursor keys; fire (or space) shoots the way you last moved. Grab the
  gems (+10), shoot the enemies (+5; they come back 3 s later) and stay away
  from them: three lives. Every five gems another enemy joins.
- **Zap** (`game_zap.c`), the mouse game. Enemies and gems wander the arena.
  The left button zaps an enemy under the crosshair and the right one takes
  a gem (+10 each); the wrong button costs 5. Sixty seconds a round. The HUD
  lights **L** and **R** while the buttons are held.

Both games use only the public APIs (`fb_*`, `font_*`, `palette_*`,
`ikbd_*`, `audio_*`) and share `game_kit.c`: the tiled arena, sprites drawn
over it and restored, the HUD, a tune and sound effects. The tune and the
effects are computed as the sound plays, and the art is original.

## What's here

- **`emul.c`**: a drop-in replacement for `rp/src/emul.c`. It keeps the
  template's boot sequence and replaces the demo menu with a loop that runs
  Arena. To run Zap instead, change `arena_start` / `arena_key` /
  `arena_frame` to `zap_start` / `zap_key` / `zap_frame`.
- **`CMakeLists.txt`**: a copy of `rp/src/CMakeLists.txt` without the
  `demo_*.c` sources and the `hardware_interp` link. The games stay.
- **`apply.sh`**: backs up `rp/` to `rp.bak`, then removes the demos and
  installs the two files above.

## Build it

```bash
examples/mini_game/apply.sh     # backs up rp/ -> rp.bak, then customizes rp/
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
# flash dist/<uuid>-<version>.uf2 to the Pico
```

To undo it: `rm -rf rp && mv rp.bak rp`. `apply.sh` refuses to overwrite an
existing `rp.bak`.

Plug a joystick into port 1 (the port the mouse does not use). ESC leaves
to GEM, and an ST reset starts a new game.

## How a game is built

- **Start**: `arena_start()` sets the palette, the input mode
  (`ikbd_set_input_mode(IKBD_INPUT_JOYSTICKS)`; Zap uses
  `IKBD_INPUT_MOUSE`) and the sound (`audio_set_pcm_callback()`), then
  draws the whole arena once.
- **Keys**: every key event goes to `arena_key()`. The IKBD reports a press
  and a release, never a repeat, so the game keeps a `held[]` table for the
  cursor keys.
- **A frame**: `arena_frame()` reads the joystick
  (`ikbd_read_joystick(1, ...)`), moves everything, draws, and ends with
  `fb_publish()`, which paces the loop to the ST's 50 Hz. Fire is the
  joystick's `pressed` latch: a tap between two frames is never missed. Zap
  reads the mouse the same way (`ikbd_read_mouse()`), and it counts its
  round in time, not frames, because moving the mouse fast can cost the ST
  frames.
- **Only what moved is drawn again**: `kit_begin_frame()` puts the floor
  back under what the last frame drew, then the game draws its sprites
  where they are now. That is a few tile blits per sprite instead of the
  240 tiles of the whole arena. The HUD is redrawn only when it changes.
- **The art** is indexed PNGs in `rp/src/assets` that share one 16-colour
  palette. `rp/src/assets/convert.sh` turns them into
  `rp/src/include/art_*.h` with `tools/png_to_bitmap.py`: an `FB_BITMAP` per
  image (or per tile of a sheet), the palette as ST colour words for
  `palette_set()`, and the transparent colour as the key for
  `fb_blit_key()`. Draw yours in any editor that saves indexed PNGs
  (Aseprite, GIMP, GrafX2).
- **D** toggles the timing readout: the frame's drawing time and the
  publish's conversion time, in microseconds.

Debug builds of Arena have knobs for measuring the scene: keypad **+** and
**-** add or remove an enemy, **S** doubles the enemies' size, **F**
freezes everything, **R** redraws the whole arena once and **A** every
second. A script sends them with `tools/dev/swd.py key` (keypad + is
`0x4E`, keypad - is `0x4A`).
