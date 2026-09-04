// SPDX-License-Identifier: GPL-3.0-or-later
//
// n64rip - recover from a bare N64 ROM the metadata N64Recomp needs.
//
// A GameCube DOL carries a section table, so DolRecomp is told where code
// lives and at what address. An N64 ROM is a flat image: a 64-byte header,
// 4KB of boot code, and then whatever the game's linker script decided. The
// recompiler cannot start from that, which is why every existing N64Recomp
// project starts from a decompilation's elf instead.
//
// This tool closes that gap. It recovers sections and function boundaries from
// the image itself and writes them as the symbol toml N64Recomp accepts
// alongside a raw ROM, so a ROM nobody has decompiled can still be recompiled.
//
// What it can and cannot do is in PLAN.md; the short version is that the boot
// segment comes out well and overlays are found only when the game builds the
// DMA table libultra games conventionally build.

#ifndef N64RIP_HPP
#define N64RIP_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace n64rip {

// ---------------------------------------------------------------------------
// The ROM

/// How the bytes were laid out in the file we were handed. Everything past
/// load() works on a normalised big-endian image.
enum class ByteOrder {
    Z64, // big endian, the native order
    N64, // little endian, every word reversed
    V64, // byte-swapped halfwords, the Doctor V64 dump order
};

const char *byte_order_name(ByteOrder order);

struct Header {
    uint32_t pi_config = 0;
    uint32_t clock_rate = 0;
    uint32_t entrypoint = 0; // as stored; see Rom::load_address
    uint32_t release = 0;
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    std::string internal_name;  // 0x20..0x33, trailing space stripped
    std::string game_id;        // media + cartridge + country, e.g. "NSME"
    char media_format = 0;
    char country = 0;
    uint8_t version = 0;
};

/// The boot chip a ROM was mastered for. Its only consequence here is the
/// entry point: IPL3 for two of them adjusts the address it jumps to, so the
/// header value is not where the boot segment actually lands.
enum class Cic {
    Unknown,
    Cic6101,
    Cic6102, // by far the most common
    Cic6103,
    Cic6105,
    Cic6106,
    Cic7102, // 6102's PAL sibling
};

const char *cic_name(Cic cic);

/// How much IPL3 copies out of the ROM before it jumps to the entry point.
constexpr uint32_t kBootRomOffset = 0x1000;
constexpr uint32_t kBootCopySize = 0x100000;

struct Rom {
    std::vector<uint8_t> data; // normalised to big-endian
    ByteOrder original = ByteOrder::Z64;
    Header header;
    Cic cic = Cic::Unknown;
    /// Where the boot segment really lands, once IPL3's adjustment is applied.
    uint32_t load_address = 0;
    /// Whether load_address came from a confident reading or a guess that
    /// scored best; see resolve_load_address().
    bool load_address_verified = false;
    uint64_t hash = 0; // of the normalised image

    /// Big-endian word at a ROM offset. Out of range reads as zero.
    uint32_t word(size_t offset) const;
    size_t size() const { return data.size(); }
};

/// Read a ROM off disk, normalise its byte order, and parse everything the
/// header and boot code can tell us. Returns nullopt with `error` set if the
/// file is not an N64 image.
std::optional<Rom> load_rom(const std::string &path, std::string &error);

// ---------------------------------------------------------------------------
// Analysis

struct FunctionRange {
    uint32_t vram = 0;
    uint32_t size = 0; // bytes, always a multiple of 4
};

struct SectionInfo {
    std::string name;
    uint32_t rom = 0;
    uint32_t vram = 0;
    uint32_t size = 0;
    /// Where the recovered code ends inside this section. Everything past it
    /// is rodata and data, which the recompiler has no use for.
    uint32_t text_size = 0;
    std::vector<FunctionRange> functions;
};

/// Counters worth printing, and worth keeping in a title record so a change to
/// the analyser can be measured rather than guessed at.
struct AnalysisReport {
    size_t words_scanned = 0;
    size_t functions_found = 0;
    size_t from_calls = 0;     // reached by following calls from the entry point
    size_t from_sweep = 0;     // found by sweeping the code those calls proved
    size_t calls_outside = 0;  // `jal` targets no section covers
    size_t invalid_words = 0;  // words inside .text that did not decode

    /// The check that says whether the boundaries are right.
    ///
    /// Every `jal` names the first instruction of a function. So each one whose
    /// target is inside a section we recovered should land exactly on a
    /// function we recovered. One that lands in the middle of a function is a
    /// boundary this analysis got wrong, and the ratio between the two is the
    /// closest thing to a score available without a decompilation to compare
    /// against.
    size_t calls_on_boundary = 0;
    size_t calls_off_boundary = 0;

    std::vector<std::string> notes;
};

struct Analysis {
    std::vector<SectionInfo> sections;
    AnalysisReport report;
};

/// Recover sections and functions from a loaded ROM.
Analysis analyze(const Rom &rom);

// ---------------------------------------------------------------------------
// Output

/// The symbol toml N64Recomp reads with `symbols_file_path`.
std::string emit_symbols_toml(const Rom &rom, const Analysis &analysis);

/// The recompiler configuration that points at it.
std::string emit_recomp_toml(const Rom &rom, const Analysis &analysis,
                             const std::string &symbols_path,
                             const std::string &rom_path,
                             const std::string &output_dir);

/// Everything the pipeline needs to know about the ROM, for the library entry
/// and for the cache key.
std::string emit_info_json(const Rom &rom, const Analysis &analysis);

} // namespace n64rip

#endif
