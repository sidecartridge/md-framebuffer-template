#!/usr/bin/env python3
"""Convert an indexed-colour PNG into a C header of FB_BITMAP images for
fb_blit() / fb_blit_key() (fb_blit.h), with the PNG's palette as ST colour
words for palette_set() (palette.h).

Draw with at most 16 colours in an editor that saves indexed PNGs (Aseprite,
GIMP: Image > Mode > Indexed, GrafX2...): a pixel's palette index is the
colour it draws, so every image of an app shares one palette, the one the app
sets. One palette entry may be fully transparent (the PNG's tRNS chunk): it
becomes NAME_KEY, the key colour fb_blit_key() skips.

  png_to_bitmap.py SHEET.png --out sheet.h [--name sheet] [--tile W H]

Without --tile the header holds one FB_BITMAP, NAME. With --tile the image
is cut into W x H tiles, left to right then top to bottom, and NAME is an
array of NAME_COUNT bitmaps: a tile set, or the frames of an animation.

The palette words are the ST's 0RRR0GGG0BBB: each 8-bit channel keeps its
top three bits. Plain Python 3, no Pillow.
"""

import argparse
import os
import re
import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_COLOURS = 16


class PngError(ValueError):
    pass


def read_indexed_png(path):
    """Width, height, the pixels' palette indices (row by row), the palette
    as (r, g, b) and the fully transparent entries."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(PNG_SIGNATURE):
        raise PngError(f"{path}: not a PNG")
    pos, header, palette, alpha, idat = len(PNG_SIGNATURE), None, None, [], b""
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            alpha = list(body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None or palette is None:
        raise PngError(f"{path}: no IHDR or no palette")
    width, height, depth, colour_type, _, _, interlace = header
    if colour_type != 3:
        raise PngError(f"{path}: not an indexed-colour PNG (save it with a "
                       f"palette of at most {MAX_COLOURS} colours)")
    if depth not in (1, 2, 4, 8) or interlace:
        raise PngError(f"{path}: {depth}-bit or interlaced, not supported")
    stride = (width * depth + 7) // 8
    raw = zlib.decompress(idat)
    rows, prev = [], bytes(stride)
    for y in range(height):
        start = y * (stride + 1)
        row = unfilter(raw[start], bytearray(raw[start + 1:start + 1 + stride]), prev)
        rows.append(row)
        prev = row
    pixels = []
    per_byte = 8 // depth
    for row in rows:
        for x in range(width):
            byte = row[x // per_byte]
            shift = 8 - depth * (x % per_byte + 1)
            pixels.append((byte >> shift) & ((1 << depth) - 1))
    transparent = [i for i, a in enumerate(alpha) if a == 0]
    for i, a in enumerate(alpha):
        if 0 < a < 255:
            raise PngError(f"{path}: palette entry {i} is half transparent")
    return width, height, pixels, palette, transparent


def unfilter(kind, row, prev):
    """Undo one row's PNG filter (a byte per pixel step for indexed images)."""
    for i in range(len(row)):
        left = row[i - 1] if i else 0
        up = prev[i]
        corner = prev[i - 1] if i else 0
        if kind == 1:
            row[i] = (row[i] + left) & 0xFF
        elif kind == 2:
            row[i] = (row[i] + up) & 0xFF
        elif kind == 3:
            row[i] = (row[i] + (left + up) // 2) & 0xFF
        elif kind == 4:
            p = left + up - corner
            pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
            pred = left if pa <= pb and pa <= pc else up if pb <= pc else corner
            row[i] = (row[i] + pred) & 0xFF
        elif kind != 0:
            raise PngError(f"unknown PNG filter {kind}")
    return bytes(row)


def st_colour(rgb):
    r, g, b = (c >> 5 for c in rgb)
    return (r << 8) | (g << 4) | b


def to_header(path, name, tile=None):
    width, height, pixels, palette, transparent = read_indexed_png(path)
    used = max(pixels) if pixels else 0
    if used >= MAX_COLOURS:
        raise PngError(f"{path}: a pixel uses palette entry {used}; the ST "
                       f"has {MAX_COLOURS} colours (0..{MAX_COLOURS - 1})")
    if len(transparent) > 1:
        raise PngError(f"{path}: palette entries {transparent} are all "
                       f"transparent; keep one (the key colour)")
    tw, th = tile or (width, height)
    if width % tw or height % th:
        raise PngError(f"{path}: {width}x{height} is not a whole number of "
                       f"{tw}x{th} tiles")
    tiles = []
    for ty in range(0, height, th):
        for tx in range(0, width, tw):
            tiles.append([pixels[(ty + y) * width + tx + x]
                          for y in range(th) for x in range(tw)])
    upper = re.sub(r"\W", "_", name).upper()
    colours = [st_colour(c) for c in palette[:MAX_COLOURS]]
    colours += [0] * (MAX_COLOURS - len(colours))
    out = [f"/* Generated by tools/png_to_bitmap.py from "
           f"{os.path.basename(path)}: do not edit. */",
           f"#ifndef {upper}_BITMAP_H", f"#define {upper}_BITMAP_H", "",
           "#include <stdint.h>", "", '#include "fb_blit.h"', "",
           f"#define {upper}_W {tw}", f"#define {upper}_H {th}"]
    if tile:
        out.append(f"#define {upper}_COUNT {len(tiles)}")
    if transparent:
        out.append(f"#define {upper}_KEY {transparent[0]}")
    out += ["", f"/* The PNG's palette, as ST colour words (palette_set()). */",
            f"static const uint16_t {name}_palette[{MAX_COLOURS}] = {{",
            "    " + ", ".join(f"0x{c:03X}" for c in colours) + ",", "};", "",
            f"static const uint8_t {name}_pixels[{len(tiles) * tw * th}] = {{"]
    for t in tiles:
        for y in range(th):
            out.append("    " + ", ".join(f"{p:2d}" for p in t[y * tw:(y + 1) * tw]) + ",")
    out.append("};")
    out.append("")
    if tile:
        out.append(f"static const struct FB_BITMAP {name}[{upper}_COUNT] = {{")
        for i in range(len(tiles)):
            out.append(f"    {{{tw}, {th}, {name}_pixels + {i * tw * th}}},")
        out.append("};")
    else:
        out.append(f"static const struct FB_BITMAP {name} = {{{tw}, {th}, {name}_pixels}};")
    out += ["", f"#endif /* {upper}_BITMAP_H */", ""]
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("png")
    ap.add_argument("--out", required=True, help="the header to write")
    ap.add_argument("--name", help="C name (default: the PNG's file name)")
    ap.add_argument("--tile", type=int, nargs=2, metavar=("W", "H"),
                    help="cut the image into W x H bitmaps")
    a = ap.parse_args(argv)
    name = a.name or os.path.splitext(os.path.basename(a.png))[0]
    if not re.fullmatch(r"[A-Za-z_]\w*", name):
        ap.error(f"{name!r} is not a C name: pass --name")
    try:
        text = to_header(a.png, name, a.tile)
    except PngError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    with open(a.out, "w") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
