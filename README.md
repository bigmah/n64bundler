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

**Two games play.** Super Mario 64 recompiles from a bare cartridge dump and
runs: its title screen, its file select, Peach's letter, the castle grounds,
Mario under the control of a pad, its music and its sound effects, and a save
file that survives quitting. Mario Builder 64, the second cartridge, recompiles
at the same coverage and reaches its startup screen. Banjo-Tooie, the third,
holds its game compressed and has to be run before it can be read at all — it
unpacks itself, hands over 9,732 functions with every internal call landing on
a boundary, and then **plays**: the opening cutscene over Spiral Mountain with
its text cards and its talking, the title screen, the file select in Banjo's
house, and a new game from there into the scene indoors with the characters
and the dialogue and the HUD. Left alone it runs its attract mode round six
worlds and comes back. Everything around all three works: a ROM is analysed,
recompiled, compiled, added to the library and launched into a window with the
renderer up. [PLAN.md](PLAN.md) has the design, the measured numbers, and what
is still missing.

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

## The cartridge that has to be run before it can be read

A third title, Banjo-Tooie, is neither of those shapes. It is a sixteen-
kilobyte loader followed by thirty-two megabytes of compressed data, and there
is no second segment to write down, because until the loader has run the code
does not exist anywhere — not in the cartridge, not at any offset, not in any
form a reader can point at. Dropped in as it stood it recovered 83 functions
out of a 32MB image and crashed a quarter of a second after Play.

So the game is made to unpack itself, and then read:

- one name in its record — `osPiRawStartDma`, which the loader calls directly
  and which librecomp carried as a stub that ends the process — is enough to
  make its cartridge reads real, and the loader then unpacks the game and jumps
  into it;
- `n64b-run --unpack` stops at the first address the runtime cannot find,
  writes the console's memory out, and says where the game was going;
- `n64rip --unpacked` takes the segments a record names out of that image and
  splices them onto the end of the ROM the recompiler reads, where the sweep
  and the signature database treat them like anything else in the cartridge;
- and the pipeline loops, because a game unpacks in stages and each stage only
  appears once the one before it is running.

Banjo-Tooie takes two rounds — the loader reveals the core, the core reveals
the main segment — and comes out at **9,732 functions with 13,802 of 13,802
internal calls landing on a recovered boundary**.

The memory image is a derived work of the player's own dump, exactly as the
library's copy of the ROM is, and it lives beside it. No part of it is ever
written into a title record.

## A game that calls itself through the exception handler

Getting Banjo-Tooie past the black window took three faults that had nothing to
do with the analysis and one that was a shape nobody had met.

**It thought the reset button was held down.** The status register the runtime
reports was zero, and a game reads that register to find out what the console
is doing rather than only to change it: Banjo-Tooie's retrace thread checks
SR_IBIT5, the pre-NMI bit, on every frame. libultra masks that bit off when
RESET is pressed, so a game that sees it clear knows it is being reset — and
Banjo-Tooie's answer is to stop the rumble motors, put the video interface back
to a plain mode, and spin until the NMI arrives. That fired on the first
retrace, which is exactly a game that configures its video and then draws
nothing forever. The runtime now reports the interrupt mask a running console
has.

**Its trigonometry returned to nowhere.** Four functions keep the caller's
return address in another register so they can call a shared body and still
come back: `or $a1, $ra, $zero; jal ...; jr $a1`. A `jr` through anything but
`$ra` was an indirect tail call, which looks the register up in the
address-to-function map — and nothing writes `$ra` in recompiled code, so the
lookup failed on zero. The recompiler now reads a `jr` through a register that
can only hold this function's return address as what it is: a return.

**It reads its cartridge with a load.** The parallel interface is not only a
DMA engine — the cartridge is mapped, and a load from `0xB0000000` plus an
offset reads a word of it. libultra never does that, so no decompilation-based
project needs it; Banjo-Tooie's loader does, one word at a time with interrupts
off, because a four-byte DMA is not worth the queue. The runtime now puts the
cartridge where the console has it.

**And every call between its overlays goes through the CPU's syscall
exception.** Thirty-two megabytes do not fit in eight, so Banjo-Tooie holds its
code in 886 overlays and links them with a table of 4,234 two-instruction
stubs: `syscall <n>` and a word saying which entry point. The exception goes to
the game's own handler, which loads the overlay, assembles a thunk, rewrites
the stub into a jump to it and returns to the stub to run it.

Nothing in the image can be followed into any of that — the stubs are reached
only through pointer tables in the game's own data, and the handler only
through the exception vector — so a record names the table and the handler, and
the analyser checks the range by shape before it uses it: every eight bytes of
a stub table begins with a `syscall`, and nothing a compiler emits ever does.
The 3,691 stubs that were not functions before are functions now. The runtime
stands in for the exception, and reads the rewritten stub and the thunk rather
than executing them, because a static recompilation cannot run code a game
wrote after it was compiled.

**And past that, its code is in no cartridge at all.** The game reads its
overlay directory straight off the cartridge, allocates a buffer, decompresses
an overlay into it and calls the code there. `[[unpacked]]` cannot answer that
one: there are 886 overlays, they are freed, and their addresses are reused, so
no fixed set of sections describes them.

So the runtime translates a function the first time the game jumps to it, with
the recompiler that is already vendored — reading the instructions out of the
console's memory rather than out of a file, recovering where the function ends
the way the analyser does on a cartridge, and keeping the body it was made from
so that an overlay which has been freed and replaced is never run from the old
translation. Generated code carries no symbol, so a backtrace through it names
each frame by the address it was translated from.

**And its text is locked to the boot chip.** Banjo-Tooie stores its text
scrambled and exclusive-ors it with a fourteen-byte key — and the key comes
from the cartridge's CIC. Bit one of the last byte of the serial interface's
sixty-four-byte block is "ask the boot chip", and a CIC-6105 answers a
challenge with a response nothing else can produce. The runtime used to
complete that transfer and leave the block alone, so the game unscrambled its
text with its own question, read the noise's first halfword as a size to
allocate, and failed. The PIF's memory round-trips now, and the challenge is
answered.

**A queue that ate itself.** Past all that it drew its world and stopped forty
seconds later — the game thread stopped submitting frames, every other thread
carried on, and nothing reported an error. Sampled while it was stopped, the
game thread was not waiting: it was spinning inside the runtime's own thread
queue. A thread is a node in the queue it waits on, so a thread queued twice
points at itself and the next walk of that queue never ends. Banjo-Tooie stops
and starts its controller thread around each serial transfer, and stopping a
thread other than the caller was an assertion the release build compiled away:
the stop left it on the message queue it was blocked on and the start added it
to the running queue as well. Both are libultra's own state machine now — stop
takes a thread out of whichever queue it is in and leaves the name of it behind,
start puts a stopped thread back where it was and does nothing to one that is
already running — and underneath them, the runtime's "take a thread out of a
queue" walked the head of the list over and over instead of walking the list.

**And its trigonometry was stubbed for being two functions.** The three busiest
functions in the whole game were doing nothing: 1,486,614 calls in a hundred and
ten seconds, each returning whatever was in the return register. They are its
sine and cosine, and they are hand-written assembly that reaches one body from
several entry points laid out in front of it. A call from another segment proves
the second entry point is a function, and cutting there takes the body away from
the first — leaving five instructions that branch forward into somebody else's
function, which the analyser stubs because it cannot be translated. The right
reading is two functions that overlap, each with its own copy of the tail, so a
function whose branches escape is walked again from its own first instruction
and keeps whatever that walk says is its body. A region that genuinely does not
divide into functions is untouched, because a walk from inside one gives up at
the first branch above its start.

**It has its music.** Banjo-Tooie's audio microcode is not in the cartridge —
it arrives in memory with the segment the loader unpacks — so a `[[microcode]]`
block may now name a block by console address and let the analyser find the
offset in the section that address falls in. That was half of it. The other half
was that the recompiler was being handed the player's cartridge and an offset
measured in the analyser's own image, which for a compressed cartridge is
thirty-eight kilobytes past the end of the ROM: nine hundred and ninety-two
`nop`s and a microcode that stopped every frame.

**And then it drew a world and would not put anything in it.** Seven actor
groups, all null, for twelve minutes. What found the reason was a second
console: `mupen64plus` with its debugger built in, plus a hundred and fifty
lines of headless frontend, is a thing to disagree with — and two runs of the
same cartridge turn "why does this not work" into "where do these two stop
agreeing", which is a question with an answer in it.

They stop agreeing three times.

**The boot ROM leaves things behind, and games look.** The CIC-6105 IPL3 —
Rare's, and only Rare's — copies part of itself to the bottom of memory and
spreads a routine out of that copy through the third megabyte. Banjo-Tooie
reads two of those words, compares them against the instructions they should
be, and when they differ sets a byte that from then on makes it refuse to put
a single object into the level. Nothing says so; the world is simply empty
forever. librecomp now leaves what the boot ROM leaves, out of the player's
own cartridge, and the runtime's memory matches a console's word for word at
the moment the game starts.

**Uncached memory is the same memory.** It reads those words through
`0xA02FB1F4`, and KSEG1 is not a second eight megabytes — it is the same eight
megabytes read past the cache. The register window this host maps so that a
store to a hardware register lands somewhere was covering the bottom of that
window too, with zeroed pages of its own, so a game that wrote through one
window and read back through the other read zero and believed it. One
`mach_vm_remap` makes the two windows the same pages.

**A thread has to be interruptible.** Between the cutscene and the title
screen the game walks its sixty sound emitters and goes round again until none
is playing. That loop asks the runtime for nothing, and the runtime can only
schedule when it is asked — so the retrace never arrived, the audio thread
never ran, the sound never finished, and every other thread sat blocked on a
message in a queue nobody would drain. Every recompiled function now begins
with a check of one word, and when the runtime sets it the thread delivers
what the hardware has been holding and lets the scheduler pick again. That is
what an interrupt does, and it is the first thing here that every game needs
rather than one.

**So: two games that play is not every game, and some ROMs will not boot at
all.** Everything the analyser and the runtime know they learned from three
cartridges, and the next one will teach them something else.

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
