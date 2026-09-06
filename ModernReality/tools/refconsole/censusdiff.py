#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which functions the reference console ran and this runtime did not.

`refdbg -c <lo> <hi> <file>` writes every address the console executed in a
window. `n64b-run` with a `--trace` module and `N64B_TRACE_ALL` prints every
function this runtime entered and how often. This turns the two into the
question worth asking:

    censusdiff.py <census.txt> <trace-all.log> <symbols.toml> [--window lo:hi]

A function the console entered and this runtime never did is a branch that
went the other way, and the highest one in the call graph is the one to look
at -- everything under it follows. The reverse list matters too, and is
usually empty: a runtime that runs code the console does not is a runtime
that has taken a path of its own.

Running one breakpoint at a time answers this for one function per run of the
game. This answers it for a whole subsystem in one.
"""

import argparse
import re
import sys


def read_symbols(path):
    """Every recovered function as (address, size, name), sorted."""
    functions = []
    for block in open(path).read().split("[[section.functions]]"):
        vram = re.search(r"vram\s*=\s*(0x[0-9A-Fa-f]+)", block)
        if vram is None:
            continue
        name = re.search(r'name\s*=\s*"([^"]+)"', block)
        size = re.search(r"size\s*=\s*(0x[0-9A-Fa-f]+)", block)
        functions.append((int(vram.group(1), 16),
                          int(size.group(1), 16) if size else 0,
                          name.group(1) if name else None))
    functions.sort()
    return functions


def read_census(path):
    return {int(line.strip(), 16) for line in open(path) if line.strip()}


def read_counts(path):
    """`N64B_TRACE_ALL`'s tally: how often each function was entered."""
    counts = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            counts[parts[1]] = int(parts[0])
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("census")
    parser.add_argument("trace")
    parser.add_argument("symbols")
    parser.add_argument("--window", help="lo:hi, to narrow the comparison")
    options = parser.parse_args()

    census = read_census(options.census)
    counts = read_counts(options.trace)
    functions = read_symbols(options.symbols)

    lo, hi = min(census, default=0), max(census, default=0) + 4
    if options.window:
        low, high = options.window.split(":")
        lo, hi = int(low, 0), int(high, 0)

    only_console, only_here, both = [], [], 0
    for address, _size, name in functions:
        if not lo <= address < hi:
            continue
        key = name if name and f"func_{address:08X}" not in counts else f"func_{address:08X}"
        ran_here = counts.get(f"func_{address:08X}", counts.get(key, 0)) > 0
        ran_there = address in census
        if ran_there and not ran_here:
            only_console.append((address, name))
        elif ran_here and not ran_there:
            only_here.append((address, name))
        elif ran_there:
            both += 1

    print(f"over 0x{lo:08X}-0x{hi:08X}: {both} functions both consoles ran")
    print(f"{len(only_console)} the console ran and this runtime did not:")
    for address, name in only_console:
        print(f"   0x{address:08X}  {name or ''}")
    print(f"{len(only_here)} this runtime ran and the console did not:")
    for address, name in only_here:
        print(f"   0x{address:08X}  {name or ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
