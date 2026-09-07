// SPDX-License-Identifier: GPL-3.0-or-later
//
// n64b-run - open a recompiled game module and play it.
//
//   n64b-run --module <game.dylib> --rom <game.z64> [--fullscreen]
//            [--config-dir <dir>] [--developer]
//
// The module carries the game's code and the facts about the cartridge; the
// ROM stays a separate file because a module holds no game data. Everything
// between them -- the console, the renderer, saving, libultra -- is in this
// executable, and is the same executable for every game in the library.

#include "host.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string>
#include <vector>

#include <librecomp/addresses.hpp>
#include <librecomp/game.hpp>
#include <librecomp/overlays.hpp>
#include <librecomp/rsp.hpp>

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string module;
    std::string rom;
    fs::path config_dir;
    bool fullscreen = false;
    bool developer = false;
    std::string unpack;
};

void usage() {
    std::fprintf(stderr,
                 "usage: n64b-run --module <game.dylib> --rom <game.z64>\n"
                 "                [--config-dir <dir>] [--fullscreen] [--developer]\n"
                 "                [--unpack <image.bin>]\n"
                 "\n"
                 "  --unpack  run until the game jumps to code that was not recompiled,\n"
                 "            then write the console's memory to <image.bin> and stop.\n"
                 "            A cartridge whose game is compressed is unpacked by its own\n"
                 "            loader and by nothing else; this is how the unpacked code is\n"
                 "            got at, so that the analyser can have a second look with it.\n");
}

fs::path default_config_dir() {
    const char *home = std::getenv("HOME");
    const fs::path base = home != nullptr ? fs::path(home) : fs::path(".");
    return base / "Library" / "Application Support" / "N64Bundler";
}

/// The module's microcode selector, which is where a recompiled audio ucode
/// would come from. Held in a file-scope pointer because librecomp's callback
/// takes no context of its own.
n64b_rsp_selector module_microcode = nullptr;

/// The sections the loaded module declared, for the callback below.
const n64b_module_v1 *loaded_module = nullptr;

/// Where `--unpack` writes, and the console's memory to write from. Both are
/// file-scope because the runtime asks for them from a callback that carries
/// no context, at a moment nothing else on the way there knows about.
std::string unpack_path;
uint8_t *console_memory = nullptr;

/// What the console has: eight megabytes with the expansion pak, which is what
/// librecomp gives every game.
constexpr size_t kRdramSizeBytes = 8 * 1024 * 1024;

/// How big the cartridge is, so the window below can hold all of it and no
/// more. Taken from the file the runtime was handed, which is the same file it
/// reads every DMA out of.
size_t cartridge_bytes = 0;

/// Put every section where the analysis says it runs.
///
/// librecomp decides where a section lives by watching the game's DMAs: it
/// emulates IPL3's copy of the first megabyte at startup and then re-places a
/// section whenever the game reads that part of the ROM again. That is right
/// for a project built from a decompilation, where a section's address is
/// whatever the game loaded it to and nothing knew it in advance.
///
/// Here something does know it in advance. A section in a module carries the
/// load address the analysis recovered -- for a title record, an address a
/// person measured -- and a game that moves a segment with a plain copy rather
/// than a DMA gives librecomp nothing to watch. Mario Builder 64 does exactly
/// that: its second segment sits inside the megabyte IPL3 copies and then runs
/// 0x36D0 higher than where that copy put it, so every function in it resolved
/// to the wrong address and the first call through a pointer failed.
///
/// This runs after librecomp's own IPL3 emulation, so it has the last word,
/// and a later DMA still overrides it -- which is what a real overlay needs.
/// Make the console's register window ordinary memory.
///
/// librecomp maps KSEG0 and nothing else, so a translated instruction that
/// stores to the RCP at `0xA4xxxxxx` lands past the end of the mapping and
/// takes the process down. That is why the analyser stubs every function that
/// touches one -- and stubbing a function loses whatever else it did.
///
/// Backing the window with zeroed memory is better on both counts. The
/// function runs, everything it does besides the register access is real, and
/// the access itself reads zero and discards writes. Zero is also the useful
/// answer: libultra's waits are "while the device is busy", and a device that
/// is never busy is one the caller stops waiting for. The runtime models the
/// hardware these registers belong to; nothing is meant to be read back.
///
/// The pages are lazily committed, so the cost is address space rather than
/// memory: only what a game actually touches is ever backed.
void map_register_window(uint8_t *rdram) {
    // KSEG1, uncached, which is the only way the registers are reached:
    // 0xA0000000 through 0xBFFFFFFF, at rdram + (address - 0x80000000).
    constexpr size_t kFirst = 0x20000000;
    constexpr size_t kLength = 0x20000000;
    if (mprotect(rdram + kFirst, kLength, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr,
                     "note: could not map the console's register window (%s). Games that reach "
                     "a hardware register directly will stop here.\n",
                     std::strerror(errno));
        return;
    }

    // The bottom of that window is not registers. It is memory.
    //
    // KSEG1 is not a second eight megabytes; it is the same eight megabytes
    // read past the cache. Every game that hands a structure to the RCP writes
    // it through one window and something reads it through the other, so the
    // two have to be the same pages -- and zeroed pages of their own are the
    // one answer that is wrong in a way nothing reports. Banjo-Tooie reads two
    // words the console's boot ROM left in memory, gets zero because it asked
    // uncached, decides a copier is running, and from then on refuses to put a
    // single object into the world. The game runs, draws and plays its music
    // for as long as you like, and is empty.
    //
    // One remap makes the two windows the same memory. The eight megabytes are
    // what the console has; everything above them in the window stays the
    // zeroed pages the registers want.
    mach_vm_address_t alias = mach_vm_address_t(rdram + kFirst);
    vm_prot_t current = VM_PROT_READ | VM_PROT_WRITE;
    vm_prot_t maximum = VM_PROT_READ | VM_PROT_WRITE;
    constexpr size_t kRdram = 8u * 1024u * 1024u;
    const kern_return_t aliased =
        mach_vm_remap(mach_task_self(), &alias, kRdram, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                      mach_task_self(), mach_vm_address_t(rdram), /*copy=*/FALSE, &current,
                      &maximum, VM_INHERIT_SHARE);
    if (aliased != KERN_SUCCESS ||
        mach_vm_protect(mach_task_self(), alias, kRdram, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        std::fprintf(stderr,
                     "note: uncached memory could not be made the same memory as cached. A game "
                     "that reads back what it wrote through 0xA0000000 will read zero.\n");
    }
}

/// Put the cartridge where the console has it.
///
/// The parallel interface is not only a DMA engine: the cartridge is mapped,
/// and a load from 0xB0000000 plus a ROM offset reads a word of it directly.
/// libultra never does that -- everything it reads it reads with a transfer --
/// so a game built from a decompilation never needs this. A game that wrote
/// its own loader may: Banjo-Tooie's overlay table is read one word at a time
/// with `lui $s0, 0xB000; or $s0, $s0, $a0; lw $t2, 0($s0)`, straight off the
/// cartridge with interrupts off, because a four-byte DMA is not worth the
/// queue.
///
/// Without this, that load lands in the zeroed pages the register window is
/// backed by and every entry of the table reads zero.
///
/// The cartridge is copied rather than aliased because the two hold their
/// bytes differently: the ROM is big-endian and the runtime keeps memory so
/// that an aligned word is a word this machine can load. `do_rom_read` is what
/// puts one into the other, and it is the same call a DMA of the whole
/// cartridge would make.
void map_cartridge_window(uint8_t *rdram) {
    (void)rdram;
    if (cartridge_bytes == 0) {
        return;
    }
    // KSEG1 again: 0xB0000000 is the cartridge's physical base seen uncached,
    // and the window mapped above already covers it.
    constexpr gpr kCartridgeWindow = gpr(int32_t(0xB0000000));
    constexpr size_t kWindowBytes = 0x0FC00000; // to 0xBFC00000, where the PIF is
    const size_t bytes = std::min(cartridge_bytes, kWindowBytes);
    recomp::do_rom_read(rdram, kCartridgeWindow, recomp::rom_base, bytes);
}

void place_sections(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    map_register_window(rdram);
    map_cartridge_window(rdram);
    n64b::set_watch_memory(rdram);
    n64b::poke_memory();
    console_memory = rdram;
    if (loaded_module == nullptr) {
        return;
    }
    // Undo the runtime's guess before making our own.
    //
    // The runtime starts a game by registering every section whose rom address
    // falls in the first megabyte as though the cartridge were one contiguous
    // image loaded at the entrypoint, which is what a game built from an elf
    // looks like. A game recovered from a bare rom is not: its segments are
    // scattered through the cartridge and land wherever the game's own loader
    // puts them, and the analysis knows where. Registering a section a second
    // time does not remove the first, so both the guess and the truth stay in
    // the address-to-function map -- and the guess, being a lower address,
    // answers first for anything the game reaches through a pointer. Every
    // indirect call in Mario Builder 64's behaviour interpreter was landing on
    // the function 0x36D0 bytes further on, which is exactly the distance
    // between where the guess put the main segment and where the game does.
    constexpr int32_t kRdramStart = 0x80000000;
    constexpr uint32_t kRdramSize = 8 * 1024 * 1024;
    unload_overlays(kRdramStart, kRdramSize);
    for (size_t i = 0; i < loaded_module->num_code_sections; i++) {
        const SectionTableEntry &section = loaded_module->code_sections[i];
        load_overlays(section.rom_addr, int32_t(section.ram_addr), section.size);
    }
}

/// Stands in for a microcode this build cannot run.
///
/// Graphics tasks never reach here -- ultramodern routes those to the
/// renderer. What is left is the audio list and the occasional JPEG or custom
/// task, and nothing identifies which microcode a ROM uses yet, so there is
/// nothing to run. Reporting the task and completing it is the right answer:
/// the game gets its interrupt, carries on, and draws. Refusing takes the
/// process down over sound.
RspExitReason unhandled_microcode(uint8_t *rdram, uint32_t ucode_addr) {
    (void)rdram;
    (void)ucode_addr;
    return RspExitReason::Broke;
}

RspUcodeFunc *select_microcode(const OSTask *task) {
    if (module_microcode != nullptr) {
        if (auto *found = reinterpret_cast<RspUcodeFunc *>(module_microcode(task))) {
            return found;
        }
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr,
                     "note: this game submitted an RSP task of type %u that no recompiled "
                     "microcode covers.\n"
                     "      It is being completed without running. Graphics are unaffected; "
                     "this is what silences the audio.\n",
                     task != nullptr ? uint32_t(task->t.type) : 0u);
        if (task != nullptr) {
            std::fprintf(stderr,
                         "      flags 0x%X ucode 0x%08X (0x%X) ucode_data 0x%08X (0x%X) "
                         "data 0x%08X (0x%X)\n",
                         uint32_t(task->t.flags), uint32_t(task->t.ucode),
                         uint32_t(task->t.ucode_size), uint32_t(task->t.ucode_data),
                         uint32_t(task->t.ucode_data_size), uint32_t(task->t.data_ptr),
                         uint32_t(task->t.data_size));
        }
    }
    return unhandled_microcode;
}

const char *rom_error_text(recomp::RomValidationError error) {
    switch (error) {
        case recomp::RomValidationError::FailedToOpen:
            return "the ROM could not be opened";
        case recomp::RomValidationError::NotARom:
            return "that file is not an N64 ROM";
        case recomp::RomValidationError::IncorrectRom:
            return "that ROM is a different game from the one this module was built for";
        case recomp::RomValidationError::IncorrectVersion:
            return "that is the right game but a different revision from the one this module "
                   "was built for";
        case recomp::RomValidationError::NotYet:
            return "that game is not supported yet";
        default:
            return "the ROM could not be used";
    }
}

} // namespace

/// Called by the runtime when a game jumps to an address nothing was
/// recompiled for, just before it gives up.
///
/// For most games that is a bug in the analysis and the address is a symptom.
/// For a cartridge whose game is compressed there are two other cases, and
/// both of them are ordinary.
///
/// The first is the expected end of an `--unpack` run: the loader has
/// finished, the game is in memory, and this address is where it starts.
/// Nothing in the image says so and no amount of reading the image will,
/// because until the loader has run the code does not exist anywhere. So the
/// console's memory goes to a file -- a derived work of the player's own
/// cartridge in the same way the ROM is, kept beside it under the game's
/// folder, never in the source tree and never in a title record.
///
/// The second is an overlay: code the game decompressed into memory it
/// allocated, which no analysis could have reached and no record could name.
/// That one is translated here and now, and the game carries on.
extern "C" recomp_func_t *recomp_missing_function(int32_t addr) {
    if (unpack_path.empty()) {
        return n64b::recompile_at(console_memory, uint32_t(addr));
    }
    if (console_memory == nullptr) {
        std::fprintf(stderr, "error: the game ended before its memory existed; nothing to "
                             "unpack.\n");
        return nullptr;
    }

    std::FILE *out = std::fopen(unpack_path.c_str(), "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "error: could not write %s (%s)\n", unpack_path.c_str(),
                     std::strerror(errno));
        return nullptr;
    }
    const size_t written = std::fwrite(console_memory, 1, kRdramSizeBytes, out);
    const bool ok = written == kRdramSizeBytes && std::fclose(out) == 0;
    if (!ok) {
        std::fprintf(stderr, "error: could not write all of %s\n", unpack_path.c_str());
        return nullptr;
    }

    // The line the pipeline reads. Said on stdout, and said in one piece, so
    // that a caller does not have to parse the runtime's chatter around it.
    std::printf("@@unpacked entry=0x%08X image=%s size=0x%zX\n", uint32_t(addr),
                unpack_path.c_str(), kRdramSizeBytes);
    std::fflush(stdout);
    return nullptr;
}

int main(int argc, char **argv) {
    Options options;
    options.config_dir = default_config_dir();

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next_arg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--module") options.module = next_arg();
        else if (arg == "--rom") options.rom = next_arg();
        else if (arg == "--config-dir") options.config_dir = next_arg();
        else if (arg == "--fullscreen") options.fullscreen = true;
        else if (arg == "--developer") options.developer = true;
        else if (arg == "--unpack") options.unpack = next_arg();
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (options.module.empty() || options.rom.empty()) {
        usage();
        return 2;
    }
    unpack_path = options.unpack;
    {
        // The size of what the runtime will read the cartridge out of, for the
        // window map_cartridge_window() fills.
        std::error_code size_error;
        const auto size = fs::file_size(options.rom, size_error);
        cartridge_bytes = size_error ? 0 : size_t(size);
    }

    std::string error;
    std::unique_ptr<n64b::Module> module = n64b::open_module(options.module, error);
    if (module == nullptr) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const n64b_module_v1 &desc = *module->desc;

    std::printf("%s (%s), %zu section(s), entry 0x%08X\n", desc.display_name, desc.game_id,
                desc.num_code_sections, desc.entrypoint_address);

    std::error_code ec;
    fs::create_directories(options.config_dir, ec);
    recomp::register_config_path(options.config_dir);

    // The window has to exist before the renderer, and both have to be on the
    // main thread on this platform.
    ultramodern::renderer::WindowHandle window_handle{};
    if (!n64b::open_window(desc.display_name, options.fullscreen, window_handle, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    n64b::init_input();
    n64b::install_trace(options.developer);
    // Zero unless this game reaches between its overlays through the CPU's
    // syscall exception, which the runtime has to stand in for.
    n64b::set_syscall_handler(desc.syscall_handler_address, desc.syscall_table_address,
                              desc.syscall_table_size);

    // Hand the module's section table to the runtime. From here on, a call
    // into an address the game loaded at runtime resolves through this.
    recomp::overlays::register_overlays(
        recomp::overlays::overlay_section_table_data_t{
            .code_sections = desc.code_sections,
            .num_code_sections = desc.num_code_sections,
            .total_num_sections = desc.total_num_sections,
        },
        recomp::overlays::overlays_by_index_t{
            .table = desc.overlays_by_index,
            .len = desc.num_overlays,
        });

    module_microcode = desc.get_rsp_microcode;
    loaded_module = &desc;

    const std::u8string game_id(reinterpret_cast<const char8_t *>(desc.game_id));

    recomp::GameEntry entry{};
    entry.rom_hash = desc.rom_hash;
    entry.internal_name = desc.internal_name;
    entry.display_name = desc.display_name;
    entry.game_id = game_id;
    // What `--game` on the command line matches, and the folder mods for this
    // game would live under.
    entry.mod_game_id = desc.game_id;
    entry.save_type = recomp::SaveType(n64b::save_type_of(desc));
    entry.is_enabled = true;
    entry.entrypoint_address = gpr(int32_t(desc.entrypoint_address));
    entry.entrypoint = desc.entrypoint;
    entry.on_init_callback = place_sections;
    recomp::register_game(entry);

    // librecomp plays from its own copy of the ROM, checked against the hash
    // the module was built from. Copying it once is what makes a game in the
    // library keep working when the file it came from moves.
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(game_id)) {
        const recomp::RomValidationError result = recomp::select_rom(options.rom, game_id);
        if (result != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "error: %s (%s)\n", rom_error_text(result), options.rom.c_str());
            return 1;
        }
    }

    ultramodern::renderer::GraphicsConfig graphics{};
    graphics.developer_mode = options.developer;
    graphics.res_option = ultramodern::renderer::Resolution::Auto;
    graphics.wm_option = options.fullscreen ? ultramodern::renderer::WindowMode::Fullscreen
                                            : ultramodern::renderer::WindowMode::Windowed;
    graphics.hr_option = ultramodern::renderer::HUDRatioMode::Original;
    graphics.api_option = ultramodern::renderer::GraphicsApi::Metal;
    // The game's own aspect ratio, pillarboxed, and deliberately not the
    // window's.
    //
    // Expanding to the window is what every widescreen hack does, and it works
    // by widening the projection -- which shows more of the world than the game
    // ever meant to draw. An N64 game culls its scenery against a four-by-three
    // frustum and submits nothing outside it, so what fills the extra width is
    // not more world but the edge of the world: on Banjo-Tooie's title screen
    // the cliffs stop in a hard vertical line and the grass above them floats
    // over nothing. A game that was drawn for four by three is shown in four by
    // three.
    graphics.ar_option = ultramodern::renderer::AspectRatio::Original;
    graphics.msaa_option = ultramodern::renderer::Antialiasing::MSAA2X;
    // The N64 ran at 60Hz whatever the display did; matching the display is
    // what makes a 120Hz panel look right rather than judder.
    graphics.rr_option = ultramodern::renderer::RefreshRate::Display;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
    graphics.rr_manual_value = 60;
    graphics.ds_option = 1;
    ultramodern::renderer::set_graphics_config(graphics);

    // How fast the console's processor was.
    //
    // A recompilation runs the game's own code hundreds of times faster than
    // the R4300 did, and for a game that paces itself against how long its work
    // took, that is not a free improvement. Banjo-Tooie asks the video
    // interface for a frame every two retraces. On a console it misses that
    // target about a third of the time, because a frame's worth of its own code
    // does not fit in two retraces; here it never missed, so it ran at a flat
    // thirty frames a second where a console manages twenty-six, and its
    // attract mode played through in sixty-nine seconds where a console takes
    // eighty-two. Its loading pauses were not there at all: three seconds of a
    // console decompressing a world is a tenth of a second of this. Holding the
    // recompiled code to a rate gives all of that back.
    //
    // The number is instructions a second rather than cycles, because what an
    // R4300 retires in a second is a fact about a processor waiting on memory
    // and not one from a manual. It is a measurement: with the code held to
    // this, every landmark of Banjo-Tooie's first two and a half minutes --
    // the title screen's length, the frame the attract mode starts at, all
    // three of its loading pauses, the frame it ends at -- lands within two
    // seconds of where the reference console puts it, and the attract mode
    // takes eighty-three seconds against the console's eighty-two.
    //
    // It agrees with mupen64plus, which is the other way of arriving at it: the
    // core advances the COP0 count register twice per instruction and that
    // register ticks at half of the R4300's 93.75 MHz, which is this number.
    constexpr double r4300_instructions_per_second = 23.4e6;

    // `N64B_CPU_RATE` overrides it, and zero lifts it entirely -- which is a
    // game running as fast as the host can carry it, the thing a static
    // recompilation is for and the thing that makes this game play too quickly.
    double cpu_rate = r4300_instructions_per_second;
    if (const char *rate = std::getenv("N64B_CPU_RATE")) {
        cpu_rate = std::strtod(rate, nullptr);
    }
    ultramodern::set_cpu_instruction_rate(cpu_rate);
    if (options.developer) {
        if (cpu_rate > 0.0) {
            std::fprintf(stderr, "note: holding the game's code to %.1f million instructions a second, "
                                 "which is what an R4300 managed.\n",
                         cpu_rate / 1e6);
        } else {
            std::fprintf(stderr, "note: the game's code runs as fast as this machine can carry it, "
                                 "which is faster than the console ever did.\n");
        }
    }

    // librecomp reads its own command line to decide which registered game to
    // start. There is exactly one, and it starts immediately.
    std::vector<char *> runtime_argv;
    std::string game_flag = "--game";
    std::string game_name = desc.game_id;
    runtime_argv.push_back(argv[0]);
    runtime_argv.push_back(game_flag.data());
    runtime_argv.push_back(game_name.data());

    recomp::Configuration config{};
    config.argc = int(runtime_argv.size());
    config.argv = runtime_argv.data();
    config.project_version = recomp::Version{1, 0, 0};
    config.window_handle = window_handle;
    config.rsp_callbacks = recomp::rsp::callbacks_t{.get_rsp_microcode = select_microcode};
    config.renderer_callbacks = n64b::renderer_callbacks();
    config.audio_callbacks = n64b::audio_callbacks();
    config.input_callbacks = n64b::input_callbacks();
    config.events_callbacks = n64b::events_callbacks();
    config.error_handling_callbacks = n64b::error_handling_callbacks();
    config.threads_callbacks = n64b::threads_callbacks();
    config.gfx_callbacks = ultramodern::gfx_callbacks_t{
        .create_gfx = nullptr,
        .create_window = nullptr,
        // Called on the main thread on every turn of librecomp's own loop,
        // which is the only place AppKit will let events be read.
        .update_gfx = [](ultramodern::gfx_callbacks_t::gfx_data_t) { n64b::pump_window(); },
    };

    recomp::start(config);

    n64b::shutdown_audio();
    SDL_Quit();
    return 0;
}
