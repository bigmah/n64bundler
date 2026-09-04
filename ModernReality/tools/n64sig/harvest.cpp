// SPDX-License-Identifier: GPL-3.0-or-later
// Reading functions and their relocations out of MIPS ELF objects.
//
// The input is the `libultra*.a` that decompilation projects ship: a GNU ar
// archive of big-endian MIPS32 objects, one per library function or small
// group of them. For each we want the bytes of every function symbol in .text,
// plus .rel.text, which is the list of fields the linker was going to fill in
// and therefore exactly the list of fields a fingerprint has to ignore.

#include "signature.hpp"

#include <cstring>
#include <fstream>
#include <map>

namespace n64sig {
namespace {

// --- byte access -------------------------------------------------------------

uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t *p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

// --- ELF32 big-endian MIPS ---------------------------------------------------

constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtRel = 9;
constexpr uint8_t kSttFunc = 2;

// The MIPS relocations that appear in .text, and how much of the word each one
// leaves alone. Everything else is compared in full.
constexpr uint32_t kRMips16 = 1;
constexpr uint32_t kRMips32 = 2;
constexpr uint32_t kRMipsRel32 = 3;
constexpr uint32_t kRMips26 = 4;
constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;
constexpr uint32_t kRMipsGpRel16 = 7;
constexpr uint32_t kRMipsLiteral = 8;
constexpr uint32_t kRMipsGot16 = 9;
constexpr uint32_t kRMipsPc16 = 10;
constexpr uint32_t kRMipsCall16 = 11;
constexpr uint32_t kRMipsGpRel32 = 12;

/// Which bits survive a relocation of this type.
uint32_t mask_for_reloc(uint32_t type) {
    switch (type) {
        case kRMips26:
            // jal / j: the opcode is fixed, the 26-bit target is not.
            return 0xFC000000u;
        case kRMipsHi16:
        case kRMipsLo16:
        case kRMipsGpRel16:
        case kRMipsGot16:
        case kRMipsCall16:
        case kRMipsLiteral:
        case kRMips16:
            // The opcode and both register fields survive; the immediate does not.
            return 0xFFFF0000u;
        case kRMips32:
        case kRMipsRel32:
        case kRMipsGpRel32:
            // A whole word of address. Nothing survives.
            return 0x00000000u;
        case kRMipsPc16:
            // A relative branch to a symbol. The displacement depends on the
            // final layout, so drop it.
            return 0xFFFF0000u;
        default:
            // Unknown, so assume the whole word moves. Being too permissive
            // here costs a missed match; being too strict costs a wrong one.
            return 0x00000000u;
    }
}

struct Section {
    uint32_t name_offset = 0;
    uint32_t type = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t link = 0;
    uint32_t info = 0;
    uint32_t entsize = 0;
    std::string name;
};

struct Object {
    const uint8_t *data = nullptr;
    size_t size = 0;
    std::vector<Section> sections;
};

bool parse_sections(Object &object) {
    const uint8_t *d = object.data;
    if (object.size < 52 || std::memcmp(d, "\x7F" "ELF", 4) != 0) {
        return false;
    }
    if (d[4] != 1 || d[5] != 2) { // 32-bit, big endian
        return false;
    }
    const uint32_t shoff = be32(d + 0x20);
    const uint16_t shentsize = be16(d + 0x2E);
    const uint16_t shnum = be16(d + 0x30);
    const uint16_t shstrndx = be16(d + 0x32);
    if (shoff == 0 || shnum == 0 || size_t(shoff) + size_t(shnum) * shentsize > object.size) {
        return false;
    }

    object.sections.resize(shnum);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + size_t(i) * shentsize;
        Section &section = object.sections[i];
        section.name_offset = be32(sh + 0x00);
        section.type = be32(sh + 0x04);
        section.offset = be32(sh + 0x10);
        section.size = be32(sh + 0x14);
        section.link = be32(sh + 0x18);
        section.info = be32(sh + 0x1C);
        section.entsize = be32(sh + 0x24);
    }

    if (shstrndx < shnum) {
        const Section &strings = object.sections[shstrndx];
        for (Section &section : object.sections) {
            const size_t at = size_t(strings.offset) + section.name_offset;
            if (at < object.size) {
                section.name = reinterpret_cast<const char *>(d + at);
            }
        }
    }
    return true;
}

/// Every relocated field in one section, as offset -> surviving bits.
std::map<uint32_t, uint32_t> relocation_masks(const Object &object, const std::string &section_name) {
    std::map<uint32_t, uint32_t> masks;
    for (const Section &section : object.sections) {
        if (section.type != kShtRel || section.name != ".rel" + section_name) {
            continue;
        }
        const uint32_t entries = section.entsize ? section.size / section.entsize : 0;
        for (uint32_t i = 0; i < entries; i++) {
            const size_t at = size_t(section.offset) + size_t(i) * section.entsize;
            if (at + 8 > object.size) {
                break;
            }
            const uint32_t offset = be32(object.data + at);
            const uint32_t info = be32(object.data + at + 4);
            const uint32_t type = info & 0xFF;
            const uint32_t mask = mask_for_reloc(type);
            // Two relocations can land on the same word; keep the stricter of
            // the two, which is the one that preserves fewer bits.
            auto existing = masks.find(offset);
            masks[offset] = (existing == masks.end()) ? mask : (existing->second & mask);
        }
    }
    return masks;
}

void harvest_object(const uint8_t *data, size_t size, const std::string &source, Database &out,
                    std::vector<std::string> *warnings) {
    Object object{data, size, {}};
    if (!parse_sections(object)) {
        if (warnings) {
            warnings->push_back(source + ": not a big-endian 32-bit ELF object");
        }
        return;
    }

    for (const Section &symtab : object.sections) {
        if (symtab.type != kShtSymtab || symtab.entsize == 0) {
            continue;
        }
        if (symtab.link >= object.sections.size()) {
            continue;
        }
        const Section &strtab = object.sections[symtab.link];
        const uint32_t count = symtab.size / symtab.entsize;

        // A function's size is usually recorded, but older MIPS toolchains
        // leave it at zero. Where that happens, the function runs to the next
        // symbol in the same section, so collect the boundaries first.
        std::map<uint32_t, std::vector<uint32_t>> starts_by_section;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *sym = data + symtab.offset + size_t(i) * symtab.entsize;
            if (symtab.offset + size_t(i) * symtab.entsize + 16 > size) {
                break;
            }
            const uint32_t value = be32(sym + 4);
            const uint16_t shndx = be16(sym + 14);
            starts_by_section[shndx].push_back(value);
        }
        for (auto &entry : starts_by_section) {
            std::sort(entry.second.begin(), entry.second.end());
        }

        for (uint32_t i = 0; i < count; i++) {
            const size_t at = size_t(symtab.offset) + size_t(i) * symtab.entsize;
            if (at + 16 > size) {
                break;
            }
            const uint8_t *sym = data + at;
            const uint32_t name_offset = be32(sym + 0);
            const uint32_t value = be32(sym + 4);
            uint32_t sym_size = be32(sym + 8);
            const uint8_t info = sym[12];
            const uint16_t shndx = be16(sym + 14);

            if ((info & 0xF) != kSttFunc || shndx >= object.sections.size()) {
                continue;
            }
            const Section &text = object.sections[shndx];
            if (text.name.rfind(".text", 0) != 0) {
                continue;
            }

            if (sym_size == 0) {
                // Run to the next symbol in this section, or to its end.
                sym_size = text.size - value;
                for (uint32_t start : starts_by_section[shndx]) {
                    if (start > value && start - value < sym_size) {
                        sym_size = start - value;
                    }
                }
            }
            if (sym_size < kMinimumSignatureWords * 4 || (sym_size & 3) != 0) {
                continue;
            }
            if (size_t(text.offset) + value + sym_size > size) {
                continue;
            }

            const size_t name_at = size_t(strtab.offset) + name_offset;
            if (name_at >= size) {
                continue;
            }
            const std::string name = reinterpret_cast<const char *>(data + name_at);
            if (name.empty()) {
                continue;
            }

            const std::map<uint32_t, uint32_t> masks = relocation_masks(object, text.name);

            Signature signature;
            signature.name = name;
            signature.source = source;
            const uint32_t words = sym_size / 4;
            signature.words.reserve(words);
            signature.masks.reserve(words);
            for (uint32_t w = 0; w < words; w++) {
                const uint32_t offset_in_section = value + w * 4;
                const uint32_t word = be32(data + text.offset + offset_in_section);
                auto reloc = masks.find(offset_in_section);
                const uint32_t mask = (reloc == masks.end()) ? 0xFFFFFFFFu : reloc->second;
                signature.words.push_back(word & mask);
                signature.masks.push_back(mask);
            }
            out.add(std::move(signature));
        }
    }
}

// --- GNU ar ------------------------------------------------------------------

/// Walk a GNU ar archive, handing each member to `visit`. Long member names
/// live in a `//` member and are referenced as `/<offset>`; the `/` member is
/// the symbol index and is skipped.
void walk_archive(const std::vector<uint8_t> &data, Database &out,
                  std::vector<std::string> *warnings) {
    size_t at = 8; // past "!<arch>\n"
    std::string long_names;

    while (at + 60 <= data.size()) {
        const char *header = reinterpret_cast<const char *>(data.data() + at);
        std::string raw_name(header, 16);
        const std::string raw_size(header + 48, 10);

        size_t member_size = 0;
        try {
            member_size = size_t(std::stoul(raw_size));
        } catch (...) {
            break;
        }
        const size_t body = at + 60;
        if (body + member_size > data.size()) {
            break;
        }

        // Trim the space padding ar uses.
        const size_t last = raw_name.find_last_not_of(' ');
        raw_name = (last == std::string::npos) ? "" : raw_name.substr(0, last + 1);

        if (raw_name == "//") {
            long_names.assign(reinterpret_cast<const char *>(data.data() + body), member_size);
        } else if (raw_name == "/" || raw_name == "/SYM64/") {
            // The symbol index. Nothing to fingerprint.
        } else {
            std::string name = raw_name;
            if (name.size() > 1 && name[0] == '/') {
                try {
                    const size_t offset = size_t(std::stoul(name.substr(1)));
                    if (offset < long_names.size()) {
                        const size_t end = long_names.find_first_of("/\n", offset);
                        name = long_names.substr(offset, end == std::string::npos ? std::string::npos
                                                                                  : end - offset);
                    }
                } catch (...) {
                }
            }
            if (!name.empty() && name.back() == '/') {
                name.pop_back();
            }
            harvest_object(data.data() + body, member_size, name, out, warnings);
        }

        at = body + member_size;
        if (at & 1) { // members are two-byte aligned
            at++;
        }
    }
}

} // namespace

bool harvest(const std::string &path, Database &out, std::string &error,
             std::vector<std::string> *warnings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open " + path;
        return false;
    }
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>{});
    if (data.size() < 16) {
        error = path + " is too small to be an archive or an object";
        return false;
    }

    if (std::memcmp(data.data(), "!<arch>\n", 8) == 0) {
        walk_archive(data, out, warnings);
        return true;
    }
    if (std::memcmp(data.data(), "\x7F" "ELF", 4) == 0) {
        harvest_object(data.data(), data.size(), path, out, warnings);
        return true;
    }
    error = path + " is neither an ar archive nor an ELF object";
    return false;
}

} // namespace n64sig
