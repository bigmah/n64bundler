#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Register one recompiled N64 game, and optionally bundle it.

Two jobs, and only the first runs by default:

  * always - draw the game's cover art and upsert it into library.json, so
    N64Bundler's list can show and launch it.
  * with --app - additionally build a double-clickable macOS .app.  The bundle
    is a thin launcher holding no game data and no runtime, only absolute paths
    to the ROM, the recompiled module, and the ModernReality build.

The .app is opt-in because a library entry is enough to play, and every bundle
adds a permanent icon to ~/Applications that the user has to clean up by hand.

A GameCube disc carries its own banner, so DolBundler's equivalent of this file
decodes one. An N64 ROM carries nothing of the sort -- there is no artwork
anywhere in the image -- so the cover is drawn here from the one thing the
cartridge does supply: its four-character code, which picks a colour, and its
name, which is written across the label.
"""

import argparse
import datetime
import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ART_W, ART_H = 128, 128
MASTER = 1024
ICONSET_SIZES = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]
BACKDROP = (28, 28, 30)


# --- image helpers -----------------------------------------------------------
# An image is (width, height, bytearray of RGB triples).

def new_image(w, h, colour):
    return (w, h, bytearray(bytes(colour) * (w * h)))


def put(img, x, y, rgb):
    w, h, data = img
    if not (0 <= x < w and 0 <= y < h):
        return
    i = (y * w + x) * 3
    data[i:i + 3] = bytes(int(max(0, min(255, c))) for c in rgb)


def get(img, x, y):
    w, _, data = img
    i = (y * w + x) * 3
    return data[i], data[i + 1], data[i + 2]


def fill_rect(img, x0, y0, x1, y1, rgb):
    for y in range(int(y0), int(y1)):
        for x in range(int(x0), int(x1)):
            put(img, x, y, rgb)


def write_png(img, path):
    w, h, data = img
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += data[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def box_downsample(img, size):
    """Exact integer box filter. MASTER is divisible by every icon size."""
    w, h, _ = img
    assert w == h and w % size == 0
    factor = w // size
    out = new_image(size, size, (0, 0, 0))
    area = factor * factor
    for y in range(size):
        for x in range(size):
            r = g = b = 0
            for dy in range(factor):
                for dx in range(factor):
                    pr, pg, pb = get(img, x * factor + dx, y * factor + dy)
                    r += pr
                    g += pg
                    b += pb
            put(out, x, y, (r // area, g // area, b // area))
    return out


# --- the cover ---------------------------------------------------------------

# Six-by-seven bitmaps, one bit per pixel, MSB first in each row. Enough to
# write a cartridge label: the alphabet, the digits, and the handful of marks
# that turn up in a game's name.
FONT = {
    "A": (0x1E, 0x21, 0x21, 0x3F, 0x21, 0x21, 0x21), "B": (0x3E, 0x21, 0x3E, 0x21, 0x21, 0x21, 0x3E),
    "C": (0x1E, 0x21, 0x20, 0x20, 0x20, 0x21, 0x1E), "D": (0x3C, 0x22, 0x21, 0x21, 0x21, 0x22, 0x3C),
    "E": (0x3F, 0x20, 0x20, 0x3E, 0x20, 0x20, 0x3F), "F": (0x3F, 0x20, 0x20, 0x3E, 0x20, 0x20, 0x20),
    "G": (0x1E, 0x21, 0x20, 0x27, 0x21, 0x21, 0x1F), "H": (0x21, 0x21, 0x21, 0x3F, 0x21, 0x21, 0x21),
    "I": (0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F), "J": (0x07, 0x02, 0x02, 0x02, 0x22, 0x22, 0x1C),
    "K": (0x21, 0x22, 0x24, 0x38, 0x24, 0x22, 0x21), "L": (0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3F),
    "M": (0x21, 0x33, 0x2D, 0x21, 0x21, 0x21, 0x21), "N": (0x21, 0x31, 0x29, 0x25, 0x23, 0x21, 0x21),
    "O": (0x1E, 0x21, 0x21, 0x21, 0x21, 0x21, 0x1E), "P": (0x3E, 0x21, 0x21, 0x3E, 0x20, 0x20, 0x20),
    "Q": (0x1E, 0x21, 0x21, 0x21, 0x25, 0x22, 0x1D), "R": (0x3E, 0x21, 0x21, 0x3E, 0x24, 0x22, 0x21),
    "S": (0x1F, 0x20, 0x20, 0x1E, 0x01, 0x01, 0x3E), "T": (0x3F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    "U": (0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x1E), "V": (0x21, 0x21, 0x21, 0x21, 0x21, 0x12, 0x0C),
    "W": (0x21, 0x21, 0x21, 0x21, 0x2D, 0x33, 0x21), "X": (0x21, 0x12, 0x0C, 0x0C, 0x0C, 0x12, 0x21),
    "Y": (0x21, 0x12, 0x0C, 0x04, 0x04, 0x04, 0x04), "Z": (0x3F, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3F),
    "0": (0x1E, 0x21, 0x23, 0x2D, 0x31, 0x21, 0x1E), "1": (0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F),
    "2": (0x1E, 0x21, 0x01, 0x06, 0x08, 0x10, 0x3F), "3": (0x3E, 0x01, 0x01, 0x1E, 0x01, 0x01, 0x3E),
    "4": (0x02, 0x06, 0x0A, 0x12, 0x3F, 0x02, 0x02), "5": (0x3F, 0x20, 0x3E, 0x01, 0x01, 0x21, 0x1E),
    "6": (0x0E, 0x10, 0x20, 0x3E, 0x21, 0x21, 0x1E), "7": (0x3F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    "8": (0x1E, 0x21, 0x21, 0x1E, 0x21, 0x21, 0x1E), "9": (0x1E, 0x21, 0x21, 0x1F, 0x01, 0x02, 0x1C),
    " ": (0, 0, 0, 0, 0, 0, 0), "-": (0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00),
    ".": (0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C), "'": (0x0C, 0x0C, 0x08, 0x00, 0x00, 0x00, 0x00),
    ":": (0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00), "!": (0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x00, 0x0C),
    "&": (0x18, 0x24, 0x24, 0x18, 0x25, 0x22, 0x1D), "/": (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x20),
}
GLYPH_W, GLYPH_H = 6, 7


def draw_text(img, text, x, y, rgb, scale=1):
    """Left-aligned, one pixel of tracking. Unknown characters are dropped."""
    cursor = x
    for character in text.upper():
        glyph = FONT.get(character)
        if glyph is None:
            continue
        for row in range(GLYPH_H):
            bits = glyph[row]
            for column in range(GLYPH_W):
                if bits & (1 << (GLYPH_W - 1 - column)):
                    fill_rect(img, cursor + column * scale, y + row * scale,
                              cursor + (column + 1) * scale, y + (row + 1) * scale, rgb)
        cursor += (GLYPH_W + 1) * scale
    return cursor


def text_width(text, scale=1):
    return sum((GLYPH_W + 1) * scale for c in text.upper() if c in FONT)


def cartridge_colour(game_id):
    """A stable hue per cartridge.

    Grey is what almost every N64 cartridge actually was, but a library of grey
    rectangles is unreadable at a glance, so the ID picks a colour the way the
    handful of coloured carts did.
    """
    seed = 0
    for character in game_id or "????":
        seed = (seed * 131 + ord(character)) & 0xFFFFFFFF
    hue = (seed % 360) / 360.0
    # HSV to RGB at a fixed, muted saturation and value, so every cover in the
    # list sits at the same weight.
    saturation, value = 0.42, 0.62
    i = int(hue * 6.0)
    f = hue * 6.0 - i
    p = value * (1.0 - saturation)
    q = value * (1.0 - saturation * f)
    t = value * (1.0 - saturation * (1.0 - f))
    r, g, b = [(value, t, p), (q, value, p), (p, value, t),
               (p, q, value), (t, p, value), (value, p, q)][i % 6]
    return (int(r * 255), int(g * 255), int(b * 255))


def wrap(name, columns):
    """Break a title into label lines of at most `columns` characters."""
    lines, current = [], ""
    for word in name.split():
        candidate = f"{current} {word}".strip()
        if len(candidate) <= columns or not current:
            current = candidate
        else:
            lines.append(current)
            current = word
    if current:
        lines.append(current)
    return lines[:4]


def draw_cover(name, game_id):
    """An N64 cartridge, seen face on, with the game's name on the label."""
    body = cartridge_colour(game_id)
    shade = tuple(int(c * 0.72) for c in body)
    highlight = tuple(min(255, int(c * 1.25)) for c in body)
    label = (232, 230, 224)
    ink = (38, 36, 34)

    img = new_image(ART_W, ART_H, BACKDROP)

    # The cartridge outline: a tall body, a narrower shoulder at the top where
    # the cartridge is gripped, and a lip along the bottom edge.
    left, right = 22, ART_W - 22
    top, bottom = 10, ART_H - 8
    shoulder = top + 12

    fill_rect(img, left + 6, top, right - 6, shoulder, shade)
    fill_rect(img, left, shoulder, right, bottom, body)
    # A soft edge down the right-hand side, so the shape reads as an object.
    fill_rect(img, right - 4, shoulder, right, bottom, shade)
    fill_rect(img, left, shoulder, left + 2, bottom, highlight)
    # The ridged grip along the bottom.
    for x in range(left + 4, right - 6, 4):
        fill_rect(img, x, bottom - 10, x + 2, bottom - 3, shade)

    # The label.
    label_left, label_right = left + 7, right - 9
    label_top, label_bottom = shoulder + 6, bottom - 16
    fill_rect(img, label_left, label_top, label_right, label_bottom, label)

    columns = max(4, (label_right - label_left - 4) // (GLYPH_W + 1))
    lines = wrap(name, columns)
    line_height = GLYPH_H + 3
    block = len(lines) * line_height
    y = label_top + max(2, ((label_bottom - label_top) - block) // 2)
    for line in lines:
        width = text_width(line)
        draw_text(img, line, label_left + ((label_right - label_left) - width) // 2, y, ink)
        y += line_height

    # The cartridge code, small, along the bottom of the label.
    code = (game_id or "")[:4]
    if code:
        width = text_width(code)
        draw_text(img, code, label_left + ((label_right - label_left) - width) // 2,
                  label_bottom - GLYPH_H - 1, (120, 118, 112))
    return img


def build_master(art):
    """Upscale the cover with nearest-neighbour and centre it on the canvas."""
    scale = MASTER // ART_W
    size = ART_W * scale
    offset = (MASTER - size) // 2
    canvas = new_image(MASTER, MASTER, BACKDROP)
    for y in range(size):
        for x in range(size):
            put(canvas, offset + x, offset + y, get(art, x // scale, y // scale))
    return canvas


def build_icns(art, destination):
    master = build_master(art)
    iconset = destination.parent / "icon.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    cache = {}
    for name, size in ICONSET_SIZES:
        if size not in cache:
            cache[size] = master if size == MASTER else box_downsample(master, size)
        write_png(cache[size], iconset / name)
    ok = subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(destination)],
        capture_output=True,
    ).returncode == 0
    shutil.rmtree(iconset, ignore_errors=True)
    return ok


def write_cover(art, path, scale=3):
    w, h, _ = art
    out = new_image(w * scale, h * scale, BACKDROP)
    for y in range(h * scale):
        for x in range(w * scale):
            put(out, x, y, get(art, x // scale, y // scale))
    write_png(out, path)


# --- the library -------------------------------------------------------------

def existing_app(path, game_id):
    """The .app path a previous run recorded for this cartridge, or ""."""
    try:
        library = json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return ""
    for game in library.get("games", []):
        if isinstance(game, dict) and game.get("game_id") == game_id:
            app = game.get("app", "")
            return app if app and Path(app).is_dir() else ""
    return ""


def upsert_library(path, entry):
    """Replace any entry with the same game ID, newest first."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        library = json.loads(path.read_text())
        games = [g for g in library.get("games", []) if isinstance(g, dict)]
    except (OSError, ValueError):
        games = []
    games = [g for g in games if g.get("game_id") != entry["game_id"]]
    games.insert(0, entry)
    path.write_text(json.dumps({"games": games}, indent=2) + "\n")


# --- the bundle --------------------------------------------------------------

LAUNCHER = """#!/bin/bash
# Generated by N64Bundler. Edit ../Resources/game.conf to change how this starts.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../Resources/game.conf"

fail() {{
  /usr/bin/osascript -e "display alert \\"$GAME_TITLE\\" message \\"$1\\" as critical" \\
    >/dev/null 2>&1
  exit 1
}}

[ -x "$RUNTIME_DIR/n64b-run" ] || \\
  fail "The ModernReality runtime is missing from $RUNTIME_DIR. Rebuild it, then drop the ROM on N64Bundler again."
[ -f "$ROM_PATH" ] || \\
  fail "The ROM is missing from $ROM_PATH. Put it back, or drop it on N64Bundler again."
[ -f "$MODULE_PATH" ] || \\
  fail "The recompiled module is missing from $MODULE_PATH. Drop the ROM on N64Bundler again to rebuild it."

mkdir -p "$LOG_DIR"
exec "$RUNTIME_DIR/n64b-run" \\
  --module "$MODULE_PATH" \\
  --rom "$ROM_PATH" \\
  >>"$LOG_DIR/{game_id}.log" 2>&1
"""


def shell_quote(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def build_bundle(args, art):
    """Write <out-dir>/<name>.app and return its path."""
    bundle = Path(args.out_dir) / f"{args.name}.app"
    shutil.rmtree(bundle, ignore_errors=True)
    macos = bundle / "Contents" / "MacOS"
    resources = bundle / "Contents" / "Resources"
    macos.mkdir(parents=True)
    resources.mkdir(parents=True)

    info = {
        "CFBundleName": args.name,
        "CFBundleDisplayName": args.name,
        "CFBundleExecutable": "run",
        "CFBundleIdentifier": f"n64.n64bundler.game.{args.game_id}",
        "CFBundleIconFile": "icon",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": False,
    }
    (bundle / "Contents" / "Info.plist").write_bytes(plistlib.dumps(info))

    log_dir = Path.home() / "Library" / "Application Support" / "N64Bundler" / "logs"
    conf = "\n".join(
        f"{key}={shell_quote(value)}"
        for key, value in [
            ("GAME_TITLE", args.name),
            ("GAME_ID", args.game_id),
            ("ROM_PATH", args.rom),
            ("MODULE_PATH", args.module),
            ("RUNTIME_DIR", args.runtime),
            ("LOG_DIR", log_dir),
        ]
    )
    (resources / "game.conf").write_text(conf + "\n")

    launcher = macos / "run"
    launcher.write_text(LAUNCHER.format(game_id=args.game_id))
    launcher.chmod(0o755)

    if not build_icns(art, resources / "icon.icns"):
        print("warning: could not build an icon; the app will use the generic one",
              file=sys.stderr)

    # Finder caches bundle metadata aggressively; touching the bundle and
    # re-registering it makes a rebuilt app pick up its new name and icon.
    os.utime(bundle)
    subprocess.run(
        ["/System/Library/Frameworks/CoreServices.framework/Frameworks/"
         "LaunchServices.framework/Support/lsregister", "-f", str(bundle)],
        capture_output=True,
    )
    return bundle


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--game-id", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--app", action="store_true",
                        help="also build a .app bundle in --out-dir")
    parser.add_argument("--out-dir", help="where --app writes the bundle")
    parser.add_argument("--library", help="library.json to upsert this game into")
    parser.add_argument("--covers", help="directory to write the cover art into")
    args = parser.parse_args()

    if args.app and not args.out_dir:
        parser.error("--app needs --out-dir")

    art = draw_cover(args.name, args.game_id)
    bundle = build_bundle(args, art) if args.app else None

    cover = ""
    if args.covers:
        cover = str(Path(args.covers) / f"{args.game_id}.png")
        write_cover(art, Path(cover))

    if args.library:
        # Preserve a bundle built by an earlier run: re-registering a game
        # without --app must not orphan an .app the user already asked for.
        previous = existing_app(args.library, args.game_id)
        upsert_library(args.library, {
            "game_id": args.game_id,
            "name": args.name,
            "rom": args.rom,
            "module": args.module,
            "app": str(bundle) if bundle else previous,
            "cover": cover,
            "added": datetime.datetime.now(datetime.timezone.utc)
                     .replace(microsecond=0).isoformat(),
        })

    if bundle:
        print(bundle)


if __name__ == "__main__":
    main()
