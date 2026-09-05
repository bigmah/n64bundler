// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the pieces of the host hand each other.
//
// n64b-run is small on purpose. librecomp and ultramodern between them are the
// console -- threads, message queues, the PI and the VI, saving, the whole of
// libultra -- and RT64 is the renderer. What is left for the host is the four
// things they cannot do for themselves: open a window, put sound somewhere,
// read a controller, and find the game.

#ifndef N64B_HOST_HPP
#define N64B_HOST_HPP

#include <cstdint>
#include <memory>
#include <string>

#include <ultramodern/ultramodern.hpp>

#include "modernreality/module_abi.h"

namespace n64b {

// --- the game module -------------------------------------------------------

/// An opened game module, and the library handle keeping it alive.
struct Module {
    void *handle = nullptr;
    const n64b_module_v1 *desc = nullptr;

    ~Module();
    Module() = default;
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
};

/// Open a module and check that it is one this host can run. Returns nullptr
/// with `error` set otherwise.
std::unique_ptr<Module> open_module(const std::string &path, std::string &error);

/// The save type the descriptor asked for, as librecomp spells it.
int save_type_of(const n64b_module_v1 &desc);

// --- the window ------------------------------------------------------------

/// Open the game window. Must be called on the main thread, before anything
/// else touches SDL.
bool open_window(const std::string &title, bool fullscreen,
                 ultramodern::renderer::WindowHandle &out, std::string &error);

/// Pump the event queue. Called from the main thread on every iteration of
/// librecomp's own loop, which is the only place it is safe to do on macOS.
void pump_window();

/// The window's SDL handle, for the parts of RT64 that ask for it.
struct SDL_Window *window_handle();

// --- the pieces that plug into ultramodern ---------------------------------

ultramodern::renderer::callbacks_t renderer_callbacks();
ultramodern::audio_callbacks_t audio_callbacks();
ultramodern::input::callbacks_t input_callbacks();
ultramodern::events::callbacks_t events_callbacks();
ultramodern::error_handling::callbacks_t error_handling_callbacks();
ultramodern::threads::callbacks_t threads_callbacks();

/// Opens the audio device. Safe to call before a game starts; the frequency is
/// set later by the game itself.
void init_audio();
void shutdown_audio();

/// Print the last functions a game entered when the process ends, however it
/// ends. Only does anything for a module built with `n64b-port --trace`.
void install_trace(bool catch_signals);

// --- code that is in no cartridge ------------------------------------------

/// Recompile the function at `vram` out of the console's own memory, and hand
/// back something callable. Null if there is no function there.
///
/// This is for a game that decompresses its code into memory it allocated,
/// where there is nothing to analyse ahead of time and no address to write
/// down. See overlays.cpp.
recomp_func_t *recompile_at(uint8_t *rdram, uint32_t vram);

/// Which function a native address belongs to, if it is inside code the
/// runtime generated, or 0. Generated code carries no symbol, so a backtrace
/// through it needs this to say anything at all.
uint32_t generated_owner(const void *address);

/// How much of that happened, said once when the game ends.
void report_recompiled(void);

/// The game's own syscall dispatch, out of the module descriptor: the handler,
/// and the table of stubs that reach it. All zero for a game whose exceptions
/// are libultra's alone, which is nearly all of them; see
/// n64b_module_v1::syscall_handler_address.
void set_syscall_handler(uint32_t handler, uint32_t table, uint32_t table_size);

/// Where the game's memory is, so a `--watch` build can print what the watched
/// address holds rather than only who touched it.
void set_watch_memory(uint8_t *rdram);
/// Write one word into the console's memory a few seconds in, from N64B_POKE.
void poke_memory();

/// Opens whatever controllers are already plugged in. Hotplug is handled from
/// the event pump.
void init_input();

} // namespace n64b

#endif
