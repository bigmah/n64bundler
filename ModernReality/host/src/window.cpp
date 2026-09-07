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

#include <ApplicationServices/ApplicationServices.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace n64b {
namespace {

SDL_Window *window = nullptr;
SDL_MetalView metal_view = nullptr;
bool fullscreen_now = false;

void toggle_fullscreen() {
    fullscreen_now = !fullscreen_now;
    SDL_SetWindowFullscreen(window, fullscreen_now ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/// This process's window, as the window server numbers it.
///
/// Found by owner rather than kept from SDL because SDL's own window id is its
/// own; the number the window server uses is the one a picture can be asked
/// for. There is one on-screen window here, so the first one this process owns
/// is it.
CGWindowID window_server_id() {
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (list == nullptr) {
        return kCGNullWindowID;
    }
    CGWindowID found = kCGNullWindowID;
    const pid_t self = getpid();
    for (CFIndex i = 0, n = CFArrayGetCount(list); i < n && found == kCGNullWindowID; i++) {
        CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef owner = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowOwnerPID);
        CFNumberRef number = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowNumber);
        int pid = 0, id = 0;
        if (owner == nullptr || number == nullptr) {
            continue;
        }
        CFNumberGetValue(owner, kCFNumberIntType, &pid);
        CFNumberGetValue(number, kCFNumberIntType, &id);
        if (pid == self) {
            found = CGWindowID(id);
        }
    }
    CFRelease(list);
    return found;
}

} // namespace

SDL_Window *window_handle() { return window; }

/// Write a picture of the window itself, and say where it went.
///
/// The frame RT64 writes back into the console's memory is not the whole of
/// what it drew: it copies a framebuffer pair's rows when that pair is done,
/// and a scene assembled out of several of them leaves the copy holding some
/// of the frame and not the rest. Read as a screenshot that is a picture with
/// things missing from it that the window has -- a character, a fire, the
/// letters of a word -- which is a comparison against a reference console
/// losing an argument it should have won.
///
/// So the picture is of the window, which is by definition what the game looks
/// like. It goes through `screencapture` rather than through CoreGraphics
/// because `CGWindowListCreateImage` is gone from the macOS 15 SDK and its
/// replacement is an asynchronous Objective-C API for a job a system tool
/// already does. The cost is a process and a frame or so of lag behind the
/// display list that asked for it, and neither matters for what this is for:
/// what is on the screen, at a named moment of the game, next to the same
/// moment on another console.
///
/// The file is a PNG whatever extension was asked for, because that is what
/// `screencapture` writes.
bool capture_window(const char *path) {
    const CGWindowID id = window_server_id();
    if (id == kCGNullWindowID) {
        return false;
    }
    std::string named = path;
    const size_t dot = named.find_last_of('.');
    const size_t slash = named.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        named.resize(dot);
    }
    named += ".png";
    // A quote in the path would end the shell's argument early, and there is
    // nothing sensible to do with such a path but decline it.
    if (named.find('\'') != std::string::npos) {
        return false;
    }

    char command[1024];
    std::snprintf(command, sizeof(command), "/usr/sbin/screencapture -x -o -t png -l%u '%s'",
                  unsigned(id), named.c_str());
    if (std::system(command) != 0) {
        // Almost always one thing: the window server stops backing a window
        // that is completely covered by another, and then there is no image to
        // copy. Worth saying out loud, because the caller's fallback is the
        // frame out of the console's memory -- which is a picture with parts of
        // the scene missing from it, and looks like a renderer bug rather than
        // a screenshot that could not be taken.
        std::fprintf(stderr,
                     "note: the window server would not photograph window %u, which usually "
                     "means it is behind another window. Falling back to the frame in memory, "
                     "which may be missing part of the scene.\n",
                     unsigned(id));
        return false;
    }
    std::fprintf(stderr, "note: wrote %s, a picture of the window.\n", named.c_str());
    return true;
}

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
