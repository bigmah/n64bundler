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
#include <fstream>
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

/// Could this be the address of code in this image?
///
/// RDRAM is 4MB, 8MB with the Expansion Pak, and running code lives in the
/// cached KSEG0 window that starts at 0x80000000. An absolute jump to anywhere
/// else is not a call this program makes -- it is a sign we are not reading
/// code that runs at the address we think it does.
///
/// Super Mario 64 is where this earns its keep. Just past the end of .text sits
/// the exception handler, which is real MIPS but is copied to 0x80000180
/// before it runs, so read in place its jumps come out at 0x84001068. Without
/// this check the sweep accepts it as a function, and the recompiler translates
/// a jump to an address that does not exist.
bool plausible_code_address(uint32_t address) {
    return address >= 0x80000000u && address < 0x80800000u && (address & 3) == 0;
}

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
            case InstrId::cpu_jal: {
                const uint32_t target = uint32_t(insn.getBranchVramGeneric());
                if (!plausible_code_address(target)) {
                    return walk;
                }
                walk.call_targets.push_back(target);
                break;
            }

            case InstrId::cpu_j: {
                // Either a jump inside the function or a tail call out of it.
                // The compilers used on this hardware reach anywhere in a
                // function with an ordinary branch, so a `j` to somewhere we
                // have not already established is part of this function is a
                // tail call, and tail calls end functions.
                const uint32_t target = uint32_t(insn.getBranchVramGeneric());
                if (!plausible_code_address(target)) {
                    return walk;
                }
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
                if (target.has_value() && plausible_code_address(*target)) {
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
    std::map<uint32_t, std::string> names;  // start -> libultra name, where known
    /// What a signature said a function is, where the name could not be used
    /// because the runtime has no implementation of it. Kept for the report.
    std::map<uint32_t, std::string> known_names;
    uint32_t text_end = 0;
};

/// Every address in this section that a `jal` names.
///
/// A call target is the strongest statement the image makes about where a
/// function begins -- stronger than anything inferred by walking, and on a par
/// with a signature match. Collected once because it is wanted twice: to score
/// the boundaries, and to protect them from being merged away.
std::set<uint32_t> collect_call_targets(const Rom &rom, const SectionInfo &section,
                                        uint32_t text_end, const Recovered &recovered) {
    std::set<uint32_t> targets;
    // Only calls inside a function the walk or the sweep validated. The gaps
    // between them are rodata that happens to sit inside .text -- a jump table,
    // a block of floats -- and a word there that looks like a `jal` is not one.
    // Trusting those invents boundaries in the middle of working functions, and
    // then a branch crossing one condemns the whole function to a stub.
    for (const auto &[start, end] : recovered.functions) {
        for (uint32_t vram = start; vram < end && vram < text_end; vram += 4) {
            const uint32_t word = rom.word(section.rom + (vram - section.vram));
            if ((word >> 26) != 0x03) { // jal
                continue;
            }
            const uint32_t target = 0x80000000u | ((word & 0x03FFFFFFu) << 2);
            if (target >= section.vram && target < text_end) {
                targets.insert(target);
            }
        }
    }
    return targets;
}

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

/// Name the libultra functions in a section, and fix its boundaries while we
/// are at it.
///
/// A signature match is worth more than a name. It is the only place in this
/// whole analysis where something external states, rather than infers, that a
/// function begins at an address and is exactly this many bytes long. So a
/// match also corrects the boundary it landed on, and creates one where the
/// walk had found nothing.
///
/// Every 4-aligned address is tried, not only the starts already found. The
/// functions most worth naming are the ones nothing calls directly -- a thread
/// entry point handed to osCreateThread, an interrupt handler installed by
/// address -- and those are precisely the ones the call graph misses.
void name_from_signatures(const Rom &rom, const SectionInfo &section,
                          const n64sig::Database &signatures, const RuntimeProvides *provides,
                          Recovered &out, AnalysisReport &report) {
    if (out.text_end <= section.vram) {
        return;
    }

    // A window of the section's words, in host order, so the matcher can work
    // on plain memory rather than re-reading the ROM word by word.
    const size_t words = (out.text_end - section.vram) / 4;
    std::vector<uint32_t> code(words);
    for (size_t i = 0; i < words; i++) {
        code[i] = rom.word(section.rom + i * 4);
    }

    // Where a match already claims bytes, so a second signature cannot be
    // matched inside the body of the first.
    std::vector<bool> claimed(words, false);

    for (size_t i = 0; i < words; i++) {
        if (claimed[i]) {
            continue;
        }
        const std::vector<const n64sig::Signature *> candidates =
            signatures.candidates(code.data() + i, words - i);
        if (candidates.empty()) {
            continue;
        }

        const n64sig::Signature *best = nullptr;
        // The names that matched just as well as the winner did. Usually one;
        // more than one is a tie a fingerprint cannot break, because what
        // separates those functions is a field the linker filled in and a
        // signature therefore masks out.
        std::vector<std::string> equally_good;
        for (const n64sig::Signature *candidate : candidates) {
            if (!n64sig::Database::matches(*candidate, code.data() + i, words - i)) {
                continue;
            }
            // Two library functions can share a prefix; the longer match is the
            // one that explains more of the image.
            if (best == nullptr || candidate->words.size() > best->words.size()) {
                best = candidate;
                equally_good.assign(1, candidate->name);
            } else if (candidate->words.size() == best->words.size() &&
                       std::find(equally_good.begin(), equally_good.end(), candidate->name) ==
                           equally_good.end()) {
                equally_good.push_back(candidate->name);
            }
        }

        if (best == nullptr) {
            continue;
        }
        if (equally_good.size() > 1) {
            std::sort(equally_good.begin(), equally_good.end());
            report.ambiguous_names.push_back(
                {section.vram + uint32_t(i * 4), best->name, std::move(equally_good)});
        }

        const uint32_t vram = section.vram + uint32_t(i * 4);
        const uint32_t size = uint32_t(best->size_bytes());

        if (!out.functions.count(vram)) {
            report.named_new_boundaries++;
        }
        // The signature knows the real extent, so it replaces whatever the walk
        // decided -- including splitting a function the walk ran together with
        // its neighbour. That is worth taking even when the name is not.
        out.functions[vram] = vram + size;

        // Keep the name only if the runtime has something to put in this
        // function's place. The recompiler stops emitting a body for any name
        // it recognises, so naming a function nothing implements trades a
        // translation that works for a link error. Left nameless, the function
        // is recompiled from the ROM like any other -- and if it turns out to
        // drive hardware, the stub pass below catches it.
        if (provides != nullptr && provides->count(best->name) == 0) {
            report.names_without_implementations++;
            out.known_names[vram] = best->name;
        } else {
            out.names[vram] = best->name;
            report.named_functions++;
        }

        for (size_t w = i; w < i + best->words.size() && w < words; w++) {
            claimed[w] = true;
        }
        i += best->words.size() - 1;
    }

    // What the exact pass could not name, and what it nearly was.
    //
    // Only at boundaries the walk already recovered, and only against the
    // signatures that share this function's first instructions, so this costs
    // one comparison per candidate per function rather than a search.
    for (auto it = out.functions.begin(); it != out.functions.end(); ++it) {
        const uint32_t vram = it->first;
        if (out.names.count(vram) != 0 || out.known_names.count(vram) != 0) {
            continue;
        }
        if (vram < section.vram || vram >= out.text_end) {
            continue;
        }
        const size_t at = (vram - section.vram) / 4;
        const size_t length = (std::min(it->second, out.text_end) - vram) / 4;
        if (length < 4) {
            continue;
        }
        const n64sig::Signature *closest = nullptr;
        double best_share = 0.0;
        for (const n64sig::Signature *candidate :
             signatures.candidates(code.data() + at, words - at)) {
            const double share =
                n64sig::Database::resemblance(*candidate, code.data() + at, words - at, length);
            if (share > best_share) {
                best_share = share;
                closest = candidate;
            }
        }
        // High enough that a coincidence of prologues does not reach it, low
        // enough that a revision's worth of differences still does.
        constexpr double kWorthSaying = 0.70;
        if (closest != nullptr && best_share >= kWorthSaying) {
            report.resemblances.push_back({vram, closest->name, best_share,
                                           provides != nullptr &&
                                               provides->count(closest->name) != 0});
        }
    }
}

/// Make every function contain its own branches.
///
/// A relative branch never leaves the function it is in -- except as a tail
/// call to the top of another one, which the recompiler recognises. Anything
/// else is a boundary this analysis invented, and the recompiler refuses to
/// translate the function rather than guess.
///
/// Switch statements are what invent them. A `jr $v0` dispatch sends control
/// into case bodies through a table in rodata, and a table is not a branch, so
/// nothing in the instruction stream connects the dispatch to the cases. The
/// walk reaches a `jr $ra` at the end of one case, concludes the function has
/// ended, and reads the next case as a new function -- which then branches
/// backwards into the shared code above it, and the recompiler stops.
///
/// Parsing the jump tables would find the cases properly and is what a
/// disassembler with time on its hands would do. This gets the same result from
/// the other end: wherever a branch crosses a boundary, the boundary was wrong,
/// so dissolve it. Merging can expose further crossings as the ranges grow, so
/// it repeats until nothing moves.
void enforce_branch_containment(const Rom &rom, const SectionInfo &section, uint32_t text_end,
                                const std::set<uint32_t> &call_targets, Recovered &out,
                                AnalysisReport &report) {
    constexpr int kMaxPasses = 8;

    for (int pass = 0; pass < kMaxPasses; pass++) {
        std::vector<uint32_t> starts;
        starts.reserve(out.functions.size());
        for (const auto &entry : out.functions) {
            starts.push_back(entry.first);
        }
        if (starts.size() < 2) {
            return;
        }
        const std::set<uint32_t> start_set(starts.begin(), starts.end());
        std::set<uint32_t> doomed;

        for (size_t i = 0; i < starts.size(); i++) {
            const uint32_t begin = starts[i];
            const uint32_t end = (i + 1 < starts.size()) ? starts[i + 1] : text_end;

            for (uint32_t vram = begin; vram < end; vram += 4) {
                const uint32_t word = rom.word(section.rom + (vram - section.vram));
                const rabbitizer::InstructionCpu insn(word, vram);
                if (!insn.isValid() || !insn.isBranch()) {
                    continue;
                }
                const uint32_t target = uint32_t(insn.getBranchVramGeneric());
                if (target >= begin && target < end) {
                    continue; // inside this function, which is the normal case
                }
                if (target < section.vram || target >= text_end) {
                    continue; // outside the recovered code; not a boundary question
                }
                if (start_set.count(target) != 0) {
                    continue; // the top of another function, which is a tail call
                }

                // The branch lands in the middle of some other function, so the
                // two are really one. Dissolve every boundary between them.
                const size_t other =
                    size_t(std::upper_bound(starts.begin(), starts.end(), target) -
                           starts.begin()) - 1;
                const size_t from = std::min(i, other);
                const size_t to = std::max(i, other);
                for (size_t k = from + 1; k <= to; k++) {
                    doomed.insert(starts[k]);
                }
            }
        }

        size_t removed = 0;
        for (uint32_t start : doomed) {
            // Two kinds of boundary outrank this pass, because both were
            // stated by the image rather than inferred from it: one a
            // signature matched, and one a `jal` points at. Dissolving a call
            // target would also be self-defeating -- the recompiler would
            // rediscover it while translating, as a function this analysis
            // never saw and so never checked for anything.
            if (out.names.count(start) != 0 || call_targets.count(start) != 0 ||
                start == section.vram) {
                continue;
            }
            out.functions.erase(start);
            removed++;
        }
        report.merged_boundaries += removed;
        if (removed == 0) {
            return;
        }
    }

    report.notes.push_back("branch containment in " + section.name +
                           " did not settle; some functions may still be split wrongly");
}

/// Does a branch still leave this function, after merging has done what it can?
///
/// Merging fixes the boundaries this analysis invented. What it cannot fix is a
/// region that has no correct division into functions: hand-written assembly
/// with several entry points sharing one body. libultra's exception preamble is
/// the example every game carries -- fifteen `jal`s arrive at an address in the
/// middle of a block whose code branches freely above and below it, so the
/// address is certainly a function start and just as certainly not the top of
/// anything self-contained.
///
/// The recompiler will not translate that, and it is right not to. Stubbing is
/// the alternative to failing the build over it, and for this particular region
/// it is also correct, since the runtime handles exceptions itself. Where it is
/// not correct it is at least visible: every one is counted and reported.
bool branches_escape(const Rom &rom, const SectionInfo &section,
                     const std::set<uint32_t> &starts, uint32_t text_end,
                     const FunctionRange &function) {
    const uint32_t end = function.vram + function.size;
    for (uint32_t vram = function.vram; vram < end; vram += 4) {
        const uint32_t word = rom.word(section.rom + (vram - section.vram));

        uint32_t target;
        if ((word >> 26) == 0x02) {
            // `j`. Rabbitizer does not call this a branch and the distinction
            // matters here: an unconditional jump out of a function is how a
            // compiler writes a tail call, and one that leaves the section is
            // a tail call into a segment loaded elsewhere. The recompiler has
            // no way to express that -- unlike a `jal`, which can fall back to
            // the runtime's function lookup.
            target = (vram & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
        } else {
            const rabbitizer::InstructionCpu insn(word, vram);
            if (!insn.isValid() || !insn.isBranch()) {
                continue;
            }
            target = uint32_t(insn.getBranchVramGeneric());
        }
        if (target >= function.vram && target < end) {
            continue;
        }
        if (target < section.vram || target >= text_end) {
            // Out of this section. For a conditional branch that is nonsense
            // and the function cannot be translated; for a jump it is an
            // ordinary tail call into another segment, which the recompiler
            // resolves against every section at once -- something this
            // function, looking at one section, cannot see. Leaving it alone
            // and letting the recompiler refuse it if it must is what keeps a
            // game with two segments from losing every function that tail
            // calls across them: Mario Builder 64 lost 113 that way.
            if ((word >> 26) == 0x02) {
                continue;
            }
            return true;
        }
        if (starts.count(target) != 0) {
            continue; // a tail call, which the recompiler handles
        }
        // A branch into the middle of another function in this section. The
        // two are really one and the recompiler cannot express that. A `jal`
        // in the same position could fall back to the runtime's function
        // lookup; a branch has no such escape.
        return true;
    }
    return false;
}

/// Whether a word hands control somewhere else and does not come back.
///
/// `jal` and `jalr` are missing on purpose: a call returns to the instruction
/// after its delay slot, so a function ending in one still falls through.
/// `syscall` is here because a game that dispatches through the exception
/// handler -- see SyscallDispatch -- does not come back to the stub either,
/// and its stubs are two instructions with no room for anything else.
bool leaves_for_good(uint32_t word) {
    const uint32_t op = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    switch (op) {
        case 0x00: { // SPECIAL
            const uint32_t funct = word & 0x3F;
            return funct == 0x08 || funct == 0x0C || funct == 0x0D; // jr, syscall, break
        }
        case 0x02:                                       // j
            return true;
        case 0x04:                                       // beq, and `b` is beq zero, zero
        case 0x14:                                       // beql
            return rs == 0 && rt == 0;
        case 0x01:                                       // REGIMM
            return rs == 0 && (rt == 0x01 || rt == 0x03); // bgez zero, bgezl zero
        default:
            return false;
    }
}

/// Whether control runs off the end of the body this function was given.
///
/// A boundary the analysis invented can land anywhere, and `branches_escape`
/// only catches the ones that cut a function where it branches. The quieter
/// half of the same mistake is a cut through straight-line code: what is left
/// above it has no branch to escape through and no return to reach, so nothing
/// about it looks wrong. The recompiler translates it happily into a C
/// function that computes a few values and falls off the end, which is to say
/// one that returns whatever the last arithmetic happened to leave behind.
///
/// Banjo-Tooie's `atan2` is five instructions of exactly this:
///
///     80013B7C: mul.s  f16, f12, f12
///     80013B80: nop
///     80013B84: mul.s  f0, f14, f14
///     80013B88: add.s  f0, f0, f16      <- what the caller reads as the angle
///     80013B8C: sqrt.s f16, f0
///     80013B90:                         <- a boundary nothing ever calls
///
/// The game converts a horizontal field of view to a vertical one with it, and
/// is told that forty degrees across is eight hundred and forty thousand
/// degrees down -- x^2 + y^2, the last thing `f0` held. Nothing crashes and
/// nothing is reported. The world is drawn through a lens ten and a half times
/// too wide for the rest of the game.
///
/// A function that has no way to leave itself is not a function, so this is
/// asked of every recovered body: ignoring the padding at the end, the last
/// instruction must either transfer control for good or sit in the delay slot
/// of one that does.
bool falls_off_the_end(const Rom &rom, const SectionInfo &section,
                       const FunctionRange &function) {
    if (function.size < 8) {
        // One instruction and its delay slot is the smallest thing that can be
        // judged at all, and a game's hand-written tables are full of them.
        return false;
    }
    const auto word_at = [&](uint32_t vram) {
        return rom.word(section.rom + (vram - section.vram));
    };

    // Trailing `nop`s are the alignment padding between one function and the
    // next, and a final jump's delay slot is often one of them; neither says
    // anything about whether the function ends.
    uint32_t last = function.vram + function.size - 4;
    while (last > function.vram && word_at(last) == 0) {
        last -= 4;
    }
    if (leaves_for_good(word_at(last))) {
        return false;
    }
    return last <= function.vram || !leaves_for_good(word_at(last - 4));
}

/// Give a function back the body a boundary cut it off from.
///
/// Hand-written assembly reaches one body from several entry points, and each
/// entry point is a function: something calls it by name, and a `jal` names the
/// first instruction of a function. But the entries are laid out one after
/// another in front of the body they share, so putting a boundary at the second
/// one -- which a call proves belongs there -- takes the body away from the
/// first, and what is left is a handful of instructions that branch forward
/// into somebody else's function.
///
/// Banjo-Tooie's trigonometry is three of these, and it is not a corner:
///
///     800136D0: lui at, 0x8004        <- one entry: sine of an angle in units
///     800136D4: lwc1 f0, 0x16B0(at)
///     800136D8: lui at, 0x8004
///     800136DC: beq  zero, zero, 0x800136F8
///     800136E0: lwc1 f2, 0x16B4(at)
///     800136E4: lui at, 0x8004        <- another: sine of an angle in degrees
///     ...
///     800136F8: mul.s f0, f0, f12     <- the body both of them run
///     ...
///     80013720: jr ra
///
/// The right reading is two functions that overlap, each with its own copy of
/// the tail -- which is exactly what the recompiler makes of two entries with
/// their own instruction lists, since it translates each function from the
/// words it was given rather than from a shared range.
///
/// So the function is walked again from its own first instruction, with nothing
/// but its own branches deciding where it ends -- the same walk that recovered
/// it in the first place, which already refuses to run past a terminator that
/// nothing branches over. If that lands past the boundary, the boundary was a
/// second entry point rather than the end of anything, and the function keeps
/// the body.
///
/// A region that genuinely does not divide into functions -- libultra's
/// exception preamble, where branches go both ways across every entry -- is not
/// touched, because a walk from inside one gives up the moment it sees a branch
/// to before where it started.
bool needs_stub(const Rom &rom, const SectionInfo &section, const FunctionRange &function);

/// True when the function was extended and now holds itself together. The size
/// is put back if it did not, so that a function this cannot help is stubbed
/// exactly as it was before.
bool settle_shared_tail(const Rom &rom, const SectionInfo &section,
                        const std::set<uint32_t> &starts, uint32_t text_end,
                        FunctionRange &function) {
    const uint32_t was = function.size;
    const Walk walk = walk_function(rom, section, function.vram);
    if (!walk.valid || walk.end > text_end || walk.end <= function.vram + was) {
        return false;
    }
    function.size = walk.end - function.vram;
    // The tail is code this function had not been looked at with, so both
    // questions are asked again over the whole of it.
    if (branches_escape(rom, section, starts, text_end, function) ||
        falls_off_the_end(rom, section, function) ||
        needs_stub(rom, section, function)) {
        function.size = was;
        return false;
    }
    return true;
}

/// The libultra accessors that are too short to fingerprint.
///
/// `n64sig` will not make a signature out of a function shorter than six
/// instructions, and it is right not to: four instructions of MIPS are not
/// distinctive and a wrong name puts the wrong implementation in a function's
/// place. But libultra has a handful of functions that are three instructions
/// -- read one hardware register, return it -- and they are the ones a game
/// calls most often. Left unnamed they are stubbed, because they touch
/// hardware, and a stubbed function returns whatever happened to be in v0.
/// `osGetCount` returning garbage is a game whose every timer is wrong.
///
/// What makes naming them safe where a short signature would not be is that
/// the register decides it. There is exactly one libultra function that reads
/// the audio interface's length register and returns it, and a function that
/// does only that is that function. The name is still checked against what the
/// runtime implements before it is used.
struct TinyAccessor {
    /// A hardware register address, or kCop0 plus a coprocessor 0 register.
    uint32_t source;
    const char *name;
};

constexpr uint32_t kCop0 = 0xC0000000u;

constexpr TinyAccessor kTinyAccessors[] = {
    {0xA4500004u, "osAiGetLength"},
    {0xA450000Cu, "osAiGetStatus"},
    {0xA4600010u, "osPiGetStatus"},
    {0xA410000Cu, "osDpGetStatus"},
    {kCop0 | 9u, "osGetCount"},   // C0_COUNT
};

/// The libultra name for a function that does nothing but read one register
/// into v0 and return, or an empty string.
std::string tiny_accessor_name(const Rom &rom, const SectionInfo &section,
                               const FunctionRange &function) {
    // Three instructions and a return; four words with the delay slot, and at
    // most six once a compiler's padding is allowed for.
    if (function.size < 8 || function.size > 0x18) {
        return {};
    }

    constexpr uint32_t kJrRa = 0x03E00008u;
    constexpr uint32_t kReturnRegister = 2; // v0

    bool returns = false;
    uint32_t source = 0;
    size_t meaningful = 0;
    uint32_t upper = 0;
    bool have_upper = false;

    for (uint32_t offset = 0; offset < function.size; offset += 4) {
        const uint32_t word = rom.word(section.rom + (function.vram + offset - section.vram));
        if (word == 0) {
            continue; // nop, and the delay slots here are all nops
        }
        if (word == kJrRa) {
            returns = true;
            continue;
        }
        meaningful++;
        if ((word >> 26) == 0x0F) { // lui
            upper = (word & 0xFFFF) << 16;
            have_upper = true;
            continue;
        }
        if ((word >> 26) == 0x23 && ((word >> 16) & 0x1F) == kReturnRegister && have_upper) {
            // lw v0, imm(base), where base was just built by the lui.
            int32_t immediate = int16_t(word & 0xFFFF);
            source = upper + uint32_t(immediate);
            continue;
        }
        if ((word >> 26) == 0x10 && ((word >> 21) & 0x1F) == 0 &&
            ((word >> 16) & 0x1F) == kReturnRegister) {
            // mfc0 v0, rd
            source = kCop0 | ((word >> 11) & 0x1F);
            continue;
        }
        return {}; // anything else and this is not a bare accessor
    }

    // A lui and a load, or a single mfc0. More than that is a function doing
    // something, and this is only for the ones that do nothing.
    if (!returns || source == 0 || meaningful > 2) {
        return {};
    }

    for (const TinyAccessor &accessor : kTinyAccessors) {
        if (accessor.source == source) {
            return accessor.name;
        }
    }
    return {};
}

/// Does this function drive coprocessor 0 in a way the recompiler cannot
/// translate?
///
/// COP0 is the CPU's control registers -- the TLB, the interrupt mask, the
/// cycle counter. The recompiler translates exactly one of them, Status, and
/// refuses the rest, which is correct: there is no TLB behind a flat block of
/// host memory and no meaning to writing one.
///
/// A decompilation never hits this, because every function that touches COP0
/// is a libultra function, and libultra is named in the elf and substituted by
/// the runtime. From recovered symbols it only stays true for the functions a
/// signature managed to name. The rest -- osUnmapTLBAll being the one Super
/// Mario 64 lands on -- arrive nameless and get translated.
///
/// Stubbing them is right rather than merely expedient. The unit here is the
/// function, and a function full of COP0 is a hardware routine in its
/// entirety; a no-op TLB unmap against memory that was never mapped is exactly
/// what it should do. What would not be right is inventing values for COP0
/// reads mid-function and hoping the surrounding code copes.
///
/// Every one of these is still a gap, so they are counted and reported. The
/// way to close one is to widen the signature database until it has a name.
bool needs_stub(const Rom &rom, const SectionInfo &section, const FunctionRange &function) {
    for (uint32_t offset = 0; offset < function.size; offset += 4) {
        const uint32_t vram = function.vram + offset;
        const uint32_t word = rom.word(section.rom + (vram - section.vram));

        // The RCP's registers used to be stubbed here, because the runtime
        // mapped no memory at their addresses and a store to one took the
        // process down. The host now backs that window with zeroed memory, so
        // the function runs: everything it does besides the register access is
        // real, and the access itself reads zero and discards writes.

        // A 64-bit float conversion. The recompiler has no case for `trunc.l.d`
        // and its family, so a function containing one takes the build down
        // rather than producing a wrong answer -- which means it has to be
        // caught here.
        //
        // Every one of these is libultra's own `__d_to_ll` and friends, which
        // the runtime implements. A named one never reaches this test; an
        // unnamed one is stubbed, and the stub is counted so the cost of
        // having no signature for it is visible.
        if ((word >> 26) == 0x11) { // COP1
            // Only an arithmetic COP1 instruction has a function field at all.
            // The rest of the opcode -- mfc1, mtc1, cfc1, the branches -- puts
            // register numbers and branch offsets in those bits, and reading
            // them as a function code stubs a third of the game.
            const uint32_t fmt = (word >> 21) & 0x1F;
            constexpr uint32_t kFmtSingle = 16;
            constexpr uint32_t kFmtDouble = 17;
            constexpr uint32_t kFmtWord = 20;
            constexpr uint32_t kFmtLong = 21;
            const bool arithmetic = fmt == kFmtSingle || fmt == kFmtDouble ||
                                    fmt == kFmtWord || fmt == kFmtLong;
            if (arithmetic) {
                const uint32_t function_field = word & 0x3F;
                const bool converts_to_long = function_field == 0x25 || // cvt.l
                                              function_field == 0x08 || // round.l
                                              function_field == 0x09 || // trunc.l
                                              function_field == 0x0A || // ceil.l
                                              function_field == 0x0B;   // floor.l
                if (fmt == kFmtLong || converts_to_long) {
                    return true;
                }
            }
        }

        // `cache`, the CPU's cache management instruction. There is no cache
        // to manage here and the recompiler refuses to translate it, so a
        // function containing one has to go whole: it is osInvalDCache,
        // osWritebackDCache, or one of their callers in libultra.
        if ((word >> 26) == 0x2F) {
            return true;
        }

        if ((word >> 26) != 0x10) { // not COP0
            continue;
        }
        const uint32_t rs = (word >> 21) & 0x1F;
        const uint32_t rd = (word >> 11) & 0x1F;
        constexpr uint32_t kCop0Status = 12;
        // The status register is modelled, so a function that only touches
        // that one runs. Reads hand back what the runtime holds, with the
        // interrupt mask a running console has; writes keep the FR bit, which
        // decides how the odd float registers are addressed, and accept and
        // drop the interrupt bits, because there are no interrupts here to
        // enable or mask.
        //
        // That is what a function turning interrupts off around a few
        // instructions is doing, and stubbing one loses the few instructions
        // rather than the interrupts. Every other coprocessor 0 register --
        // Cause, EPC, the TLB -- still stubs the function, because the runtime
        // has nothing to say for those.
        if ((rs == 0x00 /* mfc0 */ || rs == 0x04 /* mtc0 */) && rd == kCop0Status) {
            continue;
        }
        return true;
    }
    return false;
}

void recover_functions(const Rom &rom, SectionInfo &section, AnalysisReport &report,
                       uint32_t entry, const n64sig::Database *signatures,
                       const RuntimeProvides *provides,
                       const std::vector<uint32_t> *extra_entries = nullptr) {
    report.words_scanned += section.size / 4;

    Recovered recovered;
    walk_reachable(rom, section, entry, recovered, report);
    // Calls into this segment from code already recovered elsewhere. Each one
    // is a `jal`, so each one names a function outright, and following them is
    // what makes a segment reachable at all: an overlay's entry point is not
    // its first byte, and the code that calls into it is in another section.
    //
    // This is also what gets the sweep past a block of rodata in the middle of
    // a segment. The sweep stops at the first stretch past known code that
    // does not read as code; a call from outside proves there is more code
    // beyond it, and unlike a guess, a walk from that address either validates
    // as a whole function or is discarded.
    if (extra_entries != nullptr) {
        for (uint32_t seed : *extra_entries) {
            walk_reachable(rom, section, seed, recovered, report);
        }
    }
    if (recovered.functions.empty()) {
        report.notes.push_back("no code was reachable from the entry point of " + section.name);
        return;
    }

    sweep(rom, section, recovered, report);

    if (signatures != nullptr) {
        name_from_signatures(rom, section, *signatures, provides, recovered, report);
    }

    const std::set<uint32_t> call_targets =
        collect_call_targets(rom, section, recovered.text_end, recovered);

    // A tail call names a function too.
    //
    // A `j` leaving the function it sits in is how a compiler writes `return
    // f(...)`, and its target is the first instruction of f exactly as a
    // `jal`'s is. The sweep has no reason to see that: it walks a function to
    // its terminator and the terminator here *is* the jump, so whatever
    // follows never gets looked at and the target keeps whatever boundary it
    // inherited.
    //
    // Left alone the cost is not a missing name, it is a missing function. The
    // stub pass sees a jump out of the function to something that is not a
    // function start, decides the code does not divide into functions, and
    // stubs the whole thing. In Mario Builder 64 that silently removed the
    // routine that runs four of the game's initialisers and tail calls into
    // the fifth.
    for (const auto &[start, end] : std::map<uint32_t, uint32_t>(recovered.functions)) {
        for (uint32_t vram = start; vram < end; vram += 4) {
            const uint32_t word = rom.word(section.rom + (vram - section.vram));
            if ((word >> 26) != 0x02) { // j
                continue;
            }
            const uint32_t target = (vram & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
            if (target >= start && target < end) {
                continue; // a long branch inside this function, not a tail call
            }
            if (target < section.vram || target >= recovered.text_end) {
                continue; // out of the section; the recompiler resolves those
            }
            if (recovered.functions.count(target) != 0) {
                continue; // already a boundary
            }
            auto containing = recovered.functions.upper_bound(target);
            if (containing == recovered.functions.begin()) {
                continue;
            }
            --containing;
            if (containing->second <= target) {
                continue;
            }
            recovered.functions[target] = containing->second;
            containing->second = target;
            report.split_boundaries++;
        }
    }

    // A `jal` names the first instruction of a function, so a call landing in
    // the middle of one we recovered means we ran two functions together.
    // Split them. Left alone the recompiler finds the same boundary while
    // translating and invents a `static_` function at it -- which is worse,
    // because a static cannot be named, cannot be stubbed, and is discovered
    // too late for any of the checks here to have looked at it.
    //
    // This runs before branch containment on purpose. Containment is the
    // inverse operation and it already refuses to dissolve a call target, so
    // the two compose: split on what a call proves, merge on what a branch
    // proves, and a boundary that both point at stays split.
    for (uint32_t target : call_targets) {
        if (recovered.functions.count(target) != 0) {
            continue;
        }
        auto containing = recovered.functions.upper_bound(target);
        if (containing == recovered.functions.begin()) {
            continue;
        }
        --containing;
        if (containing->second <= target) {
            continue; // in a gap rather than inside a function
        }
        recovered.functions[target] = containing->second;
        containing->second = target;
        report.split_boundaries++;
    }

    enforce_branch_containment(rom, section, recovered.text_end, call_targets, recovered, report);

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
        auto named = recovered.names.find(it->first);
        FunctionRange function{it->first, end - it->first,
                               named == recovered.names.end() ? std::string() : named->second};
        auto known = recovered.known_names.find(it->first);
        if (known != recovered.known_names.end()) {
            function.known_as = known->second;
        }
        section.functions.push_back(std::move(function));
    }
    // A named function is the runtime's problem, not ours: the recompiler
    // substitutes its own implementation for every libultra name it knows. Only
    // the ones no signature reached need stubbing.
    std::set<uint32_t> final_starts;
    for (const FunctionRange &function : section.functions) {
        final_starts.insert(function.vram);
    }
    for (FunctionRange &function : section.functions) {
        if (function.name.empty()) {
            // Before deciding a hardware routine has to be stubbed, see
            // whether it is one of the handful too short to have a signature.
            const std::string tiny = tiny_accessor_name(rom, section, function);
            if (!tiny.empty() && (provides == nullptr || provides->count(tiny) != 0)) {
                function.name = tiny;
                report.named_functions++;
                report.named_by_shape++;
            }
        }
        if (!function.name.empty()) {
            continue;
        }
        if (needs_stub(rom, section, function)) {
            function.stub = true;
            report.stubbed_functions++;
            if (!function.known_as.empty()) {
                report.stubbed_by_name.push_back(function.known_as);
            }
        } else if (branches_escape(rom, section, final_starts, text_end_vram, function) ||
                   falls_off_the_end(rom, section, function)) {
            if (settle_shared_tail(rom, section, final_starts, text_end_vram, function)) {
                report.extended_over_shared_tail++;
            } else {
                function.stub = true;
                report.stubbed_unstructured++;
            }
        }
    }

    report.functions_found += section.functions.size();

}

/// Split a function wherever a call from another section lands inside it.
///
/// The per-section pass cannot see these: it only knows the calls made within
/// one segment, and a game with more than one segment calls between them
/// constantly. The boundary a cross-segment `jal` names is proved exactly the
/// same way as any other -- a call names the first instruction of a function --
/// and left unsplit it is a function the recompiler discovers for itself and
/// turns into a `static_`, which cannot be named or stubbed.
void split_at_cross_section_calls(const Rom &rom, Analysis &analysis) {
    for (const SectionInfo &caller : analysis.sections) {
        for (uint32_t offset = 0; offset < caller.size; offset += 4) {
            const uint32_t word = rom.word(caller.rom + offset);
            if ((word >> 26) != 0x03) { // jal
                continue;
            }
            const uint32_t target = 0x80000000u | ((word & 0x03FFFFFFu) << 2);

            for (SectionInfo &section : analysis.sections) {
                if (target < section.vram || target >= section.vram + section.size) {
                    continue;
                }
                auto at = std::lower_bound(
                    section.functions.begin(), section.functions.end(), target,
                    [](const FunctionRange &f, uint32_t vram) { return f.vram < vram; });
                if (at != section.functions.end() && at->vram == target) {
                    break; // already a boundary
                }
                if (at == section.functions.begin()) {
                    break; // before the first function
                }
                auto containing = std::prev(at);
                if (containing->vram + containing->size <= target) {
                    break; // in a gap between functions
                }
                FunctionRange added;
                added.vram = target;
                added.size = containing->vram + containing->size - target;
                containing->size = target - containing->vram;
                section.functions.insert(at, added);
                analysis.report.split_boundaries++;
                break;
            }
        }
    }
}

/// Look again for branches that escape, now that the split has run.
///
/// `recover_functions` already asks this of every function it recovers, but it
/// works one section at a time and so cannot see a call arriving from another.
/// `split_at_cross_section_calls` adds those boundaries afterwards, and a
/// boundary added there can cut a function in two at exactly the place a
/// branch crosses -- which is the case the first check exists for, arriving
/// after it has run. The function then reaches the recompiler with a branch
/// out of it, and the recompiler refuses the whole build over it.
///
/// Banjo-Tooie has three, all the same shape: a float helper with two entry
/// points sharing one body, where the second entry is called fifty-five times
/// from the segment the game unpacks second and the first is called from
/// nowhere at all. Splitting at the second is right -- a `jal` names the first
/// instruction of a function -- and what it leaves above the split is five
/// instructions that branch over the body into the middle of it.
void restub_after_splitting(const Rom &rom, Analysis &analysis) {
    for (SectionInfo &section : analysis.sections) {
        std::set<uint32_t> starts;
        for (const FunctionRange &function : section.functions) {
            starts.insert(function.vram);
        }
        // The section was narrowed to its code once the sweep finished, so its
        // end is the end of the text.
        const uint32_t text_end = section.vram + section.size;
        for (FunctionRange &function : section.functions) {
            // A named function is the runtime's own, and a stub is already
            // as stubbed as it is going to get.
            if (function.stub || !function.name.empty()) {
                continue;
            }
            if (branches_escape(rom, section, starts, text_end, function) ||
                falls_off_the_end(rom, section, function)) {
                if (settle_shared_tail(rom, section, starts, text_end, function)) {
                    analysis.report.extended_over_shared_tail++;
                } else {
                    function.stub = true;
                    analysis.report.stubbed_unstructured++;
                }
            }
        }
    }
}

/// Cut a game's syscall stub table into the two-instruction functions it is.
///
/// A game that dispatches through the CPU's syscall exception -- see
/// SyscallDispatch -- reaches every one of these stubs through a table of
/// pointers in its own data, so no `jal` names one and the sweep has no reason
/// to end a function at one. What it produces instead is a handful of long
/// functions made of stubs, cut only where something else happened to point,
/// and every call to a stub inside one of those is an address the runtime
/// cannot find.
///
/// Banjo-Tooie's table is 4,234 stubs and the sweep put boundaries on 543 of
/// them. The other 3,691 are functions the game calls and this analysis did
/// not have.
///
/// The record's range is checked by shape before it is used, the same way a
/// recorded microcode offset is: every eight bytes of a stub table begins with
/// a `syscall`, and nothing a compiler emits contains one at all, so the real
/// table reads every entry and anything else reads almost none.
void cut_syscall_stubs(const Rom &rom, Analysis &analysis, const TitleRecord &record) {
    const SyscallDispatch &dispatch = record.syscall;
    if (!dispatch.present) {
        return;
    }

    SectionInfo *section = nullptr;
    for (SectionInfo &candidate : analysis.sections) {
        const uint64_t end = uint64_t(candidate.vram) + candidate.size;
        if (dispatch.stubs >= candidate.vram && uint64_t(dispatch.stubs) + dispatch.size <= end) {
            section = &candidate;
            break;
        }
    }
    if (section == nullptr) {
        analysis.report.notes.push_back(
            "record: the syscall stub table is not inside any section that was recovered, so it "
            "was left alone");
        return;
    }

    const uint32_t table_end = dispatch.stubs + dispatch.size;
    const size_t entries = dispatch.size / 8;
    size_t syscalls = 0;
    for (uint32_t at = dispatch.stubs; at < table_end; at += 8) {
        if ((rom.word(section->rom + (at - section->vram)) & 0xFC00003Fu) == 0x0000000Cu) {
            syscalls++;
        }
    }
    if (syscalls != entries) {
        analysis.report.notes.push_back(
            "record: " + std::to_string(syscalls) + " of " + std::to_string(entries) +
            " entries at the recorded syscall stub table begin with a syscall, so it is not the "
            "table and was left alone");
        return;
    }

    // What the sweep made of the table is replaced outright. A function that
    // began above it keeps the part that is really code, and one that ran past
    // the end of it becomes a function of its own there -- which is what it
    // always was, since the stub before it does not fall through.
    std::vector<FunctionRange> rebuilt;
    rebuilt.reserve(section->functions.size() + entries);
    for (const FunctionRange &function : section->functions) {
        const uint32_t end = function.vram + function.size;
        if (end <= dispatch.stubs || function.vram >= table_end) {
            rebuilt.push_back(function);
            continue;
        }
        if (function.vram < dispatch.stubs) {
            FunctionRange head = function;
            head.size = dispatch.stubs - function.vram;
            rebuilt.push_back(head);
        }
        if (end > table_end) {
            FunctionRange tail;
            tail.vram = table_end;
            tail.size = end - table_end;
            rebuilt.push_back(tail);
        }
    }
    for (uint32_t at = dispatch.stubs; at < table_end; at += 8) {
        FunctionRange stub;
        stub.vram = at;
        stub.size = 8;
        rebuilt.push_back(stub);
    }
    std::sort(rebuilt.begin(), rebuilt.end(),
              [](const FunctionRange &a, const FunctionRange &b) { return a.vram < b.vram; });

    analysis.report.functions_found += rebuilt.size() - section->functions.size();
    analysis.report.syscall_stubs = entries;
    section->functions = std::move(rebuilt);
    analysis.syscall_handler = dispatch.handler;
    analysis.syscall_stubs = dispatch.stubs;
    analysis.syscall_stubs_size = dispatch.size;
}

/// Score every call in the image, once every section is known.
///
/// A call that leaves every section is code this analysis did not find -- an
/// overlay, in practice. A call that lands inside one but not on a function
/// start is a boundary we got wrong, and that is the number to watch. Both
/// have to be counted here rather than per section, because a game with more
/// than one segment calls between them constantly and scoring a section on its
/// own would report every one of those as missing code.
void score_calls(const Rom &rom, Analysis &analysis) {
    auto covering = [&](uint32_t target) -> const SectionInfo * {
        for (const SectionInfo &section : analysis.sections) {
            if (target >= section.vram && target < section.vram + section.size) {
                return &section;
            }
        }
        return nullptr;
    };

    for (const SectionInfo &section : analysis.sections) {
        for (uint32_t vram = section.vram; vram < section.vram + section.size; vram += 4) {
            const uint32_t word = rom.word(section.rom + (vram - section.vram));
            if ((word >> 26) != 0x03) { // jal
                continue;
            }
            const uint32_t target = 0x80000000u | ((word & 0x03FFFFFFu) << 2);
            const SectionInfo *home = covering(target);
            if (home == nullptr) {
                analysis.report.calls_outside++;
                continue;
            }
            // The functions in a section are in address order, so this is a
            // binary search over a few thousand entries per call rather than a
            // scan over all of them.
            const auto at = std::lower_bound(
                home->functions.begin(), home->functions.end(), target,
                [](const FunctionRange &function, uint32_t vram) { return function.vram < vram; });
            if (at != home->functions.end() && at->vram == target) {
                analysis.report.calls_on_boundary++;
            } else {
                analysis.report.calls_off_boundary++;
            }
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

void find_overlays(const Rom &rom, Analysis &analysis, const n64sig::Database *signatures,
                   const RuntimeProvides *provides) {
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
        // Deliberately not phrased as "only the boot segment was recompiled":
        // by the time this is read a title record may have supplied the
        // segments outright, which is the other half of the design and the
        // case Super Mario 64 lands in.
        analysis.report.notes.push_back(
            "no overlay table found. A game that loads code at runtime and does not tabulate "
            "where it lives needs its segments written down in a title record.");
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
        recover_functions(rom, overlay, analysis.report, overlay.vram, signatures, provides);
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

bool load_runtime_provides(const std::string &path, RuntimeProvides &out, std::string &error) {
    std::ifstream file(path);
    if (!file) {
        error = "could not open " + path;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            out.insert(line);
        }
    }
    return true;
}

namespace {

/// Which save chip the libultra names in the image point at.
///
/// A game linked against `osEepromLongRead` has an EEPROM behind it; one linked
/// against `osFlashReadArray` has FlashRAM. This does not narrow EEPROM to a
/// size and says nothing at all about SRAM, which has no libultra routines of
/// its own -- a game reaches it with a plain PI DMA to 0x08000000, which looks
/// like any other DMA. So this is evidence, not an answer.
std::string detect_save_evidence(const Analysis &analysis) {
    bool eeprom = false;
    bool flash = false;
    for (const SectionInfo &section : analysis.sections) {
        for (const FunctionRange &function : section.functions) {
            if (function.name.rfind("osEeprom", 0) == 0) {
                eeprom = true;
            } else if (function.name.rfind("osFlash", 0) == 0) {
                flash = true;
            }
        }
    }
    if (eeprom && flash) {
        return "both EEPROM and FlashRAM routines are linked in";
    }
    if (eeprom) {
        return "EEPROM routines are linked in";
    }
    if (flash) {
        return "FlashRAM routines are linked in";
    }
    return "no libultra save routine was named";
}

/// Apply a record's function corrections to the sections that cover them.
void apply_function_records(Analysis &analysis, const TitleRecord &record) {
    for (const FunctionRange &correction : record.functions) {
        for (SectionInfo &section : analysis.sections) {
            if (correction.vram < section.vram || correction.vram >= section.vram + section.size) {
                continue;
            }
            auto at = std::find_if(section.functions.begin(), section.functions.end(),
                                   [&](const FunctionRange &f) { return f.vram == correction.vram; });

            if (correction.size == 0) {
                // A boundary the analyser invented. Give its bytes to the
                // function before it, which is where they belonged.
                if (at != section.functions.end()) {
                    if (at != section.functions.begin()) {
                        std::prev(at)->size += at->size;
                    }
                    section.functions.erase(at);
                    analysis.report.notes.push_back(
                        "record: dropped the boundary at " + std::to_string(correction.vram));
                }
                break;
            }

            if (at == section.functions.end()) {
                // A boundary the analysis missed, which in practice means a
                // function nothing calls directly -- reached through a table,
                // and so invisible to anything that follows calls. Splitting
                // the function that covers it is what the analysis would have
                // done had a `jal` pointed here, so it is done the same way:
                // the predecessor ends where this one starts, and this one
                // runs to where the predecessor used to.
                auto containing = std::upper_bound(
                    section.functions.begin(), section.functions.end(), correction.vram,
                    [](uint32_t vram, const FunctionRange &f) { return vram < f.vram; });

                FunctionRange added = correction;
                if (containing != section.functions.begin()) {
                    auto previous = std::prev(containing);
                    const uint32_t previous_end = previous->vram + previous->size;
                    if (previous_end > correction.vram) {
                        if (added.size == UINT32_MAX) {
                            added.size = previous_end - correction.vram;
                        }
                        previous->size = correction.vram - previous->vram;
                    }
                }
                if (added.size == UINT32_MAX) {
                    added.size = 4;
                }
                section.functions.insert(containing, added);
                analysis.report.split_boundaries++;
            } else {
                if (correction.size != UINT32_MAX) {
                    at->size = correction.size;
                }
                if (!correction.name.empty()) {
                    at->name = correction.name;
                }
                at->stub = correction.stub;
            }
            break;
        }
    }
}

} // namespace

/// How much of a block decodes as a coprocessor 2 instruction.
///
/// The R4300 has no coprocessor 2, so nothing the CPU runs uses one and no
/// ordinary code in a cartridge contains one by accident. The signal processor
/// is all vector unit, and its microcode is dense with them. That makes the
/// ratio a check on a recorded address: a block that is really microcode reads
/// well above a tenth, and data that happens to sit at the recorded offset
/// reads near zero.
double cop2_density(const Rom &rom, uint32_t offset, uint32_t size) {
    if (size == 0 || size_t(offset) + size > rom.size()) {
        return 0.0;
    }
    size_t vector_ops = 0;
    const size_t words = size / 4;
    for (size_t i = 0; i < words; i++) {
        const uint32_t opcode = rom.word(offset + uint32_t(i) * 4) >> 26;
        constexpr uint32_t kCop2 = 0x12;
        constexpr uint32_t kLwc2 = 0x32;
        constexpr uint32_t kSwc2 = 0x3A;
        if (opcode == kCop2 || opcode == kLwc2 || opcode == kSwc2) {
            vector_ops++;
        }
    }
    return words == 0 ? 0.0 : double(vector_ops) / double(words);
}

/// Every address inside the text that the microcode's data blob names.
///
/// The recompiler turns `jr $reg` into a switch over the labels it emitted, so
/// a target it did not work out statically is a microcode that stops the first
/// time the game asks for that command. What it cannot work out is exactly the
/// interesting case: a microcode dispatches its command list through a table
/// of halfword addresses that lives in its data blob, and the blob is data --
/// nothing in the instruction stream points into it.
///
/// The blob is small and the rule is self-limiting, which is what makes
/// reading it safe. A halfword only counts when it is inside the text and
/// four-byte aligned, and an address is four bytes of the sixty-four thousand
/// a halfword could hold: the audio microcode's own table has sixteen entries
/// in range and sixteen out of it -- bit masks, which are not aligned
/// addresses -- and the masks are rejected without being known to be masks.
///
/// A wrong extra target costs a label nobody jumps to. A missing one costs the
/// game its sound, so the trade runs one way.
std::vector<uint32_t> harvest_branch_targets(const Rom &rom, const MicrocodeInfo &block) {
    std::vector<uint32_t> targets;
    if (block.data_size == 0 || size_t(block.data_rom) + block.data_size > rom.size()) {
        return targets;
    }
    // The signal processor addresses its own memory in thirteen bits, so a
    // target in a table is 0x1080 rather than 0x04001080, and that is the form
    // the recompiler labels them in too.
    constexpr uint32_t kRspMemMask = 0x1FFFu;
    const uint32_t first = block.text_address & kRspMemMask;
    const uint32_t last = first + block.size;
    for (uint32_t offset = 0; offset + 2 <= block.data_size; offset += 2) {
        // The blob is big-endian like the rest of the image, and a halfword
        // spans two of its bytes wherever it falls.
        const uint32_t word = rom.word((block.data_rom + offset) & ~3u);
        const uint32_t half = ((block.data_rom + offset) & 2u) ? (word & 0xFFFFu) : (word >> 16);
        if (half >= first && half < last && (half % 4) == 0) {
            targets.push_back(half);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

/// Where in the file a console address is, for a record that gives one.
///
/// Two places can answer. IPL3 copies the first megabyte of the cartridge to
/// the entry point, so anything in that copy is at a fixed distance from the
/// boot offset; and a segment the game unpacked has been spliced onto the end
/// of the image, where the record's own section table says it landed. Between
/// them they cover every block a record can name, which is what lets a
/// [[microcode]] carry addresses rather than offsets.
bool rom_offset_of(const Rom &rom, const TitleRecord &record, uint32_t vram, uint32_t size,
                   uint32_t &out) {
    for (const SectionInfo &section : record.sections) {
        if (vram >= section.vram && uint64_t(vram) + size <= uint64_t(section.vram) + section.size) {
            out = section.rom + (vram - section.vram);
            return true;
        }
    }
    const uint64_t from_boot = uint64_t(vram) - rom.load_address;
    if (vram >= rom.load_address && from_boot + size <= kBootCopySize) {
        out = kBootRomOffset + uint32_t(from_boot);
        return true;
    }
    return false;
}

/// Take the microcode a record names, and say whether it looks like microcode.
void adopt_microcode(const Rom &rom, Analysis &analysis, const TitleRecord &record) {
    for (const MicrocodeInfo &recorded : record.microcode) {
        MicrocodeInfo block = recorded;
        if (block.vram == 0) {
            // IPL3 copies the first megabyte of the cartridge to the entry
            // point, so a block inside that copy has an address already.
            if (block.rom < kBootRomOffset || block.rom >= kBootRomOffset + kBootCopySize) {
                analysis.report.notes.push_back(
                    "record: microcode \"" + block.name +
                    "\" is outside the boot copy, so its vram has to be recorded too");
                continue;
            }
            block.vram = rom.load_address + (block.rom - kBootRomOffset);
        }
        // Recorded by address rather than by offset, which is the only form
        // available for a cartridge whose microcode arrives in memory.
        if (block.rom == 0 && !rom_offset_of(rom, record, block.vram, block.size, block.rom)) {
            analysis.report.notes.push_back(
                "record: microcode \"" + block.name +
                "\" is at an address no section covers, so nothing can be read from it");
            continue;
        }
        if (block.data_rom == 0 && block.data_size != 0 &&
            !rom_offset_of(rom, record, block.data_vram, block.data_size, block.data_rom)) {
            analysis.report.notes.push_back(
                "record: the data blob of microcode \"" + block.name +
                "\" is at an address no section covers, so its command table cannot be read");
            block.data_size = 0;
        }
        block.cop2_density = cop2_density(rom, block.rom, block.size);
        constexpr double kLooksLikeMicrocode = 0.10;
        if (block.cop2_density < kLooksLikeMicrocode) {
            analysis.report.notes.push_back(
                "record: microcode \"" + block.name +
                "\" does not read as RSP code -- too little of it is coprocessor 2");
        }
        block.branch_targets = harvest_branch_targets(rom, block);
        analysis.microcode.push_back(block);
    }
}

Analysis analyze(const Rom &rom, const n64sig::Database *signatures,
                 const RuntimeProvides *provides, const TitleRecord *record) {
    Analysis analysis;

    SectionInfo boot;
    boot.name = "boot";
    boot.rom = kBootRomOffset;
    boot.vram = rom.load_address;
    boot.size = uint32_t(std::min<size_t>(
        kBootCopySize, rom.size() > kBootRomOffset ? rom.size() - kBootRomOffset : 0));
    recover_functions(rom, boot, analysis.report, rom.load_address, signatures, provides);

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

    find_overlays(rom, analysis, signatures, provides);

    // The record goes on last and wins. Its sections are the ones the analysis
    // could not find at all, so a section it names that overlaps one already
    // recovered replaces it rather than being added beside it -- two sections
    // covering the same address would give the recompiler two functions at it.
    if (record != nullptr) {
        for (const SectionInfo &recorded : record->sections) {
            auto overlapping = std::remove_if(
                analysis.sections.begin(), analysis.sections.end(), [&](const SectionInfo &s) {
                    return s.rom < recorded.rom + recorded.size && recorded.rom < s.rom + s.size;
                });
            if (overlapping != analysis.sections.end()) {
                analysis.sections.erase(overlapping, analysis.sections.end());
            }

            SectionInfo section = recorded;
            if (section.functions.empty()) {
                // Every call from the sections already recovered that lands in
                // this one. The record says where the segment is; these say
                // where its functions are.
                std::vector<uint32_t> entries;
                for (const SectionInfo &known : analysis.sections) {
                    for (uint32_t offset = 0; offset < known.size; offset += 4) {
                        const uint32_t word = rom.word(known.rom + offset);
                        if ((word >> 26) != 0x03) { // jal
                            continue;
                        }
                        const uint32_t target = 0x80000000u | ((word & 0x03FFFFFFu) << 2);
                        if (target >= section.vram && target < section.vram + section.size) {
                            entries.push_back(target);
                        }
                    }
                }
                std::sort(entries.begin(), entries.end());
                entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

                // The usual case: the record says where the segment is and the
                // sweep says what is in it, which is the division of labour the
                // whole design is built around.
                recover_functions(rom, section, analysis.report, section.vram, signatures, provides,
                                  &entries);
                if (section.text_size > 0) {
                    section.size = section.text_size;
                }
            }
            if (section.functions.empty()) {
                analysis.report.notes.push_back("record: no code was recovered from section \"" +
                                                section.name + "\"");
                continue;
            }
            analysis.sections.push_back(std::move(section));
        }

        std::sort(analysis.sections.begin(), analysis.sections.end(),
                  [](const SectionInfo &a, const SectionInfo &b) { return a.rom < b.rom; });

        apply_function_records(analysis, *record);

        // A resemblance is a suggestion, and a record that already names the
        // function has taken it. Saying it again turns a worklist into a list
        // of things somebody has to check off twice.
        analysis.report.resemblances.erase(
            std::remove_if(analysis.report.resemblances.begin(),
                           analysis.report.resemblances.end(),
                           [&](const AnalysisReport::Resemblance &near) {
                               for (const FunctionRange &named : record->functions) {
                                   if (named.vram == near.vram && !named.name.empty()) {
                                       return true;
                                   }
                               }
                               return false;
                           }),
            analysis.report.resemblances.end());

        // Same for a tie between two signatures: a record that names the
        // address has answered it, and repeating the question is noise.
        analysis.report.ambiguous_names.erase(
            std::remove_if(analysis.report.ambiguous_names.begin(),
                           analysis.report.ambiguous_names.end(),
                           [&](const AnalysisReport::Ambiguity &tie) {
                               for (const FunctionRange &named : record->functions) {
                                   if (named.vram == tie.vram && !named.name.empty()) {
                                       return true;
                                   }
                               }
                               return false;
                           }),
            analysis.report.ambiguous_names.end());

        adopt_microcode(rom, analysis, *record);

        cut_syscall_stubs(rom, analysis, *record);

        analysis.report.notes.push_back("record: " + record->path);
        for (const std::string &note : record->notes) {
            analysis.report.notes.push_back("record: " + note);
        }
    }

    split_at_cross_section_calls(rom, analysis);
    restub_after_splitting(rom, analysis);
    score_calls(rom, analysis);
    analysis.save_type_evidence = detect_save_evidence(analysis);
    if (record != nullptr && !record->save_type.empty()) {
        save_type_from_name(record->save_type, analysis.save_type);
    }

    return analysis;
}

} // namespace n64rip
