# N64Bundler — design and roadmap

Drop an N64 ROM in, watch it recompile, and play it from the library. The
ROM's MIPS code is statically recompiled to native arm64; the game runs as
compiled code, not interpreted.

This is the N64 counterpart to [DolBundler](../dolbundler), and it is
deliberately the same shape: a Dioxus window over a headless pipeline, a
library of games, per-game `.app` bundles that hold no game data, and a
runtime tree that carries the forks.

```
mario64.z64  ──▶  N64Bundler  ──▶  ▶ Super Mario 64   in the library
                             └──▶  ~/Applications/Super Mario 64.app   (opt in)
```

---

## The one hard difference from DolBundler

DolRecomp reads a GameCube disc's `main.dol` directly: a DOL carries its own
section table, so "where does code live, and at what address" is answered by
the file. **An N64 ROM answers neither question.** It is a flat image with a
64-byte header, 4KB of boot code, and then whatever the game's linker script
decided — no sections, no symbols, no segment table that the format itself
defines.

N64Recomp therefore does not accept a ROM. It accepts *metadata plus* a ROM:
either an ELF with symbols (what a decompilation project produces), or — since
the `symbols_file_path` option landed — a TOML describing sections and the
functions inside them, alongside the raw ROM:

```toml
[[section]]
name = "boot"
rom  = 0x001000
vram = 0x80246000
size = 0x100000

  [[section.functions]]
  name = "func_80246000"
  vram = 0x80246000
  size = 0x50
```

That format is the whole opening. It means **N64Bundler does not need a
decompilation project per game** — it needs a tool that recovers that metadata
from the ROM itself. Recovering it is the analysis DolRecomp gets for free from
the DOL header, and it is the piece this repo has to build.

That tool is `n64rip`. Everything downstream of it is plumbing that DolBundler
has already proven.

### What that means for coverage, honestly

Function boundary recovery on MIPS is tractable and largely reliable: functions
end at `jr $ra` plus a delay slot, start on alignment after that, and every
`jal` in the image confirms a target. The boot segment — the megabyte IPL3
copies from ROM `0x1000` — comes out well.

**Overlays are where coverage is lost.** Most N64 games DMA further code
segments from ROM into RAM at runtime, and nothing in the ROM is *required* to
say where those live. `n64rip` looks for the DMA tables libultra games
conventionally build (runs of `{romStart, romEnd, vramStart, vramEnd}` words),
which finds them in a good share of titles and misses them in the rest.

So the honest statement, and the one the README makes: **a game that boots is
not a game that finishes, and some ROMs will not boot at all.** The design
answer is the same one DolBundler uses for tuning — a per-title record checked
into the repo:

```
N64Bundler/titles/NSME/title.toml     sections n64rip could not find on its own,
                                      the save type, the audio microcode, and
                                      any function the analyser got wrong
```

An unknown ROM gets pure analysis. A ROM with a record gets analysis plus the
record, and the record wins. No game data is ever in one — addresses, sizes and
a hash, the same rule as `DolBundler/tuning/`.

---

## Pipeline

```
game.z64
  │
  │  1. Ingest          normalise byte order (.n64/.v64 → .z64), parse the
  │                     header, hash the image, derive the game code and name
  │
  │  2. Analyse         n64rip: CIC/IPL3 detection, boot segment bounds,
  │                     function boundary recovery, overlay DMA tables,
  │                     audio microcode identification
  │                        → <ID>.symbols.toml, <ID>.recomp.toml, <ID>.rsp.toml
  │
  │  3. Recompile       N64Recomp  symbols.toml + rom → C
  │                     RSPRecomp  audio ucode      → C
  │
  │  4. Compile         clang: C → arm64 <ID>.dylib, one module per game
  │                     exporting the n64b_module_v1 descriptor
  │
  │  5. Run             n64b-run: dlopen the module, register its sections and
  │                     game entry, start librecomp/ultramodern over RT64
  ▼
▶ in the library, and optionally ~/Applications/<Game>.app
```

Steps 2–4 are the slow ones and are cached on the ROM hash plus the analyser
and compiler revisions, so re-adding a ROM is instant.

### Why the game is a dylib

The alternative is Zelda64Recomp's shape: link the recompiled game, librecomp,
ultramodern and RT64 into one executable per title. That relinks the renderer
for every game and gives every game its own copy of it.

A bundler has a *library*. So the runtime is built once (`n64b-run`, with RT64
inside it) and each game is a small `.dylib` that it `dlopen`s — the same split
DolBundler uses, for the same reason. The module exports one descriptor:

```c
typedef struct {
    uint32_t abi_version;            // N64B_MODULE_ABI
    const char *game_id;             // "NSME"
    uint64_t rom_hash;
    uint32_t entrypoint_address;
    void (*entrypoint)(uint8_t *rdram, recomp_context *ctx);
    SectionTableEntry *code_sections; size_t num_code_sections;
    size_t total_num_sections;
    int *overlays_by_index;          size_t num_overlays;
    RspUcodeFunc *(*get_rsp_microcode)(const OSTask *task);
    int save_type;
} n64b_module_v1;
```

Undefined symbols in the module (`LOOKUP_FUNC`, the `MEM_*` helpers, every
ultramodern call) resolve against the host at load time, which is what
`-undefined dynamic_lookup` against an `-export_dynamic` host buys.

---

## Layout

```
n64bundler/
  N64Bundler/            the glue — the window, the pipeline, the packaging
    build.sh             builds everything and installs the app
    forks.sh             push a change back through the nested forks
    gui/                 the Dioxus window (Rust)
    src/
      recompn64          the pipeline, usable on its own, porcelain for the GUI
      make_game_app.py   library entry, cover art, .app only with --app
      make_app_icon.py   N64Bundler's own icon
    titles/<ID>/         per-title metadata records
  ModernReality/         the runtime and the tools around it
    tools/
      n64rip             ROM → symbols.toml  (the analyser)
      n64b-port          drives N64Recomp and compiles the module
      n64b-run           the host: dlopen a module, RT64 + librecomp
    include/modernreality/module_abi.h
    src/                 renderer, input, audio and RSP glue for the host
    vendor/
      N64ModernRuntime/  submodule; carries N64Recomp inside it
      rt64/              submodule
```

"Project Reality" was the N64's development codename, and the RCP is the
Reality Coprocessor — hence `ModernReality`, the counterpart to ModernGekko.

---

## Status

| Step | State |
|---|---|
| N64Recomp builds on macOS arm64 | done — Apple clang 17 |
| N64ModernRuntime builds on macOS arm64 | done — ultramodern + librecomp link |
| RT64 builds on macOS arm64 | done — `rt64.dylib`, native Metal backend |
| `n64rip` boundary recovery | done — 100% of Super Mario 64's internal calls land on a recovered boundary |
| `n64sig` libultra naming | done — fingerprints a `libultra*.a` and names what it finds in a ROM |
| **A bare ROM recompiles to native arm64** | **done** — Super Mario 64 (USA), 3,889 functions, 21MB of C, 3.6MB of Mach-O arm64 in about a second |
| module ABI and `n64b-port` | next |
| `n64b-run` host | not started |
| `recompn64` pipeline | not started |
| Dioxus window | not started |
| `.app` packaging | not started |
| per-title records | not started |

### Where Super Mario 64 stands

```
3,889 functions recovered: 29 by following calls, 3,862 by sweeping
10,336 of 10,336 internal calls land on a function boundary (100.00%)
35 functions named from libultra signatures
9 functions stubbed: they drive coprocessor 0 and no signature named them
1 function stubbed: hand-written assembly that does not divide into functions
1 boundary merged where a branch crossed it
1,207 calls point outside every section found; that code was not recompiled
```

The last line is the honest one. Those 1,207 calls go to Super Mario 64's
overlays, which it addresses through linker symbols rather than a table, so
nothing here finds them. The boot segment is recompiled and the rest is not,
which is exactly the coverage limit this design predicted and exactly what a
title record exists to fix.

### The second gap: libultra has to be named

Recovering boundaries was the first thing a bare ROM does not give us. Names
are the second, and they matter more than they sound like they should.

A game built with libultra calls `osCreateThread`, `osViSwapBuffer`,
`osSpTaskStart` and two hundred others, and none of that code can run as
recompiled MIPS: it talks to hardware that does not exist here. The runtime
reimplements all of it — and N64Recomp already knows to substitute those
implementations, because it carries a list of the names
(`src/symbol_lists.cpp`, 668 lines of `reimplemented_funcs`, `ignored_funcs`
and `renamed_funcs`). A decompilation project supplies the names from its elf
and the substitution happens for free.

Recovered symbols have no names, so none of it fires, and the recompiler
translates libultra's own `mtc0`/`mfc0` and MMIO code instead. That is what the
first Super Mario 64 build hit: `Unhandled cop0 register in mfc0: 10`, which is
`EntryHi`, in what is certainly one of libultra's TLB routines.

The fix is to identify libultra functions by their machine code. libultra
shipped as a static library, so `osCreateThread` is byte-identical in every
game built against the same version, and matching it is the same problem IDA's
FLIRT signatures solve. The material is freely available: the decompilation
projects ship the `libultra*.a` archives, with symbols, and a signature
database built from them names the functions in any ROM.

Once they are named, N64Recomp's existing lists do the rest, which is why this
is the next thing to build rather than a later refinement.

## Roadmap

1. **`n64rip`** — header, CIC, boot segment, function recovery, symbols TOML.
   Checked against Super Mario 64 (USA), whose decompilation gives a ground
   truth to score the recovered function list against.
2. **`n64b-port`** — run N64Recomp over the analyser's output, compile the C to
   a module, cache on the ROM hash.
3. **`n64b-run`** — the host. RT64 window, SDL input and audio, librecomp
   configuration, `dlopen` of a module.
4. **`recompn64`** — the four-step pipeline with the porcelain protocol.
5. **The window** — library, live console, per-game settings, Create App.
6. **Per-title records** and the overlay work they exist to hold.

## Not in scope yet

- **iPhone.** DolBundler's phone path exists because iOS refuses to map an
  unsigned executable page, so every game is linked into the app before it is
  signed. The same is true here and the same solution would work, but it is
  worth nothing until a game runs on the Mac.
- **Mods.** librecomp has a whole mod system and N64Recomp has a live
  recompiler behind it. Out of scope until the static path is solid.
