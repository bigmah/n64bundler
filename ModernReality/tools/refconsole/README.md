<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# refconsole — a second console to disagree with

A static recompilation can be instrumented anywhere, which is a wonderful
thing right up until the question stops being "what does my run do" and
becomes "what does a console do instead". Then there is nothing to compare
against, and the answer has to be reasoned out of the game's own code one
instruction at a time.

These two programs are the comparison. They drive `mupen64plus`'s core with
no video, no audio and no input — the core runs the cartridge with its own
stub plugins — and give a script the things a debugger gives a person:

- `refrun <rom> <out-prefix> <seconds>...` writes the console's eight
  megabytes of memory at each of those times. Two of those and two of this
  runtime's `N64B_RAMDUMP` images, and a diff says which of the game's own
  state the two runs disagree about.
- `refdbg <rom> [options]` breaks on addresses:
  - `-e <addr>` stop when the game executes it, and print `$ra`, the argument
    registers and `$v0`;
  - `-w <addr>[:<len>]` stop when the game writes it — a *physical* address,
    so 0x00126800 for 0x80126800;
  - `-g` every register, `-m <addr> <words>` memory at each stop,
    `-s <words>` the return addresses left on the stack, which is the only
    call chain a breakpoint can recover from MIPS;
  - `-r <lo> <hi>` record every execution in a range into a ring without
    stopping, and `-R <n>` print the last `n` of them when a breakpoint hits.
    A game that reaches between its overlays through a table of stubs has its
    whole linkage in one address range, so this is a call trace;
  - `-k <n>` skip the first `n` hits, `-n <n>` stop reporting after `n`,
    `-D <path>` write memory out at the hit, `-t <secs>` how long to run.

Both of them found the thing they were built for: Banjo-Tooie and this
runtime load the same eighty-one overlays in the same order, and then the
console loads twelve more. Walking back from that reaches eight instructions
that read two words the boot ROM left in memory. PLAN.md has the rest.

## Building

The core has to be one built with its debugger, which no package ships:

    git clone --depth 1 https://github.com/mupen64plus/mupen64plus-core
    brew install mupen64plus binutils          # the RSP plugin, and libopcodes
    cd mupen64plus-core/projects/unix
    LIBRARY_PATH=/opt/homebrew/opt/binutils/lib \
    CPATH=/opt/homebrew/opt/binutils/include \
      make all DEBUGGER=1 OSD=0 NEW_DYNAREC=0 -j 10 \
        STRINGS=/opt/homebrew/opt/binutils/bin/strings

Then either program, against that core:

    clang -O2 -o refdbg refdbg.c -I/opt/homebrew/include \
      -L<core-dir> -lmupen64plus -Wl,-rpath,<core-dir>
    M64P_CORE=<core-dir>/libmupen64plus.dylib ./refdbg game.z64 -e 0x80081798

`M64P_CORE` must name the same file the program is linked against, or the
plugin talks to a second copy of the core that nobody started. `M64P_RSP` and
`M64P_DATA` default to Homebrew's.

## What it is not

It is not an emulator this project uses to play anything, and it is not a
dependency of the build. Nothing here is compiled by `build.sh`. It exists so
that the next game's version of "it runs and does nothing" has something to
be measured against.

With the stub video plugin the core stops a frame or two into a game that
waits for the display processor, which is fine for everything above -- the
boot, the loading, the first seconds -- and not fine for watching a game play.
Attaching a real video plugin needs a screen.
