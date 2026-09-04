// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libultra helpers librecomp does not carry.
//
// N64Recomp keeps a list of libultra function names it will not translate, on
// the understanding that the runtime supplies each one instead. librecomp
// supplies most of them. The remainder are ones the projects it was built for
// happened to provide themselves, and a bundler that accepts any ROM does not
// get to choose which of them a game uses.
//
// Only the ones that genuinely cannot be recompiled are here. A libultra
// function whose body translates correctly -- guPerspective, strlen, bcopy --
// is better left to the recompiler, because then it is the game's own code
// running rather than an approximation of it, and this file stays small enough
// to be obviously right. What cannot be translated is 64-bit float conversion:
// the recompiler has no case for `trunc.l.d` and friends, so every one of these
// arrives as "Unhandled instruction" and takes the build down with it.
//
// The calling convention is the one librecomp's math_routines.cpp uses, which
// is the o32 convention the recompiled code was built around: a 64-bit integer
// argument arrives split across a pair of registers, a float argument in f12,
// and results go back in r2:r3 or f0.

#include "recomp.h"

#include <ultramodern/ultramodern.hpp>

namespace {

/// The 64-bit value o32 passes as a register pair.
inline uint64_t pair(gpr high, gpr low) {
    return (uint64_t(high) << 32) | (uint64_t(low) & 0xFFFFFFFFu);
}

inline void return_pair(recomp_context *ctx, uint64_t value) {
    ctx->r2 = (int32_t)(value >> 32);
    ctx->r3 = (int32_t)(value >> 0);
}

} // namespace

// --- double and float to 64-bit integer --------------------------------------
// These are the ones that actually block a build. Everything the compiler emits
// for a (long long) cast of a double routes through here.

extern "C" void __d_to_ll_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    return_pair(ctx, (uint64_t)(int64_t)ctx->f12.d);
}

extern "C" void __d_to_ull_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    return_pair(ctx, (uint64_t)ctx->f12.d);
}

extern "C" void __f_to_ull_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    return_pair(ctx, (uint64_t)ctx->f12.fl);
}

// --- 64-bit integer to double ------------------------------------------------

extern "C" void __ll_to_d_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    ctx->f0.d = (double)(int64_t)pair(ctx->r4, ctx->r5);
}

// --- the serial interface ----------------------------------------------------
//
// librecomp models the controller at the level of the public API --
// `osContStartReadData` and friends -- and substitutes those. A game that
// drives the serial interface itself gets nothing: it programs the SI
// registers, which are now ordinary memory, and then blocks forever on the
// interrupt that would have told it the transfer finished. Mario Builder 64
// does exactly that, and it is not unusual; reading the pads directly is a
// standard way to shave a frame of input latency, so any runtime that accepts
// arbitrary ROMs meets it eventually.
//
// Completing the transfer is the part that matters. The PIF's reply is left as
// it is, so a game reading its controllers this way sees no buttons pressed --
// which is a game that runs and does not respond, rather than a game that
// hangs on its first frame.

extern "C" void __osSiRawStartDma_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    ultramodern::send_si_message();
    ctx->r2 = 0;
}

// --- 64-bit integer arithmetic -----------------------------------------------
// librecomp has the division and shift pair but not these two. They would
// translate, but a native modulo is both faster and exactly right, and having
// them here means the set of libultra helpers is complete rather than nearly.

extern "C" void __ll_mod_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const int64_t a = (int64_t)pair(ctx->r4, ctx->r5);
    const int64_t b = (int64_t)pair(ctx->r6, ctx->r7);
    return_pair(ctx, b == 0 ? 0 : (uint64_t)(a % b));
}

extern "C" void __ll_rshift_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    // Signed, unlike librecomp's __ull_rshift: the `ll` spelling is the
    // arithmetic shift and the `ull` one is the logical shift.
    const int64_t a = (int64_t)pair(ctx->r4, ctx->r5);
    const uint64_t b = pair(ctx->r6, ctx->r7) & 63;
    return_pair(ctx, (uint64_t)(a >> b));
}

// --- yielding out of a spin loop ---------------------------------------------
//
// A game that waits for another thread by reading a word in a loop -- rather
// than by receiving a message -- stops the console model dead. Every thread is
// a real thread here, but only one of them runs at a time and the runtime picks
// which, and it only gets to pick when the running thread calls into it. A loop
// that only reads memory never calls into anything, so the thread that would
// write the word never runs, and the wait never ends. On the console the timer
// interrupt arrives and libultra's scheduler runs the higher-priority thread.
//
// The recompiler emits a call to this at the bottom of any loop it can prove
// only reads memory. Deliver whatever the console's own hardware has been
// waiting to deliver, then let the runtime choose a thread the way it would if
// the game had asked it something. The one-millisecond form rather than the
// blocking one: a game whose events have stopped entirely should keep spinning
// as the console would, not stop in a wait of our own making.

extern "C" void yield_self_1ms(uint8_t *rdram);

extern "C" void spin_wait(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    yield_self_1ms(rdram);
}

// --- the translation lookaside buffer ----------------------------------------
//
// librecomp holds the console's memory as one flat array indexed by
// `address - 0x80000000`, so a translated load reaches it with no lookup at
// all. That is right for every address a game normally uses: KSEG0 and KSEG1
// are fixed windows onto physical memory, and subtracting a constant is
// exactly what the hardware does with them.
//
// The TLB is the one thing it cannot express. A mapped address is in KUSEG,
// which the same subtraction sends to `rdram + address + 0x80000000` -- inside
// the four gigabytes librecomp reserves, but reserved rather than mapped, so
// the first read of one takes the process down.
//
// Super Mario 64 needs it. Its goddard segment -- the Mario head you drag
// around on the file select screen -- reads its display data through the TLB:
// the loader DMAs it into the main pool, maps it in 64KB pages at 0x04000000,
// and then everything goddard does reads it there. Unmapped, the game reaches
// the head and dies on the first word of it.
//
// What a TLB entry is, though, is an alias -- two addresses, one page -- and
// `mach_vm_remap` makes exactly that. So the entries are kept as the console
// keeps them, and the aliases are rebuilt from them whenever they change.
//
// The rebuild works in host pages rather than console pages, because it has
// to: this machine's pages are 16KB and the console's can be 4KB, and Mario
// Builder 64 maps 4KB ones. A host page can be aliased when every console page
// inside it maps to the matching physical page -- which is what a game mapping
// a buffer does, since the buffer is contiguous on both sides. When that does
// not hold there is no alias to make, and the host page is left unmapped so
// the read faults where it happens rather than reading someone else's data.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kUnmapped = 0xFFFFFFFFu;
constexpr size_t kTlbEntries = 32;
/// The smallest page the console can map, and so the step a host page has to
/// be checked in.
constexpr uint32_t kSmallestConsolePage = 0x1000u;

/// One TLB entry: two pages, either of which may be absent.
struct TlbEntry {
    uint32_t vaddr = 0;
    uint32_t page_size = 0; ///< bytes in one of the two pages; 0 when unused
    uint32_t physical[2] = {kUnmapped, kUnmapped};
};

TlbEntry tlb[kTlbEntries];
/// The host pages currently aliased, so they can be put back when they stop
/// being covered.
std::vector<uint32_t> aliased;

/// Where librecomp's flat memory puts a virtual address.
///
/// The same arithmetic `MEM_W` does, and it has to stay the same arithmetic:
/// this maps the page that a translated load will read.
uint8_t *flat_address(uint8_t *rdram, uint32_t virtual_address) {
    return rdram + (uint64_t)(int64_t)(int32_t)virtual_address - 0xFFFFFFFF80000000ull;
}

uint32_t host_page_size() {
    return uint32_t(::getpagesize());
}

/// Said once, with the numbers, because a game that maps a page this cannot
/// alias will map hundreds of them, and the numbers are what says which case
/// it is.
void complain_once(const char *why, uint32_t vaddr, uint32_t bytes) {
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "note: a TLB page could not be mapped (%s): 0x%08X, %u bytes, host pages "
                     "are %u. A game that reads its data through this mapping will stop on the "
                     "first word of it.\n",
                     why, vaddr, bytes, host_page_size());
    }
}

/// The physical address behind a virtual one, or kUnmapped.
uint32_t translate(uint32_t vaddr) {
    for (const TlbEntry &entry : tlb) {
        if (entry.page_size == 0) {
            continue;
        }
        for (int half = 0; half < 2; half++) {
            const uint32_t first = entry.vaddr + uint32_t(half) * entry.page_size;
            if (entry.physical[half] != kUnmapped && vaddr >= first &&
                vaddr < first + entry.page_size) {
                return entry.physical[half] + (vaddr - first);
            }
        }
    }
    return kUnmapped;
}

/// Alias one host page of memory at `physical` onto where `vaddr` lands.
void map_page(uint8_t *rdram, uint32_t vaddr, uint32_t physical, uint32_t bytes) {
    if ((vaddr & 0x80000000u) != 0) {
        // Only KUSEG is ever translated, and that matters here rather than
        // being pedantry: a KSEG0 address lands inside the memory librecomp
        // committed for RDRAM, and replacing a mapping there would take the
        // console's own memory away.
        complain_once("it is not a user-space address", vaddr, bytes);
        return;
    }
    mach_vm_address_t target = mach_vm_address_t(flat_address(rdram, vaddr));
    vm_prot_t current = VM_PROT_READ | VM_PROT_WRITE;
    vm_prot_t maximum = VM_PROT_READ | VM_PROT_WRITE;
    const kern_return_t remapped = mach_vm_remap(
        mach_task_self(), &target, bytes, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        mach_vm_address_t(rdram + physical), /*copy=*/FALSE, &current, &maximum, VM_INHERIT_SHARE);
    if (remapped != KERN_SUCCESS) {
        complain_once("the alias was refused", vaddr, bytes);
        return;
    }
    // The remap carries the source's protection over, but says so through an
    // out parameter rather than promising it, so ask for what is wanted.
    if (mach_vm_protect(mach_task_self(), target, bytes, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        complain_once("the alias could not be made writable", vaddr, bytes);
    }
}

/// Put an address range back the way librecomp reserved it: present, so the
/// next mapping can overwrite it, and inaccessible, so a read of an unmapped
/// address faults the way the console would.
void unmap_page(uint8_t *rdram, uint32_t vaddr, uint32_t bytes) {
    if ((vaddr & 0x80000000u) != 0) {
        return;
    }
    void *at = flat_address(rdram, vaddr);
    (void)::mmap(at, bytes, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
}

/// Rebuild every alias from the entries as they now stand.
void rebuild(uint8_t *rdram) {
    const uint32_t page = host_page_size();

    // Every host page the entries touch, once each.
    std::vector<uint32_t> wanted;
    for (const TlbEntry &entry : tlb) {
        if (entry.page_size == 0) {
            continue;
        }
        const uint32_t covered = entry.page_size * 2u;
        const uint32_t first = entry.vaddr & ~(page - 1u);
        const uint32_t last = (entry.vaddr + covered + page - 1u) & ~(page - 1u);
        for (uint32_t at = first; at != last; at += page) {
            wanted.push_back(at);
        }
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    // Anything that was aliased and is not wanted any more goes back.
    for (uint32_t at : aliased) {
        if (!std::binary_search(wanted.begin(), wanted.end(), at)) {
            unmap_page(rdram, at, page);
        }
    }
    aliased.clear();

    for (uint32_t at : wanted) {
        // A host page is aliased as one piece, so every console page inside it
        // that the game mapped has to sit at the same distance from its
        // physical address -- which is what mapping a buffer looks like, since
        // the buffer is contiguous on both sides.
        //
        // Console pages inside it that the game did not map are allowed, and
        // they are the compromise here. Mario Builder 64 maps a single 4KB
        // page, and the four console pages of a 16KB host page cannot be
        // aliased separately, so the other three end up pointing at whatever
        // physical memory follows. On the console they would fault. A game
        // that stays inside the mapping it asked for -- which is every game
        // that is working -- cannot tell the difference, and the alternative
        // is that a 4KB mapping does not work at all.
        uint32_t base = kUnmapped;
        bool consistent = true;
        for (uint32_t offset = 0; consistent && offset < page;
             offset += kSmallestConsolePage) {
            const uint32_t physical = translate(at + offset);
            if (physical == kUnmapped) {
                continue;
            }
            if (base == kUnmapped) {
                base = physical - offset;
            } else if (physical != base + offset) {
                consistent = false;
            }
        }
        if (!consistent || base == kUnmapped || (base % page) != 0) {
            complain_once("the host page it falls in is not one run of physical memory", at, page);
            unmap_page(rdram, at, page);
            continue;
        }
        map_page(rdram, at, base, page);
        aliased.push_back(at);
    }
}

/// The page size a PageMask register value means.
///
/// The mask holds one bit per 4KB above the first, from bit 13 up, so
/// 0x0001E000 is four of them plus the first: 64KB, which is what Super Mario
/// 64 asks for.
uint32_t page_size_from_mask(uint32_t mask) {
    return kSmallestConsolePage * ((mask >> 13) + 1u);
}

} // namespace

/// `osMapTLB(index, pagemask, vaddr, lo0, lo1, asid)`.
///
/// The two physical addresses are the pair of pages the entry covers, low one
/// first, and either may be -1 for "this half is not mapped". The ASID is
/// ignored: nothing here runs two address spaces.
extern "C" void osMapTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    const int32_t index = int32_t(ctx->r4);
    if (index < 0 || size_t(index) >= kTlbEntries) {
        return;
    }
    const uint32_t page_size = page_size_from_mask(uint32_t(ctx->r5));
    TlbEntry &entry = tlb[index];
    entry.page_size = page_size;
    entry.vaddr = uint32_t(ctx->r6) & ~(page_size * 2u - 1u);
    entry.physical[0] = uint32_t(ctx->r7);
    entry.physical[1] = uint32_t(MEM_W(0x10, ctx->r29));
    rebuild(rdram);
}

/// `osUnmapTLB(index)`.
extern "C" void osUnmapTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    const int32_t index = int32_t(ctx->r4);
    if (index >= 0 && size_t(index) < kTlbEntries) {
        tlb[index] = TlbEntry{};
        rebuild(rdram);
    }
}

extern "C" void osUnmapTLBAll_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    for (TlbEntry &entry : tlb) {
        entry = TlbEntry{};
    }
    rebuild(rdram);
}

/// `osSetTLBASID(asid)`. One address space, so there is nothing to set.
extern "C" void osSetTLBASID_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
}

/// `__osProbeTLB(vaddr)` -- the physical address behind a mapped one, or -1.
///
/// This is what `osVirtualToPhysical` falls back on for an address that is in
/// neither fixed window, and therefore what decides where the RDP reads a
/// segment a game placed in mapped memory.
extern "C" void __osProbeTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const uint32_t physical = translate(uint32_t(ctx->r4));
    ctx->r2 = physical == kUnmapped ? int32_t(-1) : int32_t(physical);
}
