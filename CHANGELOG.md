# Changelog

## v1.1.0 (unreleased)

### Sound

- The sound never tears and never repeats. The ST plays one VBL of samples per slice of
  the audio buffer and tells the RP which slice it plays; the RP writes only the slices
  ahead, from a timer interrupt, so a slow frame no longer holds the sound back. Before,
  the ST replayed a stale 20 ms of sound about four times a second even at 50 frames a
  second, and the last sample or two of every frame came from the next one.
- An app may take up to 80 ms between two calls to `audio_render_frame()`; a sample
  reaches the speaker at most 120 ms after its fill callback made it
  (`AUDIO_FIFO_SLICES`). When nothing is ready the last sample is held, never stale
  sound, and `audioUnderruns` counts it.

### STE DMA sound

- On an STE or a Mega STE the sound plays through the DMA sound chip: 8-bit at 12,517 Hz, no
  interrupt per sample. The ST copies each VBL's samples into a 2 KB ring in its RAM (below the
  screen pages); Timer-B stays off, which gives the ST 1.6 ms of every frame back: a mouse moved
  fast no longer costs frames there. A plain ST or Mega ST plays through the YM as before, from
  the same firmware.
- Apps give 8-bit PCM at any rate (`audio_play_pcm_loop()`, `audio_set_pcm_callback()`) or YM
  pairs as before; either plays on both outputs, converted on the RP. `audio_prefer_ym()` keeps
  an STE on the YM; `tools/wav_to_ym4.py --mode pcm` writes PCM headers.
- The audio buffer in the cartridge window grows to 2 KB: `APP_FREE` starts at `$FA4980`
  (about 14.4 KB).

### Blitter

- On an STE or a Mega STE the blitter copies each frame to the screen instead of the 68000,
  which gives the ST about 1.4 ms of every frame back (measured on a Mega STE). Where the YM plays the sound (a plain
  ST, a Mega ST fitted with a blitter) the 68000 still copies: the blitter holds the CPU off,
  and the YM's samples come late and sound rough. `fb_set_copy_mode()` picks either; the
  blitter on a Mega ST gives an app without sound about 3 ms more.
- `FB_SLACK_REPORT` now measures with MFP Timer-A, on both sound paths, and reports one byte
  (`$FB8Bxx`, in 81 µs steps). The ST reports whether it has a blitter
  (`st_session_features()`).

### Mouse and joysticks

- Apps get the mouse and both joysticks next to the keyboard, decoded on the RP: pick an
  input mode with `ikbd_set_input_mode()` (keyboard only, the default; mouse; mouse and
  joystick 1; two joysticks) and read `ikbd_read_mouse()` / `ikbd_read_joystick()` once a
  frame. Clicks and fire presses shorter than a frame are never missed. The mode survives an
  ST reset and a keyboard unplugged and plugged back in; keys held when it went away are
  released.
- The RP now sends the IKBD its commands: the ST passes on whatever the RP puts in the
  cartridge window (`ikbd_send_commands()` for other settings). At boot the ST no longer
  switches the mouse and joysticks off itself.
- Bytes lost between the keyboard and the app are detected (the ST reports how many it read
  every frame) and never turn mouse data into keys.
- A new menu entry, the input test, shows the mouse, both joysticks, the keys and the
  counters of the input path.
- Every IKBD byte costs the ST an interrupt. With the full-screen blit and the sound, a mouse
  moved fast makes the frame rate dip to 30-40 a second; the sound stays clean.
  `FB_SLACK_REPORT` in `userfw.s` measures how much of each frame the ST has left.
- The release of keypad Enter is no longer dropped.

### For developers

- `tools/dev/`: build, flash and verify the RP from the host through a Raspberry Pi Debug Probe,
  capture its debug console, read its counters and heap while it runs, grab the framebuffer as a
  PNG (what the ST shows, one frame or two consecutive ones), press SELECT, and explain a crash
  or halt it for a postmortem. In debug builds a mailbox lets the host type on the ST's keyboard
  and send the app commands (`devhooks.h`), and `swd.py ikbd-log` records the keyboard's byte
  stream. `tools/dev/README.md` describes them.
- Host tests: `make -C tests/host test` checks the pure logic on the host, and CI runs it on
  every pull request: the blits' clipping, the framebuffer layout from the chunky buffer to
  the ST's screen, the agreement of the ST's and the RP's copies of every shared constant, the
  sample converter, the audio slices, the DMA sound output and the IKBD decoder.
- The cartridge window's layout lives in one m68k include, `inc/sidecart_layout.s`, which
  `main.s` and `userfw.s` share; `userfw.s` no longer repeats its addresses.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
