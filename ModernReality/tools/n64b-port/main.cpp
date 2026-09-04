// SPDX-License-Identifier: GPL-3.0-or-later
//
// n64b-port - turn what the analyser recovered into a loadable game module.
//
//   n64b-port build --analysis <dir> --rom <rom.z64> --out <module.dylib>
//   n64b-port info  <module.dylib>
//
// Three steps, in order:
//
//   1. run N64Recomp over the analyser's symbols, which writes a few thousand
//      recompiled functions as C, a section table, and a header declaring them
//   2. write module.cpp: the section table wrapped in the descriptor the host
//      reads, plus the facts about the cartridge that are not in the code
//   3. compile all of it to one arm64 dylib
//
// Everything is cached on a key made of the ROM's hash, the tools' revisions
// and the compiler flags, so re-adding a ROM that has already been ported is
// instant and changing anything that matters is not.
//
// The module holds no game data. The recompiled functions are a translation of
// the ROM's instructions, the section table is a list of addresses, and the ROM
// itself stays a separate file that the runtime reads at startup.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace fs = std::filesystem;

namespace {

// Baked in by CMake so the tool works from anywhere without being told where
// its siblings live. Every one of them can still be overridden on the command
// line, which is what a build tree that has moved needs.
#ifndef N64B_DEFAULT_RECOMP
#define N64B_DEFAULT_RECOMP ""
#endif
#ifndef N64B_DEFAULT_RSP_RECOMP
#define N64B_DEFAULT_RSP_RECOMP ""
#endif
#ifndef N64B_DEFAULT_INCLUDES
#define N64B_DEFAULT_INCLUDES ""
#endif

/// Bumped when anything here changes what a module contains, so that every
/// cached module built by an older revision is rebuilt rather than trusted.
constexpr const char *kPortRevision = "n64b-port 1";

bool porcelain = false;

/// The porcelain protocol this shares with recompn64: `@@`-prefixed lines
/// carry structured events, everything else is log text. Run by hand, the same
/// events are printed as indented English instead -- a tool nobody can read the
/// output of is a tool nobody debugs.
void event(const char *kind, const std::string &text) {
    if (porcelain) {
        std::printf("@@%s %s\n", kind, text.c_str());
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(kind, "fail") == 0) {
        std::fprintf(stderr, "error: %s\n", text.c_str());
    } else if (std::strcmp(kind, "step") == 0) {
        // "1 3 Recompiling MIPS to C" -> "    [1/3] Recompiling MIPS to C"
        const size_t first = text.find(' ');
        const size_t second = text.find(' ', first == std::string::npos ? 0 : first + 1);
        if (second != std::string::npos) {
            std::printf("    [%s/%s] %s\n", text.substr(0, first).c_str(),
                        text.substr(first + 1, second - first - 1).c_str(),
                        text.substr(second + 1).c_str());
        }
    } else if (std::strcmp(kind, "info") == 0) {
        const size_t equals = text.find('=');
        if (equals != std::string::npos) {
            std::printf("    %s: %s\n", text.substr(0, equals).c_str(),
                        text.substr(equals + 1).c_str());
        }
    }
    std::fflush(stdout);
}

void info(const std::string &text) { event("info", text); }
void fail(const std::string &text) { event("fail", text); }

std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool write_file(const fs::path &path, const std::string &contents) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << contents;
    return file.good();
}

// ---------------------------------------------------------------------------
// Just enough JSON to read the analyser's info file, which we also write.
//
// A parser is not worth linking for a flat object of strings and numbers whose
// exact shape is fixed a few hundred lines away in emit.cpp. Anything more
// nested than that belongs in a real parser, and there is nothing more nested.

std::string json_string(const std::string &text, const std::string &key) {
    const std::string needle = "\"" + key + "\":";
    size_t at = text.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    at = text.find('"', at + needle.size());
    if (at == std::string::npos) {
        return {};
    }
    std::string out;
    for (size_t i = at + 1; i < text.size(); i++) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char escaped = text[++i];
            switch (escaped) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(escaped); break;
            }
            continue;
        }
        if (text[i] == '"') {
            break;
        }
        out.push_back(text[i]);
    }
    return out;
}

uint64_t json_number(const std::string &text, const std::string &key, uint64_t fallback = 0) {
    const std::string needle = "\"" + key + "\":";
    size_t at = text.find(needle);
    if (at == std::string::npos) {
        return fallback;
    }
    at += needle.size();
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
        at++;
    }
    if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at]))) {
        return fallback;
    }
    return std::strtoull(text.c_str() + at, nullptr, 10);
}

/// The save types in the descriptor, in the order the ABI numbers them.
int save_type_value(const std::string &name) {
    if (name == "none") return 0;
    if (name == "eep4k") return 1;
    if (name == "eep16k") return 2;
    if (name == "sram") return 3;
    if (name == "flashram") return 4;
    return 5; // allow_all
}

// ---------------------------------------------------------------------------
// Running things

std::string shell_quote(const std::string &value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

/// Runs a command, echoing its output and keeping a copy. Used for the
/// recompiler, whose complaints have to be read as well as shown.
int run_capture(const std::vector<std::string> &argv, std::string &output) {
    std::string command;
    for (const std::string &arg : argv) {
        command += shell_quote(arg);
        command += ' ';
    }
    command += "2>&1";

    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return 127;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::fputs(buffer, stdout);
        output += buffer;
    }
    std::fflush(stdout);
    const int status = pclose(pipe);
    if (status == -1) {
        return 127;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/// Runs a command, returning its exit status. Output goes wherever ours does,
/// so a compiler error lands in the same log as everything else.
int run(const std::vector<std::string> &argv, bool quiet = false) {
    std::string command;
    for (const std::string &arg : argv) {
        command += shell_quote(arg);
        command += ' ';
    }
    if (quiet) {
        command += ">/dev/null 2>&1";
    }
    const int status = std::system(command.c_str());
    if (status == -1) {
        return 127;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

std::vector<std::string> split(const std::string &value, char separator) {
    std::vector<std::string> out;
    std::string current;
    for (char c : value) {
        if (c == separator) {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

/// Identity of a tool for the cache key: its size and modification time, which
/// changes whenever it is rebuilt and costs nothing to read.
std::string tool_revision(const fs::path &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec);
    if (ec) {
        return path.string() + ":missing";
    }
    return path.filename().string() + ":" + std::to_string(size) + ":" +
           std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

// ---------------------------------------------------------------------------
// Teaching the analysis what the recompiler will not take
//
// n64rip stubs the functions it can tell are untranslatable: the ones that
// drive hardware, the ones with an instruction the recompiler has no case for,
// the ones whose branches leave the function. It cannot predict all of them.
// The recompiler does its own analysis -- working out where an indirect jump's
// table is, above all -- and a function whose table it cannot find is one it
// refuses, and there is no way to know that without asking it.
//
// So we ask it. A refusal names the function, and a named function can be
// stubbed and the recompile tried again. Two or three rounds settles every ROM
// tried, and each stub is reported, because a stubbed function is a piece of
// the game that does nothing.

/// The function names in a recompiler failure. It says the same thing three
/// ways depending on where it gave up.
std::vector<std::string> refused_functions(const std::string &output) {
    static const char *const prefixes[] = {
        "Error recompiling ",
        "Error in recompiling ",
        "Failed to analyze ",
    };
    std::vector<std::string> names;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        for (const char *prefix : prefixes) {
            const size_t at = line.find(prefix);
            if (at == std::string::npos) {
                continue;
            }
            std::string name = line.substr(at + std::strlen(prefix));
            // "Error in recompiling X, clearing output file"
            const size_t comma = name.find(',');
            if (comma != std::string::npos) {
                name.resize(comma);
            }
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) {
                name.pop_back();
            }
            if (!name.empty() &&
                std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
            break;
        }
    }
    return names;
}

/// Rewrite a recompiler configuration with more functions stubbed.
///
/// The `[patches]` table is the last thing n64rip writes, which is what makes
/// splitting the file at it safe. If that ever stops being true this has to
/// grow a TOML parser; until then it is thirty lines instead of a dependency.
std::string with_stubs(const std::string &config, const std::vector<std::string> &extra) {
    std::vector<std::string> names;
    std::string head = config;

    const size_t patches = config.find("[patches]");
    if (patches != std::string::npos) {
        head = config.substr(0, patches);
        const std::string tail = config.substr(patches);
        for (size_t at = tail.find('"'); at != std::string::npos; at = tail.find('"', at + 1)) {
            const size_t close = tail.find('"', at + 1);
            if (close == std::string::npos) {
                break;
            }
            names.push_back(tail.substr(at + 1, close - at - 1));
            at = close;
        }
    }
    for (const std::string &name : extra) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }

    std::ostringstream out;
    out << head;
    out << "[patches]\nstubs = [\n";
    for (const std::string &name : names) {
        out << "    \"" << name << "\",\n";
    }
    out << "]\n";
    return out.str();
}

// ---------------------------------------------------------------------------

struct Options {
    fs::path analysis;
    fs::path rom;
    fs::path out;
    fs::path recomp = N64B_DEFAULT_RECOMP;
    fs::path rsp_recomp = N64B_DEFAULT_RSP_RECOMP;
    std::string compiler = "clang";
    std::vector<std::string> includes;
    std::string optimisation = "-O2";
    unsigned jobs = 0;
    bool force = false;
    bool keep_c = false;
    /// Build the module with the recompiler's trace mode on, so the host can
    /// say which functions a game entered before it went wrong.
    bool trace = false;
    /// A game address to report every access to, or empty. Implies --trace.
    std::string watch;
};

/// The flags the recompiled C is built with, and why each one is here.
std::vector<std::string> compile_flags(const Options &options) {
    return {
        options.optimisation,
        // Every memory access in the output reinterprets the same rdram block
        // through a different type. That is exactly what strict aliasing says
        // cannot happen, so it has to be off; with it on the compiler is
        // entitled to reorder stores past loads and does.
        "-fno-strict-aliasing",
        // The N64's FPU has no fused multiply-add. Letting the compiler
        // contract a multiply and an add into one changes the rounding, and
        // the game's physics is downstream of it.
        "-ffp-contract=off",
        "-fno-fast-math",
        // Position-independent because it is going into a dylib.
        "-fPIC",
        // The output is machine-written and trips these on purpose: a register
        // assigned and not read is a real instruction, and a label with no
        // branch to it is the target of one the analysis proved unreachable.
        "-Wno-unused-variable",
        "-Wno-unused-but-set-variable",
        "-Wno-unused-label",
        "-Wno-parentheses-equality",
        "-Wno-unused-function",
    };
}

/// In trace mode the recompiler puts `#include "trace.h"` at the top of every
/// generated file and `TRACE_ENTRY()` at the top of every function, and leaves
/// the header to the project. This is that header.
constexpr const char *kTraceHeader = R"(// Written by n64b-port for --trace.
//
// The recompiler calls this at the top of every function it emits. The host
// keeps a ring buffer of the last few hundred and prints it when the process
// ends, however it ends -- which is how you find the function that went wrong
// several thousand calls before anything noticed.
#ifndef N64B_TRACE_H
#define N64B_TRACE_H

void n64b_trace(const char *name);

#define TRACE_ENTRY() n64b_trace(__func__);
// The recompiler emits one of these before every return. The ring buffer only
// needs entries to reconstruct the path, so this is deliberately nothing.
#define TRACE_RETURN() ;

#ifdef N64B_WATCH
// A watch on one game address, for the question static analysis cannot answer:
// which function touches this global? Every 32-bit access in the recompiled
// code goes through MEM_W, so redefining it after recomp.h has had its say is
// enough. Slow, and only ever on in a --watch build.
void n64b_watch(unsigned address, const char *where);

static inline int *n64b_mem_w(unsigned char *rdram, long long address, const char *where) {
    if ((unsigned)address == (unsigned)N64B_WATCH) {
        n64b_watch((unsigned)address, where);
    }
    return (int *)(rdram + (address - 0xFFFFFFFF80000000ll));
}

#undef MEM_W
#define MEM_W(offset, reg) (*n64b_mem_w(rdram, (long long)((reg) + (offset)), __func__))
#endif

#endif
)";

std::string cache_key(const Options &options, const std::string &rom_hash,
                      const std::string &symbols) {
    std::string material;
    material += kPortRevision;
    material += "|rom=" + rom_hash;
    material += "|recomp=" + tool_revision(options.recomp);
    material += "|symbols=" + std::to_string(XXH3_64bits(symbols.data(), symbols.size()));
    material += "|cc=" + options.compiler;
    material += options.trace ? "|trace" : "";
    material += "|watch=" + options.watch;
    for (const std::string &flag : compile_flags(options)) {
        material += "|" + flag;
    }
    for (const std::string &include : options.includes) {
        material += "|I" + include;
    }
    char out[24];
    std::snprintf(out, sizeof(out), "%016llx",
                  (unsigned long long)XXH3_64bits(material.data(), material.size()));
    return out;
}

// ---------------------------------------------------------------------------

std::string quote_c(const std::string &value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out += "\"";
    return out;
}

/// The one file of the module that is not a translation of the ROM: the
/// descriptor the host reads to find out what it has just opened.
std::string emit_module_cpp(const std::string &info_json, const std::string &rom_hash_hex,
                            const fs::path &rsp_source) {
    std::ostringstream out;
    out << "// SPDX-License-Identifier: GPL-3.0-or-later\n"
        << "// Written by n64b-port. The descriptor for one recompiled game.\n"
        << "//\n"
        << "// Everything here except the section table is a fact about the cartridge\n"
        << "// that the instruction stream does not carry. The section table itself is\n"
        << "// the recompiler's own output, included rather than copied.\n\n"
        << "#include \"modernreality/module_abi.h\"\n";

    if (!rsp_source.empty()) {
        out << "#include \"librecomp/rsp.hpp\"\n";
    }
    out << "\n#include \"recomp_overlays.inl\"\n\n";

    if (!rsp_source.empty()) {
        out << "// The game's audio microcode, recompiled by RSPRecomp.\n"
            << "extern RspExitReason " << rsp_source.stem().string()
            << "(uint8_t *rdram, uint32_t ucode_addr);\n\n"
            << "static n64b_rsp_ucode_func select_microcode(const OSTask *task) {\n"
            << "    // Graphics tasks never reach here; ultramodern routes those to the\n"
            << "    // renderer. What is left is the audio list and the odd JPEG task.\n"
            << "    return " << rsp_source.stem().string() << ";\n"
            << "}\n\n";
    } else {
        out << "// No microcode was recompiled for this game, so the host reports the\n"
            << "// unhandled task rather than running something that is not it.\n"
            << "static n64b_rsp_ucode_func select_microcode(const OSTask *task) {\n"
            << "    (void)task;\n"
            << "    return nullptr;\n"
            << "}\n\n";
    }

    const std::string game_id = json_string(info_json, "game_id");
    const std::string internal_name = json_string(info_json, "internal_name");
    const std::string display = json_string(info_json, "display_name");
    const std::string entrypoint = json_string(info_json, "entrypoint");
    const std::string save_type = json_string(info_json, "save_type");
    const uint64_t rom_size = json_number(info_json, "rom_size");

    out << "extern \"C\" N64B_MODULE_EXPORT const n64b_module_v1 n64b_module = {\n"
        << "    .abi_version = N64B_MODULE_ABI,\n"
        << "    .game_id = " << quote_c(game_id) << ",\n"
        << "    .internal_name = " << quote_c(internal_name) << ",\n"
        << "    .display_name = " << quote_c(display) << ",\n"
        << "    .rom_hash = 0x" << rom_hash_hex << "ull,\n"
        << "    .rom_size = " << rom_size << "ull,\n"
        << "    .entrypoint_address = " << entrypoint << "u,\n"
        << "    .entrypoint = recomp_entrypoint,\n"
        << "    .code_sections = section_table,\n"
        << "    .num_code_sections = ARRLEN(section_table),\n"
        << "    .total_num_sections = num_sections,\n"
        << "    .overlays_by_index = overlay_sections_by_index,\n"
        << "    .num_overlays = ARRLEN(overlay_sections_by_index),\n"
        << "    .get_rsp_microcode = select_microcode,\n"
        << "    .save_type = " << save_type_value(save_type) << ", // " << save_type << "\n"
        << "    .analyser_revision = " << quote_c(json_string(info_json, "analyser")) << ",\n"
        << "    .builder_revision = " << quote_c(kPortRevision) << ",\n"
        << "};\n";
    return out.str();
}

// ---------------------------------------------------------------------------

int build(Options options) {
    if (options.analysis.empty() || options.rom.empty() || options.out.empty()) {
        fail("build needs --analysis, --rom and --out");
        return 2;
    }
    if (!fs::exists(options.recomp)) {
        fail("the recompiler is not at " + options.recomp.string() +
             "; pass --recomp with where it is");
        return 1;
    }

    // The analyser names everything after the game ID, and there is exactly one
    // game in a directory it wrote.
    std::string id;
    for (const fs::directory_entry &entry : fs::directory_iterator(options.analysis)) {
        const std::string name = entry.path().filename().string();
        const std::string suffix = ".info.json";
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            id = name.substr(0, name.size() - suffix.size());
            break;
        }
    }
    if (id.empty()) {
        fail("no <ID>.info.json in " + options.analysis.string() + "; run n64rip analyze first");
        return 1;
    }

    const std::string info_json = read_file(options.analysis / (id + ".info.json"));
    const std::string symbols = read_file(options.analysis / (id + ".symbols.toml"));
    const fs::path config = options.analysis / (id + ".recomp.toml");
    if (info_json.empty() || symbols.empty() || !fs::exists(config)) {
        fail("the analysis in " + options.analysis.string() + " is incomplete");
        return 1;
    }

    const std::string rom_hash = json_string(info_json, "rom_hash");
    const std::string key = cache_key(options, rom_hash, symbols);
    const fs::path stamp = options.out.string() + ".stamp";

    if (!options.force && fs::exists(options.out) && read_file(stamp) == key) {
        info("module=" + options.out.string());
        info("cached=1");
        event("ok", "");
        return 0;
    }

    std::error_code ec;
    fs::create_directories(options.out.parent_path(), ec);
    fs::remove(stamp, ec);

    const fs::path generated = options.analysis / "generated";
    fs::create_directories(generated, ec);

    event("step", "1 3 Recompiling MIPS to C");
    {
        // What every round starts from. With tracing on that is a copy of the
        // analyser's configuration with trace_mode set, and the stub rounds
        // below have to amend *that* -- amending the original would silently
        // drop the tracing the moment one function had to be stubbed.
        fs::path base = config;
        if (options.trace) {
            std::string traced = read_file(config);
            const size_t input = traced.find("[input]\n");
            if (input != std::string::npos) {
                traced.insert(input + 8, "trace_mode = true\n");
            }
            base = options.analysis / (id + ".recomp.traced.toml");
            if (!write_file(base, traced)) {
                fail("could not write the traced recompiler configuration");
                return 1;
            }
        }
        fs::path current = base;
        std::vector<std::string> stubbed;
        // Eight rounds. The recompiler stops at the first function it cannot
        // take, so a round buys exactly one of them, and eight covers every
        // ROM tried -- Mario Builder 64 has five, all the same dispatch thunk
        // through a table in a segment that was never loaded. A ROM that still
        // has a new refusal after eight is one where something larger is
        // wrong, and grinding through hundreds one at a time would hide that.
        constexpr int kRounds = 32;
        bool translated = false;
        for (int round = 0; round < kRounds; round++) {
            std::string output;
            if (run_capture({options.recomp.string(), current.string()}, output) == 0) {
                translated = true;
                break;
            }
            const std::vector<std::string> refused = refused_functions(output);
            if (refused.empty()) {
                break;
            }
            for (const std::string &name : refused) {
                if (std::find(stubbed.begin(), stubbed.end(), name) == stubbed.end()) {
                    stubbed.push_back(name);
                }
            }
            current = options.analysis / (id + ".recomp.stubbed.toml");
            if (!write_file(current, with_stubs(read_file(base), stubbed))) {
                fail("could not write the amended recompiler configuration");
                return 1;
            }
            info("retry=" + std::to_string(round + 1));
        }
        if (!translated) {
            fail("the recompiler could not translate this ROM");
            return 1;
        }
        if (!stubbed.empty()) {
            info("recompiler_stubs=" + std::to_string(stubbed.size()));
            for (const std::string &name : stubbed) {
                info("recompiler_stub=" + name);
            }
        }
    }

    event("step", "2 3 Writing the module descriptor");
    // RSPRecomp output, when there is any, is a sibling of the C the
    // recompiler wrote. Nothing produces one yet; the shape is here so that
    // adding microcode identification is a change to the analyser alone.
    fs::path rsp_source;
    for (const fs::directory_entry &entry : fs::directory_iterator(generated)) {
        if (entry.path().extension() == ".cpp" && entry.path().stem().string().rfind("rsp_", 0) == 0) {
            rsp_source = entry.path();
            break;
        }
    }
    if (options.trace && !write_file(generated / "trace.h", kTraceHeader)) {
        fail("could not write the trace header");
        return 1;
    }
    if (!write_file(generated / "module.cpp", emit_module_cpp(info_json, rom_hash, rsp_source))) {
        fail("could not write the module descriptor");
        return 1;
    }

    // lookup.cpp is the recompiler's own entry-point shim, written for a
    // project that links the game into its runtime. A module carries the same
    // two facts in its descriptor, so compiling it would define them twice.
    fs::remove(generated / "lookup.cpp", ec);

    event("step", "3 3 Compiling to native arm64");

    std::vector<std::string> sources;
    for (const fs::directory_entry &entry : fs::directory_iterator(generated)) {
        const std::string extension = entry.path().extension().string();
        if (extension == ".c" || extension == ".cpp") {
            sources.push_back(entry.path().string());
        }
    }
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) {
        fail("the recompiler wrote no code");
        return 1;
    }

    const fs::path objects = options.analysis / "objects";
    fs::remove_all(objects, ec);
    fs::create_directories(objects, ec);

    std::vector<std::string> base = compile_flags(options);
    if (!options.watch.empty()) {
        base.push_back("-DN64B_WATCH=" + options.watch);
    }
    for (const std::string &include : options.includes) {
        base.push_back("-I" + include);
    }
    base.push_back("-I" + generated.string());

    unsigned jobs = options.jobs;
    if (jobs == 0) {
        jobs = std::max(1u, std::thread::hardware_concurrency());
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::vector<std::string> object_paths(sources.size());
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < jobs; worker++) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= sources.size() || failed.load()) {
                    return;
                }
                const fs::path source(sources[index]);
                const fs::path object = objects / (source.stem().string() + ".o");
                object_paths[index] = object.string();

                std::vector<std::string> argv;
                const bool is_cpp = source.extension() == ".cpp";
                argv.push_back(is_cpp ? options.compiler + "++" : options.compiler);
                argv.push_back(is_cpp ? "-std=c++20" : "-std=c11");
                for (const std::string &flag : base) {
                    argv.push_back(flag);
                }
                argv.push_back("-c");
                argv.push_back(source.string());
                argv.push_back("-o");
                argv.push_back(object.string());
                if (run(argv) != 0) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    if (failed.load()) {
        fail("the recompiled C did not compile");
        return 1;
    }

    std::vector<std::string> link;
    link.push_back(options.compiler + "++");
    link.push_back("-dynamiclib");
    // Every libultra call, every runtime helper and the whole of ultramodern is
    // an undefined symbol here on purpose: the host exports them and the loader
    // binds them when the module is opened.
    link.push_back("-Wl,-undefined,dynamic_lookup");
    link.push_back("-o");
    link.push_back(options.out.string());
    for (const std::string &object : object_paths) {
        link.push_back(object);
    }
    if (run(link) != 0) {
        fail("the module did not link");
        return 1;
    }

    if (!options.keep_c) {
        fs::remove_all(objects, ec);
    }

    if (!write_file(stamp, key)) {
        fail("could not write the cache stamp beside the module");
        return 1;
    }

    std::error_code size_ec;
    const auto size = fs::file_size(options.out, size_ec);
    info("module=" + options.out.string());
    info("cached=0");
    if (!size_ec) {
        info("module_bytes=" + std::to_string(size));
    }
    info("sources=" + std::to_string(sources.size()));
    event("ok", "");
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: n64b-port build --analysis <dir> --rom <rom.z64> --out <module.dylib>\n"
                 "                       [--recomp <N64Recomp>] [--cc <clang>] [--include <dir>]\n"
                 "                       [--opt <-O2>] [--jobs <n>] [--force] [--keep-c]\n"
                 "                       [--trace] [--watch <0xADDRESS>]\n"
                 "                       [--porcelain]\n");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    const std::string command = argv[1];
    if (command != "build") {
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        usage();
        return 2;
    }

    Options options;
    for (const std::string &include : split(N64B_DEFAULT_INCLUDES, '|')) {
        options.includes.push_back(include);
    }

    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        auto next_arg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--analysis") options.analysis = next_arg();
        else if (arg == "--rom") options.rom = next_arg();
        else if (arg == "--out") options.out = next_arg();
        else if (arg == "--recomp") options.recomp = next_arg();
        else if (arg == "--rsp-recomp") options.rsp_recomp = next_arg();
        else if (arg == "--cc") options.compiler = next_arg();
        else if (arg == "--include") options.includes.push_back(next_arg());
        else if (arg == "--opt") options.optimisation = next_arg();
        else if (arg == "--jobs") options.jobs = unsigned(std::strtoul(next_arg().c_str(), nullptr, 10));
        else if (arg == "--force") options.force = true;
        else if (arg == "--keep-c") options.keep_c = true;
        else if (arg == "--trace") options.trace = true;
        else if (arg == "--watch") { options.watch = next_arg(); options.trace = true; }
        else if (arg == "--porcelain") porcelain = true;
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    return build(std::move(options));
}
