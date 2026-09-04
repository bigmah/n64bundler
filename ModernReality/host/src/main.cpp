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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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
};

void usage() {
    std::fprintf(stderr,
                 "usage: n64b-run --module <game.dylib> --rom <game.z64>\n"
                 "                [--config-dir <dir>] [--fullscreen] [--developer]\n");
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

RspUcodeFunc *select_microcode(const OSTask *task) {
    if (module_microcode == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<RspUcodeFunc *>(module_microcode(task));
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
    graphics.ar_option = ultramodern::renderer::AspectRatio::Expand;
    graphics.msaa_option = ultramodern::renderer::Antialiasing::MSAA2X;
    // The N64 ran at 60Hz whatever the display did; matching the display is
    // what makes a 120Hz panel look right rather than judder.
    graphics.rr_option = ultramodern::renderer::RefreshRate::Display;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
    graphics.rr_manual_value = 60;
    graphics.ds_option = 1;
    ultramodern::renderer::set_graphics_config(graphics);

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
