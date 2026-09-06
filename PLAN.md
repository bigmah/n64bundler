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
| **a cartridge whose game is compressed** | done — Banjo-Tooie unpacks itself and 9,732 functions come back; see below |
| a game linked through the syscall exception | done — the record names the stub table and the handler, and the runtime stands in for the exception |
| **overlays recompiled at runtime** | done — a function a game decompresses into memory it allocated is translated the first time it is jumped to |
| the boot chip's challenge | done — the PIF's memory round-trips and a CIC-6105 challenge is answered |
| **a third game that plays** | done — Banjo-Tooie runs its opening, reaches its file select and starts a game; see below |

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

### A cartridge that has to be run before it can be read

Banjo-Tooie is a shape the analyser had not met. Super Mario 64 and Mario
Builder 64 are games with their code in the image; the sweep finds most of it
and a record says where the rest landed. Banjo-Tooie is a sixteen-kilobyte
loader followed by thirty-two megabytes of compressed data. There is no second
segment to write down, because until the loader has run the code does not
exist anywhere — not in the cartridge, not at any offset, not in any form a
reader can point at.

Dropped in as it stood, it recovered 83 functions out of a 32MB image and
reported 100% of its calls placed, because all 153 calls inside the loader do
land on a boundary. Pressing Play opened a window and closed it a quarter of a
second later.

**What the loader wanted was one function named.** `func_80001A40` is
`osPiRawStartDma`, instruction for instruction: it spins on PI_STATUS's two
busy bits, writes `osVirtualToPhysical` of its third argument to PI_DRAM_ADDR,
ors its second with the cartridge base held at 0x80000308 and masks it into
PI_CART_ADDR, and then writes size-1 to PI_WR_LEN for direction 0 and PI_RD_LEN
for direction 1. librecomp carries that one as a stub that ends the process, on
the reasoning that a game reaching it means some libultra function above it
went unnamed — which is true of a game built from a decompilation and false of
a cartridge whose loader is not libultra at all. Unnamed, its register writes
went to the zeroed pages the host backs the RCP with, nothing was transferred,
and the decompressor read a buffer of zeroes and walked its source pointer to
1. That fault was the whole of what a player saw.

**And then the game has to be run to be read.** With the driver named the
loader works, unpacks two blocks, checksums them and calls 0x80012030 — which
is not there, because it was the loader's job to put it there. So:

- `n64b-run --unpack <image>` stops at the first address the runtime cannot
  find, writes the console's eight megabytes out, and says where the game was
  going. It hangs off a weak `recomp_missing_function` in librecomp, so a host
  that does not care links exactly as before.
- a record's `[[unpacked]]` block names a segment by console address and size,
  and `n64rip --unpacked <image>` takes those bytes out of the image,
  byte-swaps them back out of the host's word order, and splices them onto the
  end of the ROM the recompiler reads. From there they are sections like any
  other — the sweep runs over them, the signature database names them, and the
  symbol file cannot tell they were not in the cartridge.
- the pipeline loops, because a game unpacks in stages and each stage only
  appears once the one before it is running. Banjo-Tooie takes two rounds: the
  loader reveals the core at 0x80012030, and the core reveals the main segment
  at 0x800815C0. The analysis reports how many of the record's segments came
  back as code, and the loop ends when that matches how many it asked for.

```
83 functions      the loader alone, which is all the image holds
817 functions     with the core it unpacks
6,039 functions   with the main segment the core unpacks
9,732 functions   with its syscall stub table cut into the functions it is
13,802 of 13,802 internal calls land on a function boundary (100.00%)
1 call points outside every section found
93 functions named from libultra signatures, 4 more by shape
```

Two things had to be fixed to get there, and both were general rather than
about this cartridge. **The escape check ran too early**: `recover_functions`
stubs a function whose branches leave it, but it works one section at a time,
and `split_at_cross_section_calls` adds boundaries afterwards — so a call
arriving from a segment unpacked later can cut a function exactly where a
branch crosses, after the only pass that would have noticed. Banjo-Tooie has
three, all float helpers with two entry points sharing one body. **And
N64Recomp read past the end of the ROM sizing a jump table**: the address comes
from a linear walk of the registers, which does not follow control flow and can
be wrong, and there was no bounds check. It segfaulted. Bounded, it names the
function and the caller stubs it — but the message also said what was really
wrong, which is that this segment's jump tables live in rodata 0x2C000 past the
end of its code, and the section had to carry the hole between them, because
the recompiler finds a jump table by assuming it sits at the same distance from
the start of the ROM as from the start of the segment.

### The black window, and the four things behind it

The window stayed black with the game apparently healthy: nine threads, every
one of them in an ordinary `osRecvMesg` or `osSendMesg` wait, the video
interface configured with a real origin and a real framebuffer, and not one
display list submitted in thirty seconds. Unlike Super Mario 64 this was never
the device drivers -- Banjo-Tooie's libultra matches the signature database
well, and `osViSwapBuffer`, `osViSetMode`, `osCreateViManager`, `osSpTaskLoad`,
`osSpTaskStartGo`, `osContInit` and the EEPROM pair are all named without a
record saying so.

**It was sitting in its own reset handler.** One thread was not waiting: it had
entered `osDpSetStatus` 8.7 billion times in thirty seconds, from a three-word
loop at 0x80014A3C that nothing can leave. What is above that loop says what it
is -- stop the rumble motors on all four controllers, read `osTvType`, put the
video interface back to a plain mode -- and what reaches it is the retrace
thread, on every frame, through this:

```
    jal   0x8001DCA0        # mfc0 $v0, Status
    andi  $t6, $v0, 0x1000  # SR_IBIT5, the pre-NMI interrupt
    bnel  $t6, $zero, ...   # set: carry on
    jal   0x800149BC        # clear: the reset button has been pressed
```

libultra masks the pre-NMI interrupt off when RESET is pressed, so a game that
sees that bit clear knows it is being reset and blanks the screen until the NMI
arrives half a second later. `cop0_status_read` returned `ctx->status_reg`,
which nothing ever writes, which is zero. The check fired on the first retrace
and the game did what it was told for the rest of the run. A game reads that
register to find out what the console is doing, not only to change it, and this
one is the whole of what a player saw.

So the status register is modelled rather than left empty. The interrupt bits
-- IE and the eight mask bits, the low half of libultra's `OS_IM_ALL` -- are
what a console running a game has and what this runtime can never change, so
they are reported set and a write to them is stored and otherwise ignored,
which is what the runtime's own `osSetIntMask` already does one level up. The
exception-level and coprocessor-usable bits are stored the same way: there are
no exceptions here, and every coprocessor a translated instruction uses is
always available, so a game turning the floating point unit on at boot is
asking for something that is already true. Only the FR bit still does anything,
and only a bit nobody has reasoned about is reported -- once, rather than by
ending the game, which is the wrong trade for a bundler and the same one the
host already makes for an RSP task it cannot run.

A side effect worth having: a function whose only coprocessor 0 access is the
status register no longer has to be stubbed, because the runtime has something
to say for it now. Banjo-Tooie's stubs fell from 20 to 10.

**Then its trigonometry returned to nowhere.** Four functions are pairs of
entry points that set up their own constants and share one body, and each keeps
the caller's return address out of the way of the call it is about to make:

```
    or    $a1, $ra, $zero    # keep the caller's return address
    jal   0x80013818         # which this is about to overwrite
    jr    $a1                # and return to it
```

A `jr` through anything but `$ra` was an indirect tail call, which looks the
register up in the address-to-function map -- and a return address is not the
start of a function. Here it failed on zero, because nothing writes `$ra` in
recompiled code at all: a call is a C call and its return address is the
host's. The recompiler now reads a `jr` as a return when every write to that
register anywhere in the function is a move from `$ra` and none of them comes
after an instruction that links. A register built with `lui` or loaded from
memory -- an exception vector, a real function pointer -- fails that test and
is still a tail call, which is what the other four in this cartridge are.

**Then it read its cartridge with a load and got zeroes.** The parallel
interface is not only a DMA engine: the cartridge is mapped, and a load from
0xB0000000 plus a ROM offset reads a word of it directly. libultra reads
everything with a transfer, so a game built from a decompilation never needs
this. A game that wrote its own loader may, and Banjo-Tooie's overlay directory
is read one word at a time with `lui $s0, 0xB000; or $s0, $s0, $a0; lw $t2,
0($s0)`, with interrupts off, because a four-byte DMA is not worth the queue.
That load landed in the zeroed pages the register window is backed by, so every
entry of the directory read zero and the first overlay it tried to load was
allocated a garbage size. The host now fills that window with the cartridge --
copied rather than aliased, because the ROM is big-endian and the runtime keeps
memory so that an aligned word is a word this machine can load, and
`do_rom_read` is the call that puts one into the other.

### A game linked through the syscall exception

Thirty-two megabytes of game do not fit in eight megabytes of console.
Banjo-Tooie holds its code in 886 overlays and loads them as it needs them, so
a call from one into another cannot be a `jal` -- the callee may not be in
memory. Every one of them goes to a two-instruction stub instead, and the 4,234
stubs in one table at 0x80082540 are the whole of the game's linkage:

```
800893C8: syscall 0x309          which function, in the code field
800893CC: addi    $t0, $zero, 0  which entry point of its overlay
```

libultra's exception preamble reads Cause, and for an exception of type Sys
alone it writes 0x80081E74 into EPC and `eret`s -- so the game's own handler
runs with the address of the trapping instruction still in `$t0`, which is how
it knows which stub it came from. The handler reads that stub's own words back
out of memory, finds the overlay in a directory on the cartridge, loads and
decompresses it, assembles a thunk, rewrites the stub into a jump to that
thunk, and returns to the stub -- which now jumps to the thunk, which calls the
function and then calls the routine that puts the overlay back.

Nothing in the image can be followed into any of that. The stubs are reached
only through tables of pointers in the game's own data, so no `jal` names one
and the sweep ran them together into a few long functions: 543 of the 4,234 had
a boundary and the other 3,691 were addresses the game calls and the runtime
could not find. The handler is reached only through the exception vector, which
is hand-written assembly this analysis stubs, and the two are tied together by
a constant inside that assembly.

So a record says where the table is and where the handler is, and neither is
trusted without a check:

- **the table checks itself.** Every eight bytes of it begins with a `syscall`,
  and nothing a compiler emits contains one at all, so the recorded range reads
  every entry or almost none. n64rip cuts it into 4,234 two-instruction
  functions, and Banjo-Tooie goes from 6,039 functions to 9,732.
- **the runtime stands in for the exception**, calling the game's handler with
  the trapping address in `$t0`, the way the preamble would.
- **and it reads the two things the game wrote after it was compiled rather
  than executing them.** The rewritten stub is a word to decode; the thunk is a
  handful of instructions the host interprets -- constants, an add, a load, a
  store and the two kinds of jump, delay slots included -- because code a game
  assembles while it runs is in no cartridge and there is nothing to translate.
  `jal` calls a translated function and comes back, which is how a thunk of
  "call the function, then put the overlay back" runs both halves; `j` and `jr`
  are the end of it. An instruction the interpreter does not know stops it and
  says which, because a thunk nobody has seen is something to look at rather
  than something to guess about. The recursion ends for the same reason it does
  on the console: the handler rewrites the stub before it returns to it.

### Code that is in no cartridge

Past its stub table, Banjo-Tooie stops being a static recompilation problem.
The game reads its overlay directory straight off the cartridge, allocates a
buffer, decompresses an overlay into it and calls the code there -- and that
code is in no part of the image a reader can point at, at an address the
allocator picked. `[[unpacked]]` cannot answer this one: there are 886
overlays, they are freed, and their addresses are reused, so no fixed set of
sections describes them.

What answers it is doing the work when the address is known, which is the
moment the game jumps to it. The runtime already notices -- `get_function`
fails -- so the hook that said where a game was going now gets the chance to
say what is there instead, and the host translates it: the same recompiler the
pipeline runs offline, over the same instructions, read out of the console's
memory rather than out of a file. N64Recomp's live recompiler is already
vendored, because librecomp drives it for mods.

Three things had to be right, and each is the kind of thing that is only
obviously necessary afterwards.

**Where the function ends.** The rule is the analyser's, applied to memory:
walk forward remembering the furthest anything inside jumps to, and end at the
first `jr $ra` whose delay slot is at or past that, because a function with
several returns branches over the earlier ones. The rest of it is about not
believing data -- a word that does not decode, a branch above the address we
were told is the start, a jump outside the console's memory -- since running
whatever happens to be there is the one outcome worth avoiding.

**Which code a translation was made from.** An overlay is freed and its address
handed to another, so a translation is only good while the code behind it is
still there. Two words do not settle that: nearly every function on this
machine begins by making a stack frame and saving the return address, so a
wrong answer looks exactly like a right one. Keeping the whole body and
comparing it is a memcmp of a few hundred bytes against memory that is already
warm, and it is the difference between running the overlay the game asked for
and running the one that used to be there.

**And a way to see inside it.** Generated code carries no symbol, so a native
backtrace through it is a bare address -- which is the least useful thing to be
looking at when a game has just gone wrong inside code that was translated a
second ago. Each translation records where it landed, and the backtrace names
the frames that fall inside one by the address they were translated from, which
is the only name they have ever had. `N64B_OVERLAYS=1` says what was translated
and what each overlay call returned, and every run reports the count however it
ends, because a game that had code translated while it ran is a game whose
analysis did not have all of it.

### The key the boot chip holds

Past the overlays, the game loads its text -- and could not. The chain is five
calls deep and every one of them worked:

```
func_800D674C(asset)     not in the cache, so load it
  func_800D5B34          this asset's type is 10, which means scrambled
    stub 0x800888A8      an overlay call, which loads and runs
      func_80316A8C      allocate, DMA the block in, unscramble it,
                         read its first halfword as the size to allocate
```

The size came back as 0xFFFB5740, which no allocator will serve, so the load
returned zero and the caller -- which does not expect a null -- dereferenced
it. The size is garbage because the unscrambling is: `func_803168C8` exclusive-
ors the block with a fourteen-byte key, and the key comes from
`func_803167E0`, which builds a challenge out of the asset index, writes it
into the serial interface's buffer, and reads the answer back.

**That is the boot chip.** Bit one of the last byte of that sixty-four-byte
block is "ask the CIC", and on a 6105 -- which is the chip in this cartridge --
the chip answers a challenge with a response that nothing else can produce.
Banjo-Tooie uses the answer as the key to its own text. The runtime completed
the transfer and left the buffer alone, so the game exclusive-ored its text
with its own question and got noise.

So the runtime holds the PIF's sixty-four bytes now, moves them in and out on
`__osSiRawStartDma` the way the hardware does, and answers the challenge when
the game asks: thirty nibbles in, twenty-eight out, two small tables and a
running key. The algorithm is not published by anyone; it was recovered from
the hardware and is what every emulator implements, and it is written down in
`ultra_gaps.cpp` beside the rest of the serial interface.

A game that drives the interface only to read its controllers is unaffected:
its command block goes into the PIF's memory and comes back out unchanged,
which is the same thing it saw before.

### A watch that can see a halfword

Finding that took `n64b-port --watch`, and the watch could not see it. Every
access in recompiled code goes through one of the `MEM_` macros and the watch
redefined only `MEM_W`, so a game's halfwords and bytes -- a count, an id, a
flag, which is most of what a watch is wanted for -- were invisible, and the
tool reported nothing at all and looked like an answer. All five widths are
watched now, and the match is on the word an access falls in rather than the
exact address, so a byte inside a watched word is reported too.

### A queue that ate itself

The game drew its world and then, forty seconds in, stopped -- the game thread
stopped submitting frames while every other thread carried on, no error
anywhere, and the point it stopped at moved between runs. That reads like a
wait nothing wakes, and it was not one. Sampling every thread while it was
stopped found the game thread *spinning*, not blocked, inside
`ultramodern::thread_queue_insert`:

```
2397 Thread: Game 6
  func_800134C4  ->  osSendMesg  ->  do_send  ->  schedule_running_thread
                 ->  ultramodern::thread_queue_insert  (+96, +108, ...)
```

A thread is a node in the queue it waits on. It can be in one queue at a time
and in it once, and the walk that finds where to insert one stops at the first
thread of lower priority -- which, for a thread that is already in that queue,
is the thread itself. `toadd->next = toadd`. Every later walk of that queue
runs forever.

What queued a thread twice is Banjo-Tooie doing something ordinary. Its
controller thread is stopped and started around each serial transfer, and
`osStopThread` on a thread other than the caller was, in ultramodern,
`assert(false)` -- which in a release build is nothing at all. So the stop left
the thread on the message queue it was blocked on, and the start put it on the
running queue as well, and the cycle closed the next time a message arrived for
that queue. That is the forty seconds: the game had to reach an SI transfer
first, and when it reached one varied.

Both halves are libultra's own state machine now, and both matter:

- **`osStopThread`** takes a thread out of whichever queue it is in and leaves
  `queue` naming that queue, exactly as `__osDequeueThread` does. Its host
  thread is already parked on its own semaphore and only a pop from a queue
  ever signals that, so a thread in no queue is a thread that does not run.
- **`osStartThread`** does nothing to a thread that is not stopped -- libultra's
  is a switch on the state and a running thread falls out of it -- and a
  stopped one goes back where it was: onto the message queue `queue` names if
  it was waiting on one, since the message it was waiting for still has not
  arrived, and onto the running queue otherwise.

Underneath both was a third thing. `thread_queue_remove` never advanced its
cursor -- it recomputed the head of the queue on every iteration and compared
that -- so it could remove a thread that was first and nothing else, and on a
queue of two or more where the thread was not first it looped forever. Nothing
had called it with anything but a head until `osStopThread` did.

`thread_queue_insert` now refuses a thread already in the queue and says so
once. It is a cheap check -- these queues are one per message queue plus the
running one, none longer than the game's nine threads -- and the alternative is
a hang with no error in a function nowhere near whatever caused it.

### Trigonometry that was stubbed for being two functions

With the scheduler fixed the game ran indefinitely and its world still did not
move. What was wrong is visible in the trace: the third, ninth and tenth
busiest functions in the whole game were stubs.

```
      743150  func_800136D0
      371575  func_800138BC
      371889  func_8001395C
```

1,486,614 calls in a hundred and ten seconds, every one of them returning
whatever happened to be in `$f0`. They are the game's sine and cosine.

They were stubbed for a reason that was almost right. Hand-written assembly
reaches one body from several entry points, and each entry point is a function:
something calls it by name, and a `jal` names the first instruction of one. But
the entries sit one after another in front of the body they share --

```
800136D0: lui at, 0x8004        <- one entry: sine of an angle in units
800136D4: lwc1 f0, 0x16B0(at)
800136D8: lui at, 0x8004
800136DC: b    0x800136F8
800136E0: lwc1 f2, 0x16B4(at)
800136E4: lui at, 0x8004        <- another: sine of an angle in degrees
...
800136F8: mul.s f0, f0, f12     <- the body both of them run
...
80013720: jr ra
```

-- so putting a boundary at the second one, which a call from the segment the
game unpacks second proves belongs there, takes the body away from the first,
and what is left is five instructions that branch forward into somebody else's
function. The analyser stubbed those, counted them, and said so.

The right reading is two functions that overlap, each carrying its own copy of
the tail, which is exactly what the recompiler makes of two entries with their
own instruction lists. So a function whose branches escape is now walked again
from its own first instruction, with nothing but its own branches deciding
where it ends -- the same walk that recovered it, which already refuses to run
past a terminator something branches over. If that lands past the boundary, the
boundary was a second entry point rather than the end of anything. Three
functions in Banjo-Tooie get their bodies back; Super Mario 64 and Mario
Builder 64 have none, which is the answer for a game whose libultra is named.

A region that genuinely has no division into functions -- libultra's exception
preamble, where branches cross every entry in both directions -- is untouched,
because a walk from inside one gives up at the first branch above its start.
Two of those are left in Banjo-Tooie and they are correctly stubbed.

### Sound, and the image an offset is into

Banjo-Tooie's audio microcode is at 0x80037880 with its data at 0x80042010 --
addresses the runtime prints out of the first task it cannot run. Neither is in
the cartridge: they arrive in memory with the core segment the loader unpacks,
so there is no rom offset to write down. A `[[microcode]]` block may now carry
`vram` and `data_vram` instead, and the analyser finds the offset from the
section the address falls in, which for a compressed cartridge is one of the
spliced `[[unpacked]]` blocks. That is the same rule the rest of the record
follows: addresses in console memory, never offsets into a file.

The size the task carries is 0x1000 and the text cannot be that long. A
libultra task's text lands 0x80 bytes into four kilobytes of instruction
memory, so the most there can be is 0xF80 -- and 0xF80 is also what the block
says about itself, because the command table in its data blob reaches 0x1FB0,
which is 0xF30 past the text address.

With that written down the microcode still did not run: it stopped early every
frame, and the generated C was nine hundred and ninety-two `nop`s. n64b-port
was handing RSPRecomp the player's cartridge and an offset measured in the
analyser's own image -- the same file for every game until now, and for a
cartridge that unpacks itself an offset thirty-eight kilobytes past the end of
the ROM, which reads as zeros. It hands over the image the analyser's own
configuration names.

**Banjo-Tooie has its music.** Peaks of fifteen to twenty-two thousand out of
thirty-two thousand, changing as the game moves through its opening.

### The two words the boot ROM leaves

Everything above got Banjo-Tooie to a world it drew and would not fill. The
terrain was there, the water moved, the music played and looped, the pad was
polled, and no actor was ever registered in any of the seven groups the game
keeps them in. Twelve minutes and fifteen thousand display lists later the
frame was the same one.

What found it was a second console. `mupen64plus` is an interpreter with a
breakpoint API, and a hundred and fifty lines of frontend turn it into
something this project did not have: a reference. Both programs are kept, in
`ModernReality/tools/refconsole`. No video, no audio, no input
-- the core runs the cartridge with its own stub plugins, and the frontend
reads RDRAM out whenever it likes, breaks on an address, watches a word for
writes, and keeps a ring of the last few thousand overlay calls. Two runs of
the same game, one here and one there, and the question stops being "why does
this not work" and becomes "where do these two stop agreeing", which is a
question with an answer in it.

They agree for a long way. Both load the same eighty-one overlays in the same
order with the same arguments. Then the console loads twelve more and this
runtime loads none, and the twelve are the level's actors.

Walking back from there:

- The overlay that populates the level's object grid calls `func_800BDCB8`,
  and calls it only when `func_8001E204()` returns zero.
- `func_8001E204` is three instructions. It returns the byte at 0x8007DB79.
- One store writes that byte, at 0x8001DED4, and the eight instructions above
  it read two words out of memory -- `0xA02FB1F4` and `0xA02FE1C0`, uncached --
  exclusive-or them with `0xAD090010` and `0xAD170014`, and or the results
  together. Zero means the words were what they should be.
- On the console they are. Here they were zero, so the byte was 0x14, and from
  then on the game refused to put a single object into the world.

`0xAD090010` is `sw $t1, 0x10($t0)` and `0xAD170014` is `sw $s7, 0x14($t0)`.
They are instructions, and the memory they are in is not the game's. Breaking
on the game's first instruction and dumping all eight megabytes says where they
came from: outside the megabyte IPL3 copies there are two hundred and twenty
five non-zero words, and they are IPL3's own.

The CIC-6105 boot chip's IPL3 -- Rare's, and only Rare's -- does three things
this runtime did not. It leaves a word of ones at the bottom of each
two-megabyte bank, from sizing memory. It copies bytes 0x554 to 0x888 of the
cartridge to 0x80000004, under the address every game loads at. And it takes a
0xC0-byte routine out of that copy and spreads it through the third megabyte
eight bytes at a time, one piece every 4080 bytes, starting at 0x802FB1F0.
Twenty-four pieces. The two words the game reads are the second word of the
first piece and the first word of the fourth.

Modelled that way -- the banks, the copy, twenty-four pieces read back out of
the copy -- the runtime's memory matches the console's exactly: zero words
differ across the whole eight megabytes at the moment the game starts. It is
in librecomp's boot, in the boot ROM's own order, because on the console the
megabyte lands on top of the copy and the libultra variables land on top of
that. Everything written comes out of the player's own cartridge; what is
hard-coded is where the boot ROM puts it, which is a fact about the console.

And it still did not work, because of the other half.

### Uncached memory is the same memory

The game reads those words through `0xA02FB1F4`, not `0x802FB1F4`. KSEG1 is
not a second eight megabytes; it is the same eight megabytes read past the
cache, and every game that hands a structure to the RCP writes it through one
window and reads it back through the other.

librecomp maps KSEG0 and nothing else, so this project already had to do
something about the rest: the register window at `0xA4000000` was backed with
its own zeroed pages, so that a translated instruction storing to a hardware
register would land somewhere rather than take the process down. That backing
started at `0xA0000000`, and the bottom eight megabytes of it are not
registers. They are RDRAM, and they were a second, empty copy of it.

Which is the one kind of wrong that nothing reports. A write goes somewhere, a
read comes back, the value is zero, and the game believes whatever zero means.
Here it meant a copier was running.

One `mach_vm_remap` makes the two windows the same pages. The eight megabytes
the console has are aliased at `0xA0000000`; everything above them stays the
zeroed pages the registers want.

### A place the runtime can take the thread away

With a world to be in, the game got as far as the end of its opening cutscene
and stopped: every byte of the console's memory identical thirty seconds
apart, the renderer still drawing the last frame it was given.

Sampling the process says who is doing what. One game thread is in
`func_800C2AB8`, which walks the game's sixty sound emitters, stops each one,
and goes round again until none is still playing. Every other thread is
blocked in `osRecvMesg`. And that is the whole of it: only one recompiled
thread runs at a time and the runtime chooses which, but it only gets to
choose when the running thread calls into it. A message from the video
interface or the audio interface is queued by a runtime thread and delivered
by the next game thread that asks the runtime for anything. A loop that only
reads memory asks for nothing -- so the retrace never arrives, the audio
thread never runs, the sound never finishes, and the loop goes round forever
waiting for it.

The recompiler already knew about the smallest form of this: a loop it can
prove reads memory and does nothing else gets a `spin_wait` at the bottom.
This loop calls functions, so nothing can be proved about it. What can be
said instead is that a function entry is always a safe place to hand the
thread over -- so every recompiled function begins with a check of one word,
and when the runtime has set it, the thread delivers whatever the hardware
has been holding and lets the scheduler pick again. That is what an interrupt
does, in the order an interrupt does it. When there is nothing waiting the
cost is a load and a branch that is never taken.

Two things stop it, and both are cases where the console would not have
interrupted either: a thread that has turned interrupts off in its status
register, and the game's own exception handler, which the host marks while it
is rewriting an overlay stub and assembling a thunk.

### A second entry point, and a return through a register

That got the game past the cutscene and into a jump to address zero.

`func_800137C4` is a floating point helper -- multiply by a constant, hand the
answer back -- and 0x800137D4 is the same helper with a different constant.
Both end `move $a1, $ra`, a call, `jr $a1`: a return through a saved return
address. Nothing in the image calls the second one, so the walk never found it
and the sweep ran it into the first; the game reaches it through a pointer,
the runtime translates it where it lands, and what it translates begins after
the `move`. The register the `jr` returns through was never loaded.

One line in the title record writes the boundary down, and the whole function
goes into the module with its own first instruction. After that the game runs
its attract mode all the way round -- Spiral Mountain, then five more worlds,
each with its own camera move -- and comes back to the title screen without
stopping.

### Where Banjo-Tooie is now

It runs, and it cannot be looked at: see "The world through the wrong lens"
below for the projection that spoils every frame of what follows.

```
boot -> unpack -> the intro cutscene -> the title screen -> the file select
```

The opening runs: the camera sweeps over Spiral Mountain at night, the river
and the banks and the bridge draw, the text cards come up -- "TWO YEARS HAVE
PASSED SINCE GRUNTILDA THE WITCH WAS DEFEATED BY BANJO AND KAZOOIE" -- Klungo
speaks with his portrait and his subtitle, the transition irises out, and the
game arrives at Banjo's house with three save slots on the wall and "PRESS (A)
TO PLAY THE GAME. GAME 1: EMPTY" across the bottom. Start skips the cutscene
from the first frame it is pressed. Left alone at the title the game loops back
into the attract mode, which is what it is supposed to do.

The measurements behind that, against the same cartridge on the reference
console:

- the tamper byte at 0x8007DB79 is 0, as it is there;
- the actor groups at 0x80132E80 fill, and group 3 lands at 0x802179D0 --
  the same address the console puts it at;
- twenty-seven to thirty-one overlays stay resident through the cutscene
  where before it fell to nine and stopped;
- the camera object moves every frame, from (529, 6944, -1904) down through
  (-1507, 1436, -1442) over the opening's first half-minute.

None of the things this took are Banjo-Tooie's. A cartridge with the same boot
chip gets the same memory, and any game at all that reads back what it wrote
through KSEG1 now reads what it wrote.

These were checked along the way and were not it, recorded so that the next
game's version of this does not start here:

- **Nothing is driving hardware that is not there.** Every function in the
  image that builds an RCP register address was listed against how often it
  ran, and the only two that run are reading the cartridge through its mapped
  window at 0xB0000000, which the runtime provides. The rest never run.
- **No libultra driver is missing.** The analyser now writes out which
  signature matches it had to leave nameless -- the seventy-five in the info
  file under `named_without_implementations` -- and they are the C library, the
  audio library, the matrix helpers, and the internals of managers the runtime
  owns outright. None of them is a driver the game reaches.
- **The floating point mode is right.** The FR bit decides whether an odd
  single-precision register is its own or the top half of the one below, and
  getting it wrong would make every double in the game wrong while leaving the
  simple things working; librecomp models it and the game sets it at boot.
- **Timers deliver.** `osSetTimer` is the runtime's, its argument reading
  matches the o32 layout for a 64-bit `OSTime`, and the timer thread posts.
- **The live recompiler is not cutting functions short.** The overlay
  translations that come out shortest are two instructions long because that
  is what they are -- `jr $ra` over `addiu $v0, $zero, 395` -- and the
  instructions the runtime translated match what a disassembly of the same
  address says.
- **Twelve minutes changes nothing.** The music loops on a forty-eight second
  cycle and the fifteen-thousandth display list is the same picture as the
  six-hundredth. There is no slow path being waited out.

### The world through the wrong lens

Banjo-Tooie boots, plays its opening, reaches its title screen and runs its
attract mode -- and everything in the world is smeared. The terrain comes out
as long radial ribbons converging on the middle of the screen, the camera
looks like it is inside the ground, and the title screen shows a river where
the console shows a rock face with a Jinjo standing in a cave.

It is one number, and it is the game's own.

The display list loads a matrix at `0x8017AFB0` every frame, and decoding it on
both consoles at the same moment says this:

```
              ours                                console
    0.0625   0.0003   0.3845   0.3841     0.6602   0.0029   0.3843   0.3839
    0.0000   0.1725  -0.0011  -0.0011     0.0000   1.8210  -0.0010  -0.0010
    0.0750  -0.0002  -0.3205  -0.3201     0.7910  -0.0024  -0.3207  -0.3204
  175.8986 -588.2182 135.0732 139.9237  1856.3077 -6221.4277 132.8866 137.7395
```

The third and fourth columns agree to four figures. The first and second are
smaller here by 10.56, both of them, exactly. Those two columns carry the
projection's horizontal and vertical scale and nothing else, so what that
factor is, is the field of view: `cot(fovy/2)` is 1.821 on the console, which
is a 58 degree lens, and 0.1725 here, which is a 160 degree one. A fisheye
that wide is the whole of what a player sees.

The renderer is drawing what it is given. RT64 identifies the microcode
correctly -- `F3DEX2.NoN.fifo 2.08`, by its own hash of the text and data the
task names -- and the matrix above is the game's, sitting in RDRAM, before
anything of ours touches it.

Where the factor comes from is one function that never runs. The game keeps a
small stack of float matrices at `0x8007B4F0`; the console fills three of them
and hands the third to `guMtxF2L`, and that third one is the camera:

```
    0.1158   0.0410  -0.5873   0.0000
   -0.0210   0.5985   0.0376   0.0000
    0.5883   0.0133   0.1169   0.0000
  963.1885 6911.2490 -2756.3979  1.0000
```

Here the second and third entries are never written at all, and the game hands
over the first, which is a fixed 0.28 scale and a translation of ten. The
camera transform is not applied, and the projection that comes out of the
multiplication is the one above.

The entry is written by `func_80019AA0`, called from `func_800DCF48`, and
`func_800DCF48` is reachable from exactly one place in the whole image: entry
2 of a table of twenty-four handlers at `0x8012306C`. `func_800DE2A4` is the
interpreter that dispatches through it -- read an opcode, index the table,
call, advance by the size in the next word -- and it runs 5,182 times in
twenty seconds here without ever seeing opcode 2. On the console it sees it
constantly, and recurses into itself through it.

Four of those twenty-four handlers ever run here -- opcodes 3, 12, 13 and 17,
and two more reached by index past the twenty-fourth -- where the console runs
many more. The one that would write the camera is opcode 2, and `func_800DCF48`
is referenced by exactly one word in the whole image: the table entry itself.

Where it stops is one step further in, and it is a strange place to stop. Take
the stream at `0x803212F8`, which both consoles walk with the same registers
and which holds the same bytes on both:

```
   +0x00  opcode  3  size 0x10
   +0x10  opcode  3  size 0x10
   +0x20  opcode  3  size 0x10
   +0x30  opcode 10  size 0x18
   +0x48  opcode  3  size 0x10
   +0x58  opcode  3  size 0
```

Opcode 3's handler is called with each of `0x803212F8`, `+0x10`, `+0x20`,
`+0x48` and `+0x58`, the same number of times each, and the interpreter is
entered at `0x803212F8` that many times and at `+0x48` never -- so it is one
walk, and it passes over `+0x30`. Table entry 10 is `0x800DD410` in memory at
that moment. And the runtime is never
once asked to look up `0x800DD410`: watching `get_function` over the whole
`0x800DC000`-`0x800DE500` range for twenty seconds lists nine addresses and
that is not one of them. The interpreter's translation is not the culprit
either -- the branch-likely at the bottom of its loop is generated correctly,
delay slot and all.

The stream is not being rewritten under the walk, either: those bytes are the
same at two frames six hundred apart, and the same on the console at two of
its own.

The answer to that turned out to be that there is more than one table. Logging
what the runtime is asked to look up, in order, alongside the handler's own
arguments, shows the command at `+0x30` dispatching to `func_800DD504` -- and
`func_800DD504` is entry 10 of a *different* table at `0x80123134`. There are
three of them, listed at `0x80123198`, and `*(0x8012CF8C)` says which is
current:

```
   table 0 at 0x8012306C   entry 2 = func_800DCF48, which sets the camera
   table 1 at 0x801230D0
   table 2 at 0x80123134   entry 2 = func_800DC628, which does nothing
```

They are three passes over the same scene graph. The console walks the node
that carries the camera in pass 0; this runtime reaches it only in pass 2,
where that opcode is a no-op.

Following that up rather than down, the chain is short and every link is
measured:

- `func_800DE498` chooses between `func_800AE160` and `func_800ADCD0` on
  whether `*(0x8012C824)` is set. `func_800AE160` runs 640 times in twenty
  seconds on the console and never here.
- `*(0x8012C824)` is written by one three-instruction function,
  `func_800DF41C`, which the console calls with a node pointer and this
  runtime never calls at all.
- Its only caller that runs is `func_801015D0`, at `0x80101630`, and the call
  is guarded: `func_80104248` is asked for the object's model and the camera
  is skipped when it answers zero.
- `func_80104248` reads a halfword at the object's `+0x8C` and looks it up in
  a container at `*(0x80136E70)`. The container is there and the right shape
  on both. The identifier is not: the three objects this runtime hands it --
  `0x801CBD4C`, `0x801CC278`, `0x801CBDE8` -- all carry zero there, and the
  console hands it a different object entirely.

So the frontier is one question, and it is a much narrower one than the
picture it came from: **the objects this runtime's camera pass iterates carry
no model identifier, where the console's carry one.** That is the same shape
as the empty world of a hundred commits ago -- an object that is there and is
not furnished -- and it is where the next session starts. What has been ruled out on the way there, so that the next look
does not start here:

- **The table is right.** Every handler address at `0x8012306C` is identical
  on both consoles, `0x800DCF48` is entry 2 in both, and the pointer to the
  table at `0x8012CF8C` is the same too.
- **The module has the handler.** `func_800DCF48` and `func_800DD410` are both
  recovered boundaries, both are compiled into the module with their own
  symbols, and the runtime's address map holds the right pointer for each.
  Neither is one of the 890 functions the live recompiler translates, and the
  live recompiler touches nothing inside the main segment at all.
- **The command data is right.** The streams the interpreter walks hold the
  same bytes at the same addresses on both consoles.
- **The projection helper is right.** `guFrustum` gets the same arguments
  here as there -- l=-608, r=608, b=-456, and the same output address -- and
  writes the same matrix.
- **The viewport is right.** Scale (152, 86, 127.8) and centre (152, 114,
  127.8) on both.
- **The display list agrees for a long way.** The two consoles' lists are
  identical, operand for operand, for the first 507 commands -- the same
  framebuffer, the same scissor, the same sub-list addresses -- and part when
  they branch into the per-frame object arena.
- **The frame rate is not the problem.** Both consoles run this part of the
  game at about twenty frames a second: `osViSwapBuffer` is called 327 times
  in twenty seconds there, and the display list goes out every three vertical
  interrupts here. The gap between them is a boot that takes ten seconds
  longer, not a game that runs slow.
- **Nor is the plumbing.** Retrace messages are delivered at sixty a second
  with none dropped and a mean delay of fifteen microseconds; the game's own
  code runs for twenty-five milliseconds in every second and waits for the
  rest; thread handoffs cost thirteen milliseconds a second across nine
  hundred of them.

The tools that answered this are in `ModernReality/tools/refconsole`, and two
of them are new: `refshot`, which screenshots the reference console at a named
*frame* rather than a named second and writes its memory out beside the
picture, and `n64dl.py`, which turns one of those images back into the display
list the game asked for. `n64b-run` grew `N64B_SCREENSHOT_RAM` to write the
same thing at the same frames, which is what makes the two comparable at all.

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
3. **The camera node Banjo-Tooie never walks.** Everything the game draws
   goes through a projection ten and a half times too wide, because the one
   handler that writes the camera's matrix is never dispatched. "The world
   through the wrong lens" above has the measurement, what it is not, and the
   one question left: why the scene graph this runtime walks has no node of
   that kind in it. Until that is answered the game is playable and unlookable
   at, which is the same thing as not playing.
4. **Playing Banjo-Tooie further than its opening.** It boots, plays its
   intro, reaches the file select, starts a game and plays the scene in
   Banjo's house, and runs its attract mode round six worlds without
   stopping. What nobody has done is play it for an hour: a game this size
   has more of the runtime to reach than four minutes of it can, and the
   three things the opening turned up -- the boot ROM's leftovers, uncached
   memory, a thread that could not be interrupted -- were each invisible
   until something asked for them. One that an hour has already turned up is
   a crash: about a hundred seconds in, ultramodern's timer thread takes a
   bad address, reading an `OSTimer` field as a host offset rather than a
   console one.
5. **Measuring an unpacked segment rather than being told it.** The two numbers
   a `[[unpacked]]` block carries were both found mechanically — the entry is
   what the runtime reported it could not find, and the extent is every word
   that differs from what IPL3 copied. Both could be done by the analyser
   instead of by hand, and then a compressed cartridge would need no record at
   all beyond the one driver name.
6. **More titles.** Three is not a sample. Everything the analyser knows how to
   do it learned from Super Mario 64, Mario Builder 64 and Banjo-Tooie, and the
   next ROM will teach it something else.

## Not in scope yet

- **iPhone.** DolBundler's phone path exists because iOS refuses to map an
  unsigned executable page, so every game is linked into the app before it is
  signed. The same is true here and the same solution would work. The reason it
  was out of scope — that no game ran on the Mac yet — has gone, but the TLB
  now uses `mach_vm_remap`, which is a thing to check before assuming the port
  is only signing.
- **Mods.** librecomp has a whole mod system and N64Recomp has a live
  recompiler behind it. Out of scope until the static path is solid.
