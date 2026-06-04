#!/usr/bin/env python3
"""Convert a PNG logo into a power-of-two, byte-per-pixel texture header
for the framebuffer rotozoom (Epic 6 cool menu).

The dark silhouette of the logo maps to a small luminance ramp
[--lo, --lo+--shades), with the darkest (core) pixels at the brightest
ramp index and the anti-aliased edges at lower indices; the background
(transparent OR near-white) maps to the --transparent index. The C side
owns the actual palette colours (so they can be colour-cycled / "beat"),
so this header is just dimensions + the index array.

Needs Pillow:  python3 -m venv venv && venv/bin/pip install Pillow
Usage:
  png_to_texture.py LOGO.png --out rp/src/include/foo.h --name foo \
      --w 128 --h 128 [--shades 4 --lo 1 --transparent 15 --crop]
"""
import argparse
from PIL import Image


def luminance(r, g, b):
    return (r * 299 + g * 587 + b * 114) // 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", required=True, help="C symbol prefix")
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--shades", type=int, default=4)
    ap.add_argument("--lo", type=int, default=1)
    ap.add_argument("--transparent", type=int, default=15)
    ap.add_argument("--crop", action="store_true",
                    help="auto-crop to the silhouette bounding box first")
    a = ap.parse_args()

    img = Image.open(a.png).convert("RGBA")

    def is_fg(p):
        r, g, b, al = p
        return al >= 128 and luminance(r, g, b) < 200

    if a.crop:
        px = img.load()
        W0, H0 = img.size
        xs = [x for y in range(H0) for x in range(W0) if is_fg(px[x, y])]
        ys = [y for y in range(H0) for x in range(W0) if is_fg(px[x, y])]
        if xs and ys:
            pad = 2
            box = (max(0, min(xs) - pad), max(0, min(ys) - pad),
                   min(W0, max(xs) + 1 + pad), min(H0, max(ys) + 1 + pad))
            img = img.crop(box)

    img = img.resize((a.w, a.h), Image.LANCZOS)
    px = img.load()

    data = []
    fg = 0
    for y in range(a.h):
        for x in range(a.w):
            r, g, b, al = px[x, y]
            lum = luminance(r, g, b)
            if al < 96 or lum >= 200:
                data.append(a.transparent)
            else:
                # dark core -> brightest shade, light edges -> dimmest
                t = (200 - lum) * a.shades // 200
                t = max(1, min(a.shades, t))
                data.append(a.lo + (t - 1))
                fg += 1

    NAME = a.name
    UP = NAME.upper()
    lines = []
    lines.append("/**")
    lines.append(f" * File: {a.out.split('/')[-1]}")
    lines.append(f" * Description: {a.w}x{a.h} byte-per-pixel rotozoom texture")
    lines.append(f" *   auto-generated from {a.png.split('/')[-1]} by")
    lines.append(f" *   tools/png_to_texture.py. Silhouette -> idx "
                 f"{a.lo}..{a.lo + a.shades - 1}, background -> idx "
                 f"{a.transparent} (transparent). Palette lives C-side.")
    lines.append(" */")
    lines.append(f"#ifndef {UP}_H")
    lines.append(f"#define {UP}_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define {UP}_W {a.w}")
    lines.append(f"#define {UP}_H {a.h}")
    lines.append(f"#define {UP}_WMASK {a.w - 1}")
    lines.append(f"#define {UP}_HMASK {a.h - 1}")
    lines.append(f"#define {UP}_TRANSPARENT {a.transparent}")
    lines.append("")
    lines.append(f"static const uint8_t {NAME}_data[{a.w} * {a.h}] = {{")
    row = "    "
    for i, v in enumerate(data):
        row += f"{v:2d}, "
        if (i + 1) % 24 == 0:
            lines.append(row.rstrip())
            row = "    "
    if row.strip():
        lines.append(row.rstrip())
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {UP}_H */")
    open(a.out, "w").write("\n".join(lines) + "\n")
    print(f"wrote {a.out}: {a.w}x{a.h}, foreground {fg} px "
          f"({100 * fg // (a.w * a.h)}%), {a.shades} shades from idx {a.lo}")


if __name__ == "__main__":
    main()
