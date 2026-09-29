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
    python3 tools/dev/swd.py heap [--elf ELF] [--watch SECONDS] [--csv FILE]
    python3 tools/dev/swd.py select short|long|release [--hold-ms MS] [--force]
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
publishes that gave up waiting for an acknowledgement (fb.c), the IKBD bytes
the keyboard ACIA lost (ikbd.c) and the ROM3 ring's overruns (commemul.c).
With --watch it prints what changed every SECONDS, with rates: while the ST
runs userfw and the app publishes every frame, both run at 50 a second.
`heap` reads newlib's malloc state: the heap's size, its peak and the free
space inside it. Without --elf, both use the cached ELF whose build ID the RP
carries (tools/dev/builds/elf, filled by flash.sh).

`select` presses the SELECT button: it forces the pin's input high through the
GPIO input override (IO_BANK0 GPIOn_CTRL.INOVER) for the hold time, so the
firmware sees a real press without any code of its own. `short` holds 300 ms
(the RP restarts), `long` holds SELECT_LONG_RESET + 1 s (rp/src/include/select.h)
and needs `--force`, because it is a factory reset (reset_deviceAndEraseFlash).
`release` clears a stuck override.

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
import struct
import subprocess
import sys
import tempfile
import time

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
            ("ikbdOverruns", "IKBD bytes lost"),
            ("commOverruns", "ROM3 ring overruns"))
# Counters that --watch also prints as a rate.
RATE_COUNTERS = ("fb_frame_tick", "s_vbl_seen")
# Variables postmortem prints when the ELF has them; a build without one
# simply lacks it. Add the app's own here.
POSTMORTEM_VARIABLES = tuple(n for n, _ in COUNTERS) + ("s_vbl_published",
                                                        "commReadIdx")
BUILD_ID_SYMBOL = "release_build_id"


class SwdError(Exception):
    pass


def openocd_command(work_area: bool = False) -> list[str]:
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


def openocd(*commands: str, check: bool = True, work_area: bool = False) -> str:
    """Run OpenOCD with `init`, the commands and `exit`; return its output.
    A run that failed on a transient debug-port error is repeated. Only a run
    that writes flash, with the cores halted, asks for OpenOCD's default work
    area (see openocd_command)."""
    args = openocd_command(work_area) + ["-c", "init"]
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
    server = openocd_command() + [
        "-c", f"gdb_port {GDB_PORT}", "-c", "tcl_port disabled",
        "-c", "telnet_port disabled"]
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
