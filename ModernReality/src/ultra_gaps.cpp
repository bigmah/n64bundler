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
// `virtual address - 0x80000000`, so a translated load reaches it with no
// lookup at all. That is right for every address a game normally uses: KSEG0
// and KSEG1 are fixed windows onto physical memory, and subtracting a constant
// is exactly what the hardware does with them.
//
// The TLB is the one thing it cannot express. A mapped address is in KUSEG,
// which the same subtraction sends to `rdram + address + 0x80000000` -- inside
// the four gigabytes librecomp reserves, but reserved rather than mapped, so
// the first read of one takes the process down.
//
// Super Mario 64 needs it. Its goddard segment -- the Mario head you drag
// around on the file select screen -- reads its display data through the TLB:
// the loader DMAs the data into the main pool, maps it in 64KB pages at
// 0x04000000, and then everything goddard does reads it there. Unnamed and
// unmapped, the game reaches the head and dies on the first word of it.
//
// What a TLB entry is, though, is an alias: two addresses, one page of memory.
// `mach_vm_remap` makes exactly that, so an entry becomes a remap of the pages
// it names onto the address the flat array gives its virtual address. The game
// then reads and writes the same bytes through either address, and the RDP,
// which only ever sees physical addresses, sees the writes too.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>

namespace {

/// Where librecomp's flat memory puts a virtual address.
///
/// The same arithmetic `MEM_W` does, and it has to stay the same arithmetic:
/// this maps the page that a translated load will read.
uint8_t *flat_address(uint8_t *rdram, uint32_t virtual_address) {
    return rdram + (uint64_t)(int64_t)(int32_t)virtual_address - 0xFFFFFFFF80000000ull;
}

/// One TLB entry's two pages, as the console numbers them.
struct TlbEntry {
    uint32_t vaddr;     ///< the first page's virtual address
    uint32_t physical;  ///< the first page's physical address, or kUnmapped
    uint32_t page_size; ///< bytes in one of the two pages
};

constexpr uint32_t kUnmapped = 0xFFFFFFFFu;
constexpr size_t kTlbEntries = 32;
TlbEntry tlb[kTlbEntries];

/// Said once, because a game that maps a page this cannot alias will map
/// hundreds of them.
void complain_once(const char *why) {
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "note: a TLB page could not be mapped (%s). A game that reads its "
                             "data through the TLB will stop here.\n",
                     why);
    }
}

/// Alias `bytes` of memory at `physical` onto where `vaddr` lands in the flat
/// map. Both addresses and the length have to be whole host pages, which on
/// this machine are 16KB; every page size libultra offers except its smallest
/// is a multiple of that.
void map_page(uint8_t *rdram, uint32_t vaddr, uint32_t physical, uint32_t bytes) {
    const size_t page = size_t(::getpagesize());
    if ((vaddr & 0x80000000u) != 0) {
        // Only KUSEG is ever translated, and that matters here rather than
        // being pedantry: a KSEG0 address lands inside the memory librecomp
        // committed for RDRAM, and replacing a mapping there would take the
        // console's own memory away.
        complain_once("it is not a user-space address");
        return;
    }
    if ((vaddr % page) != 0 || (physical % page) != 0 || (bytes % page) != 0) {
        complain_once("it is smaller than a host page, or not aligned to one");
        return;
    }
    mach_vm_address_t target = mach_vm_address_t(flat_address(rdram, vaddr));
    vm_prot_t current = VM_PROT_READ | VM_PROT_WRITE;
    vm_prot_t maximum = VM_PROT_READ | VM_PROT_WRITE;
    const kern_return_t remapped = mach_vm_remap(
        mach_task_self(), &target, bytes, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        mach_vm_address_t(rdram + physical), /*copy=*/FALSE, &current, &maximum, VM_INHERIT_SHARE);
    if (remapped != KERN_SUCCESS) {
        complain_once("the alias was refused");
        return;
    }
    // The remap carries the source's protection over, but says so through an
    // out parameter rather than promising it, so ask for what is wanted.
    if (mach_vm_protect(mach_task_self(), target, bytes, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        complain_once("the alias could not be made writable");
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

void forget(uint8_t *rdram, size_t index) {
    TlbEntry &entry = tlb[index];
    if (entry.page_size == 0) {
        return;
    }
    if (entry.physical != kUnmapped) {
        unmap_page(rdram, entry.vaddr, entry.page_size * 2);
    }
    entry = TlbEntry{};
}

/// The page size a PageMask register value means.
///
/// The mask holds one bit per 4KB above the first, from bit 13 up, so
/// 0x0001E000 is four of them plus the first: 64KB, which is what Super Mario
/// 64 asks for.
uint32_t page_size_from_mask(uint32_t mask) {
    return 0x1000u * ((mask >> 13) + 1u);
}

} // namespace

/// `osMapTLB(index, pagemask, vaddr, lo0, lo1, asid)`.
///
/// The two physical addresses are the pair of pages the entry covers, low one
/// first, and either may be -1 for "this half is not mapped". The ASID is
/// ignored: nothing here runs two address spaces.
extern "C" void osMapTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    const int32_t index = int32_t(ctx->r4);
    const uint32_t page_size = page_size_from_mask(uint32_t(ctx->r5));
    const uint32_t vaddr = uint32_t(ctx->r6) & ~(page_size * 2u - 1u);
    const uint32_t lo0 = uint32_t(ctx->r7);
    const uint32_t lo1 = uint32_t(MEM_W(0x10, ctx->r29));
    if (index < 0 || size_t(index) >= kTlbEntries) {
        return;
    }
    forget(rdram, size_t(index));
    tlb[index] = TlbEntry{vaddr, lo0, page_size};
    if (lo0 != kUnmapped) {
        map_page(rdram, vaddr, lo0, page_size);
    }
    if (lo1 != kUnmapped) {
        map_page(rdram, vaddr + page_size, lo1, page_size);
    }
}

/// `osUnmapTLB(index)`.
extern "C" void osUnmapTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    const int32_t index = int32_t(ctx->r4);
    if (index >= 0 && size_t(index) < kTlbEntries) {
        forget(rdram, size_t(index));
    }
}

extern "C" void osUnmapTLBAll_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    for (size_t index = 0; index < kTlbEntries; index++) {
        forget(rdram, index);
    }
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
    const uint32_t address = uint32_t(ctx->r4);
    ctx->r2 = int32_t(-1);
    for (const TlbEntry &entry : tlb) {
        if (entry.page_size == 0 || entry.physical == kUnmapped) {
            continue;
        }
        const uint32_t span = entry.page_size * 2u;
        if (address >= entry.vaddr && address < entry.vaddr + span) {
            ctx->r2 = int32_t(entry.physical + (address - entry.vaddr));
            return;
        }
    }
}
