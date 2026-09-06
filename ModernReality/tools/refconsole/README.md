<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# refconsole — a second console to disagree with

A static recompilation can be instrumented anywhere, which is a wonderful
thing right up until the question stops being "what does my run do" and
becomes "what does a console do instead". Then there is nothing to compare
against, and the answer has to be reasoned out of the game's own code one
instruction at a time.

These programs are the comparison. They drive `mupen64plus`'s core and give a
script the things a debugger gives a person:

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

  - `-c <lo> <hi> <file>` takes a census: every address in the window that the
    game executed, written out at the end. A breakpoint answers "did it get
    here" for one address per run of the game; this answers it for a whole
    subsystem at once, and `censusdiff.py` turns it into the list that matters.

  `M64P_GFX` names a real video plugin, and with one attached refdbg follows a
  game as long as you like rather than stopping a frame or two in. Without it
  the core's stub video never finishes a display list, which is fine for a
  question about boot and useless for a question about play.
- `refshot <rom> <shots-dir> <frame>...` takes a screenshot at each of those
  *frames*, with a real video plugin, by pausing the core and advancing it a
  frame at a time. Frames rather than seconds is the whole point: a cached
  interpreter and a pile of compiled code do not reach the same moment at the
  same time. `REFSHOT_RAM=<prefix>` writes the console's memory at each of them
  too, so a picture and the state behind it come out together, and `N64B_INPUT`
  drives the pad in exactly the spelling `n64b-run` uses.

  **A frame is one the game drew, not one the video interface scanned out.**
  `M64CMD_ADVANCE_FRAME` stops at mupen64plus's `new_frame()`, which its RSP
  calls once per graphics task -- so a frame here is one display list, and
  `n64b-run` counts the same thing for `N64B_SCREENSHOT_AFTER` and `N64B_INPUT`.
  The distinction is not pedantry: Banjo-Tooie draws twenty frames a second
  against sixty vertical interrupts, so a picture numbered in interrupts and a
  picture numbered in frames are three times apart and get further apart the
  longer the game runs. Two consoles compared on the wrong one of these look
  like a runtime running at a third of the speed, which is what they looked
  like here for most of a day. Reads of the controller are a third clock again
  -- three per frame in this game, one per interrupt -- and no script should be
  counted in them.
  Each shot line also reports the video interface's own registers, which are
  directly comparable against `n64b-run`'s: the same VI_STATUS on both consoles
  means the runtime models the pixel format, the anti-aliasing and the gamma
  the way the game asked for. VI_ORIGIN is reported and should not be counted:
  it alternates between the game's two framebuffers on every retrace whatever
  the frame rate is, because libultra keeps two `OSViContext`s and swaps them
  each time.

- `censusdiff.py <census> <trace-all.log> <symbols.toml>` says which functions
  the console entered and this runtime never did, and the reverse. The second
  file is what `n64b-run` prints with a `--trace` module and `N64B_TRACE_ALL`.
  The highest function in the list is the branch that went the other way;
  everything under it follows from that one.
- `n64dl.py <image.bin> <address>` reads a display list back out of one of
  those memory images — counted by kind, or `--trace` for every command in
  order, or `--matrix` for the fixed point matrix at an address. Two of those,
  one from each console, is how "the picture is wrong" becomes "they agree for
  five hundred and seven commands and then do not".

Each of them found the thing it was built for. `refrun` and `refdbg`: Banjo-Tooie
and this runtime load the same eighty-one overlays in the same order, and then
the console loads twelve more, which walks back to eight instructions reading
two words the boot ROM left in memory. `refshot` and `n64dl.py`: Banjo-Tooie's
world is drawn through a projection ten and a half times too wide here, and
the two consoles' display lists are identical for five hundred and seven
commands before they part. PLAN.md has the rest.

## Building

The core has to be one built with its debugger, which no package ships:

    git clone --depth 1 https://github.com/mupen64plus/mupen64plus-core
    brew install mupen64plus binutils          # the RSP plugin, and libopcodes
    cd mupen64plus-core/projects/unix
    LIBRARY_PATH=/opt/homebrew/opt/binutils/lib \
    CPATH=/opt/homebrew/opt/binutils/include \
      make all DEBUGGER=1 OSD=0 NEW_DYNAREC=0 -j 10 \
        STRINGS=/opt/homebrew/opt/binutils/bin/strings

Then any of them, against that core:

    clang -O2 -o refdbg refdbg.c -I/opt/homebrew/include \
      -L<core-dir> -lmupen64plus -Wl,-rpath,<core-dir>
    M64P_CORE=<core-dir>/libmupen64plus.dylib ./refdbg game.z64 -e 0x80081798

The core's own install name has no directory in it, so a program linked
against it finds it beside itself or through `DYLD_LIBRARY_PATH`; copying the
dylib next to the binary is the shortest way.

`M64P_CORE` must name the same file the program is linked against, or the
plugin talks to a second copy of the core that nobody started. `M64P_RSP` and
`M64P_DATA` default to Homebrew's.

## What it is not

It is not an emulator this project uses to play anything, and it is not a
dependency of the build. Nothing here is compiled by `build.sh`. It exists so
that the next game's version of "it runs and does nothing" has something to
be measured against.

`refrun` still runs on the core's stub video, which stops a frame or two into
a game that waits for the display processor: fine for boot and loading, no
good for watching a game play. `refdbg` with `M64P_GFX` and `refshot` attach a
real one, which needs a screen and opens a window.
