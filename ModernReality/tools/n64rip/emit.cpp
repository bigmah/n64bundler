// SPDX-License-Identifier: GPL-3.0-or-later
// Writing out what the analysis found, in the three shapes the pipeline needs.

#include "n64rip.hpp"

#include <cstdio>
#include <sstream>

namespace n64rip {
namespace {

std::string hex(uint32_t value, int width = 8) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%0*X", width, value);
    return buffer;
}

/// Enough escaping for a path or a game name inside a TOML or JSON string.
std::string quote(const std::string &value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", c);
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    out += "\"";
    return out;
}

} // namespace

std::string emit_symbols_toml(const Rom &rom, const Analysis &analysis) {
    std::ostringstream out;
    out << "# Written by n64rip. Recovered from the ROM, not from a decompilation:\n"
        << "# every name here is an address, because the image carries no others.\n"
        << "# " << rom.header.internal_name << " (" << rom.header.game_id << "), "
        << cic_name(rom.cic) << ", entry " << hex(rom.load_address) << "\n\n";

    for (const SectionInfo &section : analysis.sections) {
        out << "[[section]]\n"
            << "name = " << quote(section.name) << "\n"
            << "rom  = " << hex(section.rom) << "\n"
            << "vram = " << hex(section.vram) << "\n"
            << "size = " << hex(section.size) << "\n\n";

        for (const FunctionRange &function : section.functions) {
            out << "[[section.functions]]\n"
                << "name = \"func_" << hex(function.vram).substr(2) << "\"\n"
                << "vram = " << hex(function.vram) << "\n"
                << "size = " << hex(function.size, 1) << "\n\n";
        }
    }
    return out.str();
}

std::string emit_recomp_toml(const Rom &rom, const Analysis &analysis,
                             const std::string &symbols_path, const std::string &rom_path,
                             const std::string &output_dir) {
    (void)analysis;
    std::ostringstream out;
    out << "# Written by n64rip.\n"
        << "[input]\n"
        << "entrypoint = " << hex(rom.load_address) << "\n"
        << "symbols_file_path = " << quote(symbols_path) << "\n"
        << "rom_file_path = " << quote(rom_path) << "\n"
        << "output_func_path = " << quote(output_dir) << "\n"
        // One file per function is the recompiler's default and it produces
        // thousands of translation units for a game this size, which is most
        // of the wall clock of a build. Batching them costs nothing.
        << "functions_per_output_file = 256\n"
        // Recovered symbols never cover every call: an overlay this analysis did
        // not find is still called from the code that did. Let those resolve at
        // runtime rather than refusing to build the game at all.
        << "lookup_unresolved_function_calls = true\n"
        << "recomp_include = \"#include \\\"recomp.h\\\"\"\n";
    return out.str();
}

std::string emit_info_json(const Rom &rom, const Analysis &analysis) {
    std::ostringstream out;
    out << "{\n"
        << "  \"game_id\": " << quote(rom.header.game_id) << ",\n"
        << "  \"internal_name\": " << quote(rom.header.internal_name) << ",\n"
        << "  \"byte_order\": " << quote(byte_order_name(rom.original)) << ",\n"
        << "  \"cic\": " << quote(cic_name(rom.cic)) << ",\n"
        << "  \"country\": " << quote(std::string(1, rom.header.country ? rom.header.country : '?'))
        << ",\n"
        << "  \"version\": " << int(rom.header.version) << ",\n"
        << "  \"rom_size\": " << rom.size() << ",\n"
        << "  \"rom_hash\": \"" << std::hex << rom.hash << std::dec << "\",\n"
        << "  \"crc1\": " << quote(hex(rom.header.crc1)) << ",\n"
        << "  \"crc2\": " << quote(hex(rom.header.crc2)) << ",\n"
        << "  \"entrypoint\": " << quote(hex(rom.load_address)) << ",\n"
        << "  \"entrypoint_verified\": " << (rom.load_address_verified ? "true" : "false") << ",\n"
        << "  \"sections\": [\n";

    for (size_t i = 0; i < analysis.sections.size(); i++) {
        const SectionInfo &section = analysis.sections[i];
        out << "    { \"name\": " << quote(section.name) << ", \"rom\": " << quote(hex(section.rom))
            << ", \"vram\": " << quote(hex(section.vram)) << ", \"size\": " << quote(hex(section.size))
            << ", \"functions\": " << section.functions.size() << " }"
            << (i + 1 < analysis.sections.size() ? "," : "") << "\n";
    }

    out << "  ],\n"
        << "  \"functions\": " << analysis.report.functions_found << ",\n"
        << "  \"from_sweep\": " << analysis.report.from_sweep << ",\n"
        << "  \"from_calls\": " << analysis.report.from_calls << ",\n"
        << "  \"calls_outside_sections\": " << analysis.report.calls_outside << ",\n"
        << "  \"calls_on_boundary\": " << analysis.report.calls_on_boundary << ",\n"
        << "  \"calls_off_boundary\": " << analysis.report.calls_off_boundary << ",\n"
        << "  \"invalid_words\": " << analysis.report.invalid_words << ",\n"
        << "  \"notes\": [\n";
    for (size_t i = 0; i < analysis.report.notes.size(); i++) {
        out << "    " << quote(analysis.report.notes[i])
            << (i + 1 < analysis.report.notes.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace n64rip
