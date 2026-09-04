# N64Bundler

Drop an N64 ROM in, watch it recompile, and play it from the library. The
ROM's MIPS code is statically recompiled to native machine code; the game runs
as compiled code, not interpreted.

```
mario64.z64  ──▶  N64Bundler  ──▶  ▶ Super Mario 64   in the library
                             └──▶  ~/Applications/Super Mario 64.app   (opt in)
```

This is the Nintendo 64 counterpart to [DolBundler](https://github.com/bigmah/dolbundler),
and deliberately the same shape: a window over a headless pipeline, a library
of games, per-game `.app` bundles that hold no game data, and a runtime tree
that carries the forks.

**One game plays.** Super Mario 64 recompiles from a bare cartridge dump and
runs: its title screen, its file select, Peach's letter, the castle grounds,
Mario under the control of a pad, its music and its sound effects, and a save
file that survives quitting. Mario Builder 64, the second cartridge, recompiles
at the same coverage and reaches its startup screen. Everything around both
works: a ROM is analysed, recompiled, compiled, added to the library and
launched into a window with the renderer up. [PLAN.md](PLAN.md) has the design,
the measured numbers, and what is still missing.

## Quick start

```sh
git clone --recursive https://github.com/bigmah/n64bundler.git
cd n64bundler
./N64Bundler/build.sh
```

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`,
and SDL2 (`brew install sdl2`). The first build compiles RT64, which takes a
few minutes; everything after that is seconds.

`./N64Bundler/build.sh --tools-only` skips RT64 and the window, and is what to
use if all you want is the analyser.

## What is in here

N64Bundler is the glue. The heavy lifting is upstream, pinned as submodules:

| Path | What | Upstream |
|---|---|---|
| `N64Bundler/` | the window, the pipeline, the app packaging | this repo |
| `ModernReality/` | the analyser, the host, the tools around them | this repo |
| `ModernReality/vendor/N64ModernRuntime/` | `ultramodern` and `librecomp`: libultra, overlays, saves | [N64Recomp/N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) |
| `ModernReality/vendor/N64ModernRuntime/N64Recomp/` | the recompiler: MIPS to C | [N64Recomp/N64Recomp](https://github.com/N64Recomp/N64Recomp) |
| `ModernReality/vendor/rt64/` | the renderer, on Metal | [rt64/rt64](https://github.com/rt64/rt64) |

"Project Reality" was the N64's development codename and the RCP is its
Reality Coprocessor, which is where `ModernReality` gets its name — the
counterpart to DolBundler's ModernGekko.

[`THIRD_PARTY.md`](THIRD_PARTY.md) lists every piece with its license.

## The part that is different from DolBundler

A GameCube disc hands DolRecomp a `main.dol`, and a DOL carries a section
table: where the code is, and what address it runs at. **An N64 ROM carries
neither.** It is a flat image with a 64-byte header, 4KB of boot code, and then
whatever the game's linker script decided.

That is why every other N64Recomp project starts from a decompilation — the
elf is where the metadata comes from. N64Bundler cannot, because the point is
to accept a ROM nobody has decompiled. So it recovers the metadata itself:

- **`n64rip`** reads the image, identifies the boot chip, works out where the
  boot segment really lands, and recovers function boundaries by following
  calls out from the entry point and then sweeping the code that proves is
  there. On Super Mario 64 (USA) it recovers 5,034 functions, and 100% of the
  ROM's internal calls land exactly on one of them — which is the check,
  since a `jal` always names the first instruction of a function.

- **`n64sig`** puts names back on the libultra functions inside the ROM. That
  matters more than it sounds like it should: a game calls `osCreateThread` and
  two hundred others, none of which can run as recompiled MIPS because they
  drive hardware that is not there. The runtime reimplements all of it and
  N64Recomp already knows to substitute those implementations — it just needs
  the names, which an elf would have supplied. libultra shipped as a static
  library, so its functions are byte-identical in every game linked against the
  same version, and fingerprinting them is the same problem IDA's FLIRT
  signatures solve.

What it cannot recover, it says so about. Overlays — code the game DMAs out of
the ROM at runtime — are only found when the game tabulates them, and a title
that computes its segment addresses in code instead has to have them written
down in a record under `N64Bundler/titles/<ID>/`. Super Mario 64 is one of
those: its engine segment and libgoddard are reached through linker symbols,
and without the record 1,207 of its calls leave the code that was recovered.
The records hold addresses, sizes and a hash — never bytes of a game.

Between them, plus the record under `N64Bundler/titles/NSME/`, Super Mario 64
recompiles from the cartridge dump alone into 3.5MB of native arm64 — 5,034
functions, in about ten seconds. **Every one of its 14,601 internal calls lands
on a recovered function boundary, and none of them leaves the code that was
recompiled.**

Drop the ROM on the window and it lands in the library with a cover and a Play
button; press Play and a window titled Super Mario 64 opens with RT64 on Metal
behind it, and the game plays.

**What it took, past the analysis, was libultra's device drivers.** The
signature database here is built from a much later SDK than this cartridge was
linked against, so it names the parts of libultra that did not change between
them — threads, message queues, caches — and none of the drivers. Left
unnamed, they run as recompiled MIPS writing to hardware that is not there, and
the game stops the first time it waits for one: `osContInit` blocks on the
message an interrupt from the serial interface would have posted, and the
interrupt never comes.

Sixteen of them are named in the title record now, each identified from the
disassembly and each with the evidence written beside it: the four controller
and four EEPROM calls that reach the PIF, the three audio interface calls, the
PI manager, and `__osSiRawStartDma` under all of them. They were found by
taking every call from the game's own code into libultra and reading what each
one does with the hardware, rather than by matching bytes.

Two things after that were not about this cartridge at all. **A loop that only
reads memory cannot end on its own here**, because the runtime schedules only
when the game calls into it, so the recompiler yields out of one — and Super
Mario 64's audio initialiser spins on a counter with a comparison in the loop,
which the old rule did not recognise. **And libgoddard reads its data through
the TLB**: the Mario head DMAs its display data into the main pool and maps it
at 0x04000000, and librecomp holds the console's memory as one flat array with
nothing mapped there. A TLB entry is an alias — two addresses, one page — so
the runtime makes one.

Sound needed the last piece. The signal processor runs its own instruction set
and the audio list is the one microcode ultramodern does not handle itself, so
it has to be translated out of the cartridge like the rest of the code.
`RSPRecomp` does that; what was missing was knowing where it is, which nothing
in the image says, so it is a `[[microcode]]` line in the title record.

A second ROM, Mario Builder 64, is a Super Mario 64 romhack that links against
exactly the libultra a signature database could be built from here, so its
whole libultra API is named without a record. It boots, brings up its threads,
relocates its main segment, runs its game loop, and reaches its startup screen
— the game's own font, saying that SD card emulation is not detected.

Two titles is enough to see the shape of the problem: with the right libultra a
ROM gets into its own code and what is left is finding its segments; without
one, every device driver has to be identified by hand and written down. Super
Mario 64's record is what that costs — twenty-six lines of addresses, and the
game plays.

**So: a game that plays is not every game, and some ROMs will not boot at all.**
Everything the analyser knows it learned from two cartridges, and the next one
will teach it something else.

## Legal

**No game data ships here, ever.** No ROM, extracted asset, or Nintendo code;
bring your own dump of a cartridge you own. ROMs are `.gitignore`d. The
per-title records under `N64Bundler/titles/` are measurements — addresses,
sizes, and a hash — never bytes of a game.

**No libultra ships here either.** Naming the library functions inside a ROM
means comparing them against libultra's own binaries, and those are Nintendo's.
`n64sig` builds the signature database; it does not come with one. Point
`N64_LIBULTRA` at the `libultra*.a` from a decompilation project you have set
up:

```sh
N64_LIBULTRA="/path/to/decomp/lib/n64/libultra*.a" ./N64Bundler/build.sh
```

Without it a ROM still recompiles — it just does so with libultra's own
hardware routines translated rather than replaced, which stops at the first
thing the game waits for. That is a gap a title record can close, and Super
Mario 64's does; it is sixteen lines of addresses, and each of them is a
function a wider database would have named for free.

## License

GPL-3.0-or-later, see [`LICENSE`](LICENSE).

## Credits

N64Recomp and N64ModernRuntime are by [Mr-Wiseguy](https://github.com/Mr-Wiseguy)
and the N64Recomp project. RT64 is by [Dario](https://github.com/DarioSamo)
and contributors. None of this would exist without them.

## Contributing

Issues and pull requests welcome. Never attach a ROM or game assets to an
issue: a game ID and the failing address are enough.
