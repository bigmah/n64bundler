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
into the repo. Super Mario 64 is the case that proves both halves: the analyser
finds neither of its two extra code segments on its own, and with them written
down every call in the image is placed.

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
  │                     libultra naming, the title record if there is one
  │                        → <ID>.symbols.toml, <ID>.recomp.toml, <ID>.info.json
  │
  │  3. Recompile       N64Recomp  symbols.toml + rom → C
  │                     RSPRecomp  audio ucode      → C   (not wired up yet)
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
    gui/                 the Dioxus window (Rust)
    patches/             what upstream does not carry yet, applied by build.sh
    src/
      recompn64          the pipeline, usable on its own, porcelain for the GUI
      make_game_app.py   library entry, cover art, .app only with --app
      make_app_icon.py   N64Bundler's own icon
    titles/<ID>/         per-title metadata records
  ModernReality/         the runtime and the tools around it
    tools/
      n64rip             ROM → symbols.toml  (the analyser)
      n64sig             libultra*.a → a signature database
      n64b-port          drives N64Recomp and compiles the module
    host/                n64b-run: dlopen a module, RT64 + librecomp
    include/modernreality/module_abi.h
    src/ultra_gaps.cpp   the libultra helpers librecomp does not carry
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
| RT64 builds on macOS arm64 | done — static, native Metal backend |
| `n64rip` boundary recovery | done — 100% of Super Mario 64's calls land on a recovered boundary |
| `n64sig` libultra naming | done — fingerprints a `libultra*.a` and names what it finds in a ROM |
| A bare ROM recompiles to native arm64 | done — Super Mario 64 (USA), 5,034 functions, 3.5MB of Mach-O arm64 in about ten seconds |
| module ABI and `n64b-port` | done — one dylib per game, cached on the ROM hash |
| `n64b-run` host | done — RT64 on Metal, SDL audio and input, `dlopen`s a module |
| `recompn64` pipeline | done — four steps, porcelain protocol |
| Dioxus window | done — library, live console, per-game settings |
| `.app` packaging | done — cover art, icon, launcher holding no game data |
| per-title records | done — `N64Bundler/titles/NSME/title.toml` closes Super Mario 64's coverage |
| a second title | done — Mario Builder 64 recompiles too, at 100% coverage |
| **a game that draws a frame** | **not yet** — see below |

### Where Super Mario 64 stands

```
5,034 functions recovered: 31 by following calls, 5,003 by sweeping
14,601 of 14,601 internal calls land on a function boundary (100.00%)
0 calls point outside every section found
35 functions named from libultra signatures
31 functions stubbed: they drive hardware and no signature named them
```

Every call in the image now lands on a function this analysis recovered, and
none of them leaves the code it recompiled. That took the title record: Super
Mario 64 loads two further code segments — the engine, which is most of the
game's logic, and libgoddard, the Mario head on the file select screen — and it
addresses both through linker symbols rather than through a DMA table, so
nothing in the image points at them. Written down once, they are found every
time. Without the record the same ROM recovers 3,890 functions and 1,207 calls
leave the code.

The second title is Mario Builder 64, a Super Mario 64 romhack, and it is
where every boundary rule here was actually tested — Super Mario 64 was already
at 100% before any of them existed:

```
457 functions recovered
1,391 of 1,391 internal calls land on a function boundary (100.00%)
79 boundaries added where a call landed inside a function
5 functions the recompiler refused, stubbed and retried
```

Before the splitting pass it scored 77.86%, and each of the 308 calls landing
inside a function was a place the recompiler would have invented a `static_`
function of its own — which cannot be named, cannot be stubbed, and arrives too
late for any check here to have looked at it.

It recompiles to a 332KB module, loads into the host and starts. Then it fails
an indirect call to an address no section covers. The interesting part is what
that is: unlike Super Mario 64, Mario Builder 64 links against exactly the
libultra this machine has a signature database for, so the whole boot path —
`osPiStartDma`, `osCreateViManager`, `osViSwapBuffer`, `osSpTaskStartGo`,
`osCreateThread` — is named and substituted, and the game gets far enough to
start dispatching through its own tables. Its first failure was at
`0x80124FC0`, a function nothing calls directly; one `[[function]]` line in its
record fixed that and the next failure moved to a segment the analyser has not
found. Finding that segment is the same job the Super Mario 64 record already
does, and this romhack loads its code somewhere its parent does not.

**That contrast is the clearest thing two titles have shown.** With the right
libultra a ROM boots into its own code and the remaining work is finding
segments; without it a ROM stops inside libultra and no amount of segment
hunting helps.

The pipeline runs end to end: drop the ROM on the window, and about ten seconds
later Super Mario 64 is in the library with a cover, a `Play` button, and
optionally a `.app` in `~/Applications`. Pressing Play opens a window titled
Super Mario 64, brings up RT64 on Metal, allocates RDRAM, loads the ROM, and
starts the game's entry point on its own thread.

**And then the screen stays black, and this is why.** The 31 stubbed functions
are libultra, and between them they touch every register block on the machine:

```
PI 9   VI 14   SP 10   AI 8   MI 7   SI 5
```

The PI ones are the ones that matter. Super Mario 64 DMAs everything out of the
cartridge — levels, textures, the engine segment itself — through
`osPiStartDma`, and with the PI path stubbed the DMA never happens, the
completion message never arrives, and the game waits on a queue forever. The
runtime implements every one of those functions. It is only that nothing has
told the recompiler which functions they are.

### The second gap: libultra has to be named, and one archive is not enough

Recovering boundaries was the first thing a bare ROM does not give us, and that
one is solved. Names are the second, and they are what stands between this and
a game that draws.

A game built with libultra calls `osCreateThread`, `osViSwapBuffer`,
`osPiStartDma` and two hundred others, and none of that code can run as
recompiled MIPS: it talks to hardware that does not exist here. The runtime
reimplements all of it — and N64Recomp already knows to substitute those
implementations, because it carries a list of the names
(`src/symbol_lists.cpp`, 668 lines of `reimplemented_funcs`, `ignored_funcs`
and `renamed_funcs`). A decompilation project supplies the names from its elf
and the substitution happens for free.

`n64sig` supplies them instead, by fingerprinting the library. libultra shipped
as a static archive, so `osCreateThread` is byte-identical in every game linked
against the same build of it, and matching it is the problem IDA's FLIRT
signatures solve. Each word carries a mask taken from the object file's own
relocations, so the fields the linker filled in are ignored and the rest is
compared exactly.

**"The same build of it" is the whole difficulty.** libultra went through half
a dozen revisions between 1996 and 2000, and a game pins whichever one its SDK
shipped. Fingerprinted against the archive this machine has, Super Mario 64
matches 35 functions and misses the rest — and the misses are not marginal:
that archive's `__osDisableInt` is 28 instructions long and threads a global
interrupt mask through, where Super Mario 64's is the eight-instruction version
that predates it. They are the same function and share not one word.

So the fix is a database built from several revisions rather than one, which is
a matter of having the archives rather than of writing code. Failing that, a
title record can name a function outright — that is what the `[[function]]`
entries are for, and the addresses to fill in are the `stubbed` list `n64rip`
writes into `<ID>.info.json`.

One shortcut that looks promising and is not: the stubbed functions can be
identified from the registers they write, and for Super Mario 64 they fall out
cleanly — `0x803284B0` writes `PI_DRAM_ADDR`, `PI_CART_ADDR` and `PI_WR_LEN`,
so it is `osPiRawStartDma`; `0x80325DB0` writes the AI's address and length, so
it is `osAiSetNextBuffer`; `0x8032AE10` writes `MI_INTR_MASK`, so it is
`osSetIntMask`. Naming them buys nothing. librecomp's `osPiRawStartDma` is
itself a stub that puts up a message box saying the function that called it was
not properly named, and its `osSetIntMask` is empty — which is what stubbing
already does. The runtime substitutes at the level of the public API, and the
public API is exactly the part that touches no registers and so cannot be
fingerprinted this way.

Until then, a function that drives hardware and has no name is stubbed rather
than translated. That is a deliberate choice and it is the difference between a
game that does nothing and a process that dies: libultra's own code writes to
the RCP's registers at `0xA4xxxxxx`, the runtime maps only KSEG0, and the store
lands past the end of the mapping. Stubbed, it is a no-op and everything else
still runs.

## Roadmap

Everything the plan set out is built. What is left is coverage, which is
measurement and data rather than design.

1. ~~**`n64rip`**~~ — done. Header, CIC, boot segment, function recovery,
   overlay tables, save-type evidence, title records, symbols TOML.
2. ~~**`n64b-port`**~~ — done. Runs N64Recomp over the analyser's output,
   writes the module descriptor, compiles to one arm64 dylib, caches on the ROM
   hash plus the tools' revisions and the compiler flags.
3. ~~**`n64b-run`**~~ — done. RT64 on Metal, SDL window, audio and input,
   librecomp configuration, `dlopen` of a module.
4. ~~**`recompn64`**~~ — done. The four-step pipeline with the porcelain
   protocol.
5. ~~**The window**~~ — done. Library, live console, per-game settings, Create
   App.
6. ~~**Per-title records**~~ — done, and Super Mario 64 has one.

What is actually next:

1. **A wider signature database.** Several libultra revisions rather than one.
   This is the whole difference between a game that boots and a game that does
   not, and it needs archives rather than code.
2. **Audio microcode.** `RSPRecomp` is built and the module ABI carries a slot
   for the result; nothing identifies which microcode a ROM uses yet, so
   `get_rsp_microcode` returns nullptr and an audio task is reported rather
   than run.
3. **More titles.** Two is not a sample either. Everything the analyser knows
   how to do it learned from Super Mario 64 and Mario Builder 64, and the next
   ROM will teach it something else.

## Not in scope yet

- **iPhone.** DolBundler's phone path exists because iOS refuses to map an
  unsigned executable page, so every game is linked into the app before it is
  signed. The same is true here and the same solution would work, but it is
  worth nothing until a game runs on the Mac.
- **Mods.** librecomp has a whole mod system and N64Recomp has a live
  recompiler behind it. Out of scope until the static path is solid.
