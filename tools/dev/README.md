# Developer tools

Host-side tools for working on this firmware with the hardware attached: a SidecarTridge
Multi-device on an Atari ST, with a Raspberry Pi Debug Probe wired to the RP2040's SWD pins and to
its debug UART (GPIO 0/1). They build, flash and verify the RP, capture its console, read it while
it runs, grab what the ST shows and drive the app, all from the host, without touching the ST.
Python tools use the standard library only.

## Firmware support these tools rely on

- Debug builds run the console at 921,600 baud (`PICO_DEFAULT_UART_BAUD_RATE` in
  `rp/src/CMakeLists.txt`); release builds have no console at all.
- `rp.elf` keeps its symbol table (the link does not strip it): most commands find their addresses
  by symbol. The symbols never reach the `.uf2`.
- Every build carries its build ID in flash as `release_build_id` (`rp/src/build_id.cmake`).
- The counters `swd.py counters` reads are plain variables: `stSessionHellos` (st_session.c),
  `fb_frame_tick`, `s_vbl_seen` and `fbAckTimeouts` (fb.c), `ikbdOverruns`, `ikbdBytes`,
  `ikbdMousePackets`, `ikbdJoystickPackets`, `ikbdResyncs`, `ikbdCountMismatches` and
  `ikbdPowerUps` (ikbd.c), `commOverruns` (commemul.c), and `audioSlicesWritten`,
  `audioLateSlices`, `audioUnderruns` and `audioOutput` (audio.c), and `stSessionFeatures`
  (st_session.c: 1 when the ST has a blitter). Debug builds also keep `ikbdLog` /
  `ikbdLogCount` (ikbd.c, for `ikbd-log`) and, when `userfw.s` is built with `TIME_STUDY = 1`,
  the ST's stopwatch points (`fbStopwatch`, `fbStopwatchCount`, for `stopwatch`) and the slack
  of each frame's copy before the VBL that may start the next (`fbSlackHist`, `fbSlackMinUs`,
  `fbSlackLate` in fb.c).
- `stopwatch [--seconds S]` (debug builds, `TIME_STUDY = 1` in `userfw.s`): the VBL's period,
  when the ST's loop wakes, starts and ends a frame's copy and goes idle, each after its VBL,
  the copy's length and the frames a second, in microseconds (4.07 µs ticks of MFP Timer-A on
  one timeline). A 25 fps build (`APP_PROFILE=PROFILE_25FPS tools/dev/flash.sh debug`) builds in
  its own folder and its ID ends in `+25fps`.
- Debug builds carry the devhooks mailbox (`rp/src/include/devhooks.h`, included once from
  `emul.c`, served by `devhooks_poll()` in the main loop), which `key` and `app` write.

## Three rules

- **Reset through the watchdog, never with OpenOCD's `reset`.** OpenOCD's multi-core reset touches
  core 1 just after it starts. Here core 1 is the chunky-to-planar worker from the moment
  `fb_init()` launches it, and core 0 waits for it at every publish: a core 1 disturbed at start
  hangs the app. `swd.py reset` and `program` restart both cores together, as a power-on does.
- **Flash through the probe only with both cores halted and every PIO state machine and DMA
  channel stopped** (`swd.py program`, which `flash.sh` uses). Halting the cores does not stop the
  DMA: while the ST touches the cartridge, the ROM3 capture ring keeps writing bus samples into
  RAM, where the flash write stages its data. Stopping the bus ends the ST's session: after a
  flash, reset the ST to run the app again.
- **Halting the RP stops its code, not its PIO and DMA.** A `postmortem` or a frame grab stops
  core 0 (a postmortem also core 1); the cartridge keeps answering, but nothing on the RP updates
  the window until it resumes: no new frame, and the sound repeats what is in its buffer. Reads
  without a halt (`counters`, `heap`, `shared`, `crash`) change nothing.

## Debug console: `console.py`

Captures the debug console of a `debug` build (921,600 baud) to `tools/dev/logs/console.log`, with
a timestamp on every line, and shows it in the terminal. Use it instead of a serial terminal such
as CoolTerm: only one program can open the port.

```bash
python3 tools/dev/console.py watch          # leave running in a terminal
```

`watch` finds the Debug Probe by its USB name (`--port` to choose another device), waits for it when
it is unplugged, and reopens it when it returns. While it runs, other commands read the log:

```bash
python3 tools/dev/console.py since-boot                     # everything since the last boot
python3 tools/dev/console.py since-boot --boot 2            # the boot before that
python3 tools/dev/console.py tail 100
python3 tools/dev/console.py grep 'ST hello' --since-boot
python3 tools/dev/console.py wait 'Entering main loop' --timeout 30
```

`grep` and `wait` take Python regular expressions and exit with 3 when nothing matches. `wait` only
matches lines that arrive after it starts, so start it before the action that should print the
line. The log rotates to `console.log.1` at 32 MB.

## Build, flash and verify: `flash.sh`

```bash
tools/dev/flash.sh debug                  # build, flash, check over SWD
tools/dev/flash.sh release --probe        # flash through the Debug Probe without trying picotool
tools/dev/flash.sh debug --build-only     # build only
tools/dev/flash.sh debug --src /tmp/src   # build another copy of rp/src (an example applied, a patch)
```

Builds out of tree in `tools/dev/builds/<type>`, incrementally, with the settings of the
`pico_w-<type>` preset in `rp/src/CMakePresets.json` (CMake Release, `DEBUG_MODE`). It does not
touch `rp/build-*` or the submodules, and warns when a submodule is not at the version
`rp/build.sh` pins. `RP_CMAKE_BUILD_TYPE` overrides the CMake type, with a warning, in a folder of
its own. The m68k image is not rebuilt: after changing `target/atarist/`, run
`target/atarist/build.sh` first (it regenerates `rp/src/include/target_firmware.h`); `flash.sh`
warns when the m68k sources are newer than that header. Builds are dated by the HEAD commit
(`RELEASE_DATE`), so two builds of one tree are byte-identical.

The build ID is the git commit, `<sha7>`, or `<sha7>-dirty.<diff7>` when the tree has uncommitted
changes, followed by `+debug` in a debug build, so a debug and a release build of one tree never
share an ID. `rp.elf` is kept as `tools/dev/builds/elf/<type>-<id>.elf`: the `swd.py` commands
that need symbols find the ELF whose build ID the RP carries there.

Flashing tries `picotool load -f -x` and falls back to the Debug Probe when picotool cannot see
the RP (it needs the RP's USB). Then `flash.sh` checks the result over SWD with `swd.py`: the RP
booted the ELF, its flash matches the ELF byte for byte, and it carries the new build ID. On
failure it exits with 1 and prints the console since the last boot.

## Debug probe: `swd.py`

The tools talk to the RP only through picotool, the Debug Probe and the console UART, never through
the firmware's own services, so they work on a release build as on a debug one, and on a hung RP.

```bash
python3 tools/dev/swd.py running tools/dev/builds/debug/rp.elf   # booted this firmware?
python3 tools/dev/swd.py verify tools/dev/builds/debug/rp.elf    # flash identical to the ELF?
python3 tools/dev/swd.py build-id                                # which build is on the RP?
python3 tools/dev/swd.py counters --watch 2                      # frames, blits, overruns, per second
python3 tools/dev/swd.py ikbd-log ikbd.txt --seconds 60          # record the IKBD stream (debug)
python3 tools/dev/swd.py heap --watch 5 --csv tools/dev/logs/heap.csv
python3 tools/dev/swd.py shared                                  # the window's shared block
python3 tools/dev/swd.py fb screen.png                           # what the ST shows, as a PNG
python3 tools/dev/swd.py fb frame.png --pair --raw               # two consecutive frames + planar bytes
python3 tools/dev/swd.py key down down return                    # type on the ST's keyboard (debug)
python3 tools/dev/swd.py app demo 3                              # a demo dispatcher command (debug)
python3 tools/dev/swd.py select short                            # press SELECT: the RP restarts
python3 tools/dev/swd.py crash                                   # why did it last reboot?
python3 tools/dev/swd.py postmortem                              # halt, backtraces, resume
python3 tools/dev/swd.py read 0x20030000 64 window.bin           # dump memory
python3 tools/dev/swd.py program tools/dev/builds/debug/rp.elf   # flash through the probe
python3 tools/dev/swd.py reset                                   # reset the whole chip, watchdog-style
python3 tools/dev/swd.py resume                                  # release cores a debugger left halted
```

`counters` reads, without halting: the ST's hellos, the frames published, the blits the ST
acknowledged, the publishes that gave up waiting for an acknowledgement (`fbAckTimeouts`: expected
until the ST runs the app, never while it does), the keyboard ACIA's overruns, the IKBD bytes and
the mouse and joystick packets decoded, the decoder's resyncs and how many were byte-count
mismatches, the IKBD's restarts (a keyboard plugged back in), the ROM3 ring's overruns, the
VBLs of sound written, late (`audioLateSlices`) and underrun (`audioUnderruns`), and the audio
output (`audioOutput`: 1 the YM, 2 the STE's DMA chip). `--watch SECONDS`
prints what changed, with the frame, blit, audio slice and IKBD byte rates: frames and blits run
at 50 a second while the ST runs the app and the app publishes every frame (a mouse moved fast
costs the ST some: see CLAUDE.md, "The ST's budget"); audio slices run at 50 a second whatever the
frame rate.

`ikbd-log OUT` (debug builds) records every sample the IKBD decoder sees, in order, for
`--seconds` (Ctrl-C ends it early): one 16-bit sample per line, the ROM3 window in the high byte
(`82` an IKBD byte, `83` the ST's byte count at a VBL, `85` an ACIA overrun, `87` a command string
sent, `88` a hello, `01` a byte a host tool typed) and the value in the low byte. It reads the
firmware's 2,048-sample log several times a second and says how many samples it missed; at the
end it checks the ST's counts against the bytes between them, without the decoder. A script can
record while it drives the app with the `IkbdLog` class, as long as only one OpenOCD runs at a
time.

`fb` writes the framebuffer as the ST shows it, in colour: it undoes the reversed 48-byte chunks
the m68k's MOVEM blit needs, decodes the low-resolution planes and applies the published palette.
A dump takes longer than a frame, so it waits for a complete one: a watchpoint on the frame
counter, which `fb_publish()` writes last, stops core 0 until the dump is done (about 0.1 s). Core
1 and the ST keep running. `--pair` writes two consecutive frames (`OUT-1`, `OUT-2`), `--raw` also
the planar bytes in the ST's order, and `--now` reads the window as it is, for an app that no longer
publishes.

`shared` prints the command sentinel (as the ST's `move.l` reads it), the frame counter's low word,
the shared-variable slots (named by any `*_SVAR_*` define in `rp/src/include`), the palette, the
head of the audio buffer and the boot status with its message. The offsets come from
`cart_shared.h`.

`key` and `app` need a `debug` build. They write the devhooks mailbox (found by its
`devhooksMailbox` symbol) and wait for the main loop to acknowledge it. `key` types on the ST's
keyboard: each key, a scancode (`0x02`) or a name (`esc`, `return`, `space`, `up`, `down`, `left`,
`right`, `1`-`0`, `a`-`z`, `f1`-`f10`...), is pressed and released (`--press` / `--release` for one
half), and its bytes enter with the ST's own (outside the ST's byte count), so the app cannot tell
the difference. `app NAME [WORD]`
runs the command defined as `DEVHOOKS_APP_<NAME>` in `rp/src/include`; the demo dispatcher
(`demo.h`, `demo_dispatcher_devhook()`) has:

- `demo N`: launch menu entry N (1-4 the demos, 5 the input test, 6 Arena, 7 Zap); `menu`: back
  to the menu. In debug builds Arena takes knobs as keys, for scripts: `key 0x4E` / `key 0x4A`
  (keypad + / -) add or remove an enemy, `key s` doubles their size, `key f` freezes the game,
  `key r` / `key a` redraw the arena once / every second.
- `overlay 0|1`: the DRAW/C2P readout (the hidden `D` key).
- `slow_frame MS`: every frame takes MS milliseconds longer (0 stops it): an app late with its
  frames, on demand. The sound and the publish handshake must survive it.
- `input_mode N`: the input mode (0 keyboard, 1 mouse, 2 mouse + joystick 1, 3 joysticks), in any
  demo.
- `ikbd_cmd BYTE...`: IKBD command bytes (at most 12; `0` waits a VBL), sent by the ST one per
  VBL: to try what an IKBD does, e.g. `ikbd_cmd 0x16` asks for both sticks' state.
- `audio_out 0|1`: from the ST's next boot, the DMA chip where there is one (0) or the YM (1):
  both outputs on one STE with a reset in between.
- `tone HZ`: a sine through the PCM path, whose clicks are easy to hear; `tone 0` goes back to
  `DEMO.YMS`.
- `copy_mode MODE [PIECE]`: who copies the frame on the ST, from its next VBL (0 auto: the
  blitter on the DMA sound path; 1 the CPU; 2 the blitter), and the blitter's chunks per piece
  (0: 40). With the stopwatch on, the slack histogram shows what each one leaves.

An app adds its own the same way: a `DEVHOOKS_APP_<NAME>` define and a handler set with
`devhooks_setAppHandler()`.

`select` needs no firmware code: it forces the SELECT pin's input high through the RP2040's GPIO
input override for 300 ms (`short`: the RP restarts) or `SELECT_LONG_RESET` + 1 s (`long`, which
needs `--force`: it is a factory reset that erases the global settings, and Booster then clears
every app's settings). `select release` clears an override left behind.

`crash` prints the watchdog reason and scratch registers of the last reboot without stopping the
RP, with code addresses resolved to source lines by `addr2line`. This firmware leaves nothing in
the scratch registers today.

`postmortem` halts the RP and prints both cores' backtraces (core 1 is the chunky-to-planar worker,
parked in its RAM loop between jobs), the registers, the watchdog registers and the counters
through GDB (`$ARM_GDB_PATH/bin/arm-none-eabi-gdb`), then resumes it; `--leave-halted` keeps it
stopped for `swd.py resume`. It runs without GDB's memory map: to build one, OpenOCD probes the
flash size on connect with a routine in its work area, and core 0 then resumed with its main stack
pointer inside that area and locked up.

`heap` reads newlib's own malloc state while the RP runs: the heap's size (from the end of `.bss`
to `__StackLimit`, where the cartridge window starts), the arena taken so far, the **peak** arena
ever reached with how close that came to the window, and, by walking the heap's chunks, the bytes
in use, the free bytes in the arena, in how many blocks, and the largest. `--watch SECONDS` samples
until Ctrl-C; `--csv FILE` appends every sample.

OpenOCD loads small routines into a RAM work area (`verify_image`'s CRC). `rp2040.cfg` puts it at
`0x20010000`, inside this firmware's live RAM; `swd.py` gives every run that does not write flash
2 KB at the bottom of `SCRATCH_X` instead, backed up and restored. That is core 1's 4 KB stack, of
which core 1 uses about 100 bytes at the top, and OpenOCD runs routines only with the cores
halted. Flash writes keep the default, with the cores halted and a reset after.

OpenOCD is `$OPENOCD`, `openocd` on `PATH`, or `../pico/openocd/src/openocd`; its scripts come
from `$PICO_OPENOCD_PATH`. A command that fails on a momentary debug-port drop (common while the
firmware changes its clock early in boot) is retried. Close a VS Code debug session first: only one
program can use the probe.

## Hardware harness: `tools_harness.py`

```bash
python3 tools/dev/tools_harness.py --build --flash --reset   # every tool above, against the device
```

Runs every tool against a debug build on the RP and prints PASS or FAIL for each check: the running
firmware and its build ID, the console, `counters`, `heap`, `shared`, the mailbox (`key` into a
demo and back, `app demo`), `fb` and `fb --pair`, `slow_frame` (the publish rate drops and comes
back), `crash`, `postmortem` (and that the app runs on after it) and a SELECT press, which restarts
the RP. `--build` also builds both types and checks their flags and symbols, `--flash` flashes the
debug build first, and `--reset` restarts the RP at the end and checks core 1 is alive. It needs
`console.py watch` running (it reads its log, never the UART), not the ST. It writes a JSON report
to `logs/` and exits 0 only when every check passes.

## Measuring builds

- `measure_builds.sh [OUT_DIR]` builds each build type out of tree with `-fstack-usage` and
  `-fcallgraph-info=su`, and reports the flash and RAM sections and the heap's room.
- `stackdepth.py BUILD_DIR roots main fb_core1_loop fb_c2p_bottom_job` computes the worst-case
  stack depth below each function from those builds (static call edges only, so treat its answer
  as a floor: core 1's jobs and the demos' function pointers are called indirectly).

`logs/` and `builds/` are generated here and are gitignored.
