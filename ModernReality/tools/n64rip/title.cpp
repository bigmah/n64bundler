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

    return true;
}

} // namespace n64rip
