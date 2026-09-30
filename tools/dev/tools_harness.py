#!/usr/bin/env python3
"""Exercise every tools/dev command against the running device and say, check
by check, whether it behaves.

Needs a debug build on the RP (the mailbox commands), the Debug Probe, and
`tools/dev/console.py watch` running: this reads the console *log*
(tools/dev/logs/console.log) and never opens the UART. The ST need not run
the app: the checks hold either way. SELECT restarts the RP on the way.

    python3 tools/dev/tools_harness.py            # device checks
    python3 tools/dev/tools_harness.py --build    # also build both types and check them
    python3 tools/dev/tools_harness.py --flash    # also flash the debug build first
    python3 tools/dev/tools_harness.py --reset    # also restart the RP at the end

Leaves the demo menu on screen. Exits 0 when every check passes. Writes
tools/dev/logs/tools-harness-<time>.json.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

DEV = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(DEV, "..", ".."))
LOG = os.path.join(DEV, "logs", "console.log")
BOOTED = r"Entering main loop"
sys.path.insert(0, DEV)
import swd  # noqa: E402

results = []


def check(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    return ok


def run(*args, timeout=600):
    p = subprocess.run(args, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def swdpy(*args, timeout=120):
    return run(sys.executable, os.path.join(DEV, "swd.py"), *args, timeout=timeout)


def first_line(out):
    return out.strip().splitlines()[0] if out.strip() else ""


class LogCursor:
    """What the console log gained since the cursor was placed."""

    def __init__(self):
        self.pos = os.path.getsize(LOG) if os.path.exists(LOG) else 0

    def new(self):
        if not os.path.exists(LOG):
            return ""
        size = os.path.getsize(LOG)
        start = self.pos if size >= self.pos else 0  # rotated
        with open(LOG, "rb") as f:
            f.seek(start)
            return f.read().decode("utf-8", "replace")

    def wait(self, pattern, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.new()):
                return True
            time.sleep(0.2)
        return False


def frames_published():
    rc, out = swdpy("counters")
    m = re.search(r"frames published (\d+)", out)
    return int(m.group(1)) if m else None


def publish_rate(seconds=2.0):
    """Frames published per second, from two readings of the counter."""
    a, t0 = frames_published(), time.monotonic()
    time.sleep(seconds)
    b, t1 = frames_published(), time.monotonic()
    return None if None in (a, b) else (b - a) / (t1 - t0)


# ---------------------------------------------------------------------------
def build_checks():
    print("build")
    for t in ("release", "debug"):
        rc, out = run(os.path.join(DEV, "flash.sh"), t, "--build-only", timeout=900)
        check(f"{t} builds", rc == 0, out.strip().splitlines()[-1] if out.strip() else "")
    flags = {t: open(os.path.join(DEV, "builds", t, "CMakeFiles", "rp.dir", "flags.make")).read()
             for t in ("release", "debug")}
    check("both types compile at -O3 (CMake Release)",
          all("-O3" in f for f in flags.values()))
    check("release compiles with _DEBUG=0", "-D_DEBUG=0" in flags["release"])
    check("debug compiles with _DEBUG=1", "-D_DEBUG=1" in flags["debug"])
    check("921,600 baud in debug only",
          "PICO_DEFAULT_UART_BAUD_RATE=921600" in flags["debug"]
          and "PICO_DEFAULT_UART_BAUD_RATE" not in flags["release"])
    for t, want in (("release", False), ("debug", True)):
        syms = swd.elf_symbols(os.path.join(DEV, "builds", t, "rp.elf"),
                               "devhooksMailbox", "release_build_id",
                               "__rom_in_ram_start__", "fbAckTimeouts")
        check(f"{t} ELF keeps symbols", "__rom_in_ram_start__" in syms
              and "fbAckTimeouts" in syms)
        check(f"{t} ELF carries release_build_id", "release_build_id" in syms)
        check(f"{t} {'has' if want else 'has no'} devhooks mailbox",
              ("devhooksMailbox" in syms) == want)
    a = open(os.path.join(DEV, "builds", "release", "rp.uf2"), "rb").read()
    b = open(os.path.join(DEV, "builds", "debug", "rp.uf2"), "rb").read()
    check("release and debug images differ", a != b)


def device_checks(do_reset):
    elf = os.path.join(DEV, "builds", "debug", "rp.elf")
    print("device")
    rc, out = swdpy("running", elf, "--timeout", "10")
    check("RP runs the debug ELF", rc == 0, out.strip())
    rc, out = swdpy("verify", elf)
    check("flash matches the ELF", rc == 0, out.strip())
    built = open(os.path.join(DEV, "builds", "debug", "generated", "build_id",
                              "build_id.h")).read()
    want = re.search(r'RELEASE_BUILD_ID "(.*)"', built).group(1)
    rc, out = swdpy("build-id")
    check("build ID on the device is the one built", out.strip() == want,
          f"device {out.strip()}, built {want}")

    print("console log (read-only)")
    rc, out = run(sys.executable, os.path.join(DEV, "console.py"), "since-boot")
    check("boot reached the main loop", BOOTED in out)
    check("no panic or hard fault since boot",
          not re.search(r"PANIC|HardFault|\*\*\* ", out))
    tail = out.splitlines()[-15:]
    printable = sum(ch.isprintable() or ch in "\t" for ln in tail for ch in ln)
    total = sum(len(ln) for ln in tail) or 1
    check("console text is clean (baud rate matches)", printable / total > 0.98,
          f"{100 * printable / total:.1f}% printable in the last 15 lines")

    print("counters, heap, shared (no halt)")
    a = frames_published()
    time.sleep(1.0)
    b = frames_published()
    check("counters: the app publishes frames", None not in (a, b) and b > a,
          f"frames published {a} -> {b}")
    rc, out = swdpy("heap")
    check("heap walks newlib's chunks", rc == 0 and "in use" in out, first_line(out))
    rc, out = swdpy("shared")
    check("shared reads the sentinel, the frame counter and the boot status",
          rc == 0 and all(s in out for s in ("command sentinel", "frame counter",
                                             "boot status")))

    print("mailbox: key, app")
    rc, _ = swdpy("app", "menu")
    check("app menu", rc == 0)
    cur = LogCursor()
    rc, out = swdpy("key", "1")
    check("key 1 launches the first demo", rc == 0 and cur.wait(r"starting demo 'uridium'"),
          out.strip())
    cur = LogCursor()
    rc, out = swdpy("key", "esc")
    check("key esc goes back to the menu",
          rc == 0 and cur.wait(r"ESC from demo 'uridium' -> back to menu"), out.strip())
    cur = LogCursor()
    rc, out = swdpy("app", "demo", "2")
    check("app demo 2 launches the second demo",
          rc == 0 and cur.wait(r"starting demo '3d'"), out.strip())

    print("fb (core 0 stops for each dump)")
    png = os.path.join(DEV, "logs", "tools-harness-fb.png")
    rc, out = swdpy("fb", png, "--scale", "1")
    check("fb writes a PNG of a published frame",
          rc == 0 and os.path.getsize(png) > 1000 and "frame" in out, out.strip())
    rc, out = swdpy("fb", png, "--pair", "--scale", "1")
    nums = [int(n) for n in re.findall(r"\(frame (\d+)", out)]
    check("fb --pair grabs two consecutive frames",
          rc == 0 and len(nums) == 2 and (nums[1] - nums[0]) & 0xFFFF == 1, str(nums))
    a = frames_published()
    time.sleep(1.0)
    b = frames_published()
    check("the app runs on after the grabs", None not in (a, b) and b > a,
          f"frames published {a} -> {b}")

    print("slow_frame, overlay")
    base = publish_rate()
    rc, _ = swdpy("app", "slow_frame", "100")
    slow = publish_rate()
    rc2, _ = swdpy("app", "slow_frame", "0")
    again = publish_rate()
    check("slow_frame 100 slows the publishes, 0 restores them",
          rc == 0 and rc2 == 0 and None not in (base, slow, again)
          and slow < 0.6 * base and again > 0.8 * base,
          f"{base:.1f} -> {slow:.1f} -> {again:.1f} frames/s"
          if None not in (base, slow, again) else "")
    rc, _ = swdpy("app", "overlay", "0")
    rc2, _ = swdpy("app", "overlay", "1")
    check("app overlay 0 and 1", rc == 0 and rc2 == 0)
    swdpy("app", "menu")

    print("crash, postmortem")
    rc, out = swdpy("crash")
    check("crash reads the watchdog reason", rc == 0 and "watchdog reason" in out)
    rc, out = swdpy("postmortem", timeout=180)
    check("postmortem: backtrace reaches emul_start and both cores resume",
          rc == 0 and "emul_start" in out and "fb_core1_loop" in out
          and "both cores released" in out)
    a = frames_published()
    time.sleep(1.0)
    b = frames_published()
    check("the app runs on after the postmortem", None not in (a, b) and b > a,
          f"frames published {a} -> {b}")

    print("select short (restarts the RP)")
    cur = LogCursor()
    rc, out = swdpy("select", "short")
    check("select short: SELECT seen, the RP restarts into its main loop",
          rc == 0 and cur.wait(r"SELECT button pushed") and cur.wait(BOOTED, 30),
          out.strip())

    if do_reset:
        print("reset")
        cur = LogCursor()
        rc, out = swdpy("reset")
        booted = cur.wait(BOOTED, timeout=30)
        rc2, _ = swdpy("running", elf, "--timeout", "10")
        check("reset: the RP reboots into the same firmware", rc == 0 and booted and rc2 == 0)
        a = frames_published()
        time.sleep(1.0)
        b = frames_published()
        check("reset: core 1 is alive (frames are published)",
              None not in (a, b) and b > a, f"frames published {a} -> {b}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()
    started = time.strftime("%Y%m%d-%H%M%S")
    os.makedirs(os.path.join(DEV, "logs"), exist_ok=True)
    if args.build:
        build_checks()
    if args.flash:
        print("flash")
        cur = LogCursor()
        rc, out = run(os.path.join(DEV, "flash.sh"), "debug", timeout=900)
        check("flash.sh debug builds, flashes and verifies", rc == 0,
              out.strip().splitlines()[-1] if out.strip() else "")
        cur.wait(BOOTED, 30)
    device_checks(args.reset)
    failed = [r for r in results if not r["ok"]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    report = os.path.join(DEV, "logs", f"tools-harness-{started}.json")
    with open(report, "w") as f:
        json.dump({"started": started, "results": results}, f, indent=2)
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
