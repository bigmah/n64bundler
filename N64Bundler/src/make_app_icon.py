#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw N64Bundler's own app icon: a cartridge with an arrow dropping into it.

The counterpart to DolBundler's disc, and the same picture of what the app
does: you give it a cartridge and it gives you a game.

Everything is rendered at 1024 and box-downsampled to each icon size, so the
downsample doubles as the antialiasing pass.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_game_app import ICONSET_SIZES, MASTER, box_downsample, new_image, put, write_png

TOP = (52, 62, 96)
BOTTOM = (20, 22, 34)
CART = (150, 156, 168)
CART_DARK = (96, 102, 116)
LABEL = (228, 232, 240)
ARROW = (236, 243, 255)
RADIUS = 0.22  # corner radius as a fraction of the canvas


def rounded_alpha(x, y, size, radius):
    """Coverage of a rounded square at a point, 0.0 to 1.0."""
    dx = max(radius - x, x - (size - radius), 0.0)
    dy = max(radius - y, y - (size - radius), 0.0)
    if dx == 0.0 or dy == 0.0:
        inside = 0 <= x <= size and 0 <= y <= size
        return 1.0 if inside else 0.0
    distance = (dx * dx + dy * dy) ** 0.5
    return max(0.0, min(1.0, radius - distance + 0.5))


def blend(base, colour, alpha):
    return tuple(int(base[i] * (1 - alpha) + colour[i] * alpha) for i in range(3))


def draw(rounded=True):
    size = MASTER
    # iOS masks its own corners and rejects an icon with any transparency, so
    # the phone's copy would be the same picture on a square plate.
    radius = size * RADIUS if rounded else 0.0
    canvas = new_image(size, size, (0, 0, 0))

    cx = size / 2

    # The cartridge, sitting in the lower two thirds: a body, a narrower
    # shoulder above it, a label inset into the face, and the ridged connector
    # along the bottom.
    body_left, body_right = size * 0.245, size * 0.755
    body_top, body_bottom = size * 0.455, size * 0.875
    shoulder_left, shoulder_right = size * 0.315, size * 0.685
    shoulder_top = size * 0.395
    label_left, label_right = size * 0.315, size * 0.685
    label_top, label_bottom = size * 0.505, size * 0.755
    grip_top, grip_bottom = size * 0.795, size * 0.850

    # The arrow, dropping in from above.
    shaft_half = size * 0.045
    shaft_top = size * 0.100
    shaft_bottom = size * 0.230
    head_tip = size * 0.350
    head_half = size * 0.125

    for py in range(size):
        y = py + 0.5
        t = y / size
        plate = tuple(int(TOP[i] * (1 - t) + BOTTOM[i] * t) for i in range(3))
        for px in range(size):
            x = px + 0.5
            coverage = rounded_alpha(x, y, size, radius)
            if coverage <= 0.0:
                put(canvas, px, py, (0, 0, 0))
                continue
            colour = plate

            if shoulder_top <= y < body_top and shoulder_left <= x <= shoulder_right:
                colour = CART_DARK
            elif body_top <= y <= body_bottom and body_left <= x <= body_right:
                # A lit left edge and a shadowed right one, so the face reads
                # as an object rather than a rectangle.
                if x < body_left + size * 0.02:
                    colour = CART
                elif x > body_right - size * 0.035:
                    colour = CART_DARK
                else:
                    colour = CART
                if grip_top <= y <= grip_bottom and body_left + size * 0.03 <= x <= body_right - size * 0.03:
                    # Contacts: on for two units, off for two.
                    colour = CART_DARK if int((x - body_left) / (size * 0.022)) % 2 == 0 else CART
                elif label_top <= y <= label_bottom and label_left <= x <= label_right:
                    colour = LABEL

            in_shaft = shaft_top <= y <= shaft_bottom and abs(x - cx) <= shaft_half
            in_head = False
            if shaft_bottom <= y <= head_tip:
                span = head_half * (1 - (y - shaft_bottom) / (head_tip - shaft_bottom))
                in_head = abs(x - cx) <= span
            if in_shaft or in_head:
                colour = ARROW

            put(canvas, px, py, blend((0, 0, 0), colour, coverage))
    return canvas


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", help="write a macOS .icns here")
    parser.add_argument("--png", help="write the square 1024x1024 icon here")
    args = parser.parse_args()
    if not args.out and not args.png:
        parser.error("one of --out or --png is required")

    if args.png:
        square = Path(args.png)
        square.parent.mkdir(parents=True, exist_ok=True)
        write_png(draw(rounded=False), square)
        print(square)
    if not args.out:
        return

    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    master = draw()

    iconset = destination.parent / "n64bundler.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    cache = {}
    for name, size in ICONSET_SIZES:
        if size not in cache:
            cache[size] = master if size == MASTER else box_downsample(master, size)
        write_png(cache[size], iconset / name)
    result = subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(destination)],
        capture_output=True,
    )
    shutil.rmtree(iconset, ignore_errors=True)
    if result.returncode != 0:
        print(result.stderr.decode(errors="replace"), file=sys.stderr)
        raise SystemExit("iconutil could not build the icon")
    print(destination)


if __name__ == "__main__":
    main()
