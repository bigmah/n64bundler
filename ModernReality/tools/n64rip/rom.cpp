// SPDX-License-Identifier: GPL-3.0-or-later
// Loading an N64 ROM: byte order, header, boot chip, and where the boot
// segment actually lands.

#include "n64rip.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace n64rip {

const char *byte_order_name(ByteOrder order) {
    switch (order) {
        case ByteOrder::Z64: return "z64 (big endian)";
        case ByteOrder::N64: return "n64 (little endian)";
        case ByteOrder::V64: return "v64 (byte-swapped)";
    }
    return "unknown";
}

const char *cic_name(Cic cic) {
    switch (cic) {
        case Cic::Cic6101: return "CIC-6101";
        case Cic::Cic6102: return "CIC-6102";
        case Cic::Cic6103: return "CIC-6103";
        case Cic::Cic6105: return "CIC-6105";
        case Cic::Cic6106: return "CIC-6106";
        case Cic::Cic7102: return "CIC-7102";
        case Cic::Unknown: break;
    }
    return "unknown";
}

uint32_t Rom::word(size_t offset) const {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
}

namespace {

constexpr uint32_t kMagicZ64 = 0x80371240;
constexpr uint32_t kMagicN64 = 0x40123780;
constexpr uint32_t kMagicV64 = 0x37804012;

/// Rewrite the image in place so every read past here is big-endian.
void normalise(std::vector<uint8_t> &data, ByteOrder order) {
    switch (order) {
        case ByteOrder::Z64:
            break;
        case ByteOrder::V64: // halfwords swapped
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                std::swap(data[i], data[i + 1]);
            }
            break;
        case ByteOrder::N64: // whole words reversed
            for (size_t i = 0; i + 3 < data.size(); i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
    }
}

/// The plain CRC-32 every N64 tool uses to fingerprint IPL3.
uint32_t crc32(std::span<const uint8_t> bytes) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : bytes) {
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/// FNV-1a over the whole image. Only ever compared against itself, so the
/// choice of function matters less than that it is stable across versions.
uint64_t hash_image(std::span<const uint8_t> bytes) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t byte : bytes) {
        h ^= byte;
        h *= 0x100000001B3ull;
    }
    return h;
}

Cic identify_cic(const Rom &rom) {
    if (rom.size() < kBootRomOffset) {
        return Cic::Unknown;
    }
    const uint32_t crc = crc32(std::span<const uint8_t>(rom.data).subspan(0x40, 0xFC0));
    switch (crc) {
        case 0x6170A4A1u: return Cic::Cic6101;
        case 0x90BB6CB5u: return Cic::Cic6102;
        case 0x0B050EE0u: return Cic::Cic6103;
        case 0x98BC2C86u: return Cic::Cic6105;
        case 0xACC8580Au: return Cic::Cic6106;
        case 0x009E9EA3u: return Cic::Cic7102;
        default: return Cic::Unknown;
    }
}

std::string trim(const std::string &value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

Header parse_header(const Rom &rom) {
    Header header;
    header.pi_config = rom.word(0x00);
    header.clock_rate = rom.word(0x04);
    header.entrypoint = rom.word(0x08);
    header.release = rom.word(0x0C);
    header.crc1 = rom.word(0x10);
    header.crc2 = rom.word(0x14);

    std::string name;
    for (size_t i = 0x20; i < 0x34 && i < rom.size(); i++) {
        const char c = static_cast<char>(rom.data[i]);
        // Some ROMs pad with NULs rather than spaces, and a few carry
        // untranslatable bytes in a Japanese title.
        name.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
    }
    header.internal_name = trim(name);

    if (rom.size() > 0x3F) {
        header.media_format = static_cast<char>(rom.data[0x3B]);
        header.country = static_cast<char>(rom.data[0x3E]);
        header.version = rom.data[0x3F];
        char id[5] = {
            static_cast<char>(rom.data[0x3B]), static_cast<char>(rom.data[0x3C]),
            static_cast<char>(rom.data[0x3D]), static_cast<char>(rom.data[0x3E]), 0,
        };
        for (char &c : id) {
            if (c != 0 && (c < 0x20 || c >= 0x7F)) {
                c = '_';
            }
        }
        header.game_id = id;
    }
    return header;
}

} // namespace

/// IPL3 for two of the boot chips does not jump to the address in the header:
/// it adjusts it first, and the ROM's author compensated when they wrote the
/// header. So the header value alone does not say where the boot segment
/// lands, and getting it wrong puts every recovered function at the wrong
/// address.
///
/// Rather than trust the table, we check it. Every `jal` in the boot segment
/// encodes an absolute address, so the right load address is the one that puts
/// the most of them inside the segment we are about to claim covers them. The
/// table supplies the candidates and the count picks the winner, which also
/// catches a ROM whose boot chip we failed to identify at all.
static void resolve_load_address(Rom &rom) {
    const uint32_t stored = rom.header.entrypoint;

    std::vector<int32_t> candidates;
    switch (rom.cic) {
        case Cic::Cic6103: candidates = {-0x100000, 0, -0x200000}; break;
        case Cic::Cic6106: candidates = {-0x200000, 0, -0x100000}; break;
        default: candidates = {0, -0x100000, -0x200000}; break;
    }

    const size_t boot_bytes =
        std::min<size_t>(kBootCopySize, rom.size() > kBootRomOffset ? rom.size() - kBootRomOffset : 0);

    // Collect every jal target once; they do not depend on the load address.
    std::vector<uint32_t> call_targets;
    for (size_t offset = 0; offset + 4 <= boot_bytes; offset += 4) {
        const uint32_t insn = rom.word(kBootRomOffset + offset);
        if ((insn >> 26) == 0x03) { // jal
            call_targets.push_back(0x80000000u | ((insn & 0x03FFFFFFu) << 2));
        }
    }

    uint32_t best_address = stored;
    size_t best_hits = 0;
    bool first = true;
    for (int32_t adjustment : candidates) {
        const uint32_t base = stored + static_cast<uint32_t>(adjustment);
        size_t hits = 0;
        for (uint32_t target : call_targets) {
            if (target >= base && target < base + boot_bytes) {
                hits++;
            }
        }
        if (first || hits > best_hits) {
            best_address = base;
            best_hits = hits;
            first = false;
        }
    }

    rom.load_address = best_address;
    // A boot segment of any size has hundreds of internal calls. Landing far
    // below that means the segment is not where we think it is, and the number
    // is worth carrying into the report rather than silently proceeding.
    rom.load_address_verified = best_hits >= 32 && !call_targets.empty();
}

std::optional<Rom> load_rom(const std::string &path, std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open " + path;
        return std::nullopt;
    }

    Rom rom;
    rom.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (rom.data.size() < 0x1000) {
        error = "too small to be an N64 ROM (" + std::to_string(rom.data.size()) + " bytes)";
        return std::nullopt;
    }

    const uint32_t magic = (uint32_t(rom.data[0]) << 24) | (uint32_t(rom.data[1]) << 16) |
                           (uint32_t(rom.data[2]) << 8) | uint32_t(rom.data[3]);
    switch (magic) {
        case kMagicZ64: rom.original = ByteOrder::Z64; break;
        case kMagicN64: rom.original = ByteOrder::N64; break;
        case kMagicV64: rom.original = ByteOrder::V64; break;
        default:
            error = "not an N64 ROM: the image starts with " +
                    [&] {
                        char buffer[16];
                        std::snprintf(buffer, sizeof(buffer), "0x%08X", magic);
                        return std::string(buffer);
                    }() +
                    ", which is none of the three known byte orders";
            return std::nullopt;
    }

    normalise(rom.data, rom.original);
    rom.header = parse_header(rom);
    rom.cic = identify_cic(rom);
    rom.hash = hash_image(rom.data);
    resolve_load_address(rom);
    return rom;
}

} // namespace n64rip
