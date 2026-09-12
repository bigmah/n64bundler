// SPDX-License-Identifier: GPL-3.0-or-later
//
// The small callbacks: what to do each frame, how to show an error, and what
// to call a thread.

#include "host.hpp"

#include <SDL.h>

#include <cstdio>
#include <string>

#include <librecomp/game.hpp>

namespace n64b {
namespace {

void vi_callback() {
    // Once per frame on the VI thread. Nothing here yet; a frame counter or a
    // frame-pacing hook would go in.
}

void gfx_init_callback() {
    // The renderer is up. Sound can start now without the first buffer being
    // queued against a device that is about to be reconfigured.
    //
    // Unless nobody is here to listen: a game being driven headless has no
    // window, no renderer and no reason to hold an audio device open -- and
    // opening one per game would mean a machine running thirty of them at once
    // opening thirty.
    if (gym_headless()) {
        return;
    }
    init_audio();
}

void message_box(const char *message) {
    std::fprintf(stderr, "%s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "N64Bundler", message, window_handle());
}

std::string game_thread_name(const OSThread *thread) {
    // macOS truncates a thread name at 15 bytes, and the game's own thread ids
    // are the only distinguishing thing about them anyway.
    return "Game " + std::to_string(thread != nullptr ? thread->id : 0);
}

} // namespace

ultramodern::events::callbacks_t events_callbacks() {
    return ultramodern::events::callbacks_t{
        .vi_callback = vi_callback,
        .gfx_init_callback = gfx_init_callback,
    };
}

ultramodern::error_handling::callbacks_t error_handling_callbacks() {
    return ultramodern::error_handling::callbacks_t{
        .message_box = message_box,
    };
}

ultramodern::threads::callbacks_t threads_callbacks() {
    return ultramodern::threads::callbacks_t{
        .get_game_thread_name = game_thread_name,
    };
}

} // namespace n64b
