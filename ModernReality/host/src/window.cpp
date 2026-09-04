// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window, and the event pump that goes with it.
//
// Both live on the main thread and nowhere else. On macOS that is not a
// preference: AppKit will not let a window be created or an event be read from
// any other thread, and librecomp's own main loop is built around calling back
// into the frontend for exactly this, so the arrangement fits.
//
// What RT64 needs from here is two pointers: the NSWindow, which it asks for
// the size and the refresh rate, and the CAMetalLayer it renders into.

#include "host.hpp"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cstdio>

namespace n64b {
namespace {

SDL_Window *window = nullptr;
SDL_MetalView metal_view = nullptr;
bool fullscreen_now = false;

void toggle_fullscreen() {
    fullscreen_now = !fullscreen_now;
    SDL_SetWindowFullscreen(window, fullscreen_now ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

} // namespace

SDL_Window *window_handle() { return window; }

bool open_window(const std::string &title, bool fullscreen,
                 ultramodern::renderer::WindowHandle &out, std::string &error) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        error = std::string("SDL could not start: ") + SDL_GetError();
        return false;
    }

    uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_METAL;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        fullscreen_now = true;
    }

    window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 720, flags);
    if (window == nullptr) {
        error = std::string("could not open a window: ") + SDL_GetError();
        return false;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) {
        error = std::string("could not read the native window handle: ") + SDL_GetError();
        return false;
    }

    // The renderer wants the layer, not the view that owns it: plume treats
    // WindowHandle::view as a CAMetalLayer and hands it straight to Metal.
    metal_view = SDL_Metal_CreateView(window);
    if (metal_view == nullptr) {
        error = std::string("could not attach a Metal layer to the window: ") + SDL_GetError();
        return false;
    }

    out.window = info.info.cocoa.window;
    out.view = SDL_Metal_GetLayer(metal_view);
    return true;
}

void pump_window() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    window != nullptr &&
                    event.window.windowID == SDL_GetWindowID(window)) {
                    ultramodern::quit();
                }
                break;

            case SDL_KEYDOWN:
                // The two bindings every emulator has. Everything else the
                // game sees as a controller.
                if (event.key.keysym.sym == SDLK_F11 ||
                    (event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT))) {
                    toggle_fullscreen();
                } else if (event.key.keysym.sym == SDLK_ESCAPE && fullscreen_now) {
                    toggle_fullscreen();
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                init_input();
                break;

            default:
                break;
        }
    }
}

} // namespace n64b
