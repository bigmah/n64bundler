// SPDX-License-Identifier: GPL-3.0-or-later
//
// That a second view of the console's memory is the same memory.
//
// This is worth a test of its own rather than being left to a game, because
// the way it fails is silent. A view that is not the same pages is a view of
// zeroes that accepts writes and forgets them: the game runs, draws, plays its
// music, and is subtly or completely wrong -- Banjo-Tooie reads two words the
// boot ROM left in memory, gets zero because it asked uncached, decides a
// copier is running, and refuses to put a single object into the world. Nothing
// reports that. So the property is checked directly.
//
// It is also the one piece of this that had to be rewritten to leave macOS, so
// it is the piece most worth being able to run first on a machine that is not
// the one it was written on.

#include "modernreality/console_memory.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// Two views of one page are the same memory only to the kernel. To the compiler
// they are two addresses in one mapping a known distance apart, which cannot
// overlap, so it may move a read through one ahead of a write through the other
// -- and then the check reads what was there before the write, and fails a
// property that holds. Clang and GCC both do at -O2. So every access that has to
// see through a view is volatile: stored and loaded where it is written, in the
// order it is written.
void put(uint8_t *at, const char (&word)[5]) {
    volatile uint8_t *bytes = at;
    for (int i = 0; i < 4; i++) {
        bytes[i] = uint8_t(word[i]);
    }
}

bool holds(const uint8_t *at, const char (&word)[5]) {
    const volatile uint8_t *bytes = at;
    for (int i = 0; i < 4; i++) {
        if (bytes[i] != uint8_t(word[i])) {
            return false;
        }
    }
    return true;
}

constexpr size_t kRdram = 8u * 1024u * 1024u;
/// Where the register window starts, the same distance librecomp's flat memory
/// puts between KSEG0 and KSEG1.
constexpr size_t kUncached = 0x20000000u;

} // namespace

int main() {
    // Stand in for what librecomp does: reserve a large range, commit the
    // console's memory at the bottom of it, and leave the rest to be mapped
    // over later.
    const size_t reserved = kUncached + kRdram;
    auto *base = static_cast<uint8_t *>(
        ::mmap(nullptr, reserved, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0));
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "could not reserve %zu bytes\n", reserved);
        return 1;
    }
    if (::mprotect(base, kRdram, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "could not commit the console's memory\n");
        return 1;
    }

    // Something already in it, because the real one has the first megabyte of
    // the cartridge in it by the time the backing object arrives and losing
    // that would start a game at an entrypoint of zeroes.
    for (size_t i = 0; i < kRdram; i += 4096) {
        put(base + i, "keep");
    }

    std::string error;
    check(n64b::back_console_memory(base, kRdram, -1, 0, error),
          ("the console's memory takes a backing object" + (error.empty() ? "" : ": " + error))
              .c_str());
    check(n64b::console_memory_backed(), "and says so");

    bool kept = true;
    for (size_t i = 0; i < kRdram && kept; i += 4096) {
        kept = holds(base + i, "keep");
    }
    check(kept, "what was in it is still in it");

    // The register window: KSEG1 is the same eight megabytes read past the
    // cache, not a second eight megabytes.
    if (::mprotect(base + kUncached, kRdram, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "could not commit the register window\n");
        return 1;
    }
    check(n64b::alias_console_memory(base + kUncached, 0, kRdram),
          "the uncached window becomes a second view");

    put(base + 0x1000, "cach");
    check(holds(base + kUncached + 0x1000, "cach"),
          "a write through the cached window is read through the uncached one");
    put(base + kUncached + 0x2000, "uncd");
    check(holds(base + 0x2000, "uncd"), "and the other way round");

    // A TLB entry: a page of the console's memory showing up somewhere else.
    // The real one lands in KUSEG; here it only has to be a different address.
    const size_t page = size_t(::getpagesize());
    const size_t physical = 4 * page;
    uint8_t *elsewhere = base + kUncached + kRdram - page;
    if (::mprotect(elsewhere, page, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "could not commit the TLB page\n");
        return 1;
    }
    check(n64b::alias_console_memory(elsewhere, physical, page), "a TLB page is aliased");
    put(base + physical, "tlb!");
    check(holds(elsewhere, "tlb!"), "and is the page it was mapped from");

    put(elsewhere + 64, "back");
    check(holds(base + physical + 64, "back"), "writes to it land in the console");

    n64b::unalias_console_memory(elsewhere, page);
    check(holds(base + physical, "tlb!"), "putting it back leaves the console's own memory alone");

    // A view that ran off the end of the object would read as zero and write
    // nowhere, which is the failure this is all here to avoid.
    check(!n64b::alias_console_memory(elsewhere, kRdram, page),
          "a view starting past the end is refused");
    check(!n64b::alias_console_memory(elsewhere, kRdram - page / 2, page),
          "a view running off the end is refused");

    std::printf("\n%s\n", failures == 0 ? "all good" : "SOMETHING IS WRONG");
    return failures == 0 ? 0 : 1;
}
