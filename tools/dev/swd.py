#!/usr/bin/env python3
"""
swd — read, check and drive a running RP2040 through the Debug Probe.

Single-file, stdlib-only (Python >= 3.10). Runs OpenOCD with the CMSIS-DAP
probe for each command. It never uses the firmware's own services: memory is
read through the debug port while the CPU keeps running, so it works on a
release build as on a debug one, and on a hung RP.

Usage:
    python3 tools/dev/swd.py running ELF [--timeout S]
    python3 tools/dev/swd.py verify ELF
    python3 tools/dev/swd.py build-id [ELF ...]
    python3 tools/dev/swd.py read ADDRESS LENGTH OUTFILE
    python3 tools/dev/swd.py program ELF
    python3 tools/dev/swd.py resume
    python3 tools/dev/swd.py reset
    python3 tools/dev/swd.py counters [--elf ELF] [--watch SECONDS]
    python3 tools/dev/swd.py ikbd-log OUT [--seconds S] [--elf ELF]
    python3 tools/dev/swd.py stopwatch [--seconds S] [--elf ELF]
    python3 tools/dev/swd.py heap [--elf ELF] [--watch SECONDS] [--csv FILE]
    python3 tools/dev/swd.py select short|long|release [--hold-ms MS] [--force]
    python3 tools/dev/swd.py key KEY... [--press | --release] [--elf ELF]
    python3 tools/dev/swd.py app NAME [WORD ...] [--elf ELF]
    python3 tools/dev/swd.py fb OUT.png [--pair] [--raw] [--now] [--scale N]
    python3 tools/dev/swd.py shared [--elf ELF] [--all]
    python3 tools/dev/swd.py crash [--elf ELF]
    python3 tools/dev/swd.py postmortem [--elf ELF] [--leave-halted]

`running` waits until the vector table register (VTOR) holds the ELF's RAM
vector table, which the SDK's runtime init installs, and core 0 is not halted:
the RP has booted a firmware with that layout and got past its startup code.
`resume` releases both cores after a debugger left them halted. `verify`
compares the whole flashed image with the ELF. `build-id` reads the
`release_build_id` string from flash; with no ELF it tries every ELF in
tools/dev/builds/elf. `program` flashes the ELF and resets the RP; `reset` only
resets it. Both reset the whole chip through the watchdog, never with OpenOCD's
`reset` (see chip_reset).

`counters` reads the firmware's counters while it runs: the ST's hellos
(st_session.c), the frames published and the blits the ST acknowledged, the
publishes that gave up waiting for an acknowledgement (fb.c), the keyboard
ACIA's overruns, the IKBD bytes and packets decoded, the decoder's resyncs and
how many of them were byte-count mismatches (ikbd.c, ikbd_demux.h), the ROM3
ring's overruns (commemul.c), and the audio slices written, late and underrun
(audio.c). With --watch it prints what changed every SECONDS, with rates:
while the ST runs userfw and the app publishes every frame, frames and blits
run at 50 a second; audio slices do whatever the frame rate.
`ikbd-log` (debug builds) records every sample the IKBD decoder sees, in
order, for S seconds (Ctrl-C ends it early), into OUT, one 16-bit sample per
line in hex: the ROM3 window in the high byte ($82 an IKBD byte, $83 the ST's
byte count at a VBL, $85 an ACIA overrun, $87 the input mode, $88 a hello;
$01 a byte a host tool typed) and the value in the low byte. It reads the
firmware's log (ikbdLog, 2048 samples, 2.4 s of a mouse moved flat out)
several times a second and says how many samples it missed. At the end it
checks the recording on its own, without the decoder: the IKBD bytes between
two counts must be what the ST says it read.
`heap` reads newlib's malloc state: the heap's size, its peak and the free
space inside it. Without --elf, both use the cached ELF whose build ID the RP
carries (tools/dev/builds/elf, filled by flash.sh).

`select` presses the SELECT button: it forces the pin's input high through the
GPIO input override (IO_BANK0 GPIOn_CTRL.INOVER) for the hold time, so the
firmware sees a real press without any code of its own. `short` holds 300 ms
(the RP restarts), `long` holds SELECT_LONG_RESET + 1 s (rp/src/include/select.h)
and needs `--force`, because it is a factory reset (reset_deviceAndEraseFlash).
`release` clears a stuck override.

`key` and `app` need a debug build: they write the debug mailbox
(rp/src/include/devhooks.h) and wait until the main loop acknowledges it.
`key` types on the ST's keyboard: each KEY (a scancode such as 0x02, or a
name: esc, return, space, up, down, left, right, 1-0, a-z, f1-f10...) is pressed and
released, and the bytes enter where the ST's own do, so the app cannot tell
the difference. `app` sends a command named by a DEVHOOKS_APP_<NAME> define in
rp/src/include, with optional 16-bit words: the demo dispatcher has `demo N`,
`menu`, `overlay 0|1`, `slow_frame MS`, `input_mode 0-3`, `ikbd_cmd BYTE...`,
`audio_out 0|1`, `tone HZ` and `copy_mode 0-2 [PIECE]` (demo.h).

`stopwatch` (debug builds, with TIME_STUDY = 1 in userfw.s) reads the ST's
stopwatch points for a few seconds: the VBL's period, when the loop wakes, when
a frame's copy starts and ends and when the loop goes idle, each after its
VBL, the copy's length, and the frames a second, in microseconds (min,
median, 95th percentile, max, mean). The ST reports 4.07 us ticks of MFP
Timer-A on one timeline, so a copy that runs across a VBL is measured whole.

`fb` writes the framebuffer as the ST shows it, in colour, as a PNG: it waits
for the next complete frame (a watchpoint on the frame counter stops core 0
for the dump, about 0.1 s; core 1 and the ST keep running). `--pair` writes
two consecutive frames, OUT-1 and OUT-2; `--raw` also the planar bytes;
`--now` reads the window as it is, for an app that no longer publishes.
`shared` prints the command sentinel, the frame counter, the shared-variable
slots, the palette, the head of the audio buffer and the boot status.

`crash` explains the last reboot without stopping the RP: the watchdog reason
and scratch registers, with code addresses resolved to source lines.
`postmortem` halts the RP and prints both cores' backtraces, the registers, the
watchdog registers and key variables through GDB
($ARM_GDB_PATH/bin/arm-none-eabi-gdb or arm-none-eabi-gdb), then resumes it
unless --leave-halted. Halting stops the RP's code and pauses its timer, not
its PIO and DMA: nothing on the RP updates the cartridge window until it
resumes.

OpenOCD is $OPENOCD, `openocd` on PATH, or ../pico/openocd/src/openocd next to
the repo; its scripts come from $PICO_OPENOCD_PATH (as in .vscode/launch.json),
else the tcl/ folder of a source build.

Exit codes:
    0  success
    1  generic / unexpected, or OpenOCD failed
    2  argparse usage error
    3  check failed: not running the ELF, image differs, or ID not found
"""

from __future__ import annotations

import argparse
import ast
import glob
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INCLUDE_DIR = os.path.join(REPO, "rp", "src", "include")
VTOR = 0xE000ED08
DHCSR = 0xE000EDF0
DHCSR_S_HALT = 1 << 17
DHCSR_RELEASE = 0xA05F0000  # DBGKEY; clears C_HALT and C_DEBUGEN
CORES = ("rp2040.core0", "rp2040.core1")
IO_BANK0 = 0x40014000
INOVER_SHIFT = 16
INOVER_HIGH = 3
SELECT_SHORT_MS = 300
WATCHDOG_REASON = 0x40058008
WATCHDOG_SCRATCH0 = 0x4005800C
GDB_PORT = 3333
# The counters `counters` reads without halting and `postmortem` prints, with
# their labels: plain globals and a few static variables with unique names.
COUNTERS = (("stSessionHellos", "ST hellos"),
            ("fb_frame_tick", "frames published"),
            ("s_vbl_seen", "blits acknowledged"),
            ("fbAckTimeouts", "publishes that timed out"),
            ("ikbdOverruns", "IKBD ACIA overruns"),
            ("ikbdBytes", "IKBD bytes"),
            ("ikbdMousePackets", "mouse packets"),
            ("ikbdJoystickPackets", "joystick packets"),
            ("ikbdResyncs", "IKBD resyncs"),
            ("ikbdCountMismatches", "IKBD count mismatches"),
            ("ikbdPowerUps", "IKBD power-ups"),
            ("commOverruns", "ROM3 ring overruns"),
            ("audioSlicesWritten", "audio slices written"),
            ("audioLateSlices", "audio slices late"),
            ("audioUnderruns", "audio underruns"),
            ("audioOutput", "audio output (1 YM, 2 DMA)"),
            ("stSessionFeatures", "ST features (1 blitter)"))
# Counters that --watch also prints as a rate.
RATE_COUNTERS = ("fb_frame_tick", "s_vbl_seen", "audioSlicesWritten", "ikbdBytes")
# Variables postmortem prints when the ELF has them; a build without one
# simply lacks it. Add the app's own here.
POSTMORTEM_VARIABLES = tuple(n for n, _ in COUNTERS) + ("s_vbl_published",
                                                        "commReadIdx")
BUILD_ID_SYMBOL = "release_build_id"
# DevhooksMailbox (rp/src/include/devhooks.h): offsets of its fields.
MAILBOX_SYMBOL = "devhooksMailbox"
MAILBOX_MAGIC = 0x444B4831
MB_SEQ, MB_ACK, MB_KIND, MB_RESULT, MB_CMD, MB_SIZE, MB_PAYLOAD = (
    4, 8, 12, 16, 20, 22, 24)
MAILBOX_WORDS = 16
KIND_KEY, KIND_APP = 1, 2
# IKBD scancodes (the ST's keyboard, the PC/XT set) by name, for `key`.
SCANCODES = {"esc": 0x01, "backspace": 0x0E, "tab": 0x0F, "return": 0x1C,
             "space": 0x39, "up": 0x48, "down": 0x50, "left": 0x4B,
             "right": 0x4D, "help": 0x62, "undo": 0x61}
SCANCODES.update({str(n % 10): 0x01 + n for n in range(1, 11)})
SCANCODES.update(zip("qwertyuiop", range(0x10, 0x1A)))
SCANCODES.update({f"f{n}": 0x3A + n for n in range(1, 11)})
SCANCODES.update(zip("asdfghjkl", range(0x1E, 0x27)))
SCANCODES.update(zip("zxcvbnm", range(0x2C, 0x33)))
CART_SHARED_H = os.path.join(INCLUDE_DIR, "cart_shared.h")
ST_WINDOW = 0xFA0000  # where the ST sees the cartridge window
# Core 0's DWT watchpoint comparators (ARMv6-M): writing 0 to FUNCTIONn
# disables comparator n.
DWT_FUNCTION = (0xE0001028, 0xE0001038)


class SwdError(Exception):
    pass


def openocd_command(work_area: bool = False,
                    core0_only: bool = False) -> list[str]:
    ocd = os.environ.get("OPENOCD") or shutil.which("openocd")
    if not ocd:
        local = os.path.join(REPO, "..", "pico", "openocd", "src", "openocd")
        if os.access(local, os.X_OK):
            ocd = local
    if not ocd:
        raise SwdError("no openocd: set OPENOCD")
    cmd = [ocd]
    scripts = os.environ.get("PICO_OPENOCD_PATH") or os.path.join(
        os.path.dirname(os.path.realpath(ocd)), "..", "tcl")
    if os.path.isfile(os.path.join(scripts, "interface", "cmsis-dap.cfg")):
        cmd += ["-s", scripts]
    # rp2040.cfg makes the two cores an SMP pair: a halt or a watchpoint on one
    # involves the other. core0_only leaves core 1 out of the session.
    if core0_only:
        cmd += ["-c", "set USE_CORE 0"]
    cmd += ["-f", "interface/cmsis-dap.cfg", "-f", "target/rp2040.cfg",
            "-c", "adapter speed 5000"]
    # OpenOCD's rp2040.cfg puts its work area, where it loads routines such as
    # verify_image's CRC and the flash-size probe of a GDB connect, at
    # 0x20010000: inside the firmware's live RAM (md-microfirmware-template
    # found the CRC routine over live data after a flash, and a HardFault
    # after it). Every run that does not write flash gets a 2 KB work area at
    # the bottom of SCRATCH_X instead, backed up and restored. SCRATCH_X is
    # core 1's 4 KB stack, and core 1 always runs here (the c2p worker), but
    # it uses about 100 bytes at the top; and OpenOCD runs routines only with
    # the cores halted. Flash writes keep the default: they halt the cores and
    # stop DMA first (quiesce_commands) and reset the chip after.
    if not work_area:
        cmd += ["-c", "rp2040.core0 configure -work-area-phys 0x20040000 "
                      "-work-area-size 0x800 -work-area-backup 1"]
    return cmd


# The debug port can drop for a moment, for example while the firmware changes
# the clock and core voltage early in boot. OpenOCD then reports one of these.
TRANSIENT = re.compile(r"Failed to read memory|Error connecting DP|"
                       r"Examination failed|DP initialisation failed")
ATTEMPTS = 4


def openocd(*commands: str, check: bool = True, work_area: bool = False,
            core0_only: bool = False) -> str:
    """Run OpenOCD with `init`, the commands and `exit`; return its output.
    A run that failed on a transient debug-port error is repeated. Only a run
    that writes flash, with the cores halted, asks for OpenOCD's default work
    area (see openocd_command)."""
    args = openocd_command(work_area, core0_only) + ["-c", "init"]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "exit"]
    for attempt in range(ATTEMPTS):
        proc = subprocess.run(args, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        failed = proc.returncode != 0 or "Failed to read memory" in out
        if not (failed and TRANSIENT.search(out)) or attempt == ATTEMPTS - 1:
            break
        time.sleep(0.5)
    if check and proc.returncode != 0:
        lines = [l for l in out.splitlines()
                 if l.startswith("Error") and "algo" not in l]
        raise SwdError("openocd failed: " + (" / ".join(lines[-3:]) or
                                             out.strip()[-300:]))
    return out


def read_word(address: int, core: str = CORES[0]) -> int:
    out = openocd(f"targets {core}", f"mdw 0x{address:08x}")
    m = re.search(rf"0x{address:08x}:\s+([0-9a-fA-F]{{8}})", out)
    if not m:
        raise SwdError(f"cannot read 0x{address:08x}")
    return int(m.group(1), 16)


def read_words(addresses: list[int]) -> list[int]:
    """Several 32-bit words in one OpenOCD run."""
    out = openocd(f"targets {CORES[0]}",
                  *(f"mdw 0x{a:08x}" for a in addresses))
    values = []
    for a in addresses:
        m = re.search(rf"0x{a:08x}:\s+([0-9a-fA-F]{{8}})", out)
        if not m:
            raise SwdError(f"cannot read 0x{a:08x}")
        values.append(int(m.group(1), 16))
    return values


def read_memory(address: int, length: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "dump.bin")
        openocd(f"dump_image {path} 0x{address:08x} {length}")
        with open(path, "rb") as f:
            data = f.read()
    if len(data) != length:
        raise SwdError(f"read {len(data)} of {length} bytes")
    return data


def elf_symbols(elf: str, *names: str) -> dict[str, tuple[int, int]]:
    """Address and size of each named symbol present in the ELF."""
    nm = shutil.which("arm-none-eabi-nm")
    if not nm:
        raise SwdError("arm-none-eabi-nm not on PATH")
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True,
                         check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in names:
            found[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        elif len(parts) == 3 and parts[2] in names:
            found.setdefault(parts[2], (int(parts[0], 16), 0))
    return found


def cmd_running(args: argparse.Namespace) -> int:
    # boot2 points VTOR at the flash vector table before the firmware starts,
    # at the same address in every build; the SDK's runtime init then moves it
    # to RAM. Only the RAM table proves this firmware got past its startup.
    syms = elf_symbols(args.elf, "ram_vector_table", "__vectors")
    table = syms.get("ram_vector_table") or syms.get("__vectors")
    if not table:
        raise SwdError(f"{args.elf} has no vector table symbol")
    tables = {table[0]}
    deadline = time.monotonic() + args.timeout
    last = "no answer"
    while True:
        try:
            vtor = read_word(VTOR)
            halted = read_word(DHCSR) & DHCSR_S_HALT
            if vtor in tables and not halted:
                print(f"running: VTOR 0x{vtor:08x}")
                return 0
            last = f"VTOR 0x{vtor:08x}" + (", core 0 halted" if halted else "")
        except SwdError as exc:
            last = str(exc)
        if time.monotonic() >= deadline:
            print(f"not running {args.elf} after {args.timeout:g} s ({last})",
                  file=sys.stderr)
            return 3
        time.sleep(0.5)


def cmd_verify(args: argparse.Namespace) -> int:
    out = openocd(f"verify_image {args.elf}", check=False)
    m = re.search(r"verified (\d+) bytes in ([\d.]+)s", out)
    if m:
        print(f"flash matches {args.elf} ({m.group(1)} bytes)")
        return 0
    diffs = len(re.findall(r"^diff \d+ address", out, re.M))
    if diffs:
        print(f"flash differs from {args.elf} (at least {diffs} bytes)",
              file=sys.stderr)
        return 3
    raise SwdError("verify failed: " + out.strip()[-300:])


def read_build_id(elf: str) -> str | None:
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym or sym[1] == 0:
        return None
    data = read_memory(sym[0], sym[1])
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def cmd_build_id(args: argparse.Namespace) -> int:
    elfs = args.elf or sorted(
        glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
        key=os.path.getmtime, reverse=True)
    tried = set()
    for elf in elfs:
        sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
        if not sym or sym in tried:
            continue
        tried.add(sym)
        build_id = read_build_id(elf)
        expected = elf_build_id(elf)
        if build_id and (args.elf or build_id == expected):
            print(build_id)
            return 0
    print("no build ID found at the release_build_id address of "
          f"{len(tried)} ELF layout(s)", file=sys.stderr)
    return 3


def elf_build_id(elf: str) -> str | None:
    """The build ID string stored in the ELF file itself."""
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym:
        return None
    headers = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                             capture_output=True, text=True, check=True).stdout
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                         headers, re.M):
        name, size, vma = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if vma <= sym[0] < vma + size:
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "section.bin")
                subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                                f"--only-section={name}", elf, path],
                               check=True)
                with open(path, "rb") as f:
                    f.seek(sym[0] - vma)
                    data = f.read(sym[1])
            return data.split(b"\0", 1)[0].decode("ascii", errors="replace")
    return None


def header_defines(*paths: str) -> dict[str, int]:
    """Integer #defines of one or more C headers, resolving references between
    them. Pass several headers when a define is built on one from another
    header."""
    raw: dict[str, str] = {}
    for header in paths:
        with open(header, encoding="utf-8", errors="replace") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text).replace("\\\n", " ")
        raw.update(re.findall(
            r"^\s*#\s*define\s+([A-Za-z_]\w*)[ \t]+([^\n]+)$", text, re.M))
    path = paths[0] if paths else "<none>"
    values: dict[str, int] = {}

    def resolve(name: str, depth: int = 0) -> int | None:
        if name in values:
            return values[name]
        if name not in raw or depth > 20:
            return None
        expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", raw[name])
        # Drop C casts like (uint32_t)(...): the value is what matters here.
        expr = re.sub(r"\(\s*(?:u?int\d+_t|unsigned|signed|int|long|short|"
                      r"size_t|char)\s*\)", " ", expr)
        for ref in set(re.findall(r"\b[A-Za-z_]\w*\b", expr)):
            v = resolve(ref, depth + 1)
            if v is None:
                return None
            expr = re.sub(rf"\b{ref}\b", str(v), expr)
        try:
            tree = ast.parse(expr.strip(), mode="eval")
        except SyntaxError:
            return None
        allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant,
                   ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div,
                   ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd, ast.USub,
                   ast.Invert)
        if not all(isinstance(n, allowed) for n in ast.walk(tree)):
            return None
        v = eval(compile(tree, path, "eval"), {"__builtins__": {}})
        if not isinstance(v, (int, float)):
            return None
        values[name] = int(v)
        return values[name]

    for name in raw:
        resolve(name)
    return values


def matching_elf(explicit: str | None) -> str:
    """The ELF given, or the cached ELF whose build ID the RP carries."""
    if explicit:
        return explicit
    elfs = sorted(glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
                  key=os.path.getmtime, reverse=True)
    for elf in elfs:
        expected = elf_build_id(elf)
        if expected and read_build_id(elf) == expected:
            return elf
    raise SwdError("no cached ELF matches the RP's build ID: pass --elf")


def cartridge_window(elf: str) -> int:
    base = elf_symbols(elf, "__rom_in_ram_start__").get("__rom_in_ram_start__")
    if not base:
        raise SwdError(f"{elf} has no __rom_in_ram_start__")
    return base[0]


def st_words(data: bytes) -> list[int]:
    """The 16-bit words the ST reads from a stretch of the window. The cart bus
    swaps the two bytes of each word, so a word the ST reads is the RP's
    little-endian uint16 at the same offset."""
    return list(struct.unpack(f"<{len(data) // 2}H", data))


def st_long(data: bytes, offset: int) -> int:
    """A longword as the ST's move.l reads it: two words, high word first."""
    hi, lo = struct.unpack_from("<HH", data, offset)
    return (hi << 16) | lo


def st_text(data: bytes) -> str:
    """A NUL-terminated string as the ST reads it."""
    raw = b"".join(struct.pack(">H", w) for w in st_words(data))
    return raw.split(b"\0", 1)[0].decode("ascii", errors="replace")


def palette_rgb(words: list[int]) -> list[tuple[int, int, int]]:
    """ST palette words (0RRR0GGG0BBB) to RGB. With the STE's extra bit
    (bit 3 of a nibble) in use anywhere, all 16 are read as STE colours."""
    ste = any(w & 0x888 for w in words)

    def level(n: int) -> int:
        if ste:
            return (((n & 7) << 1) | ((n >> 3) & 1)) * 17
        return (n & 7) * 255 // 7
    return [(level(w >> 8), level(w >> 4), level(w)) for w in words]


def write_png_rgb(path: str, width: int, height: int, rows: list[bytes]) -> None:
    """8-bit RGB PNG; each row is width * 3 bytes."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + row for row in rows)
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def image_from_cart_fb(defs: dict[str, int], fb: bytes) -> list[int]:
    """The planar image as the ST shows it, from the cartridge framebuffer.
    The m68k's MOVEM blit stores 48-byte chunks backwards, so the RP keeps
    them in reverse order, with a short tail in natural order after them (see
    "Framebuffer chunk layout" in cart_shared.h): undo that."""
    words = st_words(fb)
    chunk = defs["CART_FB_CHUNK_BYTES"] // 2
    count = defs["CART_FB_CHUNK_COUNT"]
    image = []
    for c in range(count):
        k = count - 1 - c
        image += words[k * chunk:(k + 1) * chunk]
    return image + words[count * chunk:]


def image_rows(image: list[int], rgb: list[tuple[int, int, int]],
               scale: int) -> list[bytes]:
    """ST low resolution, 320x200 in 16 colours: each 16-pixel group is four
    plane words, bit 15 the leftmost pixel."""
    rows = []
    for y in range(200):
        line = bytearray()
        for g in range(20):
            p0, p1, p2, p3 = image[y * 80 + g * 4:y * 80 + g * 4 + 4]
            for bit in range(15, -1, -1):
                idx = (((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) |
                       (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3))
                line += bytes(rgb[idx]) * scale
        rows.extend([bytes(line)] * scale)
    return rows


def release_core0_watch() -> None:
    """Disable core 0's watchpoints and release it, whatever state a frame
    grab left: with debug halting off a watchpoint cannot stop it again."""
    openocd(*(f"mww 0x{f:08x} 0" for f in DWT_FUNCTION),
            f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}",
            check=False, core0_only=True)


def grab_frames(elf: str, count: int, now: bool,
                timeout_ms: int = 1000) -> list[tuple[int, bytes, bytes]]:
    """(frame counter, framebuffer, palette) for `count` consecutive frames.

    fb_publish() writes the frame counter last, after the framebuffer, and a
    dump takes longer than a frame. So core 0 is stopped by a watchpoint on
    the counter: at each hit a frame is complete, and the next one cannot
    start until the dump is done and core 0 resumes. Core 1 and the ST keep
    running. `now` reads the window as it is, without waiting."""
    base = cartridge_window(elf)
    defs = header_defines(CART_SHARED_H)
    counter = base + defs["CART_FB_FRAME_COUNTER_OFFSET"]
    fb = base + defs["CART_FRAMEBUFFER_OFFSET"]
    fb_size = defs["CART_FRAMEBUFFER_SIZE"]
    pal = base + defs["CART_PALETTE_OFFSET"]
    pal_size = defs["CART_PALETTE_SIZE"]
    with tempfile.TemporaryDirectory() as tmp:
        cmds = [] if now else ["halt", f"wp 0x{counter:08x} 4 w"]
        for i in range(count):
            if not now:
                cmds += ["resume", f"wait_halt {timeout_ms}"]
            cmds += [f"dump_image {tmp}/fb{i}.bin 0x{fb:08x} {fb_size}",
                     f"dump_image {tmp}/pal{i}.bin 0x{pal:08x} {pal_size}",
                     f"mdw 0x{counter:08x}"]
        if not now:
            cmds += [f"rwp 0x{counter:08x}", "resume"]
        try:
            out = openocd(*cmds, check=False, core0_only=True)
        finally:
            if not now:
                release_core0_watch()
        if not now and out.count("halted due to watchpoint") < count:
            raise SwdError(f"no frame published within {timeout_ms} ms: is "
                           "the app publishing? (--now reads the window as it is)")
        values = re.findall(rf"0x{counter:08x}:\s+([0-9a-fA-F]{{8}})", out)
        frames = []
        for i in range(count):
            with open(f"{tmp}/fb{i}.bin", "rb") as f:
                fb_data = f.read()
            with open(f"{tmp}/pal{i}.bin", "rb") as f:
                pal_data = f.read()
            if len(fb_data) != fb_size or len(pal_data) != pal_size or i >= len(values):
                raise SwdError("frame dump incomplete: " + out.strip()[-200:])
            frames.append((int(values[i], 16) & 0xFFFF, fb_data, pal_data))
    return frames


def cmd_fb(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    defs = header_defines(CART_SHARED_H)
    frames = grab_frames(elf, 2 if args.pair else 1, args.now)
    stem, ext = os.path.splitext(args.out)
    for i, (frame, fb, pal) in enumerate(frames):
        path = f"{stem}-{i + 1}{ext or '.png'}" if args.pair else args.out
        image = image_from_cart_fb(defs, fb)
        rgb = palette_rgb(st_words(pal))
        write_png_rgb(path, 320 * args.scale, 200 * args.scale,
                      image_rows(image, rgb, args.scale))
        note = f"frame {frame}"
        if args.raw:
            raw = os.path.splitext(path)[0] + ".bin"
            with open(raw, "wb") as f:
                f.write(b"".join(struct.pack(">H", w) for w in image))
            note += f", planar bytes in {raw}"
        print(f"wrote {path} ({note})")
    return 0


def cmd_shared(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    base = cartridge_window(elf)
    d = header_defines(CART_SHARED_H)
    start = d["CART_CMD_SENTINEL_OFFSET"]
    end = d["CART_APP_FREE_OFFSET"]
    data = read_memory(base + start, end - start)

    def at(name: str) -> int:
        return d[name] - start

    def st_addr(name: str) -> str:
        return f"${ST_WINDOW + d[name]:06X}"

    commands = {v: n for n, v in d.items() if n.startswith("CART_CMD_")
                and n != "CART_CMD_SENTINEL_OFFSET"}
    sentinel = st_long(data, at("CART_CMD_SENTINEL_OFFSET"))
    print(f"cartridge window 0x{base:08x} (ST ${ST_WINDOW:06X}), ELF "
          f"{os.path.basename(elf)}")
    print(f"  {st_addr('CART_CMD_SENTINEL_OFFSET')}  command sentinel    "
          f"0x{sentinel:08x} {commands.get(sentinel, '')}")
    counter = st_words(data[at("CART_FB_FRAME_COUNTER_OFFSET"):][:4])
    print(f"  {st_addr('CART_FB_FRAME_COUNTER_OFFSET')}  frame counter       "
          f"low word {counter[0]} (what the ST compares)")
    # Shared-variable slots: the palette's are shown above; the others by
    # index, named by any *_SVAR_* define in rp/src/include.
    names: dict[int, list[str]] = {}
    for name, value in include_defines().items():
        if "_SVAR_" in name and 0 <= value < d["CART_SHARED_VARIABLES_SLOTS"]:
            names.setdefault(value, []).append(name)
    first_pal = (d["CART_PALETTE_OFFSET"] - d["CART_SHARED_VARIABLES_OFFSET"]) // 4
    pal_slots = range(first_pal, first_pal + d["CART_PALETTE_SIZE"] // 4)
    for i in range(d["CART_SHARED_VARIABLES_SLOTS"]):
        if i in pal_slots:
            continue
        off = at("CART_SHARED_VARIABLES_OFFSET") + i * 4
        value = st_long(data, off)
        label = " / ".join(names.get(i, []))
        if not label and (value == 0 and not args.all):
            continue
        print(f"  ${ST_WINDOW + d['CART_SHARED_VARIABLES_OFFSET'] + i * 4:06X}  "
              f"slot {i:2} {label or '-':<12} 0x{value:08x}  {value}")
    pal = st_words(data[at("CART_PALETTE_OFFSET"):][:d["CART_PALETTE_SIZE"]])
    print(f"  {st_addr('CART_PALETTE_OFFSET')}  palette             " +
          " ".join(f"{w:03X}" for w in pal))
    audio = data[at("CART_AUDIO_BUFFER_OFFSET"):][:16]
    print(f"  {st_addr('CART_AUDIO_BUFFER_OFFSET')}  audio buffer head   " +
          " ".join(f"{w:04X}" for w in st_words(audio)) + " (vA vB pairs)")
    status = st_words(data[at("CART_BOOT_STATUS_OFFSET"):][:2])[0]
    message = st_text(data[at("CART_BOOT_MESSAGE_OFFSET"):]
                      [:d["CART_BOOT_MESSAGE_SIZE"]])
    print(f"  {st_addr('CART_BOOT_STATUS_OFFSET')}  boot status         "
          f"{status}" + (f" (vetoed: {message!r})" if status else " (start)"))
    return 0


def mailbox_request(elf: str, kind: int, command_id: int, words: list[int],
                    timeout: float = 5.0) -> int:
    """Send one request through the debug mailbox; return its result."""
    if len(words) > MAILBOX_WORDS:
        raise SwdError(f"at most {MAILBOX_WORDS} payload words")
    sym = elf_symbols(elf, MAILBOX_SYMBOL).get(MAILBOX_SYMBOL)
    if not sym:
        raise SwdError(f"{os.path.basename(elf)} has no {MAILBOX_SYMBOL}: "
                       "a debug build is needed")
    base = sym[0]
    magic, seq, ack = struct.unpack("<III", read_memory(base, 12))
    if magic != MAILBOX_MAGIC:
        raise SwdError(f"no mailbox at 0x{base:08x} (magic 0x{magic:08x})")
    if seq != ack:
        raise SwdError("the previous request was never acknowledged: "
                       "is the main loop running?")
    commands = [f"mww 0x{base + MB_KIND:08x} {kind}",
                f"mwh 0x{base + MB_CMD:08x} {command_id}",
                f"mwh 0x{base + MB_SIZE:08x} {len(words) * 2}"]
    for i, word in enumerate(words):
        commands.append(f"mwh 0x{base + MB_PAYLOAD + 2 * i:08x} {word & 0xFFFF}")
    # seq last: the firmware acts as soon as seq differs from ack.
    commands.append(f"mww 0x{base + MB_SEQ:08x} {ack + 1}")
    openocd(*commands)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new_ack, _, result = struct.unpack(
            "<III", read_memory(base + MB_ACK, MB_RESULT + 4 - MB_ACK))
        if new_ack == ack + 1:
            return result
        time.sleep(0.1)
    raise SwdError(f"no acknowledge within {timeout:g} s: "
                   "is the main loop running?")


def scancode(name: str) -> int:
    code = SCANCODES.get(name.lower())
    if code is None:
        try:
            code = int(name, 0)
        except ValueError:
            raise SwdError(f"unknown key {name!r}: a scancode (0x02) or one "
                           f"of {', '.join(sorted(SCANCODES))}") from None
    if not 0 < code < 0x80:
        raise SwdError(f"scancode {code:#x} is not a key")
    return code


def cmd_key(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    codes = [scancode(k) for k in args.keys]
    if args.press:
        data = codes
    elif args.release:
        data = [c | 0x80 for c in codes]
    else:
        data = [b for c in codes for b in (c, c | 0x80)]
    fed = mailbox_request(elf, KIND_KEY, 0, data)
    print(f"fed {fed} IKBD byte(s): " + " ".join(f"{b:02X}" for b in data))
    return 0 if fed == len(data) else 3


def cmd_app(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    name = "DEVHOOKS_APP_" + args.name.upper()
    command_id = include_defines().get(name)
    if command_id is None:
        raise SwdError(f"no {name} define in rp/src/include")
    words = [int(w, 0) for w in args.words]
    result = mailbox_request(elf, KIND_APP, command_id, words)
    print(f"{name}: result {result}")
    return 0 if result else 3


def cmd_read(args: argparse.Namespace) -> int:
    data = read_memory(int(args.address, 0), int(args.length, 0))
    with open(args.outfile, "wb") as f:
        f.write(data)
    print(f"read {len(data)} bytes from {args.address} into {args.outfile}")
    return 0


HEAP_SYMBOLS = ("end", "__StackLimit", "heap_end.0", "__malloc_sbrk_base",
                "__malloc_max_sbrked_mem", "__malloc_av_")


def heap_snapshot(elf: str) -> dict:
    """Heap figures read from newlib's own malloc state, no firmware help.

    The heap grows by sbrk from `end` towards `__StackLimit` and never gives
    memory back, so __malloc_max_sbrked_mem is the peak the heap ever reached,
    transient peaks included. Free space inside the heap comes from walking
    its chunks: each chunk's size word carries PREV_INUSE for the chunk
    before it, and the walk ends at the top chunk (av_[2]). The walk reads
    RAM while the CPU runs, so a chunk that changes mid-read can break it:
    `walk_ok` is False then and the caller may simply read again."""
    sym = elf_symbols(elf, *HEAP_SYMBOLS)
    missing = [n for n in HEAP_SYMBOLS if n not in sym]
    if missing:
        raise SwdError(f"{os.path.basename(elf)} lacks {', '.join(missing)}")
    start, limit = sym["end"][0], sym["__StackLimit"][0]
    brk, base, peak, top = struct.unpack("<4I", b"".join(
        read_memory(sym[n][0] + off, 4) for n, off in
        (("heap_end.0", 0), ("__malloc_sbrk_base", 0),
         ("__malloc_max_sbrked_mem", 0), ("__malloc_av_", 8))))
    snap = {"size": limit - start, "arena": (brk - start) if brk else 0,
            "peak": peak, "headroom": limit - (brk or start),
            "peak_headroom": limit - start - peak, "walk_ok": False}
    if not brk or base in (0, 0xFFFFFFFF) or not (base <= top < brk):
        return snap
    heap = read_memory(base, brk - base)

    def word(addr: int) -> int:
        return struct.unpack_from("<I", heap, addr - base)[0]

    used = free = largest = free_chunks = 0
    # The first chunk is not always at the sbrk base: malloc moves it forward
    # so that its user pointer (chunk + 8) is 8-byte aligned.
    p = base + (-(base + 8) & 7)
    while p < top:
        size = word(p + 4) & ~3
        if size < 16 or p + size > top:
            return snap
        if word(p + size + 4) & 1:
            used += size
        else:
            free += size
            free_chunks += 1
            largest = max(largest, size)
        p += size
    top_size = word(top + 4) & ~3
    if p != top or top + top_size > brk + 16:
        return snap
    snap.update(walk_ok=True, used=used, free=free + top_size,
                largest=max(largest, top_size), free_chunks=free_chunks)
    return snap


def heap_line(snap: dict) -> str:
    kb = lambda n: f"{n / 1024:.1f} KB"
    line = (f"heap {kb(snap['size'])}: arena {kb(snap['arena'])}, "
            f"peak {kb(snap['peak'])} (closest to the stack: "
            f"{kb(snap['peak_headroom'])}), never used {kb(snap['headroom'])}")
    if snap["walk_ok"]:
        line += (f"; in use {kb(snap['used'])}, free in arena "
                 f"{kb(snap['free'])} in {snap['free_chunks'] + 1} blocks, "
                 f"largest {kb(snap['largest'])}")
    else:
        line += "; chunk walk failed (heap changed while reading)"
    return line


STOPWATCH_TICK_US = 10 / 2.4576  # MFP Timer-A /10 (userfw.s TIME_STUDY)
STOPWATCH_POINTS = {0: "wake", 1: "copy", 2: "copied", 3: "idle", 4: "VBL"}


def cmd_stopwatch(args: argparse.Namespace) -> int:
    """The ST's stopwatch points (userfw.s TIME_STUDY), read while it runs."""
    elf = matching_elf(args.elf)
    sym = elf_symbols(elf, "fbStopwatch", "fbStopwatchCount")
    if len(sym) < 2:
        raise SwdError(f"{os.path.basename(elf)} has no stopwatch (a debug build?)")
    ring, ring_bytes = sym["fbStopwatch"]
    count_addr, n = sym["fbStopwatchCount"][0], ring_bytes // 4

    def snapshot() -> tuple[int, bytes]:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "ring.bin")
            out = openocd(f"targets {CORES[0]}", f"mdw 0x{count_addr:08x}",
                          f"dump_image {path} 0x{ring:08x} {ring_bytes}")
            m = re.search(rf"0x{count_addr:08x}:\s+([0-9a-fA-F]{{8}})", out)
            if not m:
                raise SwdError("cannot read fbStopwatchCount")
            with open(path, "rb") as f:
                return int(m.group(1), 16), f.read()

    seen, _ = snapshot()
    points, lost, started = [], 0, time.time()
    while time.time() - started < args.seconds:
        count, data = snapshot()
        top = count - 1  # the newest may be half-written
        first = max(seen, top - n + 1)
        lost += first - seen
        points += [struct.unpack_from("<I", data, (i % n) * 4)[0] for i in range(first, top)]
        seen = max(seen, top)
    elapsed = time.time() - started
    if not points:
        print("no stopwatch points: is userfw built with TIME_STUDY = 1?")
        return 3
    # One timeline: the ST reports the low 16 bits of its ticks.
    timeline, last, base = [], None, 0
    for r in points:
        point, ticks = r >> 16, r & 0xFFFF
        if last is not None and ticks < last:
            base += 0x10000
        last = ticks
        timeline.append((point, (base + ticks) * STOPWATCH_TICK_US))
    after_vbl, periods, copies = {}, [], []
    vbl = start = None
    for point, us in timeline:
        if point == 4:
            if vbl is not None:
                periods.append(us - vbl)
            vbl = us
            continue
        if vbl is not None:
            after_vbl.setdefault(point, []).append(us - vbl)
        if point == 1:
            start = us
        elif point == 2 and start is not None:
            copies.append(us - start)
            start = None

    def row(name: str, values: list[float]) -> str:
        v = sorted(values)
        cells = (v[0], v[len(v) // 2], v[min(len(v) - 1, int(0.95 * len(v)))], v[-1],
                 sum(v) / len(v))
        return f"  {name:<14}" + "".join(f"{c:9.1f}" for c in cells)

    print(f"{len(points)} points in {elapsed:.1f} s ({lost} not read); microseconds")
    print(f"  {'':<14}{'min':>9}{'median':>9}{'p95':>9}{'max':>9}{'mean':>9}")
    if periods:
        print(row("VBL period", periods))
    for p in sorted(after_vbl):
        print(row(f"{STOPWATCH_POINTS.get(p, p)} @VBL+", after_vbl[p]))
    if copies:
        print(row("copy", copies))
    frames = len(after_vbl.get(1, []))
    vbls = len(periods) + 1
    print(f"  {frames} frames in {vbls} VBLs: {50.0 * frames / max(vbls, 1):.1f} fps at 50 Hz")
    return 0


def cmd_counters(args: argparse.Namespace) -> int:
    """The firmware's counters, read while the RP runs."""
    elf = matching_elf(args.elf)
    sym = elf_symbols(elf, *(n for n, _ in COUNTERS))
    counters = [(n, label) for n, label in COUNTERS if n in sym]
    if not counters:
        raise SwdError(f"{os.path.basename(elf)} has none of the counters")
    missing = [n for n, _ in COUNTERS if n not in sym]
    if missing:
        print(f"({os.path.basename(elf)} lacks {', '.join(missing)})")
    names = [n for n, _ in counters]
    prev = None
    t_prev = 0.0
    try:
        while True:
            now = time.monotonic()
            values = dict(zip(names, read_words([sym[n][0] for n in names])))
            if prev is None or any(values[n] < prev[n] for n in names):
                if prev is not None:
                    print(time.strftime("%H:%M:%S ") +
                          "counters went back: the RP restarted", flush=True)
                print(time.strftime("%H:%M:%S ") + ", ".join(
                    f"{label} {values[n]}" for n, label in counters), flush=True)
            else:
                dt = now - t_prev
                parts = []
                for n, label in counters:
                    d = values[n] - prev[n]
                    rate = f" ({d / dt:.1f}/s)" if n in RATE_COUNTERS else ""
                    parts.append(f"{label} +{d}{rate}")
                print(time.strftime("%H:%M:%S ") + ", ".join(parts), flush=True)
            if not args.watch:
                return 0
            prev, t_prev = values, now
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0


def ikbd_log_summary(samples: list[int], missed: int) -> str:
    """What a recording holds, and the ST's byte counts checked against the
    IKBD bytes between them. A gap in the recording restarts the check."""
    kinds = {0x82: 0, 0x83: 0, 0x85: 0, 0x87: 0, 0x88: 0, 0x01: 0}
    base = None
    checked = mismatches = 0
    for s in samples:
        if s is None:
            base = None
            continue
        window, value = s >> 8, s & 0xFF
        kinds[window] = kinds.get(window, 0) + 1
        if window == 0x82 and base is not None:
            base = (base + 1) & 0xFF
        elif window == 0x83:
            if base is not None:
                checked += 1
                mismatches += base != value
            base = value
        elif window == 0x88:
            base = None
    return (f"{len([s for s in samples if s is not None])} samples, "
            f"{missed} missed: {kinds[0x82]} IKBD bytes, {kinds[0x83]} counts, "
            f"{kinds[0x85]} overruns, {kinds[0x87]} mode reports, "
            f"{kinds[0x88]} hellos, {kinds[0x01]} typed by a host tool; "
            f"{checked} counts checked, {mismatches} wrong")


class IkbdLog:
    """The firmware's IKBD log (ikbdLog, debug builds), read while it runs."""

    def __init__(self, elf: str):
        sym = elf_symbols(elf, "ikbdLog", "ikbdLogCount")
        if "ikbdLog" not in sym or "ikbdLogCount" not in sym:
            raise SwdError(f"{os.path.basename(elf)} has no IKBD log "
                           "(debug builds have one)")
        self.addr, self.size = sym["ikbdLog"]
        self.entries = self.size // 2
        self.count_addr = sym["ikbdLogCount"][0]
        self.pattern = re.compile(rf"0x{self.count_addr:08x}:\s+([0-9a-fA-F]{{8}})")
        self.seen = None
        self.missed = 0

    def poll(self) -> list[int | None]:
        """The samples written since the last poll, a None where some were
        missed (or the RP restarted)."""
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "log.bin")
            # The count, the log, the count: the samples written before the
            # first read and not overwritten before the second are the ones
            # the dump holds for sure.
            out = openocd(f"targets {CORES[0]}", f"mdw 0x{self.count_addr:08x}",
                          f"dump_image {path} 0x{self.addr:08x} {self.size}",
                          f"mdw 0x{self.count_addr:08x}")
            with open(path, "rb") as f:
                data = f.read()
        before, after = (int(v, 16) for v in self.pattern.findall(out)[:2])
        batch: list[int | None] = []
        if self.seen is None or before < self.seen:
            if self.seen is not None:
                batch.append(None)
            self.seen = before
        first = max(self.seen, after - self.entries)
        if first > self.seen:
            self.missed += first - self.seen
            batch.append(None)
        for n in range(first, before):
            i = n % self.entries
            batch.append(data[2 * i] | data[2 * i + 1] << 8)
        self.seen = before
        return batch


def cmd_ikbd_log(args: argparse.Namespace) -> int:
    """Record the IKBD decoder's input (see the module docstring)."""
    log = IkbdLog(matching_elf(args.elf))
    samples: list[int | None] = []
    end = time.monotonic() + args.seconds
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    with open(args.out, "w") as f:
        try:
            log.poll()  # from now on
            while time.monotonic() < end:
                batch = log.poll()
                samples += batch
                f.writelines("gap\n" if s is None else f"{s:04x}\n" for s in batch)
                f.flush()
        except KeyboardInterrupt:
            pass
    print(ikbd_log_summary(samples, log.missed))
    return 0


def cmd_heap(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    csv = None
    if args.csv:
        new = not os.path.exists(args.csv)
        csv = open(args.csv, "a")
        if new:
            csv.write("time,arena,peak,peak_headroom,headroom,used,free,"
                      "largest,free_blocks\n")
    try:
        while True:
            snap = heap_snapshot(elf)
            if not snap["walk_ok"]:
                snap = heap_snapshot(elf)
            print(time.strftime("%H:%M:%S ") + heap_line(snap), flush=True)
            if csv:
                csv.write(",".join([time.strftime("%Y-%m-%d %H:%M:%S")] + [
                    str(snap.get(k, "")) for k in
                    ("arena", "peak", "peak_headroom", "headroom", "used",
                     "free", "largest")] +
                    [str(snap["free_chunks"] + 1) if snap["walk_ok"] else ""])
                    + "\n")
                csv.flush()
            if not args.watch:
                return 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0
    finally:
        if csv:
            csv.close()


def cmd_resume(args: argparse.Namespace) -> int:
    """Release both cores from a debug halt. OpenOCD's own resume fails in a
    new OpenOCD run, and a halted core 1 also pauses the RP2040's timer,
    which leaves core 0 asleep forever."""
    commands = []
    for core in CORES:
        commands += [f"targets {core}", f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
    openocd(*commands)
    states = [read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES]
    if any(states):
        print("still halted: " + ", ".join(
            c for c, h in zip(CORES, states) if h), file=sys.stderr)
        return 3
    print("both cores released")
    return 0


def include_defines() -> dict[str, int]:
    """Every integer #define in rp/src/include, resolved across headers."""
    return header_defines(*sorted(glob.glob(os.path.join(INCLUDE_DIR, "*.h"))))


def cmd_select(args: argparse.Namespace) -> int:
    defs = include_defines()
    gpio = defs["SELECT_GPIO"]
    ctrl = IO_BANK0 + 4 + 8 * gpio
    normal = read_word(ctrl) & ~(3 << INOVER_SHIFT)
    if args.press == "release":
        openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
        print(f"SELECT (GPIO {gpio}) override cleared")
        return 0
    if args.press == "long" and not args.force:
        raise SwdError("a long press is a factory reset: it erases the global "
                       "settings, and Booster then clears every app's "
                       "settings (reset_deviceAndEraseFlash): add --force")
    hold = args.hold_ms or (SELECT_SHORT_MS if args.press == "short"
                            else defs["SELECT_LONG_RESET"] + 1000)
    pressed = normal | (INOVER_HIGH << INOVER_SHIFT)
    try:
        openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}", f"sleep {hold}",
                f"mww 0x{ctrl:08x} 0x{normal:08x}")
    finally:
        # Never leave the button pressed, even if OpenOCD failed mid-hold.
        if read_word(ctrl) & (3 << INOVER_SHIFT):
            openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
    print(f"SELECT (GPIO {gpio}) held {hold} ms")
    return 0


def resolve_address(elf: str, address: int) -> str:
    """`function at file:line` for a code address, or '' when not code."""
    if not (0x10000000 <= address < 0x10100000 or
            0x20000000 <= address < 0x20030000):
        return ""
    out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-i", "-p", "-e",
                          elf, f"0x{address & ~1:08x}"],
                         capture_output=True, text=True).stdout.strip()
    return "" if out.startswith("??") else out.replace("\n", "; ")


def cmd_crash(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    reason, *scratch = struct.unpack("<5I", read_memory(WATCHDOG_REASON, 20))
    print(f"watchdog reason 0x{reason:08x}; scratch 0-3: " +
          " ".join(f"0x{v:08x}" for v in scratch))
    for i, v in enumerate(scratch):
        where = resolve_address(elf, v)
        if where:
            print(f"  scratch {i}: {where}")
    return 0


def gdb_command() -> str:
    gdb_dir = os.environ.get("ARM_GDB_PATH")
    candidates = [os.path.join(gdb_dir, "bin", "arm-none-eabi-gdb")] if gdb_dir else []
    candidates.append(shutil.which("arm-none-eabi-gdb") or "")
    for c in candidates:
        if c and os.access(c, os.X_OK):
            return c
    raise SwdError("no arm-none-eabi-gdb: set ARM_GDB_PATH")


def cmd_postmortem(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    gdb = gdb_command()
    # No GDB memory map: to build one, OpenOCD probes the flash size on
    # connect with a routine in its work area, and afterwards core 0's MSP
    # still held the routine's stack (0x20040400, in the work area) while its
    # SP was right; resumed, core 0 locked up at once. GDB needs no memory
    # map to read the stacks and variables.
    server = openocd_command() + [
        "-c", f"gdb_port {GDB_PORT}", "-c", "tcl_port disabled",
        "-c", "telnet_port disabled", "-c", "gdb_memory_map disable"]
    if args.leave_halted:
        # OpenOCD resumes the target when GDB detaches, unless told not to.
        for core in CORES:
            server += ["-c", f"{core} configure -event gdb-detach {{}}"]
    proc = subprocess.Popen(server, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if f"port {GDB_PORT}" in line:
                ready = True
                break
        if not ready:
            raise SwdError("OpenOCD did not start its GDB server")
        present = elf_symbols(elf, *POSTMORTEM_VARIABLES)
        gdb_cmds = ["set pagination off", "set confirm off",
                    "set print pretty on",
                    f"target extended-remote localhost:{GDB_PORT}",
                    "monitor halt",
                    "echo \\n=== threads (one per core)\\n", "info threads",
                    "echo \\n=== backtraces\\n", "thread apply all bt",
                    "echo \\n=== registers (current core)\\n",
                    "info registers",
                    "echo \\n=== watchdog reason and scratch 0-3\\n",
                    f"x/5xw 0x{WATCHDOG_REASON:08x}"]
        if present:
            gdb_cmds.append("echo \\n=== variables\\n")
            for name in POSTMORTEM_VARIABLES:
                if name in present:
                    gdb_cmds += [f"echo {name} = ", f"output {name}",
                                 "echo \\n"]
        if not args.leave_halted:
            gdb_cmds.append("monitor resume")
        gdb_cmds.append("detach")
        argv = [gdb, "-nx", "-batch", elf]
        for c in gdb_cmds:
            argv += ["-ex", c]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        print(out.stdout.rstrip())
        errors = [l for l in out.stderr.splitlines()
                  if l.strip() and "warning" not in l.lower()]
        if errors:
            print("\n".join(errors[-5:]), file=sys.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if args.leave_halted:
        halted = [c for c in CORES if read_word(DHCSR, c) & DHCSR_S_HALT]
        print("\nleft halted: " + (", ".join(halted) or "nothing") +
              " (swd.py resume to continue)")
        return 0
    # OpenOCD leaves debug mode enabled (C_DEBUGEN) after GDB detaches; clear
    # it on both cores so the RP runs exactly as before the halt.
    return cmd_resume(args)


PSM_WDSEL = 0x40010008
PSM_WDSEL_ALL_BUT_OSCILLATORS = 0x0001FFFC
WATCHDOG_CTRL = 0x40058000
WATCHDOG_CTRL_TRIGGER = 0x80000000


def chip_reset() -> None:
    """Reset the whole chip at once, the way the watchdog does.

    Not OpenOCD's own `reset`: its multi-core sequence touches core 1 again a
    few milliseconds after core 0 has started. Here core 1 is the c2p worker
    from the moment fb_init() launches it, and core 0 waits for it at every
    publish (fb_core1_wait()), so a core 1 disturbed at start hangs the app
    (md-microfirmware-template lost its core-1 SELECT watcher that way). A
    watchdog-style reset restarts both cores together, like a power-on reset,
    and leaves the debugger nothing to do."""
    openocd(f"mww 0x{PSM_WDSEL:08x} 0x{PSM_WDSEL_ALL_BUT_OSCILLATORS:08x}",
            f"mww 0x{WATCHDOG_CTRL:08x} 0x{WATCHDOG_CTRL_TRIGGER:08x}",
            check=False)
    time.sleep(0.5)
    # `program` leaves the cores halted; make sure a halt does not survive.
    try:
        if any(read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES):
            commands = []
            for core in CORES:
                commands += [f"targets {core}",
                             f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
            openocd(*commands)
    except SwdError:
        pass  # the debug port can blink while the chip restarts


DMA_BASE = 0x50000000
DMA_CHANNELS = 12
DMA_AL1_CTRL = 0x10      # CTRL alias that does not trigger the channel
DMA_CHAN_ABORT = 0x50000444
PIO_CTRL = (0x50200000, 0x50300000)


def quiesce_commands() -> list[str]:
    """Halt both cores, then stop every PIO state machine and DMA channel.

    Halting the cores does not stop the RP2040's DMA: the ROM3 capture ring
    keeps writing bus samples into RAM for as long as the ST touches the
    cartridge. A flash write that stages its data in that RAM then programs
    bus samples instead of code (md-drives-emulator found 20 bytes of an image
    arrived as 0x80xx words while the ST was reset-looping). PIO first, so no new
    DREQ arrives; then clear each channel's enable without triggering it, then
    abort whatever is in flight."""
    cmds = []
    for core in CORES:
        cmds += [f"targets {core}", "halt"]
    cmds += [f"mww 0x{ctrl:08x} 0" for ctrl in PIO_CTRL]
    cmds += [f"mww 0x{DMA_BASE + 0x40 * n + DMA_AL1_CTRL:08x} 0"
             for n in range(DMA_CHANNELS)]
    cmds += [f"mww 0x{DMA_CHAN_ABORT:08x} 0x{(1 << DMA_CHANNELS) - 1:x}",
             f"targets {CORES[0]}"]
    return cmds


def cmd_program(args: argparse.Namespace) -> int:
    # Not OpenOCD's `program`: it resets with OpenOCD's own sequence first,
    # which leaves DMA running (see quiesce_commands) and touches core 1.
    out = openocd(*quiesce_commands(), f"flash write_image erase {args.elf}",
                  f"verify_image {args.elf}", check=False, work_area=True)
    if not re.search(r"verified \d+ bytes", out):
        chip_reset()
        raise SwdError("flash write did not verify: " + " / ".join(
            l.strip() for l in out.splitlines() if l.startswith("Error"))[-300:])
    chip_reset()
    print(f"flashed {args.elf}")
    return 0


def cmd_reset(args: argparse.Namespace) -> int:
    chip_reset()
    print("chip reset (watchdog-style, both cores together)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="swd.py", description="Read and check a running RP2040 over SWD.")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("running", help="wait until the RP runs the ELF")
    r.add_argument("elf")
    r.add_argument("--timeout", type=float, default=30.0)
    r.set_defaults(func=cmd_running)

    v = sub.add_parser("verify", help="compare the flash with the ELF")
    v.add_argument("elf")
    v.set_defaults(func=cmd_verify)

    b = sub.add_parser("build-id", help="read the build ID from flash")
    b.add_argument("elf", nargs="*")
    b.set_defaults(func=cmd_build_id)

    m = sub.add_parser("read", help="dump a memory range to a file")
    m.add_argument("address")
    m.add_argument("length")
    m.add_argument("outfile")
    m.set_defaults(func=cmd_read)

    g = sub.add_parser("program", help="flash the ELF and reset")
    g.add_argument("elf")
    g.set_defaults(func=cmd_program)

    rs = sub.add_parser("resume", help="release both cores from a debug halt")
    rs.set_defaults(func=cmd_resume)

    rst = sub.add_parser("reset", help="reset the whole chip, watchdog-style")
    rst.set_defaults(func=cmd_reset)

    cn = sub.add_parser("counters", help="the firmware's counters, "
                        "without halting")
    cn.add_argument("--elf")
    cn.add_argument("--watch", type=float, metavar="SECONDS",
                    help="print what changed every SECONDS until Ctrl-C")
    cn.set_defaults(func=cmd_counters)

    il = sub.add_parser("ikbd-log", help="record the IKBD decoder's input "
                        "(debug builds)")
    il.add_argument("out")
    il.add_argument("--seconds", type=float, default=60.0)
    il.add_argument("--elf")
    il.set_defaults(func=cmd_ikbd_log)

    sw = sub.add_parser("stopwatch", help="the ST's stopwatch points "
                        "(debug builds, TIME_STUDY)")
    sw.add_argument("--seconds", type=float, default=4.0)
    sw.add_argument("--elf")
    sw.set_defaults(func=cmd_stopwatch)

    fbp = sub.add_parser("fb", help="grab the framebuffer as a PNG")
    fbp.add_argument("out")
    fbp.add_argument("--elf")
    fbp.add_argument("--pair", action="store_true",
                     help="two consecutive frames, OUT-1 and OUT-2")
    fbp.add_argument("--raw", action="store_true",
                     help="also write the planar bytes as the ST shows them")
    fbp.add_argument("--now", action="store_true",
                     help="do not wait for a published frame")
    fbp.add_argument("--scale", type=int, default=2)
    fbp.set_defaults(func=cmd_fb)

    sh = sub.add_parser("shared", help="the cartridge window's shared block")
    sh.add_argument("--elf")
    sh.add_argument("--all", action="store_true",
                    help="also print unnamed slots that are zero")
    sh.set_defaults(func=cmd_shared)

    k = sub.add_parser("key", help="type on the ST's keyboard (debug builds)")
    k.add_argument("keys", nargs="+",
                   help="scancodes or names; each is pressed and released")
    k.add_argument("--elf")
    upd = k.add_mutually_exclusive_group()
    upd.add_argument("--press", action="store_true", help="press only")
    upd.add_argument("--release", action="store_true", help="release only")
    k.set_defaults(func=cmd_key)

    ap = sub.add_parser("app", help="send an app command (DEVHOOKS_APP_*)")
    ap.add_argument("name")
    ap.add_argument("words", nargs="*", help="16-bit payload words")
    ap.add_argument("--elf")
    ap.set_defaults(func=cmd_app)

    hp = sub.add_parser("heap", help="heap size, peak and free space")
    hp.add_argument("--elf")
    hp.add_argument("--watch", type=float, metavar="SECONDS",
                    help="sample again every SECONDS until Ctrl-C")
    hp.add_argument("--csv", help="also append each sample to this CSV file")
    hp.set_defaults(func=cmd_heap)

    se = sub.add_parser("select", help="press the SELECT button")
    se.add_argument("press", choices=("short", "long", "release"))
    se.add_argument("--hold-ms", type=int)
    se.add_argument("--force", action="store_true",
                    help="allow a long press")
    se.set_defaults(func=cmd_select)

    cr = sub.add_parser("crash", help="explain the last reboot")
    cr.add_argument("--elf")
    cr.set_defaults(func=cmd_crash)

    pm = sub.add_parser("postmortem", help="halt, dump backtraces, resume")
    pm.add_argument("--elf")
    pm.add_argument("--leave-halted", action="store_true")
    pm.set_defaults(func=cmd_postmortem)

    return p


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (SwdError, subprocess.CalledProcessError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
