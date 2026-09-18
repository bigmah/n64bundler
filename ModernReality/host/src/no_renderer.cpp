// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host, built without a renderer.
//
// `-DMODERNREALITY_RENDERER=OFF` leaves RT64, renderer.cpp and window.cpp out
// of the build, which is the whole of this host's dependency on a GPU, a
// graphics API and a shader compiler. What is left runs games and cannot show
// them -- which is exactly what an environment driving one headless wants, and
// on a machine with no GPU is the difference between a build that works and a
// build that cannot be configured.
//
// This file is what the rest of the host still expects to find. It is small on
// purpose: everything here is either a counter the headless renderer in gym.cpp
// keeps for itself, or a window operation that has nothing to do and says so.
// Doing it this way rather than with `#if`s scattered through main.cpp means
// there is one place to read to know what a renderer-less build is missing.

#include "host.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace n64b {

/// Frames the game has drawn, one per display list.
///
/// Normally renderer.cpp owns this and RT64 advances it. With no renderer the
/// only writer is the headless renderer in gym.cpp, which is the one that
/// counts display lists and drops them -- and counting them is not incidental:
/// a display list is what a frame of the game *is* to an environment, so this
/// is the clock a step is measured in.
std::atomic<uint64_t> drawn_frames{0};

uint64_t frames_drawn() { return drawn_frames.load(std::memory_order_relaxed); }

// --- the window ------------------------------------------------------------

bool open_window(const std::string &, bool, bool, ultramodern::renderer::WindowHandle &,
                 std::string &error) {
    error = "this n64b-run was built without a renderer, so it cannot open a window.\n"
            "       It can only run a game nobody is watching: pass --gym <name> --headless.\n"
            "       Build with -DMODERNREALITY_RENDERER=ON for a host that draws.";
    return false;
}

void pump_window() {}

ultramodern::renderer::WindowHandle absent_window() {
    ultramodern::renderer::WindowHandle handle{};
#if defined(__APPLE__)
    handle.window = reinterpret_cast<void *>(1);
    handle.view = reinterpret_cast<void *>(1);
#else
    handle = reinterpret_cast<ultramodern::renderer::WindowHandle>(1);
#endif
    return handle;
}

struct SDL_Window *window_handle() { return nullptr; }

bool capture_window(const char *) { return false; }

// --- the renderer ----------------------------------------------------------

/// There is no renderer to plug in, so the one that draws nothing is plugged in
/// instead.
///
/// main.cpp already chooses this for a headless gym. Returning it here as well
/// means the other paths fail at `open_window` above, with a message that says
/// what to do, rather than somewhere further in with a null callback.
ultramodern::renderer::callbacks_t renderer_callbacks() {
    return headless_renderer_callbacks();
}

} // namespace n64b
