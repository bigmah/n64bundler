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

So the honest statement, and the one the README makes: **a game that plays is
not every game, and some ROMs will not boot at all.** The design answer is the
same one DolBundler uses for tuning — a per-title record checked into the
repo. Super Mario 64 is the case that proves both halves: the analyser
finds neither of its two extra code segments on its own, and with them written
down every call in the image is placed.

```
N64Bundler/titles/NSME/title.toml     sections n64rip could not find on its own,
                                      the libultra functions no signature named,
                                      where the audio microcode is, the save
                                      type, and any boundary the analyser got
                                      wrong
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
  │                     RSPRecomp  each [[microcode]] → C
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
| **a game that draws a frame** | done — Mario Builder 64 draws its startup screen; see below |
| the TLB | done — `osMapTLB` and its family, aliased with `mach_vm_remap` |
| audio microcode | done — `RSPRecomp` wired in, one `[[microcode]]` line per block |
| scripted input, audio levels | done — `N64B_INPUT`, `N64B_LEVELS`; see below |
| **a game that plays** | done — Super Mario 64, with sound, input and saves |

### Where Super Mario 64 stands

```
5,034 functions recovered: 387 by following calls, 4,654 by sweeping
14,601 of 14,601 internal calls land on a function boundary (100.00%)
0 calls point outside every section found
39 functions named from libultra signatures, plus 16 named in the title record
9 functions stubbed: they drive hardware and nothing named them
1 block of RSP microcode, 42% coprocessor 2
```

Every call in the image lands on a function this analysis recovered, and none
of them leaves the code it recompiled. That took the title record: Super Mario
64 loads two further code segments — the engine, which is most of the game's
logic, and libgoddard, the Mario head on the file select screen — and it
addresses both through linker symbols rather than through a DMA table, so
nothing in the image points at them. Written down once, they are found every
time. Without the record the same ROM recovers 3,890 functions and 1,207 calls
leave the code.

**And it plays.** The title screen, the file select, Peach's letter, the castle
grounds, Mario under the control of a pad, the music and the sound effects, and
a save file that survives quitting.

### What took Super Mario 64 from a black window to a game

Four things, and each was invisible until the one in front of it was gone.

**It stopped inside the serial interface.** The game loop thread reached
`init_controllers`, called `osContInit`, and blocked on the message an SI
interrupt would have posted. There is no SI and there is no interrupt, so that
is where it stayed — and that, rather than anything about the analysis, was the
black window.

The signature database is built from a much later SDK than this cartridge was
linked against, so it names what did not change between them — threads, message
queues, caches, the task interface — and none of the device drivers. Sixteen of
those are named in the record now, each read out of the disassembly rather than
guessed, with the evidence beside it. They were found by listing every call the
game's own code makes into the libultra range and asking what each one does with
the hardware: which registers it touches, what constants it builds, what shape
its arguments have. `osContInit` sets a one-shot flag and waits until the
counter passes 500,000, which is the half-second libultra gives the controllers
after a cold boot. `osEepromLongRead` rejects an address of 64 or more, which is
the 4kbit chip's block count, and loops over the single-block call.
`osAiSetFrequency` writes the DAC rate register. None of that is ambiguous.

A side effect worth having: with the EEPROM calls named, the analyser's save
type detection now has evidence to work from. It says `eep4k` because the
EEPROM routines are linked in, where before it said "no libultra save routine
was named".

**Then it drew three frames and stopped.** The audio session initialiser waits
for the sound thread by setting a counter to zero and reading it in a loop. The
recompiler already knew this shape from Mario Builder 64 — a loop the runtime
can never break, because it only gets to schedule when the game calls into it —
but only for a loop of loads and nops. This one compares as it goes, and a
`slt` is neither.

The rule is about what the loop *carries* now rather than what it contains.
Walk one iteration: if every register it reads before defining is one that only
a load writes, then nothing survives from iteration to iteration except what
came out of memory, so the loop cannot end on its own. `while (p != end) p++`
reads the pointer it increments and is rejected by the same test. Loads and
register arithmetic are allowed inside; a store, a call, a second branch or a
coprocessor access is not.

**Then it died in libgoddard.** The Mario head reads its display data through
the TLB: the loader DMAs it into the main pool and maps it in 64KB pages at
0x04000000. librecomp holds the console's memory as one flat array indexed by
`address - 0x80000000`, which sends a mapped address into the four gigabytes it
reserves and never maps.

A TLB entry is an alias — two addresses, one page — and `mach_vm_remap` makes
exactly that. So the runtime keeps the entries as the console keeps them and
rebuilds the aliases whenever they change, one *host* page at a time: this
machine's pages are 16KB and the console's can be 4KB, so the unit has to be
the larger one. A host page is aliased when every console page inside it agrees
on a distance from its physical address, which is what mapping a buffer looks
like. Console pages the game did not map are allowed inside one and end up
pointing at whatever physical memory follows; on the console they would fault,
and a game that stays inside the mapping it asked for cannot tell the
difference. Mario Builder 64 maps a single 4KB page, and that compromise is
what makes it work at all.

**And it was silent.** ultramodern draws a graphics task itself, so the only
microcode that has to be translated is whatever else a game submits — the audio
list. `RSPRecomp` was built and the module ABI had a slot for the result;
nothing filled it, so every audio task was reported complete without running.

Where the microcode is cannot be recovered. The CPU never executes a word of
it, so there is no call to follow, and the address reaches the task through a
linker symbol the way the engine segment does. It is a `[[microcode]]` line in
the record, and three things about it had to be right:

- **The size the task carries is wrong.** Super Mario 64 sets `ucode_size` to
  0x800 and the boot microcode ignores it, filling instruction memory to the
  end. Two of the sixteen command handlers live past 0x800.
- **The recompiler could not find the command handlers.** A microcode
  dispatches through a table of halfword addresses in its data blob, and the
  blob is data — nothing in the instruction stream points into it. The analyser
  reads it out: a halfword counts as a branch target when it is inside the text
  and four-byte aligned, which is four bytes out of the sixty-four thousand a
  halfword could hold. That finds all sixteen entries of the table and rejects
  the sixteen bit masks beside them without having to know they are masks.
- **A microcode that goes wrong must not take the game down.** The module wraps
  each block so one that stops early reports its task complete and says so
  once. Taking the process down over sound is the wrong trade for a bundler,
  and the host already made that trade for a task it cannot run at all.

A recorded offset is checked by shape before it is trusted. The R4300 has no
coprocessor 2, so nothing the CPU runs contains one; Super Mario 64's audio
microcode reads 42% coprocessor 2 where data reads near zero.

### A pad a script can hold, and a level meter

The same argument the screenshot is here for, twice over. A game past its title
screen cannot be reached by waiting — something has to press Start — and
whether a game is making a sound is a thing you find out by listening, which a
script cannot do.

```
N64B_INPUT="300:start,308:,430:a,438:,2170:up"   holds port one
N64B_LEVELS=1                                     the loudest sample each second
N64B_SCREENSHOT_AFTER="900,3300,5400"             several frames, not one
```

Frames are counted in reads of controller one, which is once per game frame in
anything that polls the way libultra intends. That is how everything above
was checked: Start to leave the title, A to pick a file, A twice to dismiss Lakitu,
and the stick to walk Mario up to the castle bridge, with the frame at each
step written out as a PPM. And the levels are how "it plays its music" became a
measurement — peaks between a third and nine tenths of full scale at 32kHz,
with silence in the transitions and nowhere else.

The second title is Mario Builder 64, a Super Mario 64 romhack, and it is
where every boundary rule here was actually tested — Super Mario 64 was already
at 100% before any of them existed:

```
2,657 functions recovered across three sections
8,963 of 8,963 internal calls land on a function boundary (100.00%)
626 boundaries added where a call landed inside a function
```

Before the splitting pass it scored 77.86%, and each of the calls landing
inside a function was a place the recompiler would have invented a `static_`
function of its own — which cannot be named, cannot be stubbed, and arrives too
late for any check here to have looked at it.

**It needs no record for its libultra, because it links against exactly the
libultra this machine has a signature database for.** 57 functions are named,
in one contiguous run from `0x80130960` to `0x80136CC0` — which is what a
correctly identified static library looks like — and they are the whole public
API: `osPiStartDma`, `osCreateViManager`, `osViSetMode`, `osViSwapBuffer`,
`osSpTaskStartGo`, `osCreateThread`, `osRecvMesg`. That is the difference
between the two cartridges in one line: Super Mario 64 needed sixteen device
drivers identified by hand, and this one needed none. It was the first to draw
for exactly that reason, before any of the work above existed.

It was black for a while, and the reason came down to one value: the game
handed `osViSwapBuffer` a null framebuffer, every frame, forever, because it
never reached the code that fills the pointer in. What it had not reached was
its own behaviour interpreter, and the four faults below are what was in the
way.

Getting it that far took a title record and three things in the host:

- **The record's `main` segment.** Mario Builder 64 moves nearly all of its
  code 0x36D0 higher than where IPL3's copy put it. Nothing in the image says
  so; the address was found by taking every `jal` target in the region and
  every offset that looks like a function prologue, and asking which difference
  between them occurs most often. 756 of 1,593 targets agree on it and 75 on
  the next best, which is not a close call.
- **The host places sections where the analysis says, not where it sees a
  DMA.** librecomp decides a section's address by watching the game read the
  ROM. That is right for a decompilation, where nothing knew the address in
  advance. Here the module carries it — and Mario Builder 64 moves its segment
  with a plain CPU copy, which there is nothing to watch. Every function in it
  resolved 0x36D0 low until the host said otherwise.
- **An RSP task with no microcode is completed, not fatal.** Graphics tasks go
  to the renderer and never reach that path; what is left is audio. Taking the
  process down over sound is the wrong trade for a bundler.

And it took the thing this project did not have: **a way to see what a
recompiled game was doing when it went wrong.** `n64b-port --trace` turns on
the recompiler's trace mode and supplies the header it expects; the host keeps
the last few hundred function entries in a ring and prints them however the
process ends. Rounds of "run it, read the last function, add the address it
could not resolve to the record" took Mario Builder 64 from dying on its first
indirect call to running its game loop; the record names 47 functions now, and
every one of them is a function nothing calls directly.

**It draws.** This is Mario Builder 64's startup screen, read out of the
console's memory exactly as the video interface would scan it:

```
note: wrote frame.ppm, 320 by 237, from the frame at 0x0040E5C0
320x237, 3 colours
  000000 x72417     <- background
  ffffff x2932      <- body text
  ffff00 x491       <- the heading
```

It is the game's own font, drawn from the game's own display lists, saying that
SD card emulation is not detected and the level editor will not be able to
save. The game then waits for a button.

Getting from nineteen black frames to that took four separate faults, and each
one was invisible until the one in front of it was gone.

**The runtime was registering every section twice.** A game starts by having
the runtime register every section whose rom address falls in the first
megabyte, as though the cartridge were one contiguous image loaded at the
entrypoint — which is what a game built from an elf looks like. A game
recovered from a bare rom is not: its segments are scattered and land where its
own loader puts them. Registering a section again does not remove the first
registration, so both the guess and the truth stayed in the address-to-function
map, and the guess, at the lower address, answered first. Every indirect call
in Mario Builder 64's behaviour interpreter was landing on the function 0x36D0
further on — silently, because that address is a real function too. The host
now discards the guess before placing what the analysis found.

**Ten behaviour commands and seven geo commands were not functions.** Mario
Builder 64 runs each object through an interpreter whose whole body is
`handler = table[*script++]; handler()`, and the sixty-five handlers are
reached only through that table, so no `jal` points at them and nothing splits
the function each one sits inside. A table of code pointers is not enough on
its own to name a function — a `switch` compiles to one too, and its entries
are labels inside a function — and measured on this cartridge the two kinds do
not separate cleanly: the real tables are 76% to 94% preceded by a return and
the switch tables reach 58%. What does separate them is how much the walk
already found. Every real table here was between 78% and 97% recovered
boundaries and every switch table was 0%, so the gaps in the nearly-complete
ones are safe to name, and they are named in the record.

**The game loads code to the top of RAM.** One of its behaviour commands,
`0x16`, is four words: a ram address and a rom range. Five scripts issue it and
all five name the same 3KB segment, which lands at `0x807FF3A0`, immediately
under the top of an expanded 8MB console. The geo layouts those scripts build
then call into it. Nothing in the analysis had ever seen that code, so the
first call into it was an indirect call to an address in no section at all.
It is a `[[section]]` in the record now.

**And it was busy-waiting.** The last fault was not in the game at all. It sets
a counter to zero and reads it in a three-instruction loop until the audio
thread raises it. On the console the timer interrupt breaks that loop and
libultra runs the higher-priority thread. Here every thread is real but only
one runs at a time, the runtime picks which, and it only gets to pick when the
running thread calls into it — and a loop that only reads memory never does. So
the thread that would raise the counter never ran. The recompiler now
recognises a loop that cannot terminate on its own — a backward branch of at
most four instructions whose body and delay slot are loads and nops — and emits
a yield at the bottom of it. Exactly one loop in each of the two cartridges
tested matches. The display-list count went from 19 to 421 in the same thirty
seconds.

### A frame you can look at

A window is the real answer to "does this game draw", and a window is no answer
at all to a script, a log, or a machine whose screen is locked. The renderer
copies each finished frame back into the console's own memory in the console's
own format, which is what a real video interface would be reading, so that copy
is the frame. `N64B_SCREENSHOT=<path>` writes it as a PPM — no graphics API, no
readback, thirty lines. `N64B_SCREENSHOT_AFTER=<frames>` picks when.

### Two register windows and a watchpoint

Chasing all of that turned up three more things worth having.

**The console's registers are ordinary memory now.** librecomp maps KSEG0 and
nothing else, so a translated instruction storing to the RCP at `0xA4xxxxxx`
lands past the end of the mapping and takes the process down. That is why the
analyser used to stub every function touching one -- and stubbing a function
loses everything else it did. The host now backs `0xA0000000`-`0xBFFFFFFF` with
lazily committed zeroed pages, so those functions simply run: everything but
the register access is real, the access reads zero and discards writes, and
zero is the useful answer because libultra's waits are all "while the device is
busy". Stubs fell from 28 to 11 on Super Mario 64 and from 38 to 10 on Mario
Builder 64.

**A trace that separates the threads, and a backtrace when it stops.** One ring
buffer of function entries is all idle thread: it enters a function tens of
millions of times a second and the game thread's last dozen calls are nowhere.
Kept per thread, with runs of a repeated pair folded into a count, the thread
that stopped is one short list. And because the recompiled code calls a
recompiled function as an ordinary C function, a native backtrace names the
game's own call stack — which is how the loop that was calling a behaviour
handler four billion times was found, in one run.

**Two libultra functions are named by shape rather than by signature.**
`n64sig` will not fingerprint a function shorter than six instructions, and it
is right not to. But `osAiGetLength` and `osGetCount` are three instructions --
read one register, return it -- and the register decides which they are. Both
games were calling `osAiGetLength` forty-five times a second and getting
whatever was in `v0`.

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
7. ~~**Audio microcode**~~ — done. `RSPRecomp` runs over each `[[microcode]]`
   block in the record and the module picks between them by the address the
   task names.
8. ~~**The TLB**~~ — done. `osMapTLB` and its family, aliased with
   `mach_vm_remap` a host page at a time.

What is actually next:

1. **A wider signature database.** Several libultra revisions rather than one.
   This is the difference between a game that plays out of the box and a game
   whose sixteen device drivers have to be identified by hand first, and it
   needs archives rather than code.
2. **Finding the microcode without being told.** A ROM's RSP code is visible by
   shape — the R4300 has no coprocessor 2, so a block dense with it is
   microcode and nothing else is — and the address is one the game's own code
   builds. On Super Mario 64 those two rules together leave three candidates,
   one of which is the audio microcode; on Mario Builder 64 they leave
   forty-one, so the rule is not ready. What separates them is probably the
   text's extent, which is also the number the record has to carry today.
3. **More titles.** Two is not a sample. Everything the analyser knows how to
   do it learned from Super Mario 64 and Mario Builder 64, and the next ROM
   will teach it something else.

## Not in scope yet

- **iPhone.** DolBundler's phone path exists because iOS refuses to map an
  unsigned executable page, so every game is linked into the app before it is
  signed. The same is true here and the same solution would work. The reason it
  was out of scope — that no game ran on the Mac yet — has gone, but the TLB
  now uses `mach_vm_remap`, which is a thing to check before assuming the port
  is only signing.
- **Mods.** librecomp has a whole mod system and N64Recomp has a live
  recompiler behind it. Out of scope until the static path is solid.
