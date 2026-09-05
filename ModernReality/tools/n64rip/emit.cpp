// SPDX-License-Identifier: GPL-3.0-or-later
// Writing out what the analysis found, in the three shapes the pipeline needs.

#include "n64rip.hpp"

#include <algorithm>

#include <cstdio>
#include <sstream>
#include <vector>

namespace n64rip {
namespace {

std::string hex(uint32_t value, int width = 8) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%0*X", width, value);
    return buffer;
}

/// How many of the record's unpacked segments turned into code.
///
/// A segment is there when a section of its name came out of the analysis with
/// functions in it. The memory image is always the full eight megabytes, so a
/// segment the game has not written yet reads as zeroes and recovers nothing --
/// which is the difference this counts, and which is how the pipeline knows to
/// run the game again.
unsigned unpacked_recovered(const Analysis &analysis, const TitleRecord *record) {
    if (record == nullptr) {
        return 0;
    }
    unsigned found = 0;
    for (const UnpackedSection &segment : record->unpacked) {
        for (const SectionInfo &section : analysis.sections) {
            if (section.name == segment.name && !section.functions.empty()) {
                found++;
                break;
            }
        }
    }
    return found;
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
            // A libultra name is not decoration: the recompiler matches on it
            // to substitute the runtime's own implementation. Everything else
            // is named after where it lives, since the image says no more.
            const std::string name =
                function.name.empty() ? ("func_" + hex(function.vram).substr(2)) : function.name;
            out << "[[section.functions]]\n"
                << "name = " << quote(name) << "\n"
                << "vram = " << hex(function.vram) << "\n"
                << "size = " << hex(function.size, 1) << "\n\n";
        }
    }
    return out.str();
}

std::string emit_recomp_toml(const Rom &rom, const Analysis &analysis,
                             const std::string &symbols_path, const std::string &rom_path,
                             const std::string &output_dir) {
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

    // Functions that drive coprocessor 0 and that no signature named. See
    // needs_stub() in analyze.cpp for why the whole function goes rather than
    // the instruction.
    std::vector<std::string> stubs;
    for (const SectionInfo &section : analysis.sections) {
        for (const FunctionRange &function : section.functions) {
            // Only unnamed functions are ever stubbed; a named one is already
            // being replaced by the runtime, and the recompiler rejects a stub
            // naming a function it has renamed.
            if (function.stub && function.name.empty()) {
                stubs.push_back("func_" + hex(function.vram).substr(2));
            }
        }
    }
    if (!stubs.empty()) {
        out << "\n# Hardware routines the runtime models itself. Each one is a function a\n"
            << "# wider libultra signature database would have named instead.\n"
            << "[patches]\nstubs = [\n";
        for (const std::string &name : stubs) {
            out << "    " << quote(name) << ",\n";
        }
        out << "]\n";
    }
    return out.str();
}

std::string emit_info_json(const Rom &rom, const Analysis &analysis,
                           const TitleRecord *record) {
    std::ostringstream out;
    out << "{\n"
        << "  \"game_id\": " << quote(rom.header.game_id) << ",\n"
        << "  \"internal_name\": " << quote(rom.header.internal_name) << ",\n"
        << "  \"display_name\": " << quote(display_name(rom, record)) << ",\n"
        << "  \"save_type\": " << quote(save_type_name(analysis.save_type)) << ",\n"
        << "  \"save_type_evidence\": " << quote(analysis.save_type_evidence) << ",\n"
        << "  \"title_record\": " << quote(record == nullptr ? "" : record->path) << ",\n"
        // How many segments this game does not carry in its cartridge, and how
        // many of them the memory image actually held. Zero and zero for every
        // game that keeps its code where a reader can find it.
        //
        // The second number is what says whether the game has been run enough
        // times. A game that unpacks in stages only reveals the next stage once
        // the last one is recompiled and running, so the two numbers agree only
        // when there is nothing left to find.
        << "  \"unpacked_segments\": " << (record == nullptr ? 0u : unsigned(record->unpacked.size()))
        << ",\n"
        << "  \"unpacked_recovered\": " << unpacked_recovered(analysis, record) << ",\n"
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
        << "  \"microcode\": [\n";

    // What the module has to recompile for the signal processor, and where the
    // game loads it -- which is how the module picks between blocks, since the
    // task names its microcode by address.
    for (size_t i = 0; i < analysis.microcode.size(); i++) {
        const MicrocodeInfo &block = analysis.microcode[i];
        out << "    { \"name\": " << quote(block.name) << ", \"rom\": " << quote(hex(block.rom))
            << ", \"vram\": " << quote(hex(block.vram)) << ", \"size\": " << quote(hex(block.size))
            << ", \"text_address\": " << quote(hex(block.text_address))
            << ", \"cop2\": " << int(block.cop2_density * 100.0 + 0.5)
            << ", \"branch_targets\": [";
        for (size_t t = 0; t < block.branch_targets.size(); t++) {
            out << (t == 0 ? "" : ", ") << quote(hex(block.branch_targets[t]));
        }
        out << "] }" << (i + 1 < analysis.microcode.size() ? "," : "") << "\n";
    }

    out << "  ],\n"
        << "  \"functions\": " << analysis.report.functions_found << ",\n"
        << "  \"from_sweep\": " << analysis.report.from_sweep << ",\n"
        << "  \"from_calls\": " << analysis.report.from_calls << ",\n"
        << "  \"calls_outside_sections\": " << analysis.report.calls_outside << ",\n"
        << "  \"calls_on_boundary\": " << analysis.report.calls_on_boundary << ",\n"
        << "  \"calls_off_boundary\": " << analysis.report.calls_off_boundary << ",\n"
        << "  \"invalid_words\": " << analysis.report.invalid_words << ",\n"
        << "  \"named_functions\": " << analysis.report.named_functions << ",\n"
        << "  \"named_new_boundaries\": " << analysis.report.named_new_boundaries << ",\n"

        << "  \"stubbed_functions\": " << analysis.report.stubbed_functions << ",\n"
        << "  \"stubbed_unstructured\": " << analysis.report.stubbed_unstructured << ",\n"
        << "  \"extended_over_shared_tail\": " << analysis.report.extended_over_shared_tail
        << ",\n"
        // The game's own syscall handler, and how many stubs reach it. Zero
        // and zero for a game that leaves the exception to libultra, which is
        // nearly all of them.
        << "  \"syscall_handler\": " << quote(hex(analysis.syscall_handler)) << ",\n"
        << "  \"syscall_table\": " << quote(hex(analysis.syscall_stubs)) << ",\n"
        << "  \"syscall_table_size\": " << quote(hex(analysis.syscall_stubs_size)) << ",\n"
        << "  \"syscall_stubs\": " << analysis.report.syscall_stubs << ",\n"
        << "  \"merged_boundaries\": " << analysis.report.merged_boundaries << ",\n"
        << "  \"names_without_implementations\": "
        << analysis.report.names_without_implementations << ",\n"
        << "  \"named_without_implementations\": [\n";
    // Which ones, not just how many. A count says a gap exists; the names say
    // what is in it, and whether any of them is a driver -- a libultra
    // function the runtime does not replace runs as translated MIPS over
    // hardware that is not there, and reads as a game that runs and does not
    // do the thing.
    {
        std::vector<std::pair<std::string, uint32_t>> known;
        for (const SectionInfo &section : analysis.sections) {
            for (const FunctionRange &function : section.functions) {
                if (!function.known_as.empty() && function.name.empty()) {
                    known.emplace_back(function.known_as, function.vram);
                }
            }
        }
        std::sort(known.begin(), known.end());
        known.erase(std::unique(known.begin(), known.end()), known.end());
        // The address as well as the name, because what somebody does with
        // this is write a [[function]] line, and a [[function]] line is an
        // address and a name.
        for (size_t i = 0; i < known.size(); i++) {
            out << "    { \"name\": " << quote(known[i].first) << ", \"vram\": "
                << quote(hex(known[i].second)) << " }" << (i + 1 < known.size() ? "," : "")
                << "\n";
        }
    }
    out << "  ],\n"
        << "  \"stubbed\": [\n";
    // Every address here is a function the runtime would have implemented if
    // something had named it, and a line somebody can put in a title record.
    {
        std::vector<std::string> stubbed;
        for (const SectionInfo &section : analysis.sections) {
            for (const FunctionRange &function : section.functions) {
                if (function.stub && function.name.empty()) {
                    stubbed.push_back(hex(function.vram));
                }
            }
        }
        for (size_t i = 0; i < stubbed.size(); i++) {
            out << "    " << quote(stubbed[i]) << (i + 1 < stubbed.size() ? "," : "") << "\n";
        }
    }
    out << "  ],\n"
        << "  \"notes\": [\n";
    for (size_t i = 0; i < analysis.report.notes.size(); i++) {
        out << "    " << quote(analysis.report.notes[i])
            << (i + 1 < analysis.report.notes.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace n64rip
