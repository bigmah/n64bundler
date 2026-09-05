// SPDX-License-Identifier: GPL-3.0-or-later
//
// n64sig - putting names back on the libultra functions inside a ROM.
//
// Recovering where the functions are (n64rip) is only half of what a
// decompilation's elf would have given us. The other half is what they are
// called, and for one particular set of functions that matters enormously.
//
// A game built with libultra calls osCreateThread, osViSwapBuffer,
// osSpTaskStart and a couple of hundred others, and none of that code can run
// as recompiled MIPS: it drives hardware that does not exist on a Mac. The
// runtime reimplements all of it, and N64Recomp already knows to substitute
// those implementations -- it carries the list of names in symbol_lists.cpp.
// Supply the names and the substitution happens. Supply none, and the
// recompiler faithfully translates libultra's own TLB and MMIO code, which is
// where a build from recovered symbols first falls over.
//
// libultra shipped as a static library, so osCreateThread is byte-identical in
// every game linked against the same version. That makes this the same problem
// IDA's FLIRT signatures solve: fingerprint the functions in the library, then
// look for those fingerprints in the image.
//
// The fingerprint has to ignore the fields the linker filled in. An object
// file says exactly which those are -- that is what its relocations are -- so
// each word carries a mask, and a jal is compared on its opcode alone while
// the instructions around it are compared in full.

#ifndef N64SIG_HPP
#define N64SIG_HPP

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace n64sig {

/// One function's fingerprint.
struct Signature {
    std::string name;
    /// The function's instruction words, with every linker-filled field zeroed.
    std::vector<uint32_t> words;
    /// Which bits of each word are worth comparing. A `jal` keeps its opcode
    /// and drops its target; a `lui` with a %hi relocation keeps its opcode and
    /// registers and drops its immediate.
    std::vector<uint32_t> masks;
    /// Which archive member it came from, for reporting a bad match.
    std::string source;

    size_t size_bytes() const { return words.size() * 4; }
};

/// Signatures indexed for lookup by what a function starts with.
class Database {
public:
    /// Add a signature. Short ones are dropped: a four-instruction function is
    /// not distinctive enough to name anything safely.
    void add(Signature signature);

    /// Every signature whose first words could match the code at `words`.
    /// Callers still have to verify; this only narrows the field.
    std::vector<const Signature *> candidates(const uint32_t *words, size_t count) const;

    /// Does `signature` match the words at `code`, in full?
    static bool matches(const Signature &signature, const uint32_t *code, size_t available);

    /// How much of `signature` the code at `code` agrees with, from 0 to 1,
    /// where a function of a different length is penalised by the difference.
    ///
    /// An exact match names a function; this says what a function nearly is.
    /// libultra shipped in revisions, and the same routine differs between two
    /// of them by a handful of instructions -- a register allocated
    /// differently, a branch the compiler inverted -- which is enough to fail
    /// a match and not nearly enough to hide what the function is. Somebody
    /// reading "0x80030170 is 87% of osEepromProbe" writes one line in a title
    /// record and gets the runtime's implementation; without it they have an
    /// unnamed function among nine thousand others.
    static double resemblance(const Signature &signature, const uint32_t *code, size_t available,
                              size_t function_words);

    bool load(const std::string &path, std::string &error);
    bool save(const std::string &path, std::string &error) const;

    size_t size() const { return signatures_.size(); }
    const std::vector<Signature> &all() const { return signatures_; }

private:
    /// How many leading words go into the bucket key. Four instructions is
    /// enough to split the table into small buckets and short enough that
    /// almost every function has that many.
    static constexpr size_t kKeyWords = 4;
    static uint64_t key_for(const uint32_t *words, const uint32_t *masks, size_t count);

    std::vector<Signature> signatures_;
    std::unordered_map<uint64_t, std::vector<size_t>> by_key_;
    /// The distinct mask patterns the stored signatures use across their
    /// leading words. A lookup has to try each one, because the bucket a
    /// signature landed in depends on its own relocations and arbitrary code
    /// carries no relocations to consult. There are only a handful in
    /// practice: nearly every function begins with instructions the linker
    /// never touched, and the rest begin with a relocated `lui`.
    std::vector<std::array<uint32_t, kKeyWords>> masks_seen_;
};

/// The shortest function worth fingerprinting, in instructions. Below this,
/// distinct library functions collide with each other and with game code.
constexpr size_t kMinimumSignatureWords = 6;

/// Read every function out of a MIPS ELF `.a` archive or a bare `.o`, and turn
/// each into a signature. Appends to `out`; returns false with `error` set if
/// the file is not one of those.
bool harvest(const std::string &path, Database &out, std::string &error,
             std::vector<std::string> *warnings = nullptr);

} // namespace n64sig

#endif
