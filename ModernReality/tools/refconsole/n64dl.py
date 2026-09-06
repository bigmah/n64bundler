#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read a display list out of a console's memory.

`refshot` and `n64b-run` both write the console's eight megabytes at a named
frame. This turns one of those images back into the thing the game actually
asked the graphics processor for -- and two of them, one from each console,
turn "the picture is wrong" into "the game and the console agree for five
hundred and seven commands and then do not", which is a question with an
answer in it.

    n64dl.py <image.bin> <address> [--trace] [--matrix <address>]

`--trace` prints every command in order, following `G_DL` branches, which is
what a diff wants. Without it the commands are counted by kind, which is what
"are we drawing a third of the geometry" wants. `--matrix` decodes the fixed
point matrix at an address instead, because a projection that is wrong by a
factor is invisible in the command stream and obvious in the numbers.

A word is a word the host can load, which is how both consoles hold RDRAM.
"""

import argparse
import struct
import sys

# The F3D family's command names, by the byte a command begins with. The
# opcodes below 0x10 are the ones F3DEX2 moved down there; everything from 0xD7
# up is shared with every other version of the interface.
NAMES = {
    0x00: "NOOP", 0x01: "VTX", 0x02: "MODIFYVTX", 0x03: "CULLDL", 0x04: "BRANCH_Z",
    0x05: "TRI1", 0x06: "TRI2", 0x07: "QUAD",
    0xD7: "TEXTURE", 0xD8: "POPMTX", 0xD9: "GEOMETRYMODE", 0xDA: "MTX",
    0xDB: "MOVEWORD", 0xDC: "MOVEMEM", 0xDD: "LOAD_UCODE", 0xDE: "DL", 0xDF: "ENDDL",
    0xE0: "SPNOOP", 0xE1: "RDPHALF_1", 0xE2: "SETOTHERMODE_L", 0xE3: "SETOTHERMODE_H",
    0xE4: "TEXRECT", 0xE5: "TEXRECTFLIP", 0xE6: "RDPLOADSYNC", 0xE7: "RDPPIPESYNC",
    0xE8: "RDPTILESYNC", 0xE9: "RDPFULLSYNC", 0xEA: "SETKEYGB", 0xEB: "SETKEYR",
    0xEC: "SETCONVERT", 0xED: "SETSCISSOR", 0xEE: "SETPRIMDEPTH", 0xEF: "RDPSETOTHERMODE",
    0xF0: "LOADTLUT", 0xF2: "SETTILESIZE", 0xF3: "LOADBLOCK", 0xF4: "LOADTILE",
    0xF5: "SETTILE", 0xF6: "FILLRECT", 0xF7: "SETFILLCOLOR", 0xF8: "SETFOGCOLOR",
    0xF9: "SETBLENDCOLOR", 0xFA: "SETPRIMCOLOR", 0xFB: "SETENVCOLOR", 0xFC: "SETCOMBINE",
    0xFD: "SETTIMG", 0xFE: "SETZIMG", 0xFF: "SETCIMG",
}

RDRAM_BASE = 0x80000000
RDRAM_SIZE = 0x800000


def load(path):
    with open(path, "rb") as image:
        data = image.read()
    if len(data) < RDRAM_SIZE:
        sys.exit(f"{path} is {len(data)} bytes; a console's memory is {RDRAM_SIZE}")
    return data


def word(image, address):
    offset = (address - RDRAM_BASE) & ~3
    if not 0 <= offset < RDRAM_SIZE - 3:
        return None
    return struct.unpack_from("<I", image, offset)[0]


def walk(image, address, out, depth=0, budget=None):
    """Every command from `address` on, following the branches it takes.

    A `G_DL` with its push byte set is a call and comes back; without it the
    branch is the end of this list. Segmented addresses other than segment
    zero are left alone: the segment table lives in the graphics processor's
    own state rather than in memory, so following one would be a guess.
    """
    if budget is None:
        budget = [40000]
    if depth > 16:
        return
    while budget[0] > 0:
        budget[0] -= 1
        w0, w1 = word(image, address), word(image, address + 4)
        if w0 is None or w1 is None:
            return
        address += 8
        op = w0 >> 24
        out.append((depth, NAMES.get(op, f"?{op:02X}"), w0, w1))
        if op == 0xDF:  # G_ENDDL
            return
        if op == 0xDE:  # G_DL
            segment, offset = w1 >> 24, w1 & 0xFFFFFF
            if segment == 0 and 0 < offset < RDRAM_SIZE:
                walk(image, RDRAM_BASE + offset, out, depth + 1, budget)
            if (w0 >> 16) & 0xFF:  # no push: this branch does not return
                return


def matrix(image, address):
    """The fixed point matrix at an address, as the numbers it stands for.

    A matrix on this console is sixteen integer halves followed by sixteen
    fractional ones, which is a layout nothing else uses and no hex dump
    reads.
    """
    words = [word(image, address + 4 * i) for i in range(16)]
    if any(w is None for w in words):
        sys.exit(f"0x{address:08X} is not in the console's memory")
    rows = []
    for row in range(4):
        values = []
        for column in range(4):
            index = row * 4 + column
            shift = 16 if index % 2 == 0 else 0
            whole = (words[index // 2] >> shift) & 0xFFFF
            fraction = (words[8 + index // 2] >> shift) & 0xFFFF
            value = (whole << 16) | fraction
            if value & 0x80000000:
                value -= 1 << 32
            values.append(value / 65536.0)
        rows.append(values)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image")
    parser.add_argument("address", help="where the display list starts, or the matrix with --matrix")
    parser.add_argument("--trace", action="store_true", help="every command in order")
    parser.add_argument("--matrix", action="store_true", help="decode a matrix instead")
    options = parser.parse_args()

    image = load(options.image)
    address = int(options.address, 0)

    if options.matrix:
        for row in matrix(image, address):
            print("   " + " ".join(f"{value:12.5f}" for value in row))
        return

    out = []
    walk(image, address, out)
    if options.trace:
        for depth, name, w0, w1 in out:
            print(f"{'  ' * min(depth, 8)}{name} {w0:08X} {w1:08X}")
        return

    counts = {}
    for _, name, _, _ in out:
        counts[name] = counts.get(name, 0) + 1
    print(f"{options.image} @ 0x{address:08X}: {len(out)} commands")
    for name in sorted(counts, key=lambda k: -counts[k]):
        print(f"   {name:18s} {counts[name]}")


if __name__ == "__main__":
    main()
