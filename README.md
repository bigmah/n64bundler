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

**This is under construction.** [PLAN.md](PLAN.md) has the design, what works
today, and what does not.

## Quick start

```sh
git clone --recursive https://github.com/bigmah/n64bundler.git
cd n64bundler
./N64Bundler/build.sh
```

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
The first build compiles RT64, which takes a while.

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
  there. On Super Mario 64 (USA) it recovers about 3,890 functions, and 100% of the
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
down in a record under `N64Bundler/titles/<ID>/`.

Between them, Super Mario 64 recompiles from the cartridge dump alone into
3.6MB of native arm64 — 3,889 functions, in about a second of compiling. Of
those, 10 are stubbed: nine drive coprocessor 0 and one is libultra's exception
preamble, which has no correct division into functions at all. 1,207 calls
leave the code that was recovered, and those go to the overlays nothing here
finds.

**So: a game that boots is not a game that finishes, and some ROMs will not
boot at all.** That is the honest state of it, and the per-title records exist
to close the gap one title at a time.

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
hardware routines translated rather than replaced, which does not get far.

## License

GPL-3.0-or-later, see [`LICENSE`](LICENSE).

## Credits

N64Recomp and N64ModernRuntime are by [Mr-Wiseguy](https://github.com/Mr-Wiseguy)
and the N64Recomp project. RT64 is by [Dario](https://github.com/DarioSamo)
and contributors. None of this would exist without them.

## Contributing

Issues and pull requests welcome. Never attach a ROM or game assets to an
issue: a game ID and the failing address are enough.
