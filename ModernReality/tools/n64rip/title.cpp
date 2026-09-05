// SPDX-License-Identifier: GPL-3.0-or-later
//
// Title records, and the two things the analysis reports that are not
// addresses: what the game is called and what it saves to.
//
// A record is the design's answer to the coverage limit. Function recovery on
// MIPS is reliable; finding the code a game DMAs out of the ROM at runtime is
// not, because nothing in the image is required to say where it lives. When
// the analyser cannot find a segment, somebody measures it once and writes it
// down, and every later build of that game gets it for free.
//
// A record holds addresses, sizes, names and a hash. It never holds bytes of a
// game -- the same rule the ROMs themselves follow.

#include "n64rip.hpp"

#include <toml++/toml.hpp>

#include <cctype>
#include <filesystem>

namespace n64rip {

const char *save_type_name(SaveType type) {
    switch (type) {
        case SaveType::None: return "none";
        case SaveType::Eep4k: return "eep4k";
        case SaveType::Eep16k: return "eep16k";
        case SaveType::Sram: return "sram";
        case SaveType::Flashram: return "flashram";
        case SaveType::AllowAll: return "allow_all";
    }
    return "allow_all";
}

bool save_type_from_name(const std::string &name, SaveType &out) {
    if (name == "none") { out = SaveType::None; return true; }
    if (name == "eep4k" || name == "eeprom4k") { out = SaveType::Eep4k; return true; }
    if (name == "eep16k" || name == "eeprom16k") { out = SaveType::Eep16k; return true; }
    if (name == "sram") { out = SaveType::Sram; return true; }
    if (name == "flashram" || name == "flash") { out = SaveType::Flashram; return true; }
    if (name == "allow_all" || name == "unknown") { out = SaveType::AllowAll; return true; }
    return false;
}

std::string display_name(const Rom &rom, const TitleRecord *record) {
    if (record != nullptr && !record->display_name.empty()) {
        return record->display_name;
    }

    // Header names are shouted, padded, and sometimes abbreviated:
    // "SUPER MARIO 64      ", "LEGEND OF ZELDA". Title-casing is a decent
    // guess and a record is where a better one goes.
    std::string out;
    bool start_of_word = true;
    for (char c : rom.header.internal_name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u)) {
            start_of_word = true;
            out.push_back(' ');
            continue;
        }
        out.push_back(start_of_word ? char(std::toupper(u)) : char(std::tolower(u)));
        start_of_word = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.empty()) {
        out = rom.header.game_id.empty() ? "Unknown Game" : rom.header.game_id;
    }
    return out;
}

namespace {

/// Reads `0x1234`, `4660` or `"0x1234"` -- a record is written by hand, and
/// TOML integers cannot carry the leading zeroes an address is usually typed
/// with, so a quoted address has to work too.
bool read_address(const toml::node *node, uint32_t &out) {
    if (node == nullptr) {
        return false;
    }
    if (auto value = node->value<int64_t>()) {
        out = uint32_t(*value);
        return true;
    }
    if (auto value = node->value<std::string>()) {
        try {
            out = uint32_t(std::stoull(*value, nullptr, 0));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }
    return false;
}

bool read_hash(const toml::node *node, uint64_t &out) {
    if (node == nullptr) {
        return false;
    }
    if (auto value = node->value<std::string>()) {
        try {
            out = std::stoull(*value, nullptr, 16);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }
    if (auto value = node->value<int64_t>()) {
        out = uint64_t(*value);
        return true;
    }
    return false;
}

} // namespace

bool load_title_record(const std::string &path, TitleRecord &out, std::string &error) {
    toml::table table;
    try {
        table = toml::parse_file(path);
    } catch (const toml::parse_error &err) {
        error = "could not read the title record " + path + ": " + std::string(err.description());
        return false;
    }

    out.path = path;

    if (const toml::table *title = table["title"].as_table()) {
        out.game_id = (*title)["game_id"].value_or(std::string{});
        out.display_name = (*title)["name"].value_or(std::string{});
        out.save_type = (*title)["save_type"].value_or(std::string{});
        read_hash((*title)["rom_hash"].node(), out.rom_hash);
        if (const toml::array *notes = (*title)["notes"].as_array()) {
            for (const toml::node &note : *notes) {
                out.notes.push_back(note.value_or(std::string{}));
            }
        }
    }

    if (!out.save_type.empty()) {
        SaveType parsed = SaveType::AllowAll;
        if (!save_type_from_name(out.save_type, parsed)) {
            error = path + ": save_type \"" + out.save_type + "\" is not one of none, eep4k, "
                    "eep16k, sram, flashram, allow_all";
            return false;
        }
    }

    if (const toml::array *sections = table["section"].as_array()) {
        for (const toml::node &node : *sections) {
            const toml::table *section = node.as_table();
            if (section == nullptr) {
                continue;
            }
            SectionInfo info;
            info.name = (*section)["name"].value_or(std::string{});
            const bool have_rom = read_address((*section)["rom"].node(), info.rom);
            const bool have_vram = read_address((*section)["vram"].node(), info.vram);
            const bool have_size = read_address((*section)["size"].node(), info.size);
            if (info.name.empty() || !have_rom || !have_vram || !have_size) {
                error = path + ": every [[section]] needs a name, rom, vram and size";
                return false;
            }

            if (const toml::array *functions = (*section)["functions"].as_array()) {
                for (const toml::node &fnode : *functions) {
                    const toml::table *function = fnode.as_table();
                    if (function == nullptr) {
                        continue;
                    }
                    FunctionRange range;
                    range.name = (*function)["name"].value_or(std::string{});
                    if (!read_address((*function)["vram"].node(), range.vram) ||
                        !read_address((*function)["size"].node(), range.size)) {
                        error = path + ": every function in a [[section]] needs a vram and size";
                        return false;
                    }
                    info.functions.push_back(std::move(range));
                }
            }
            info.from_record = true;
            out.sections.push_back(std::move(info));
        }
    }

    // Corrections, applied to whatever section ends up covering the address.
    if (const toml::array *functions = table["function"].as_array()) {
        for (const toml::node &node : *functions) {
            const toml::table *function = node.as_table();
            if (function == nullptr) {
                continue;
            }
            FunctionRange range;
            range.name = (*function)["name"].value_or(std::string{});
            range.stub = (*function)["stub"].value_or(false);
            if (!read_address((*function)["vram"].node(), range.vram)) {
                error = path + ": every [[function]] needs a vram";
                return false;
            }
            // Absent means "leave the size alone"; zero means "this is not a
            // function, drop the boundary".
            if (!read_address((*function)["size"].node(), range.size)) {
                range.size = UINT32_MAX;
            }
            out.functions.push_back(std::move(range));
        }
    }

    // RSP microcode. A rom offset and a size; the address the game loads it at
    // follows from IPL3's copy unless the record says otherwise, and the text
    // address is the same 0x04001080 in every libultra task.
    if (const toml::array *blocks = table["microcode"].as_array()) {
        for (const toml::node &node : *blocks) {
            const toml::table *block = node.as_table();
            if (block == nullptr) {
                continue;
            }
            MicrocodeInfo info;
            info.name = (*block)["name"].value_or(std::string{});
            const bool have_rom = read_address((*block)["rom"].node(), info.rom);
            const bool have_size = read_address((*block)["size"].node(), info.size);
            if (!have_rom || !have_size) {
                error = path + ": every [[microcode]] needs a rom and a size";
                return false;
            }
            if (info.name.empty()) {
                error = path + ": every [[microcode]] needs a name";
                return false;
            }
            if (!read_address((*block)["vram"].node(), info.vram)) {
                info.vram = 0;
            }
            uint32_t text_address = 0;
            if (read_address((*block)["text_address"].node(), text_address)) {
                info.text_address = text_address;
            }
            if (!read_address((*block)["data_rom"].node(), info.data_rom) ||
                !read_address((*block)["data_size"].node(), info.data_size)) {
                info.data_rom = 0;
                info.data_size = 0;
            }
            out.microcode.push_back(std::move(info));
        }
    }

    // The game's own syscall dispatch. Two addresses and a size; what they
    // mean, and how they were read out of the disassembly, is in
    // SyscallDispatch.
    if (const toml::table *dispatch = table["syscall"].as_table()) {
        const bool have_stubs = read_address((*dispatch)["stubs"].node(), out.syscall.stubs);
        const bool have_size = read_address((*dispatch)["size"].node(), out.syscall.size);
        const bool have_handler = read_address((*dispatch)["handler"].node(), out.syscall.handler);
        if (!have_stubs || !have_size || !have_handler) {
            error = path + ": [syscall] needs stubs, size and handler";
            return false;
        }
        if (out.syscall.size == 0 || (out.syscall.size % 8) != 0) {
            error = path + ": [syscall] size must be a non-zero multiple of eight, because a "
                           "stub is two instructions";
            return false;
        }
        out.syscall.present = true;
    }

    // Segments the game's own loader unpacks. A vram and a size; where they
    // come from is a memory image taken after the loader ran, and the record
    // never holds a byte of it.
    if (const toml::array *blocks = table["unpacked"].as_array()) {
        for (const toml::node &node : *blocks) {
            const toml::table *block = node.as_table();
            if (block == nullptr) {
                continue;
            }
            UnpackedSection info;
            info.name = (*block)["name"].value_or(std::string{});
            const bool have_vram = read_address((*block)["vram"].node(), info.vram);
            const bool have_size = read_address((*block)["size"].node(), info.size);
            if (info.name.empty() || !have_vram || !have_size) {
                error = path + ": every [[unpacked]] needs a name, vram and size";
                return false;
            }
            out.unpacked.push_back(std::move(info));
        }
    }

    return true;
}

bool splice_unpacked(Rom &rom, TitleRecord &record, const std::vector<uint8_t> &image,
                     std::string &error) {
    // What `n64b-run --unpack` writes, and what the addresses in the record are
    // relative to.
    constexpr uint32_t kRamStart = 0x80000000u;

    for (const UnpackedSection &segment : record.unpacked) {
        if (segment.vram < kRamStart) {
            error = "unpacked segment \"" + segment.name + "\" is not a console address";
            return false;
        }
        const uint64_t start = uint64_t(segment.vram) - kRamStart;
        const uint64_t end = start + segment.size;
        if ((segment.vram % 4) != 0 || (segment.size % 4) != 0) {
            error = "unpacked segment \"" + segment.name + "\" is not word-aligned";
            return false;
        }
        if (end > image.size()) {
            error = "unpacked segment \"" + segment.name + "\" runs past the end of the memory "
                    "image; it was taken from a different build, or the loader did not get as "
                    "far as writing it";
            return false;
        }

        // A ROM offset is a place in a file the recompiler will read, so the
        // segment goes on a boundary rather than wherever the last one ended.
        while ((rom.data.size() % 16) != 0) {
            rom.data.push_back(0);
        }
        SectionInfo section;
        section.name = segment.name;
        section.rom = uint32_t(rom.data.size());
        section.vram = segment.vram;
        section.size = segment.size;
        section.from_record = true;

        // librecomp holds console memory with each word in the host's order,
        // so that a translated load is a plain load. The cartridge holds it
        // big-endian. Put it back.
        for (uint64_t at = start; at < end; at += 4) {
            rom.data.push_back(image[at + 3]);
            rom.data.push_back(image[at + 2]);
            rom.data.push_back(image[at + 1]);
            rom.data.push_back(image[at + 0]);
        }

        record.sections.push_back(std::move(section));
    }
    return true;
}

} // namespace n64rip
