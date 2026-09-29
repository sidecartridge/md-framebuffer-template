# Changelog

## v1.1.0 (unreleased)

### For developers

- `tools/dev/`: build, flash and verify the RP from the host through a Raspberry Pi Debug Probe,
  capture its debug console, read its counters and heap while it runs, grab the framebuffer as a
  PNG (what the ST shows, one frame or two consecutive ones), press SELECT, and explain a crash
  or halt it for a postmortem. In debug builds a mailbox lets the host type on the ST's keyboard
  and send the app commands (`devhooks.h`). `tools/dev/README.md` describes them.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
