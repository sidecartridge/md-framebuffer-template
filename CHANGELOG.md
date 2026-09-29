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

### For developers

- `tools/dev/`: build, flash and verify the RP from the host through a Raspberry Pi Debug Probe,
  capture its debug console, read its counters and heap while it runs, grab the framebuffer as a
  PNG (what the ST shows, one frame or two consecutive ones), press SELECT, and explain a crash
  or halt it for a postmortem. In debug builds a mailbox lets the host type on the ST's keyboard
  and send the app commands (`devhooks.h`). `tools/dev/README.md` describes them.
- Host tests: `make -C tests/host test` checks the pure logic on the host, and CI runs it on
  every pull request: the blits' clipping, the framebuffer layout from the chunky buffer to
  the ST's screen, the agreement of the ST's and the RP's copies of every shared constant, and
  the sample converter.
- The cartridge window's layout lives in one m68k include, `inc/sidecart_layout.s`, which
  `main.s` and `userfw.s` share; `userfw.s` no longer repeats its addresses.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
