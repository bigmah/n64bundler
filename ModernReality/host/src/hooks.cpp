// SPDX-License-Identifier: GPL-3.0-or-later
//
// The handful of symbols the recompiled code expects its host to define.
//
// recomp.h declares a few functions that librecomp does not implement, on the
// understanding that whatever project links a recompiled game supplies them.
// A decompilation-based project writes them by hand for its one game; a
// bundler writes them once for every game it will ever open.

#include "host.hpp"

#include <cstdio>

/// A `syscall` instruction in the recompiled code.
///
/// The N64's own libultra uses it for nothing a game reaches -- the exception
/// handler is hand-written assembly this analysis stubs -- so in practice one
/// of these means the recompiler translated something that was not code, and
/// the address says where. Reporting it and carrying on is right: the game may
/// well never touch that path, and taking the process down would turn a
/// possible black frame into a certain crash.
extern "C" void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx,
                                       int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr,
                     "note: the game executed a syscall at 0x%08X. Nothing in libultra does "
                     "that, so this is most likely data the analysis read as code.\n",
                     uint32_t(instruction_vram));
    }
}
