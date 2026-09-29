#!/usr/bin/env python3
"""
console — capture the microfirmware debug console to a log file.

Single-file, stdlib-only (Python >= 3.10). Reads the Raspberry Pi Debug
Probe's UART bridge (GPIO 0/1 of the RP, 921,600 baud in debug builds),
prefixes each line with a timestamp, appends it to a log file and mirrors
it to the terminal. The other subcommands read that log file, so an agent
or a script can follow the console while a person watches it live.

Usage:
    python3 tools/dev/console.py watch [--port PORT] [--baud N]
    python3 tools/dev/console.py tail [N]
    python3 tools/dev/console.py since-boot [--boot K]
    python3 tools/dev/console.py grep PATTERN [--since-boot]
    python3 tools/dev/console.py wait PATTERN [--timeout S]

`watch` finds the probe by its USB product name unless `--port` is given.
Only one program can read the port: close CoolTerm (or any other serial
terminal) first. The log is `tools/dev/logs/console.log` (override with
`--log` or `SIDECART_CONSOLE_LOG`); it rotates to `console.log.1` at 32 MB.

Exit codes:
    0  success
    1  generic / unexpected
    2  argparse usage error
    3  `wait` timed out, or `grep` found nothing
"""

from __future__ import annotations

import argparse
import datetime
import errno
import fcntl
import os
import re
import struct
import subprocess
import sys
import termios
import time

DEFAULT_BAUD = 921600
PROBE_PRODUCT = "Debug Probe"
ROTATE_BYTES = 32 * 1024 * 1024
LOG_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs",
                           "console.log")
# First line of every boot: `App. v1.1.0 (2026-09-15 00:00:00). DEBUG mode.`
BOOT_RE = re.compile(r"App\. v\S+ \(")
# macOS: set a non-standard baud rate on an open tty (IOSSIOSPEED).
IOSSIOSPEED = 0x80045402


def log_path(args: argparse.Namespace) -> str:
    return args.log or os.environ.get("SIDECART_CONSOLE_LOG") or LOG_DEFAULT


def find_probe_port() -> str | None:
    """Return the Debug Probe's UART callout device from the USB registry."""
    try:
        out = subprocess.run(
            ["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
            capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    product = ""
    for line in out.splitlines():
        m = re.search(r'"USB Product Name" = "([^"]*)"', line)
        if m:
            product = m.group(1)
            continue
        m = re.search(r'"IOCalloutDevice" = "([^"]*)"', line)
        if m and PROBE_PRODUCT in product:
            return m.group(1)
    return None


def open_port(port: str, baud: int) -> int:
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        # Exclusive access: a second reader would split the stream.
        fcntl.ioctl(fd, termios.TIOCEXCL)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0                                   # iflag
        attrs[1] = 0                                   # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                   # lflag
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        std = getattr(termios, f"B{baud}", None)
        if std is not None:
            attrs[4] = attrs[5] = std
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        else:
            fcntl.ioctl(fd, IOSSIOSPEED, struct.pack("L", baud))
        termios.tcflush(fd, termios.TCIFLUSH)
    except OSError:
        os.close(fd)
        raise
    return fd


class LogWriter:
    """Timestamps lines, appends them to the log and mirrors them."""

    def __init__(self, path: str, mirror: bool) -> None:
        self.path = path
        self.mirror = mirror
        self.at_line_start = True
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "a", encoding="utf-8", errors="replace")

    def _rotate_if_needed(self) -> None:
        if self.f.tell() < ROTATE_BYTES or not self.at_line_start:
            return
        self.f.close()
        os.replace(self.path, self.path + ".1")
        self.f = open(self.path, "a", encoding="utf-8", errors="replace")

    def write(self, text: str) -> None:
        out = []
        for piece in re.split(r"(\n)", text.replace("\r", "")):
            if not piece:
                continue
            if self.at_line_start and piece != "\n":
                now = datetime.datetime.now()
                out.append(now.strftime("%Y-%m-%d %H:%M:%S.") +
                           f"{now.microsecond // 1000:03d} ")
                self.at_line_start = False
            out.append(piece)
            if piece == "\n":
                self.at_line_start = True
        chunk = "".join(out)
        self.f.write(chunk)
        self.f.flush()
        if self.mirror:
            sys.stdout.write(chunk)
            sys.stdout.flush()
        self._rotate_if_needed()

    def note(self, message: str) -> None:
        if not self.at_line_start:
            self.write("\n")
        self.write(f"[console.py] {message}\n")


def cmd_watch(args: argparse.Namespace) -> int:
    writer = LogWriter(log_path(args), mirror=not args.quiet)
    fd = -1
    waiting_reported = False
    try:
        while True:
            if fd < 0:
                port = args.port or find_probe_port()
                try:
                    if port is None:
                        raise FileNotFoundError("Debug Probe not found")
                    fd = open_port(port, args.baud)
                    writer.note(f"reading {port} at {args.baud} baud")
                    waiting_reported = False
                except OSError as exc:
                    if exc.errno == errno.EBUSY:
                        print(f"{port} is in use by another program "
                              "(close CoolTerm first)", file=sys.stderr)
                        return 1
                    if not waiting_reported:
                        writer.note(f"waiting for the probe: {exc}")
                        waiting_reported = True
                    time.sleep(1.0)
                    continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                data = None
            except OSError as exc:
                writer.note(f"port lost: {exc}")
                os.close(fd)
                fd = -1
                continue
            if data:
                writer.write(data.decode("utf-8", errors="replace"))
            else:
                time.sleep(0.02)
    except KeyboardInterrupt:
        return 0
    finally:
        if fd >= 0:
            os.close(fd)


def read_lines(path: str) -> list[str]:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        print(f"no log at {path}: is `console.py watch` running?",
              file=sys.stderr)
        return []


def boot_starts(lines: list[str]) -> list[int]:
    """Index of the first line of each boot (the line before the banner
    when it is the `main()` trace that precedes it)."""
    starts = []
    for i, line in enumerate(lines):
        if BOOT_RE.search(line):
            j = i
            while j > 0 and i - j < 3 and (
                    "main():" in lines[j - 1] or
                    lines[j - 1].split(" ", 2)[-1].strip() == ""):
                j -= 1
            starts.append(j)
    return starts


def lines_since_boot(lines: list[str], boot: int) -> list[str] | None:
    starts = boot_starts(lines)
    if len(starts) < boot:
        return None
    return lines[starts[-boot]:]


def cmd_tail(args: argparse.Namespace) -> int:
    lines = read_lines(log_path(args))
    print("\n".join(lines[-args.n:]))
    return 0


def cmd_since_boot(args: argparse.Namespace) -> int:
    lines = read_lines(log_path(args))
    selected = lines_since_boot(lines, args.boot)
    if selected is None:
        print(f"fewer than {args.boot} boots in the log", file=sys.stderr)
        return 3
    print("\n".join(selected))
    return 0


def cmd_grep(args: argparse.Namespace) -> int:
    lines = read_lines(log_path(args))
    if args.since_boot:
        lines = lines_since_boot(lines, 1) or []
    pattern = re.compile(args.pattern)
    hits = [line for line in lines if pattern.search(line)]
    if not hits:
        return 3
    print("\n".join(hits))
    return 0


def cmd_wait(args: argparse.Namespace) -> int:
    """Wait for a line matching PATTERN that arrives after this call."""
    path = log_path(args)
    pattern = re.compile(args.pattern)
    try:
        offset = os.path.getsize(path)
    except FileNotFoundError:
        offset = 0
    deadline = time.monotonic() + args.timeout
    pending = ""
    while time.monotonic() < deadline:
        try:
            size = os.path.getsize(path)
        except FileNotFoundError:
            size = 0
        if size < offset:        # rotated
            offset = 0
        if size > offset:
            with open(path, encoding="utf-8", errors="replace") as f:
                f.seek(offset)
                pending += f.read()
                offset = f.tell()
            *complete, pending = pending.split("\n")
            for line in complete:
                if pattern.search(line):
                    print(line)
                    return 0
        time.sleep(0.1)
    print(f"timed out after {args.timeout:g} s waiting for "
          f"{args.pattern!r}", file=sys.stderr)
    return 3


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="console.py",
        description="Capture and read the microfirmware debug console.")
    p.add_argument("--log", help=f"log file (default {LOG_DEFAULT})")
    sub = p.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("watch", help="read the probe UART into the log")
    w.add_argument("--port", help="serial device (default: find the probe)")
    w.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    w.add_argument("-q", "--quiet", action="store_true",
                   help="do not mirror to the terminal")
    w.set_defaults(func=cmd_watch)

    t = sub.add_parser("tail", help="print the last N lines")
    t.add_argument("n", nargs="?", type=int, default=50)
    t.set_defaults(func=cmd_tail)

    s = sub.add_parser("since-boot", help="print the log since a boot")
    s.add_argument("--boot", type=int, default=1,
                   help="1 = last boot, 2 = the one before, ...")
    s.set_defaults(func=cmd_since_boot)

    g = sub.add_parser("grep", help="print lines matching a regex")
    g.add_argument("pattern")
    g.add_argument("--since-boot", action="store_true")
    g.set_defaults(func=cmd_grep)

    wt = sub.add_parser("wait", help="wait for a new line matching a regex")
    wt.add_argument("pattern")
    wt.add_argument("--timeout", type=float, default=30.0)
    wt.set_defaults(func=cmd_wait)
    return p


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
