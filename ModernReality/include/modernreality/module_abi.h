// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract between a recompiled game and the host that runs it.
//
// Every game N64Bundler recompiles becomes one `.dylib` exporting a single
// symbol, `n64b_module`, of the type below. The host (`n64b-run`) is built
// once with RT64, librecomp and ultramodern inside it, and `dlopen`s a module
// per game. That is the whole reason this file exists: a bundler has a library
// of games, and linking the renderer into each of them would give every game
// its own copy of it.
//
// The module is deliberately thin. It carries the recompiled functions, the
// section table describing where they came from in the ROM, and the handful of
// facts the runtime cannot work out for itself -- the save chip, the entry
// point, which microcode the RSP tasks are. Everything else, including every
// libultra call the game makes, is an undefined symbol resolved against the
// host at load time, which is what `-undefined dynamic_lookup` against an
// `-export_dynamic` host buys.
//
// No game data is ever in a module: the ROM stays a separate file that the
// runtime reads at startup, and `rom_hash` is what ties the two together.

#ifndef MODERNREALITY_MODULE_ABI_H
#define MODERNREALITY_MODULE_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "recomp.h"
#include "librecomp/sections.h"
#include "ultramodern/ultra64.h"

// Bumped whenever the struct below changes shape. The host refuses a module
// that does not match, because a stale module is a crash with no explanation.
#define N64B_MODULE_ABI 1u

// The one symbol a module exports.
#define N64B_MODULE_SYMBOL "n64b_module"

#if defined(__GNUC__)
#define N64B_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define N64B_MODULE_EXPORT
#endif

// librecomp's RspExitReason, opaque here. A scoped enum with no fixed
// underlying type is still int, so declaring it without the definition is
// enough to spell the microcode function's type -- and it keeps rsp_vu.hpp,
// which drags in a vector unit implementation, out of every module build.
enum class RspExitReason;

/// The signature librecomp's RSP interpreter calls a recompiled microcode with.
using n64b_rsp_ucode_func = RspExitReason (*)(uint8_t *rdram, uint32_t ucode_addr);

/// Picks the microcode for a task the game submitted, or nullptr if the module
/// has none for it. Mirrors recomp::rsp::callbacks_t::get_rsp_microcode.
using n64b_rsp_selector = n64b_rsp_ucode_func (*)(const OSTask *task);

/// Mirrors recomp::SaveType. Duplicated as a plain int in the descriptor so
/// that a module built against one runtime revision still loads into the next.
enum n64b_save_type {
    N64B_SAVE_NONE = 0,
    N64B_SAVE_EEP4K = 1,
    N64B_SAVE_EEP16K = 2,
    N64B_SAVE_SRAM = 3,
    N64B_SAVE_FLASHRAM = 4,
    N64B_SAVE_ALLOW_ALL = 5,
};

struct n64b_module_v1 {
    /// N64B_MODULE_ABI at the time the module was built.
    uint32_t abi_version;

    /// The four-character cartridge code, "NSME" for Super Mario 64 (USA).
    const char *game_id;
    /// The name out of the ROM header, which the runtime shows and validates on.
    const char *internal_name;
    /// What the library and the .app call this game.
    const char *display_name;

    /// XXH3-64 of the whole big-endian image, the same hash librecomp checks a
    /// stored ROM against. The module holds no game data; this is the link.
    uint64_t rom_hash;
    /// Size of the image the module was built from, for a cheap early mismatch.
    uint64_t rom_size;

    /// Where the boot code jumps, out of the ROM header.
    uint32_t entrypoint_address;
    /// The recompiled function at that address.
    recomp_func_t *entrypoint;

    /// The section table the recompiler wrote, in ROM order.
    SectionTableEntry *code_sections;
    size_t num_code_sections;
    /// Includes sections with no functions in them, which the runtime still
    /// counts when it resolves a relocation.
    size_t total_num_sections;

    /// Which section index each overlay slot loads, -1 for an empty slot.
    int *overlays_by_index;
    size_t num_overlays;

    /// nullptr if the game's microcode was not recompiled, in which case the
    /// host reports the unhandled task rather than running the wrong code.
    n64b_rsp_selector get_rsp_microcode;

    /// One of n64b_save_type.
    int save_type;

    /// What produced this module, for the "what am I running" line in a log.
    const char *analyser_revision;
    const char *builder_revision;
};

extern "C" N64B_MODULE_EXPORT const n64b_module_v1 n64b_module;

#endif
