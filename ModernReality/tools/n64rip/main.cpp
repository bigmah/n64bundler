// SPDX-License-Identifier: GPL-3.0-or-later
//
//   n64rip inspect <rom>                    what the header and boot code say
//   n64rip analyze <rom> --out-dir <dir>    recover the metadata and write it
//
// `analyze` writes four files into --out-dir, named after the game ID:
//
//   <ID>.symbols.toml   sections and functions, for N64Recomp's symbols input
//   <ID>.recomp.toml    the recompiler configuration that points at it
//   <ID>.info.json      the header, the hash, and what the analysis found
//   <ID>.z64            the ROM normalised to big endian, if it was not already
//
// The normalised copy exists because both the recompiler and the runtime read
// the ROM as big endian, and a .v64 or .n64 dump is neither.

#include "n64rip.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: n64rip inspect <rom>\n"
                 "       n64rip analyze <rom> --out-dir <dir> [--signatures <db>]\n"
                 "                     [--runtime-provides <list>] [--title <record.toml>]\n"
                 "                     [--titles <dir>] [--quiet]\n");
}

bool write_file(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "error: could not write %s\n", path.string().c_str());
        return false;
    }
    file << contents;
    return file.good();
}

void print_summary(const n64rip::Rom &rom) {
    std::printf("Game name: %s\n", rom.header.internal_name.c_str());
    std::printf("Game ID:   %s\n", rom.header.game_id.c_str());
    std::printf("Byte order: %s\n", n64rip::byte_order_name(rom.original));
    std::printf("Boot chip: %s\n", n64rip::cic_name(rom.cic));
    std::printf("Entrypoint: 0x%08X%s\n", rom.load_address,
                rom.load_address_verified ? "" : "  (unconfirmed)");
    std::printf("ROM size:  %.1f MB\n", double(rom.size()) / (1024.0 * 1024.0));
    std::printf("ROM hash:  %016llx\n", static_cast<unsigned long long>(rom.hash));
}

void print_analysis(const n64rip::Analysis &analysis) {
    std::printf("\nSections:\n");
    for (const n64rip::SectionInfo &section : analysis.sections) {
        std::printf("  %-8s rom 0x%06X  vram 0x%08X  %7u bytes  %5zu functions\n",
                    section.name.c_str(), section.rom, section.vram, section.size,
                    section.functions.size());
    }
    const n64rip::AnalysisReport &report = analysis.report;
    std::printf("\n%zu functions recovered: %zu by following calls, %zu by sweeping\n",
                report.functions_found, report.from_calls, report.from_sweep);

    // The one number that says whether the boundaries are right: a `jal` names
    // the first instruction of a function, so every call that stays inside a
    // recovered section should land on a recovered function.
    const size_t landed = report.calls_on_boundary + report.calls_off_boundary;
    if (landed > 0) {
        std::printf("%zu of %zu internal calls land on a function boundary (%.2f%%)\n",
                    report.calls_on_boundary, landed,
                    100.0 * double(report.calls_on_boundary) / double(landed));
    }
    if (report.named_functions > 0) {
        std::printf("%zu functions named from libultra signatures, %zu of them at boundaries "
                    "the walk had not found\n",
                    report.named_functions, report.named_new_boundaries);
    }
    if (report.named_by_shape > 0) {
        std::printf("%zu of them by shape rather than by signature: libultra accessors too short "
                    "to fingerprint,\n    named by the hardware register they read\n",
                    report.named_by_shape);
    }
    if (report.names_without_implementations > 0) {
        std::printf("%zu signature matches were left nameless: the runtime has no "
                    "implementation to stand in for them\n",
                    report.names_without_implementations);
    }
    if (report.stubbed_functions > 0) {
        std::printf("%zu functions stubbed: they drive hardware the runtime models itself, and "
                    "no signature named them.\n"
                    "    Each one is a libultra function running as a no-op. Their addresses are "
                    "in the info file;\n"
                    "    a title record that names one gets the runtime's implementation of it "
                    "instead.\n",
                    report.stubbed_functions);
    }
    if (!report.stubbed_by_name.empty()) {
        std::printf("    %zu of those are functions a signature identified and the runtime does "
                    "not implement,\n    so the behaviour is simply gone:",
                    report.stubbed_by_name.size());
        for (size_t i = 0; i < report.stubbed_by_name.size(); i++) {
            std::printf("%s%s", i == 0 ? " " : ", ", report.stubbed_by_name[i].c_str());
        }
        std::printf("\n");
    }
    if (report.split_boundaries > 0) {
        std::printf("%zu boundaries added where a call landed inside a function\n",
                    report.split_boundaries);
    }
    if (report.merged_boundaries > 0) {
        std::printf("%zu boundaries merged where a branch crossed them\n",
                    report.merged_boundaries);
    }
    if (report.stubbed_unstructured > 0) {
        std::printf("%zu functions stubbed: hand-written assembly that does not divide into "
                    "functions\n",
                    report.stubbed_unstructured);
    }
    if (report.calls_outside > 0) {
        std::printf("%zu calls point outside every section found; that code was not "
                    "recompiled\n",
                    report.calls_outside);
    }
    for (const std::string &note : report.notes) {
        std::printf("note: %s\n", note.c_str());
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 2;
    }

    const std::string command = argv[1];
    const std::string rom_path = argv[2];
    std::string out_dir;
    std::string signatures_path;
    std::string provides_path;
    std::string title_path;
    std::string titles_dir;
    bool quiet = false;

    for (int i = 3; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--signatures" && i + 1 < argc) {
            signatures_path = argv[++i];
        } else if (arg == "--runtime-provides" && i + 1 < argc) {
            provides_path = argv[++i];
        } else if (arg == "--title" && i + 1 < argc) {
            title_path = argv[++i];
        } else if (arg == "--titles" && i + 1 < argc) {
            titles_dir = argv[++i];
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    std::string error;
    std::optional<n64rip::Rom> rom = n64rip::load_rom(rom_path, error);
    if (!rom) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (command == "inspect") {
        print_summary(*rom);
        return 0;
    }

    if (command != "analyze") {
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        usage();
        return 2;
    }

    if (out_dir.empty()) {
        std::fprintf(stderr, "error: analyze needs --out-dir\n");
        return 2;
    }

    // Without a signature database the recovered code is nameless, and the
    // recompiler will translate libultra's own hardware routines rather than
    // substituting the runtime's implementations of them. It builds; it does
    // not get far.
    n64sig::Database signatures;
    bool have_signatures = false;
    if (!signatures_path.empty()) {
        if (!signatures.load(signatures_path, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        have_signatures = true;
    }

    n64rip::RuntimeProvides provides;
    bool have_provides = false;
    if (!provides_path.empty()) {
        if (!n64rip::load_runtime_provides(provides_path, provides, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        have_provides = true;
    }

    // A record is looked up by game ID unless one was named outright. Most of
    // the time there is none, and the analysis stands on its own.
    n64rip::TitleRecord record;
    bool have_record = false;
    if (title_path.empty() && !titles_dir.empty() && !rom->header.game_id.empty()) {
        const std::filesystem::path candidate =
            std::filesystem::path(titles_dir) / rom->header.game_id / "title.toml";
        if (std::filesystem::exists(candidate)) {
            title_path = candidate.string();
        }
    }
    if (!title_path.empty()) {
        if (!n64rip::load_title_record(title_path, record, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        // A record's addresses were measured against one image. A different
        // revision of the same cartridge moves them, and applying them anyway
        // would put sections where there is no code. Analysing without the
        // record is the right answer -- it is what an unknown ROM gets, and it
        // still produces a game -- so this is a note rather than a failure.
        if (record.rom_hash != 0 && record.rom_hash != rom->hash) {
            std::fprintf(stderr,
                         "note: %s was measured against a different dump of this cartridge "
                         "(record %016llx, this ROM %016llx); analysing without it\n",
                         title_path.c_str(), (unsigned long long)record.rom_hash,
                         (unsigned long long)rom->hash);
        } else {
            have_record = true;
        }
    }

    const n64rip::Analysis analysis = n64rip::analyze(
        *rom, have_signatures ? &signatures : nullptr, have_provides ? &provides : nullptr,
        have_record ? &record : nullptr);
    if (analysis.sections.empty() || analysis.sections.front().functions.empty()) {
        std::fprintf(stderr,
                     "error: no code was recovered from this ROM. It may be encrypted, "
                     "compressed, or not an N64 game at all.\n");
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const std::filesystem::path dir(out_dir);
    const std::string id = rom->header.game_id.empty() ? "UNKNOWN" : rom->header.game_id;

    // The recompiler reads the ROM itself, so it needs one in the order it
    // expects. A z64 dump is already right and is not copied.
    std::filesystem::path recomp_rom = std::filesystem::absolute(rom_path);
    if (rom->original != n64rip::ByteOrder::Z64) {
        recomp_rom = dir / (id + ".z64");
        std::ofstream normalised(recomp_rom, std::ios::binary);
        normalised.write(reinterpret_cast<const char *>(rom->data.data()),
                         std::streamsize(rom->data.size()));
        if (!normalised.good()) {
            std::fprintf(stderr, "error: could not write the normalised ROM to %s\n",
                         recomp_rom.string().c_str());
            return 1;
        }
    }

    const std::filesystem::path symbols = dir / (id + ".symbols.toml");
    const std::filesystem::path config = dir / (id + ".recomp.toml");
    const std::filesystem::path info = dir / (id + ".info.json");
    const std::filesystem::path generated = dir / "generated";
    std::filesystem::create_directories(generated, ec);

    if (!write_file(symbols, n64rip::emit_symbols_toml(*rom, analysis)) ||
        !write_file(config, n64rip::emit_recomp_toml(*rom, analysis, symbols.string(),
                                                     recomp_rom.string(), generated.string())) ||
        !write_file(info, n64rip::emit_info_json(*rom, analysis,
                                                 have_record ? &record : nullptr))) {
        return 1;
    }

    if (!quiet) {
        print_summary(*rom);
        std::printf("Title:     %s\n",
                    n64rip::display_name(*rom, have_record ? &record : nullptr).c_str());
        print_analysis(analysis);
        std::printf("Save type: %s (%s)\n", n64rip::save_type_name(analysis.save_type),
                    analysis.save_type_evidence.c_str());
        std::printf("\nwrote %s\n", symbols.string().c_str());
        std::printf("wrote %s\n", config.string().c_str());
        std::printf("wrote %s\n", info.string().c_str());
    }
    return 0;
}
