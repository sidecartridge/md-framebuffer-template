"""tools/png_to_bitmap.py, the indexed-PNG converter: every PNG filter and
bit depth it reads, the palette as ST words, the key colour, tiles in
order, the images it refuses, and a header that compiles and gives back
every pixel through FB_BITMAP."""

import contextlib
import importlib.util
import io
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

from test_layout import REPO, RP_SRC

TOOL = os.path.join(REPO, "tools", "png_to_bitmap.py")
_spec = importlib.util.spec_from_file_location("png_to_bitmap", TOOL)
tool = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tool)

HERE = os.path.dirname(os.path.abspath(__file__))
PALETTE = [(0, 0, 0), (255, 255, 255), (219, 36, 0), (0, 146, 73),
           (36, 73, 255), (255, 219, 0), (109, 109, 109), (182, 0, 182)]


def chunk(kind, body):
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body)))


def filtered(kind, row, prev):
    """Apply PNG filter `kind` to one row (the encoder's side)."""
    out = bytearray()
    for i, x in enumerate(row):
        left = row[i - 1] if i else 0
        up = prev[i]
        corner = prev[i - 1] if i else 0
        if kind == 0:
            pred = 0
        elif kind == 1:
            pred = left
        elif kind == 2:
            pred = up
        elif kind == 3:
            pred = (left + up) // 2
        else:
            p = left + up - corner
            pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
            pred = left if pa <= pb and pa <= pc else up if pb <= pc else corner
        out.append((x - pred) & 0xFF)
    return bytes([kind]) + bytes(out)


def write_png(path, width, height, pixels, depth=8, palette=PALETTE,
              alpha=None, colour_type=3):
    stride = (width * depth + 7) // 8
    raw, prev = b"", bytes(stride)
    for y in range(height):
        row = bytearray(stride)
        for x in range(width):
            v = pixels[y * width + x]
            bit = x * depth
            row[bit // 8] |= v << (8 - depth - bit % 8)
        raw += filtered(y % 5, bytes(row), prev)  # every filter, in turn
        prev = bytes(row)
    data = tool.PNG_SIGNATURE + chunk(
        b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour_type, 0, 0, 0))
    data += chunk(b"PLTE", bytes(c for rgb in palette for c in rgb))
    if alpha is not None:
        data += chunk(b"tRNS", bytes(alpha))
    data += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def pattern(width, height, colours=len(PALETTE)):
    return [(x * 3 + y * 5 + x * y) % colours for y in range(height) for x in range(width)]


class Reading(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.png = os.path.join(self.dir, "t.png")

    def test_every_filter_at_every_depth(self):
        for depth in (1, 2, 4, 8):
            colours = min(1 << depth, len(PALETTE))
            for width in (1, 5, 16, 23):
                with self.subTest(depth=depth, width=width):
                    pixels = pattern(width, 11, colours)
                    write_png(self.png, width, 11, pixels, depth)
                    w, h, got, palette, transparent = tool.read_indexed_png(self.png)
                    self.assertEqual((w, h, got), (width, 11, pixels))
                    self.assertEqual((palette, transparent), (PALETTE, []))

    def test_palette_as_st_words(self):
        self.assertEqual([tool.st_colour(c) for c in PALETTE],
                         [0x000, 0x777, 0x610, 0x042, 0x127, 0x760, 0x333, 0x505])

    def test_refused_images(self):
        cases = {
            "not indexed": dict(colour_type=2, pixels=[0] * 12),
            "colour 16": dict(pixels=[16] + [0] * 11,
                              palette=[(i * 8, 0, 0) for i in range(17)]),
            "two transparent entries": dict(pixels=[0] * 12, alpha=[0, 0]),
            "half transparent": dict(pixels=[0] * 12, alpha=[255, 128]),
        }
        for what, kw in cases.items():
            with self.subTest(what):
                write_png(self.png, 4, 3, **kw)
                with self.assertRaises(tool.PngError):
                    tool.to_header(self.png, "t")
        write_png(self.png, 5, 3, [0] * 15)
        with self.assertRaises(tool.PngError):  # not whole tiles
            tool.to_header(self.png, "t", (2, 3))


class Header(unittest.TestCase):
    """The header, compiled with the firmware's fb_blit.h and read back."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)

    def compile_and_dump(self, header, name, count):
        with open(os.path.join(self.dir, "img.h"), "w") as f:
            f.write(header)
        src = os.path.join(self.dir, "dump.c")
        exe = os.path.join(self.dir, "dump")
        bitmaps = f"{name}" if count else f"(&{name})"
        with open(src, "w") as f:
            f.write(f"""#include <stdio.h>
#include "img.h"
int main(void) {{
  for (int i = 0; i < 16; i++) printf("%d ", {name}_palette[i]);
  printf("\\n");
  for (int n = 0; n < {count or 1}; n++) {{
    const struct FB_BITMAP *b = &{bitmaps}[n];
    printf("%d %d", b->width, b->height);
    for (int i = 0; i < b->width * b->height; i++) printf(" %d", b->data[i]);
    printf("\\n");
  }}
  return 0;
}}
""")
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", self.dir,
                        "-I", os.path.join(HERE, "shim"), "-I", os.path.join(RP_SRC, "include"),
                        "-o", exe, src], check=True)
        lines = subprocess.run([exe], check=True, capture_output=True,
                               text=True).stdout.split("\n")
        palette = [int(v) for v in lines[0].split()]
        bitmaps = [[int(v) for v in line.split()] for line in lines[1:] if line]
        return palette, bitmaps

    def test_one_bitmap_with_a_key(self):
        png = os.path.join(self.dir, "ship.png")
        pixels = pattern(12, 7)
        write_png(png, 12, 7, pixels, alpha=[255, 255, 255, 0])
        header = tool.to_header(png, "ship")
        self.assertIn("#define SHIP_KEY 3", header)
        self.assertIn("#define SHIP_W 12", header)
        palette, bitmaps = self.compile_and_dump(header, "ship", 0)
        self.assertEqual(palette, [tool.st_colour(c) for c in PALETTE] + [0] * 8)
        self.assertEqual(bitmaps, [[12, 7] + pixels])

    def test_tiles_left_to_right_then_down(self):
        png = os.path.join(self.dir, "sheet.png")
        width, height, tw, th = 12, 6, 4, 3
        pixels = pattern(width, height)
        write_png(png, width, height, pixels, depth=4)
        header = tool.to_header(png, "sheet", (tw, th))
        self.assertIn("#define SHEET_COUNT 6", header)
        self.assertNotIn("SHEET_KEY", header)
        _, bitmaps = self.compile_and_dump(header, "sheet", 6)
        for n, got in enumerate(bitmaps):
            tx, ty = (n % 3) * tw, (n // 3) * th
            want = [pixels[(ty + y) * width + tx + x] for y in range(th) for x in range(tw)]
            self.assertEqual(got, [tw, th] + want, n)

    def test_command_line(self):
        png = os.path.join(self.dir, "gem.png")
        out = os.path.join(self.dir, "gem.h")
        write_png(png, 8, 8, pattern(8, 8))
        self.assertEqual(tool.main([png, "--out", out, "--tile", "4", "8"]), 0)
        with open(out) as f:
            self.assertIn("static const struct FB_BITMAP gem[GEM_COUNT]", f.read())
        write_png(png, 8, 8, [0] * 64, colour_type=2)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(tool.main([png, "--out", out]), 1)
        self.assertIn("not an indexed-colour PNG", err.getvalue())


if __name__ == "__main__":
    unittest.main()
