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
