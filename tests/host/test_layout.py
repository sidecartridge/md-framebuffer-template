"""The ST side and the RP side agree on what they share.

The ST's names come from target/atarist/src/inc/sidecart_layout.s (the
window, the command values, the ROM3 signalling windows) and userfw.s (the
blit's chunk size, the audio timer); the RP's from the headers and sources
under rp/src. Each row of PAIRS is one shared constant: adding a constant
both sides must agree on means adding a row. A mismatch is the "works on the
RP, garbage on the ST" bug, so fix the sources, never the table."""

import ast
import os
import re
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ST_SRC = os.path.join(REPO, "target", "atarist", "src")
RP_SRC = os.path.join(REPO, "rp", "src")

MFP_CLOCK_HZ = 2457600
# MFP timer control values in delay mode -> prescaler.
MFP_PRESCALER = {1: 4, 2: 10, 3: 16, 4: 50, 5: 64, 6: 100, 7: 200}
# The STE's DMA sound rates, by the mode register's bits 1-0.
DMA_RATES = [6258, 12517, 25033, 50066]


def evaluate(expr, known):
    """An integer expression of numbers, known names and + - * / << >> & | ^,
    or None when it uses anything else."""
    expr = expr.strip()
    for name in sorted(set(re.findall(r"\b[A-Za-z_]\w*", expr)), key=len, reverse=True):
        if name not in known:
            return None
        expr = re.sub(rf"\b{name}\b", str(known[name]), expr)
    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None
    allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Add,
               ast.Sub, ast.Mult, ast.Div, ast.FloorDiv, ast.LShift,
               ast.RShift, ast.BitAnd, ast.BitOr, ast.BitXor, ast.USub,
               ast.Invert)
    if not all(isinstance(n, allowed) for n in ast.walk(tree)):
        return None
    value = eval(compile(tree, "<layout>", "eval"), {"__builtins__": {}})
    return int(value) if isinstance(value, (int, float)) else None


def asm_equs(*paths):
    """NAME equ EXPR from vasm sources: $hex, %binary and decimal numbers."""
    raw = []
    for path in paths:
        with open(path, encoding="latin-1") as f:
            for line in f:
                m = re.match(r"^([A-Za-z_]\w*):?\s+equ\s+([^;]+)", line, re.I)
                if m:
                    expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", m.group(2))
                    expr = re.sub(r"%([01]+)", r"0b\1", expr)
                    raw.append((m.group(1), expr))
    return resolve(raw)


def c_defines(*paths):
    """#define NAME EXPR from C sources, without casts and number suffixes."""
    raw = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text).replace("\\\n", " ")
        for name, expr in re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)[ \t]+([^\n]+)$",
                                     text, re.M):
            expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr)
            expr = re.sub(r"\(\s*(?:u?int\d+_t|unsigned|int|long|size_t)\s*\)", " ", expr)
            raw.append((name, expr))
    return resolve(raw)


def resolve(raw):
    known = {}
    for _ in range(len(raw) + 1):  # definitions may refer to later ones
        progress = False
        for name, expr in raw:
            if name not in known:
                value = evaluate(expr, known)
                if value is not None:
                    known[name] = value
                    progress = True
        if not progress:
            break
    return known


def st_names():
    return asm_equs(os.path.join(ST_SRC, "inc", "sidecart_layout.s"),
                    os.path.join(ST_SRC, "userfw.s"))


def rp_names():
    inc = os.path.join(RP_SRC, "include")
    return c_defines(os.path.join(inc, "cart_shared.h"), os.path.join(inc, "ikbd.h"),
                     os.path.join(inc, "audio_sample.h"),
                     os.path.join(RP_SRC, "fb.c"), os.path.join(RP_SRC, "audio.c"))


def cartridge_budget():
    """target/atarist/build.sh's limit on BOOT.BIN."""
    with open(os.path.join(REPO, "target", "atarist", "build.sh")) as f:
        return int(re.search(r"^cartridge_max=(\d+)", f.read(), re.M).group(1))


def window(st, name):
    """An ST address in the cartridge window, as an offset into it."""
    return st[name] - st["ROM4_ADDR"]


def rom3(st, name):
    """A ROM3 signalling address, as the low 16 bits the RP's ring captures."""
    return st[name] & 0xFFFF


# (what it is, the ST's value, the RP's value)
PAIRS = [
    # The cartridge code budget, three times: the ST's layout, the RP's, and
    # the check on BOOT.BIN in target/atarist/build.sh.
    ("cartridge code budget", lambda st: st["CARTRIDGE_CODE_SIZE"],
     lambda rp: rp["CART_CARTRIDGE_CODE_SIZE"]),
    ("cartridge code budget in target/atarist/build.sh", lambda st: cartridge_budget(),
     lambda rp: rp["CART_CARTRIDGE_CODE_SIZE"]),
    # The command sentinel, and the values the RP writes to it.
    ("command sentinel", lambda st: window(st, "CMD_MAGIC_SENTINEL_ADDR"),
     lambda rp: rp["CART_CMD_SENTINEL_OFFSET"]),
    ("CMD_NOP", lambda st: st["CMD_NOP"], lambda rp: rp["CART_CMD_NOP"]),
    ("CMD_RESET", lambda st: st["CMD_RESET"], lambda rp: rp["CART_CMD_RESET"]),
    ("CMD_BOOT_GEM", lambda st: st["CMD_BOOT_GEM"], lambda rp: rp["CART_CMD_BOOT_GEM"]),
    ("CMD_START", lambda st: st["CMD_START"], lambda rp: rp["CART_CMD_START"]),
    # The frame counter the blit is gated on.
    ("frame counter", lambda st: window(st, "FB_FRAME_COUNTER_ADDR"),
     lambda rp: rp["CART_FB_FRAME_COUNTER_OFFSET"]),
    # The shared-variable slots and the palette inside them.
    ("shared variables", lambda st: window(st, "SHARED_VARIABLES"),
     lambda rp: rp["CART_SHARED_VARIABLES_OFFSET"]),
    ("IKBD command block", lambda st: window(st, "IKBD_OUT_ADDR"),
     lambda rp: rp["CART_IKBD_OUT_OFFSET"]),
    ("IKBD command block size", lambda st: st["IKBD_OUT_SIZE"],
     lambda rp: rp["CART_IKBD_OUT_SIZE"]),
    ("IKBD command bytes", lambda st: st["IKBD_OUT_MAX"], lambda rp: rp["CART_IKBD_OUT_MAX"]),
    ("IKBD command bytes' offset", lambda st: window(st, "IKBD_OUT_BYTES"),
     lambda rp: rp["CART_IKBD_OUT_OFFSET"] + 4),
    ("IKBD command busy bit", lambda st: 1 << st["IKBD_OUT_BUSY_BIT"],
     lambda rp: rp["CART_IKBD_OUT_BUSY"]),
    ("palette", lambda st: window(st, "PALETTE_ADDR"), lambda rp: rp["CART_PALETTE_OFFSET"]),
    ("palette size", lambda st: st["PALETTE_SIZE"], lambda rp: rp["CART_PALETTE_SIZE"]),
    # The audio buffer.
    ("audio buffer", lambda st: window(st, "AUDIO_BUFFER_ADDR"),
     lambda rp: rp["CART_AUDIO_BUFFER_OFFSET"]),
    ("audio buffer size", lambda st: st["AUDIO_BUFFER_SIZE"],
     lambda rp: rp["CART_AUDIO_BUFFER_SIZE"]),
    ("audio slices", lambda st: st["AUDIO_SLICES"], lambda rp: rp["CART_AUDIO_SLICES"]),
    ("audio slice size", lambda st: st["AUDIO_SLICE_BYTES"],
     lambda rp: rp["CART_AUDIO_SLICE_BYTES"]),
    ("DMA ring", lambda st: st["AUDIO_DMA_RING_BYTES"],
     lambda rp: rp["CART_AUDIO_DMA_RING_BYTES"]),
    ("DMA position unit", lambda st: st["AUDIO_DMA_POS_UNIT"],
     lambda rp: rp["CART_AUDIO_DMA_POS_UNIT"]),
    # The profiles: the word, and what each one sets on the ST.
    ("profile word", lambda st: window(st, "PROFILE_ADDR"), lambda rp: rp["CART_PROFILE_OFFSET"]),
    *[(f"profile {p}: {what}", lambda st, k=f"PROFILE_{p}_{name}": st[k],
       lambda rp, k=f"CART_PROFILE_{p}_{name}": rp[k])
      for p in ("50FPS", "25FPS")
      for what, name in (("VBLs a frame", "VBLS"), ("Timer-B count", "TIMERB_COUNT"),
                         ("DMA mode", "DMA_MODE"), ("DMA lead", "DMA_LEAD"))],
    ("profile: 50 fps", lambda st: st["PROFILE_50FPS"], lambda rp: rp["CART_PROFILE_50FPS"]),
    ("profile: 25 fps", lambda st: st["PROFILE_25FPS"], lambda rp: rp["CART_PROFILE_25FPS"]),
    ("audio output word", lambda st: window(st, "AUDIO_OUT_ADDR"),
     lambda rp: rp["CART_AUDIO_OUT_OFFSET"]),
    ("audio output: auto", lambda st: st["AUDIO_OUT_AUTO"], lambda rp: rp["CART_AUDIO_OUT_AUTO"]),
    ("audio output: YM", lambda st: st["AUDIO_OUT_YM"], lambda rp: rp["CART_AUDIO_OUT_YM"]),
    ("copy mode word", lambda st: window(st, "BLIT_MODE_ADDR"),
     lambda rp: rp["CART_BLIT_MODE_OFFSET"]),
    ("copy mode: auto", lambda st: st["BLIT_MODE_AUTO"], lambda rp: rp["CART_BLIT_MODE_AUTO"]),
    ("copy mode: CPU", lambda st: st["BLIT_MODE_CPU"], lambda rp: rp["CART_BLIT_MODE_CPU"]),
    ("copy mode: blitter", lambda st: st["BLIT_MODE_BLITTER"],
     lambda rp: rp["CART_BLIT_MODE_BLITTER"]),
    # The boot block the ST reads in pre_auto.
    ("boot status", lambda st: window(st, "BOOT_STATUS_ADDR"),
     lambda rp: rp["CART_BOOT_STATUS_OFFSET"]),
    ("boot message", lambda st: window(st, "BOOT_MESSAGE_ADDR"),
     lambda rp: rp["CART_BOOT_MESSAGE_OFFSET"]),
    ("boot message size", lambda st: st["BOOT_MESSAGE_SIZE"],
     lambda rp: rp["CART_BOOT_MESSAGE_SIZE"]),
    # The app's own arena.
    ("APP_FREE", lambda st: window(st, "APP_FREE_ADDR"), lambda rp: rp["CART_APP_FREE_OFFSET"]),
    # The framebuffer, and the blit's shape the RP lays it out for.
    ("framebuffer", lambda st: window(st, "FRAMEBUFFER_ADDR"),
     lambda rp: rp["CART_FRAMEBUFFER_OFFSET"]),
    ("framebuffer size", lambda st: st["FRAMEBUFFER_SIZE"],
     lambda rp: rp["CART_FRAMEBUFFER_SIZE"]),
    ("lines the blit copies", lambda st: st["FB_COPY_LINES"],
     lambda rp: rp["CART_FB_BLIT_LINES"]),
    ("bytes the blit copies", lambda st: st["FBDRV_TOTAL_BYTES"],
     lambda rp: rp["CART_FB_BLIT_BYTES"]),
    ("MOVEM chunk", lambda st: st["FBDRV_ITER_BYTES"], lambda rp: rp["CART_FB_CHUNK_BYTES"]),
    ("MOVEM chunks", lambda st: st["FBDRV_MAIN_ITERS"], lambda rp: rp["CART_FB_CHUNK_COUNT"]),
    ("MOVEM tail", lambda st: st["FBDRV_TAIL_BYTES"], lambda rp: rp["CART_FB_CHUNK_TAIL"]),
    # The ROM3 signalling windows the RP's ring filters on.
    ("IKBD byte window", lambda st: rom3(st, "IKBD_WINDOW_BASE"),
     lambda rp: rp["IKBD_WINDOW_LO16"]),
    ("blit-done window", lambda st: rom3(st, "VBLSYNC_ADDR"),
     lambda rp: rp["FB_VBLSYNC_HIBYTE"]),
    ("blit-done window (cart_shared.h)", lambda st: rom3(st, "VBLSYNC_ADDR"),
     lambda rp: rp["CART_ROM3_BLIT_DONE_WINDOW"]),
    ("audio slice window", lambda st: rom3(st, "AUDIO_SLICE_WINDOW"),
     lambda rp: rp["CART_ROM3_AUDIO_SLICE_WINDOW"]),
    ("ACIA overrun window", lambda st: rom3(st, "IKBD_OVERRUN_ADDR"),
     lambda rp: rp["CART_ROM3_IKBD_OVERRUN_WINDOW"]),
    ("IKBD byte count window", lambda st: rom3(st, "IKBD_COUNT_WINDOW"),
     lambda rp: rp["CART_ROM3_IKBD_COUNT_WINDOW"]),
    ("IKBD commands sent window", lambda st: rom3(st, "IKBD_OUT_WINDOW"),
     lambda rp: rp["CART_ROM3_IKBD_OUT_WINDOW"]),
    ("hello window", lambda st: rom3(st, "ST_HELLO_WINDOW"),
     lambda rp: rp["CART_ROM3_HELLO_WINDOW"]),
    ("TOS high byte window", lambda st: rom3(st, "ST_TOS_HI_WINDOW"),
     lambda rp: rp["CART_ROM3_TOS_HI_WINDOW"]),
    ("TOS low byte window", lambda st: rom3(st, "ST_TOS_LO_WINDOW"),
     lambda rp: rp["CART_ROM3_TOS_LO_WINDOW"]),
    ("stopwatch point window", lambda st: rom3(st, "STUDY_POINT_WINDOW"),
     lambda rp: rp["CART_ROM3_STUDY_POINT_WINDOW"]),
    ("stopwatch ticks high window", lambda st: rom3(st, "STUDY_HI_WINDOW"),
     lambda rp: rp["CART_ROM3_STUDY_HI_WINDOW"]),
    ("stopwatch ticks low window", lambda st: rom3(st, "STUDY_LO_WINDOW"),
     lambda rp: rp["CART_ROM3_STUDY_LO_WINDOW"]),
    ("DMA position window", lambda st: rom3(st, "DMA_POS_WINDOW"),
     lambda rp: rp["CART_ROM3_DMA_POS_WINDOW"]),
    ("ST features window", lambda st: rom3(st, "ST_FEATURES_WINDOW"),
     lambda rp: rp["CART_ROM3_ST_FEATURES_WINDOW"]),
    ("ST feature: blitter", lambda st: st["ST_FEATURE_BLITTER"],
     lambda rp: rp["CART_ST_FEATURE_BLITTER"]),
    # The stopwatch: Timer-A's tick, in nanoseconds (fb.c converts).
    ("stopwatch tick (ns)", lambda st: round(
        MFP_PRESCALER[st["TIMERA_STUDY_DIV10"]] * 1e9 / MFP_CLOCK_HZ),
     lambda rp: rp["FB_STOPWATCH_TICK_NS"]),
    # The sample rates: Timer-B plays what the RP converts at its rate, the
    # DMA chip what it converts at the chip's.
    *[(f"profile {p}: YM rate (Hz)", lambda st, p=p: round(
        MFP_CLOCK_HZ / (MFP_PRESCALER[st["TIMERB_PRESCALER"]] * st[f"PROFILE_{p}_TIMERB_COUNT"])),
       lambda rp, p=p: rp[f"CART_PROFILE_{p}_YM_RATE_HZ"]) for p in ("50FPS", "25FPS")],
    *[(f"profile {p}: DMA rate (Hz)", lambda st, p=p: DMA_RATES[st[f"PROFILE_{p}_DMA_MODE"] & 3],
       lambda rp, p=p: rp[f"CART_PROFILE_{p}_DMA_RATE_HZ"]) for p in ("50FPS", "25FPS")],
    # YM sources (the built-in jingle, .YMS files) at the RP's source rate.
    ("built-in jingle's sample rate (Hz)", lambda st: rp_names()["AUDIO_YM_SOURCE_RATE_HZ"],
     lambda rp: rp["AUDIO_SAMPLE_RATE_HZ"]),
]
PROFILES = ("50FPS", "25FPS")


class Layout(unittest.TestCase):

    def test_pairs_agree(self):
        st, rp = st_names(), rp_names()
        for what, st_value, rp_value in PAIRS:
            with self.subTest(what):
                self.assertEqual(st_value(st), rp_value(rp),
                                 f"{what}: the ST side and the RP side differ")

    def test_audio_bytes_per_frame(self):
        """In each profile the RP refills what the ST plays in one frame: two
        bytes per Timer-B interrupt, within one sample; and a DMA FIFO slot
        holds a VBL of the chip's samples."""
        st, rp = st_names(), rp_names()
        for p in PROFILES:
            with self.subTest(p):
                rate = MFP_CLOCK_HZ / (MFP_PRESCALER[st["TIMERB_PRESCALER"]] *
                                       st[f"PROFILE_{p}_TIMERB_COUNT"])
                samples = rate * rp["AUDIO_FRAME_PERIOD_US"] / 1e6
                ym = rp[f"CART_PROFILE_{p}_YM_BYTES"]
                self.assertLess(abs(ym / 2 - samples), 1,
                                f"{ym} bytes per frame for {samples:.1f} samples")
                self.assertEqual(ym % 2, 0)
                dma = rp[f"CART_PROFILE_{p}_DMA_RATE_HZ"] * rp["AUDIO_FRAME_PERIOD_US"] / 1e6
                self.assertGreaterEqual(rp[f"CART_PROFILE_{p}_DMA_BYTES"], dma)

    def test_audio_slices(self):
        """The slices fit in the audio buffer, a VBL of samples fits in one in
        each profile, and the ST's shift is the slice size."""
        st, rp = st_names(), rp_names()
        self.assertLessEqual(rp["CART_AUDIO_SLICES"] * rp["CART_AUDIO_SLICE_BYTES"],
                             rp["CART_AUDIO_BUFFER_SIZE"])
        for p in PROFILES:
            self.assertLessEqual(rp[f"CART_PROFILE_{p}_YM_BYTES"], rp["CART_AUDIO_SLICE_BYTES"])
        self.assertEqual(1 << st["AUDIO_SLICE_SHIFT"], st["AUDIO_SLICE_BYTES"])
        slices = rp["CART_AUDIO_SLICES"]
        self.assertEqual(slices & (slices - 1), 0, "the ST masks the slice number")

    def test_dma_ring(self):
        """The DMA ring masks as a power of two and its position report fits
        a byte; in each profile the lead goes by the report's unit, covers
        three VBLs and stays under half a ring, and the chip plays mono."""
        st, rp = st_names(), rp_names()
        ring, unit = rp["CART_AUDIO_DMA_RING_BYTES"], rp["CART_AUDIO_DMA_POS_UNIT"]
        self.assertEqual(ring & (ring - 1), 0)
        self.assertEqual(ring // unit, 256, "the ST reports the position in one byte")
        self.assertEqual(st["UFW_DMA_RING"] % ring, 0, "the ST masks ring offsets")
        self.assertEqual(1 << 4, st["AUDIO_DMA_POS_UNIT"], "the ST shifts by 4")
        for p in PROFILES:
            with self.subTest(p):
                lead = rp[f"CART_PROFILE_{p}_DMA_LEAD"]
                self.assertEqual(lead % unit, 0)
                self.assertLess(lead, ring // 2)
                self.assertGreaterEqual(lead, 3 * rp[f"CART_PROFILE_{p}_DMA_RATE_HZ"] // 50)
                self.assertTrue(st[f"PROFILE_{p}_DMA_MODE"] & 0x80, "mono")

    def test_rom3_windows_distinct(self):
        """Every ROM3 signalling window has a high byte of its own."""
        rp = rp_names()
        windows = {n: v for n, v in rp.items()
                   if n.startswith("CART_ROM3_") and n.endswith("_WINDOW")}
        windows["IKBD_WINDOW_LO16"] = rp["IKBD_WINDOW_LO16"]
        by_value = {}
        for name, value in windows.items():
            by_value.setdefault(value & rp["CART_ROM3_WINDOW_MASK"], []).append(name)
        for value, names in by_value.items():
            with self.subTest(f"${value:04X}"):
                self.assertEqual(len(names), 1, names)

    def test_ikbd_command_slots(self):
        """The IKBD command block is shared-variable slots, not the palette's."""
        rp = rp_names()
        offset, size = rp["CART_IKBD_OUT_OFFSET"], rp["CART_IKBD_OUT_SIZE"]
        start = rp["CART_SHARED_VARIABLES_OFFSET"]
        self.assertEqual((offset - start) % 4, 0)
        self.assertGreaterEqual(offset, start)
        self.assertLessEqual(offset + size, start + rp["CART_SHARED_VARIABLES_SLOTS"] * 4)
        palette = rp["CART_PALETTE_OFFSET"]
        self.assertTrue(offset + size <= palette or offset >= palette + rp["CART_PALETTE_SIZE"])

    def test_window_blocks_in_order(self):
        """The blocks do not overlap, and the framebuffer ends the window."""
        rp = rp_names()
        blocks = [
            ("CART_CMD_SENTINEL_OFFSET", 4), ("CART_FB_FRAME_COUNTER_OFFSET", 4),
            ("CART_SHARED_VARIABLES_OFFSET", rp["CART_SHARED_VARIABLES_SLOTS"] * 4),
            ("CART_AUDIO_BUFFER_OFFSET", rp["CART_AUDIO_BUFFER_SIZE"]),
            ("CART_BOOT_STATUS_OFFSET", 2),
            ("CART_BOOT_MESSAGE_OFFSET", rp["CART_BOOT_MESSAGE_SIZE"]),
            ("CART_APP_FREE_OFFSET", 0),
            ("CART_FRAMEBUFFER_OFFSET", rp["CART_FRAMEBUFFER_SIZE"]),
        ]
        self.assertGreaterEqual(rp["CART_CMD_SENTINEL_OFFSET"], rp["CART_CARTRIDGE_CODE_SIZE"])
        for (a, size), (b, _) in zip(blocks, blocks[1:]):
            with self.subTest(f"{a} before {b}"):
                self.assertLessEqual(rp[a] + size, rp[b])
        self.assertEqual(rp["CART_FRAMEBUFFER_OFFSET"] + rp["CART_FRAMEBUFFER_SIZE"],
                         rp["CART_REGION_END"])


if __name__ == "__main__":
    unittest.main()
