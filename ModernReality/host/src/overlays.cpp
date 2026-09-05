// SPDX-License-Identifier: GPL-3.0-or-later
//
// Code that is in no cartridge: recompiling a function the first time a game
// jumps into it.
//
// A static recompilation translates what the image holds. That is the whole of
// two of the three cartridges here, and it is not the whole of the third: a
// game too big to hold in memory at once splits its code into overlays,
// compresses them, and decompresses one into a buffer it allocated when it
// wants it. The code that runs then is in no part of the cartridge a reader
// can point at, and it is at an address the game's own allocator picked -- so
// there is nothing to analyse ahead of time and nothing for a title record to
// write down. Banjo-Tooie has 886 of them and frees them as it goes, handing
// the same address to a different overlay later.
//
// What is left is to do the work when the address is known, which is the
// moment the game jumps to it. The runtime already notices: `get_function`
// fails, and a weak hook says where. From there the pieces are the ones the
// offline pipeline uses -- the same recompiler, over the same instructions,
// with the code read out of the console's memory rather than out of a file.
//
// The result is cached on the address, and checked against the words still
// there before it is reused, because an overlay that has been freed and
// replaced is the one thing this must not get wrong.

#include "host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <algorithm>

#include "rabbitizer.hpp"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"

namespace {

/// A function that has been compiled, and enough of what it was compiled from
/// to know whether it is still there.
struct Compiled {
    recomp_func_t *function = nullptr;
    /// The bytes this was compiled from.
    ///
    /// An overlay is freed and its address handed to another, so a translation
    /// is only good while the code it was made from is still there. Two words
    /// would not settle it: nearly every function on this machine begins by
    /// making a stack frame and saving the return address, so a wrong answer
    /// would look right. The whole body is compared, which for a function of a
    /// few hundred bytes is a memcmp against memory that is already warm.
    std::vector<uint8_t> made_from;
    /// The generated code, which has to outlive every call into it.
    std::unique_ptr<N64Recomp::LiveGeneratorOutput> output;
    /// The section address the generated code was given a pointer to. Kept
    /// alive for the same reason.
    std::unique_ptr<int32_t> section_address;
};

/// Where each translation landed, so that a crash inside one can be named.
///
/// Generated code has no symbol and no line, so a native backtrace through it
/// is a bare address -- which is the least useful thing to be looking at when
/// a game has just gone wrong inside code that was translated a second ago.
struct Landed {
    const uint8_t *start;
    size_t size;
    uint32_t vram;
};

std::mutex compile_lock;
std::unordered_map<uint32_t, Compiled> compiled;
std::vector<Landed> landed;
size_t compiled_count = 0;
size_t failed_count = 0;

/// The word at a console address, in the order the console holds it.
uint32_t console_word(const uint8_t *rdram, uint32_t vram) {
    uint32_t word = 0;
    std::memcpy(&word, rdram + (vram - 0x80000000u), sizeof(word));
    return word;
}

/// Where the function starting at `vram` ends.
///
/// The rule the analyser uses on a cartridge, applied to memory. Walk forward
/// remembering the furthest anything inside jumps to, and end at the first
/// `jr $ra` whose delay slot is at or past that -- a function with several
/// returns branches over the earlier ones, so a return before the last branch
/// target is one of those rather than the end of the function.
///
/// The rest of it is about not believing data. A word that does not decode, a
/// branch above the address we were told is the start, a jump to somewhere
/// that is not console memory: each of those says this address is not the top
/// of a function, and running whatever is there instead is the one outcome
/// worth avoiding. Zero for all of them.
uint32_t function_length(const uint8_t *rdram, uint32_t vram) {
    // Longer than any function either recovered cartridge holds, and short
    // enough that walking off the end of an overlay stops rather than reading
    // eight megabytes.
    constexpr uint32_t kLongest = 0x8000;
    constexpr uint32_t kJrRa = 0x03E00008u;
    constexpr uint32_t kRamStart = 0x80000000u;
    constexpr uint32_t kRamEnd = kRamStart + 8u * 1024u * 1024u;

    auto reachable = [](uint32_t target) {
        return target >= kRamStart && target < kRamEnd && (target & 3) == 0;
    };

    uint32_t furthest = vram;
    for (uint32_t at = vram; at + 8 <= kRamEnd && at - vram < kLongest; at += 4) {
        const uint32_t word = console_word(rdram, at);
        const rabbitizer::InstructionCpu instruction(word, at);
        if (!instruction.isValid()) {
            return 0;
        }

        if (instruction.isBranch()) {
            const uint32_t target = uint32_t(instruction.getBranchVramGeneric());
            if (target < vram || target >= vram + kLongest) {
                return 0; // the middle of a function rather than the top of one
            }
            furthest = std::max(furthest, target);
            continue;
        }

        switch (instruction.getUniqueId()) {
            case rabbitizer::InstrId::UniqueId::cpu_jal:
                if (!reachable(uint32_t(instruction.getBranchVramGeneric()))) {
                    return 0;
                }
                break;

            case rabbitizer::InstrId::UniqueId::cpu_j: {
                // Either a jump inside the function or a tail call out of it.
                const uint32_t target = uint32_t(instruction.getBranchVramGeneric());
                if (!reachable(target)) {
                    return 0;
                }
                if (target >= vram && target <= furthest) {
                    break;
                }
                if (furthest <= at) {
                    return at + 8 - vram; // a tail call ends a function
                }
                break;
            }

            case rabbitizer::InstrId::UniqueId::cpu_jr:
                if (furthest > at) {
                    break; // something below still branches past here
                }
                if (word == kJrRa) {
                    return at + 8 - vram;
                }
                // A jump through a register is a jump table, and the function
                // carries on into its cases.
                break;

            default:
                break;
        }
    }
    return 0;
}

} // namespace

recomp_func_t *n64b::recompile_at(uint8_t *rdram, uint32_t vram) {
    if (rdram == nullptr || vram < 0x80000000u) {
        return nullptr;
    }

    std::lock_guard<std::mutex> held(compile_lock);

    const auto seen = compiled.find(vram);
    if (seen != compiled.end()) {
        const Compiled &entry = seen->second;
        if (entry.function != nullptr &&
            std::memcmp(rdram + (vram - 0x80000000u), entry.made_from.data(),
                        entry.made_from.size()) == 0) {
            return entry.function;
        }
        if (entry.function == nullptr && entry.made_from.empty()) {
            return nullptr; // asked before, and it was not a function then either
        }
        // The code changed, so this address belongs to something else now.
        compiled.erase(seen);
    }

    Compiled entry;
    const uint32_t length = function_length(rdram, vram);
    if (length == 0) {
        std::fprintf(stderr,
                     "note: the game jumped to 0x%08X, which is not in any recompiled section "
                     "and does not read as the start of a function.\n",
                     vram);
        failed_count++;
        compiled.emplace(vram, std::move(entry));
        return nullptr;
    }

    // The window the recompiler gets to read.
    //
    // It needs more than the function: a jump table is data beside the code
    // rather than inside it, and the recompiler finds one by reading the
    // addresses out of it. How much more is a guess either way, so it is
    // generous in both directions and clamped to the console's memory.
    constexpr uint32_t kRamStart = 0x80000000u;
    constexpr uint32_t kRamEnd = kRamStart + 8 * 1024 * 1024;
    constexpr uint32_t kBefore = 0x4000;
    constexpr uint32_t kAfter = 0x10000;
    const uint32_t window_start = vram - kRamStart > kBefore ? vram - kBefore : kRamStart;
    uint32_t window_end = vram + length + kAfter;
    if (window_end > kRamEnd || window_end < vram) {
        window_end = kRamEnd;
    }

    N64Recomp::Context context{};
    // The recompiler reads its instructions and its jump tables out of this,
    // big-endian, the way they sit in a cartridge. The console holds a word
    // the other way round, so every one of them turns over on the way in.
    context.rom.resize(window_end - window_start);
    for (uint32_t at = window_start; at < window_end; at += 4) {
        const uint32_t word = __builtin_bswap32(console_word(rdram, at));
        std::memcpy(context.rom.data() + (at - window_start), &word, sizeof(word));
    }

    N64Recomp::Section section{};
    section.rom_addr = 0;
    section.ram_addr = window_start;
    section.size = window_end - window_start;
    section.name = "overlay";
    section.executable = true;
    // Nothing relocates this: the game already put it where it goes, and the
    // addresses inside it are the ones it was decompressed to hold.
    section.fixed_address = true;
    context.sections.push_back(std::move(section));
    context.section_functions.resize(1);

    std::vector<uint32_t> words;
    words.reserve(length / 4);
    for (uint32_t at = vram; at < vram + length; at += 4) {
        words.push_back(__builtin_bswap32(console_word(rdram, at)));
    }
    char name[32];
    std::snprintf(name, sizeof(name), "overlay_%08X", vram);
    context.functions.emplace_back(vram, vram - window_start, std::move(words), name, 0);
    context.section_functions[0].push_back(0);
    context.functions_by_vram[vram].push_back(0);

    // Every call this function makes goes through the runtime's lookup: what
    // it calls is either in the module, or in another overlay, and this
    // context knows about neither.
    context.use_lookup_for_all_function_calls = true;
    context.lookup_unresolved_function_calls = true;

    entry.section_address = std::make_unique<int32_t>(int32_t(window_start));

    N64Recomp::LiveGeneratorInputs inputs{};
    inputs.base_event_index = 0;
    inputs.cop0_status_write = cop0_status_write;
    inputs.cop0_status_read = cop0_status_read;
    inputs.switch_error = switch_error;
    inputs.do_break = do_break;
    inputs.get_function = get_function;
    inputs.syscall_handler = recomp_syscall_handler;
    inputs.pause_self = pause_self;
    inputs.trigger_event = nullptr;
    inputs.reference_section_addresses = entry.section_address.get();
    inputs.local_section_addresses = entry.section_address.get();
    inputs.run_hook = nullptr;

    N64Recomp::LiveGenerator generator{context.functions.size(), inputs};
    std::vector<std::vector<uint32_t>> statics{};
    std::ostringstream sink{};
    const bool translated =
        N64Recomp::recompile_function_live(generator, context, 0, sink, statics, false);
    auto output = std::make_unique<N64Recomp::LiveGeneratorOutput>(generator.finish());

    if (!translated || !output->good || output->functions.empty()) {
        std::fprintf(stderr,
                     "note: the code at 0x%08X (0x%X bytes) could not be recompiled while the "
                     "game ran.\n",
                     vram, length);
        failed_count++;
        compiled.emplace(vram, std::move(entry));
        return nullptr;
    }

    // What was translated, when it is being looked at. A game that reaches a
    // lot of its overlays compiles a lot of functions, so this is off unless
    // asked for.
    static const bool announce = std::getenv("N64B_OVERLAYS") != nullptr;
    if (announce) {
        std::fprintf(stderr, "overlay: recompiled 0x%08X, 0x%X bytes\n", vram, length);
    }

    entry.made_from.assign(rdram + (vram - 0x80000000u),
                           rdram + (vram - 0x80000000u) + length);
    landed.push_back({static_cast<const uint8_t *>(output->code), output->code_size, vram});
    entry.function = output->functions[0];
    entry.output = std::move(output);
    recomp_func_t *const result = entry.function;
    compiled.emplace(vram, std::move(entry));
    compiled_count++;
    return result;
}

uint32_t n64b::generated_owner(const void *address) {
    // Deliberately not locked: this is called from a signal handler, and a
    // torn read of a vector that only ever grows costs one unnamed frame.
    const uint8_t *at = static_cast<const uint8_t *>(address);
    for (const Landed &block : landed) {
        if (at >= block.start && at < block.start + block.size) {
            return block.vram;
        }
    }
    return 0;
}

void n64b::report_recompiled(void) {
    if (compiled_count == 0 && failed_count == 0) {
        return;
    }
    std::fprintf(stderr, "note: %zu function(s) recompiled while the game ran, %zu refused.\n",
                 compiled_count, failed_count);
}
