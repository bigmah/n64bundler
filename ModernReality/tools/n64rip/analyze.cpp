// SPDX-License-Identifier: GPL-3.0-or-later
// Recovering sections and function boundaries from a bare ROM.
//
// The recompiler needs to be told, for every piece of code in the image, where
// it lives in the ROM, what address it runs at, and where each function starts
// and ends. A decompilation project answers all three from its elf. Here we
// have to work it out from the instruction stream.
//
// The approach is to follow the code rather than to scan for it. Starting at
// the entry point, walk each function to its end; every `jal` along the way
// names another function outright, because the instruction encodes an absolute
// target. That yields the code reachable by direct call, which is nearly all
// of it, and — more usefully — it yields a trustworthy answer to the question
// scanning cannot answer: where .text stops and .rodata begins. Only inside
// that boundary is it safe to sweep linearly for the functions nothing calls
// directly, the ones reached through a pointer or a virtual table.
//
// Deciding where a function ends is the fiddly part, because `jr $ra` is not
// it: a function with an early return has several, and the last instruction of
// one is often a tail call rather than a return at all. What settles it is
// whether anything branches past the candidate end. If a branch further down
// is still outstanding, the function continues.

#include "n64rip.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "rabbitizer.hpp"

namespace n64rip {
namespace {

using InstrId = rabbitizer::InstrId::UniqueId;

/// `jr $ra`, the one word that is unambiguously a return.
constexpr uint32_t kJrRa = 0x03E00008;

/// Code is emitted in 16-byte blocks by every compiler used on this hardware,
/// so a function starts aligned and the gap before one is padding.
constexpr uint32_t kCodeAlignment = 16;

/// No function on this machine is this long. A walk that runs past it has
/// wandered out of code and into something that merely decodes.
constexpr uint32_t kMaxFunctionSize = 0x8000;

struct Walk {
    bool valid = false;
    uint32_t end = 0;                 // one past the last byte of the function
    std::vector<uint32_t> call_targets; // every `jal` seen inside it, and any
                                        // absolute `jr` it tail-calls through
};

/// Where a `jr $rs` is going, when the answer is written down in front of it.
///
/// Not every function ends in `jr $ra`. A hand-written stub — the game's own
/// entry point is one on every N64 game — loads an absolute address and jumps
/// to it, which is a tail call with the destination spelled out two
/// instructions earlier:
///
///     lui   $t2, 0x8024
///     addiu $t2, $t2, 0x6DF8
///     jr    $t2
///
/// Reading that pair matters more than its rarity suggests: it is the only
/// edge out of the entry point, so without it the walk that follows calls
/// through the binary never leaves the first function.
///
/// A `jr` through a jump table gets its register from a load instead, no pair
/// is found, and the caller carries on through the switch's cases.
std::optional<uint32_t> resolve_jump_register(const Rom &rom, const SectionInfo &section,
                                              uint32_t jr_vram, uint32_t rs) {
    constexpr int kLookback = 16;
    bool have_lo = false;
    uint32_t lo = 0;

    for (int back = 1; back <= kLookback; back++) {
        const uint32_t vram = jr_vram - uint32_t(back) * 4;
        if (vram < section.vram) {
            return std::nullopt;
        }
        const uint32_t word = rom.word(section.rom + (vram - section.vram));
        const uint32_t op = word >> 26;
        const uint32_t rt = (word >> 16) & 0x1F;
        const uint32_t base = (word >> 21) & 0x1F;
        const uint32_t imm = word & 0xFFFF;

        if (!have_lo) {
            // addiu rs, x, lo  /  ori rs, x, lo
            if ((op == 0x09 || op == 0x0D) && rt == rs) {
                lo = (op == 0x09) ? uint32_t(int32_t(int16_t(imm))) : imm;
                have_lo = true;
                // The pair may name a different register in its high half.
                rs = base;
            }
            continue;
        }
        // lui rs, hi
        if (op == 0x0F && rt == rs) {
            return (imm << 16) + lo;
        }
    }
    return std::nullopt;
}

/// Walk one function from `start` and decide where it ends.
///
/// Returns invalid if anything says this is not the start of a function: a
/// word that does not decode, a branch to before `start` (which means the real
/// function began earlier and we are standing in the middle of it), or a run
/// longer than any function on this hardware.
Walk walk_function(const Rom &rom, const SectionInfo &section, uint32_t start) {
    Walk walk;
    const uint32_t section_end = section.vram + section.size;
    if (start < section.vram || start >= section_end || (start & 3) != 0) {
        return walk;
    }

    // The furthest point anything so far has branched to. While it is ahead of
    // us the function cannot have ended, however many returns we pass.
    uint32_t furthest_branch = start;

    for (uint32_t vram = start; vram + 8 <= section_end; vram += 4) {
        if (vram - start >= kMaxFunctionSize) {
            return walk;
        }

        const uint32_t offset = section.rom + (vram - section.vram);
        const uint32_t word = rom.word(offset);
        const rabbitizer::InstructionCpu insn(word, vram);

        if (!insn.isValid()) {
            return walk;
        }

        if (insn.isBranch()) {
            const uint32_t target = uint32_t(insn.getBranchVramGeneric());
            if (target < start) {
                // Branching above our supposed start: this is the middle of a
                // function, not the top of one.
                return walk;
            }
            if (target >= start + kMaxFunctionSize) {
                return walk;
            }
            furthest_branch = std::max(furthest_branch, target);
            continue;
        }

        switch (insn.getUniqueId()) {
            case InstrId::cpu_jal:
                walk.call_targets.push_back(uint32_t(insn.getBranchVramGeneric()));
                break;

            case InstrId::cpu_j: {
                // Either a jump inside the function or a tail call out of it.
                // The compilers used on this hardware reach anywhere in a
                // function with an ordinary branch, so a `j` to somewhere we
                // have not already established is part of this function is a
                // tail call, and tail calls end functions.
                const uint32_t target = uint32_t(insn.getBranchVramGeneric());
                if (target >= start && target <= furthest_branch) {
                    break;
                }
                if (furthest_branch <= vram) {
                    walk.valid = true;
                    walk.end = vram + 8; // include the delay slot
                    return walk;
                }
                break;
            }

            case InstrId::cpu_jr: {
                if (furthest_branch > vram) {
                    // Something below still branches past here, so whatever
                    // this jump is, the function has not ended.
                    break;
                }
                if (word == kJrRa) {
                    walk.valid = true;
                    walk.end = vram + 8; // include the delay slot
                    return walk;
                }
                const std::optional<uint32_t> target =
                    resolve_jump_register(rom, section, vram, (word >> 21) & 0x1F);
                if (target.has_value()) {
                    walk.call_targets.push_back(*target);
                    walk.valid = true;
                    walk.end = vram + 8;
                    return walk;
                }
                // A jump through a register nothing in front of us loaded is a
                // jump table, and the function carries on into its cases.
                break;
            }

            default:
                break;
        }
    }

    return walk;
}

/// Round a function's end up over the padding that separates it from the next.
uint32_t skip_padding(const Rom &rom, const SectionInfo &section, uint32_t vram) {
    const uint32_t section_end = section.vram + section.size;
    while (vram < section_end && (vram % kCodeAlignment) != 0 &&
           rom.word(section.rom + (vram - section.vram)) == 0) {
        vram += 4;
    }
    return vram;
}

/// Everything found in one section, keyed on where each function starts.
struct Recovered {
    std::map<uint32_t, uint32_t> functions; // start -> end
    uint32_t text_end = 0;
};

/// Phase one: follow calls out from the section's entry point.
///
/// This is the part that can be trusted. Every function it reports was reached
/// by a `jal` from a function that itself validated, so nothing here came from
/// misreading data as code, and the highest address it reaches is a sound
/// lower bound for where .text runs to.
void walk_reachable(const Rom &rom, const SectionInfo &section, uint32_t entry,
                    Recovered &out, AnalysisReport &report) {
    const uint32_t section_end = section.vram + section.size;
    std::vector<uint32_t> queue{entry};
    std::set<uint32_t> rejected;

    while (!queue.empty()) {
        const uint32_t start = queue.back();
        queue.pop_back();
        if (out.functions.count(start) || rejected.count(start)) {
            continue;
        }

        const Walk walk = walk_function(rom, section, start);
        if (!walk.valid) {
            rejected.insert(start);
            continue;
        }

        out.functions[start] = walk.end;
        out.text_end = std::max(out.text_end, walk.end);
        report.from_calls++;

        for (uint32_t target : walk.call_targets) {
            if (target >= section.vram && target < section_end && (target & 3) == 0) {
                if (!out.functions.count(target) && !rejected.count(target)) {
                    queue.push_back(target);
                }
            }
        }
    }
}

/// Phase two: sweep the region phase one proved is code.
///
/// A function nothing calls directly — reached only through a pointer, or only
/// from an overlay we have not recompiled — is invisible to the call graph but
/// still sits in the middle of .text between two functions that are not. So
/// walk the section end to end, and wherever there is a gap the reachable set
/// did not cover, try to read a function out of it.
///
/// Past the reachable end the same sweep keeps going, and that is what finds
/// the true end of .text: the first place a function refuses to validate once
/// there are no more calls vouching for the address.
void sweep(const Rom &rom, const SectionInfo &section, Recovered &out, AnalysisReport &report) {
    const uint32_t section_end = section.vram + section.size;
    uint32_t vram = section.vram;

    // How far past the last reachable function to keep looking before calling
    // it data. One failure is enough when we are already past known code, but
    // a little slack absorbs an alignment gap between two blocks of functions.
    constexpr uint32_t kSlack = 0x40;
    uint32_t limit = out.text_end + kSlack;

    while (vram < section_end && vram < limit) {
        // Step over anything already known, and over the padding after it.
        auto known = out.functions.find(vram);
        if (known != out.functions.end()) {
            vram = skip_padding(rom, section, known->second);
            continue;
        }

        // A function may already cover this address without starting on it.
        auto before = out.functions.upper_bound(vram);
        if (before != out.functions.begin()) {
            --before;
            if (before->second > vram) {
                vram = skip_padding(rom, section, before->second);
                continue;
            }
        }

        vram = skip_padding(rom, section, vram);
        if (vram >= section_end || vram >= limit) {
            break;
        }

        const Walk walk = walk_function(rom, section, vram);
        if (walk.valid) {
            out.functions[vram] = walk.end;
            report.from_sweep++;
            if (walk.end > out.text_end) {
                out.text_end = walk.end;
                limit = out.text_end + kSlack;
            }
            vram = walk.end;
            continue;
        }

        if (vram >= out.text_end) {
            // No call vouches for this address and it does not read as code.
            // That is the end of .text.
            break;
        }
        // Inside proven code: a stretch we cannot parse, most likely a jump
        // table's worth of rodata inlined by the compiler. Step over it.
        report.invalid_words++;
        vram += 4;
    }
}

void recover_functions(const Rom &rom, SectionInfo &section, AnalysisReport &report,
                       uint32_t entry) {
    report.words_scanned += section.size / 4;

    Recovered recovered;
    walk_reachable(rom, section, entry, recovered, report);
    if (recovered.functions.empty()) {
        report.notes.push_back("no code was reachable from the entry point of " + section.name);
        return;
    }
    sweep(rom, section, recovered, report);

    section.text_size = recovered.text_end - section.vram;
    // Round out to a code block so the section boundary falls where the
    // compiler put one.
    section.text_size = (section.text_size + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
    section.text_size = std::min(section.text_size, section.size);

    const uint32_t text_end_vram = section.vram + section.text_size;

    // Each function runs to the next. Deliberately contiguous and gap-free:
    // while translating, the recompiler discovers functions of its own —
    // anything a `jal` reaches that we missed becomes a `static_` function —
    // and it can only do that inside a range some function already covers.
    section.functions.reserve(recovered.functions.size());
    for (auto it = recovered.functions.begin(); it != recovered.functions.end(); ++it) {
        auto next = std::next(it);
        const uint32_t end = (next == recovered.functions.end()) ? text_end_vram : next->first;
        if (end <= it->first) {
            continue;
        }
        section.functions.push_back(FunctionRange{it->first, end - it->first});
    }
    report.functions_found += section.functions.size();

    // Now that .text has a boundary, every call in it can be scored. A call
    // that leaves the section is code this analysis did not find — an overlay,
    // in practice. A call that stays inside but does not land on a function
    // start is a boundary we got wrong, and that is the number to watch.
    for (uint32_t vram = section.vram; vram < text_end_vram; vram += 4) {
        const uint32_t word = rom.word(section.rom + (vram - section.vram));
        if ((word >> 26) != 0x03) { // jal
            continue;
        }
        const uint32_t target = 0x80000000u | ((word & 0x03FFFFFFu) << 2);
        if (target < section.vram || target >= text_end_vram) {
            report.calls_outside++;
        } else if (recovered.functions.count(target)) {
            report.calls_on_boundary++;
        } else {
            report.calls_off_boundary++;
        }
    }
}

// ---------------------------------------------------------------------------
// Overlays

/// A code segment DMA'd out of the ROM at runtime.
///
/// Nothing in the ROM format records these. What many libultra games do carry
/// is a table the game itself reads to perform the DMA, four words per entry:
/// where the segment sits in the ROM, and where it is copied to. Finding that
/// table finds the overlays.
///
/// This is a heuristic and it is honest about being one. A game that computes
/// its segment addresses in code rather than tabulating them — Super Mario 64
/// is the well-known example, which hands the linker's `_*SegmentRomStart`
/// symbols straight to the DMA call — has nothing here to find. Those titles
/// need their segments written down in a title record instead.
struct OverlayCandidate {
    uint32_t rom_start = 0;
    uint32_t rom_end = 0;
    uint32_t vram_start = 0;
    uint32_t vram_end = 0;
};

bool plausible_entry(const Rom &rom, const OverlayCandidate &entry) {
    if (entry.rom_start < kBootRomOffset || entry.rom_end <= entry.rom_start) {
        return false;
    }
    if (entry.rom_end > rom.size()) {
        return false;
    }
    // RDRAM is 4MB, 8MB with the Expansion Pak, and a segment is copied into
    // the cached KSEG0 window.
    if (entry.vram_start < 0x80000000u || entry.vram_start >= 0x80800000u) {
        return false;
    }
    if (entry.vram_end <= entry.vram_start || entry.vram_end > 0x80800000u) {
        return false;
    }
    if ((entry.rom_start & 1) || (entry.vram_start & 3)) {
        return false;
    }
    // A compressed segment expands, so the ROM extent may be smaller than the
    // RAM one; it can never be larger.
    const uint32_t rom_span = entry.rom_end - entry.rom_start;
    const uint32_t vram_span = entry.vram_end - entry.vram_start;
    if (rom_span > vram_span) {
        return false;
    }
    return vram_span >= 0x40 && vram_span < 0x400000u;
}

/// Does a candidate segment actually look like code? A segment table's entries
/// point at graphics and audio data far more often than at code, and
/// recompiling a texture bank is worse than skipping it.
bool looks_like_code(const Rom &rom, const OverlayCandidate &entry) {
    const uint32_t span = std::min<uint32_t>(entry.rom_end - entry.rom_start, 0x400);
    if (span < 0x40) {
        return false;
    }
    size_t valid = 0;
    size_t total = 0;
    bool has_return = false;
    for (uint32_t offset = 0; offset + 4 <= span; offset += 4) {
        const uint32_t word = rom.word(entry.rom_start + offset);
        const rabbitizer::InstructionCpu insn(word, entry.vram_start + offset);
        total++;
        if (insn.isValid()) {
            valid++;
        }
        if (word == kJrRa) {
            has_return = true;
        }
    }
    return total > 0 && has_return && (valid * 10) >= (total * 9);
}

void find_overlays(const Rom &rom, Analysis &analysis) {
    if (analysis.sections.empty()) {
        return;
    }
    const SectionInfo boot = analysis.sections.front();

    // The table lives in the boot segment's data, past the code. Scanning the
    // code as well would only produce false positives.
    const size_t scan_start = boot.rom + boot.text_size;
    const size_t scan_end = std::min<size_t>(size_t(boot.rom) + kBootCopySize, rom.size());

    std::vector<OverlayCandidate> found;
    std::set<uint32_t> seen_rom_starts;

    for (size_t offset = scan_start; offset + 16 <= scan_end; offset += 4) {
        OverlayCandidate entry{rom.word(offset), rom.word(offset + 4), rom.word(offset + 8),
                               rom.word(offset + 12)};
        if (!plausible_entry(rom, entry) || !looks_like_code(rom, entry)) {
            continue;
        }
        if (!seen_rom_starts.insert(entry.rom_start).second) {
            continue;
        }
        found.push_back(entry);
    }

    if (found.empty()) {
        analysis.report.notes.push_back(
            "no overlay table found; only the boot segment was recompiled. A game that "
            "loads code at runtime needs its segments written down in a title record.");
        return;
    }

    std::sort(found.begin(), found.end(),
              [](const OverlayCandidate &a, const OverlayCandidate &b) {
                  return a.rom_start < b.rom_start;
              });

    size_t accepted = 0;
    for (size_t i = 0; i < found.size(); i++) {
        const OverlayCandidate &entry = found[i];
        char name[32];
        std::snprintf(name, sizeof(name), "ovl%02zu", i);

        SectionInfo overlay;
        overlay.name = name;
        overlay.rom = entry.rom_start;
        overlay.vram = entry.vram_start;
        overlay.size = entry.rom_end - entry.rom_start;
        recover_functions(rom, overlay, analysis.report, overlay.vram);
        if (overlay.functions.empty()) {
            continue;
        }
        overlay.size = overlay.text_size;
        analysis.sections.push_back(std::move(overlay));
        accepted++;
    }

    analysis.report.notes.push_back("found " + std::to_string(accepted) +
                                    " overlay segment(s) by scanning for a DMA table");
}

} // namespace

Analysis analyze(const Rom &rom) {
    Analysis analysis;

    SectionInfo boot;
    boot.name = "boot";
    boot.rom = kBootRomOffset;
    boot.vram = rom.load_address;
    boot.size = uint32_t(std::min<size_t>(
        kBootCopySize, rom.size() > kBootRomOffset ? rom.size() - kBootRomOffset : 0));
    recover_functions(rom, boot, analysis.report, rom.load_address);

    // The section is narrowed to the code once we know where it ends. The
    // recompiler sizes the last function it discovers for itself against the
    // section's end, so a section that ran on into rodata would hand it a
    // function several hundred kilobytes long made mostly of texture data.
    if (boot.text_size > 0) {
        boot.size = boot.text_size;
    }
    analysis.sections.push_back(std::move(boot));

    if (!rom.load_address_verified) {
        analysis.report.notes.push_back(
            "the boot segment's load address could not be confirmed from the calls in it; "
            "the recovered addresses may all be off by a fixed amount");
    }

    find_overlays(rom, analysis);
    return analysis;
}

} // namespace n64rip
