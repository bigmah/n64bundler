// SPDX-License-Identifier: GPL-3.0-or-later
//
// Controllers.
//
// The N64 pad is four face buttons, two shoulders, a Z trigger under the
// middle, a d-pad, a four-way C cluster and one analog stick. A modern gamepad
// has two sticks and no Z, so the C buttons go on the right stick -- which is
// what everyone who has played an N64 game on a modern pad expects -- and Z
// goes on the left trigger, where the finger already is.
//
// The keyboard is here as the fallback for someone who has just recompiled
// their first ROM and has no pad plugged in, not as a serious control scheme.

#include "host.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace n64b {
namespace {

// libultra's button bits, in the order the OSContPad reports them.
constexpr uint16_t kA = 0x8000;
constexpr uint16_t kB = 0x4000;
constexpr uint16_t kZ = 0x2000;
constexpr uint16_t kStart = 0x1000;
constexpr uint16_t kDUp = 0x0800;
constexpr uint16_t kDDown = 0x0400;
constexpr uint16_t kDLeft = 0x0200;
constexpr uint16_t kDRight = 0x0100;
constexpr uint16_t kL = 0x0020;
constexpr uint16_t kR = 0x0010;
constexpr uint16_t kCUp = 0x0008;
constexpr uint16_t kCDown = 0x0004;
constexpr uint16_t kCLeft = 0x0002;
constexpr uint16_t kCRight = 0x0001;

constexpr int kMaxControllers = 4;
/// Past this the stick is treated as pushed; below it, as centred. The right
/// stick standing in for four digital buttons is why this exists at all.
constexpr float kCButtonThreshold = 0.5f;
/// SDL reports triggers as 0..32767. Half way is a press.
constexpr int16_t kTriggerThreshold = 16384;

std::mutex pads_mutex;
std::array<SDL_GameController *, kMaxControllers> pads{};

bool keyboard_only() {
    for (SDL_GameController *pad : pads) {
        if (pad != nullptr) {
            return false;
        }
    }
    return true;
}

float axis(SDL_GameController *pad, SDL_GameControllerAxis which) {
    const float raw = float(SDL_GameControllerGetAxis(pad, which)) / 32767.0f;
    // A little deadzone, because a worn stick that rests at 0.04 makes a game
    // drift forever and looks like a bug in the recompilation.
    return std::fabs(raw) < 0.12f ? 0.0f : raw;
}

void read_keyboard(uint16_t &buttons, float &x, float &y) {
    const uint8_t *keys = SDL_GetKeyboardState(nullptr);
    if (keys == nullptr) {
        return;
    }
    auto down = [&](SDL_Scancode code, uint16_t bit) {
        if (keys[code] != 0) {
            buttons |= bit;
        }
    };
    down(SDL_SCANCODE_X, kA);
    down(SDL_SCANCODE_C, kB);
    down(SDL_SCANCODE_Z, kZ);
    down(SDL_SCANCODE_RETURN, kStart);
    down(SDL_SCANCODE_A, kL);
    down(SDL_SCANCODE_S, kR);
    down(SDL_SCANCODE_I, kCUp);
    down(SDL_SCANCODE_K, kCDown);
    down(SDL_SCANCODE_J, kCLeft);
    down(SDL_SCANCODE_L, kCRight);
    down(SDL_SCANCODE_UP, kDUp);
    down(SDL_SCANCODE_DOWN, kDDown);
    down(SDL_SCANCODE_LEFT, kDLeft);
    down(SDL_SCANCODE_RIGHT, kDRight);

    if (keys[SDL_SCANCODE_W]) y += 1.0f;
    if (keys[SDL_SCANCODE_S] == 0 && keys[SDL_SCANCODE_D]) x += 1.0f;
    if (keys[SDL_SCANCODE_Q]) x -= 1.0f;
    if (keys[SDL_SCANCODE_E]) y -= 1.0f;
}

void poll_input() {
    // The window's own pump is what actually drains the queue; SDL's gamepad
    // state is updated from there. Nothing to do here.
}

// --- a pad a script can hold ------------------------------------------------
//
// The same argument the screenshot is here for. A window is the real answer to
// "does this game play", and a window is no answer at all to a script, a log,
// or a machine whose screen is locked -- and unlike a frame, a game past its
// title screen cannot be reached by waiting. Something has to press Start.
//
// `N64B_INPUT` is a list of `<frame>:<buttons>` separated by commas, and each
// entry holds until the next one:
//
//     N64B_INPUT="90:start,94:,150:a,154:,200:up"
//
// The names are the pad's own -- a b z start l r, du dd dl dr for the d-pad,
// cu cd cl cr for the C buttons -- joined with `+`, and up down left right
// push the analog stick. An empty list of buttons releases everything, which
// is what makes a press a press rather than a hold.
//
// Frames are the frames the *game* has drawn -- `n64b::frames_drawn()`, one per
// display list -- which is the clock `N64B_SCREENSHOT_AFTER` uses and the clock
// `refshot` counts on the reference console. That matters because the point of
// spelling a script this way is that one script drives both consoles: a game
// that draws twenty frames a second has three video interrupts per frame and a
// pad it reads on each of them, so counting either of those instead would put
// the same script in three different places in the game.

struct ScriptedFrame {
    unsigned long at;
    uint16_t buttons;
    float x, y;
};

std::vector<ScriptedFrame> scripted;

/// Parse `N64B_INPUT` once, into frames sorted by when they start.
const std::vector<ScriptedFrame> &input_script() {
    static bool parsed = false;
    if (parsed) {
        return scripted;
    }
    parsed = true;
    const char *spec = std::getenv("N64B_INPUT");
    if (spec == nullptr) {
        return scripted;
    }
    auto token = [](const std::string &name, ScriptedFrame &frame) {
        static const struct { const char *name; uint16_t bit; } kNames[] = {
            {"a", kA}, {"b", kB}, {"z", kZ}, {"start", kStart},
            {"l", kL}, {"r", kR},
            {"du", kDUp}, {"dd", kDDown}, {"dl", kDLeft}, {"dr", kDRight},
            {"cu", kCUp}, {"cd", kCDown}, {"cl", kCLeft}, {"cr", kCRight},
        };
        for (const auto &known : kNames) {
            if (name == known.name) {
                frame.buttons |= known.bit;
                return true;
            }
        }
        if (name == "up")    { frame.y =  1.0f; return true; }
        if (name == "down")  { frame.y = -1.0f; return true; }
        if (name == "left")  { frame.x = -1.0f; return true; }
        if (name == "right") { frame.x =  1.0f; return true; }
        return name.empty();
    };

    const std::string all = spec;
    size_t at = 0;
    while (at <= all.size()) {
        const size_t comma = std::min(all.find(',', at), all.size());
        const std::string entry = all.substr(at, comma - at);
        at = comma + 1;
        const size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        ScriptedFrame frame{std::strtoul(entry.c_str(), nullptr, 10), 0, 0.0f, 0.0f};
        const std::string buttons = entry.substr(colon + 1);
        size_t part = 0;
        while (part <= buttons.size()) {
            const size_t plus = std::min(buttons.find('+', part), buttons.size());
            if (!token(buttons.substr(part, plus - part), frame)) {
                std::fprintf(stderr, "note: N64B_INPUT: no button called \"%s\"\n",
                             buttons.substr(part, plus - part).c_str());
            }
            part = plus + 1;
        }
        scripted.push_back(frame);
    }
    std::sort(scripted.begin(), scripted.end(),
              [](const ScriptedFrame &a, const ScriptedFrame &b) { return a.at < b.at; });
    std::fprintf(stderr, "note: playing %zu scripted inputs from N64B_INPUT\n", scripted.size());
    return scripted;
}

/// What the script says the pad is holding on this read, if it says anything.
bool scripted_input(uint16_t *buttons_out, float *x_out, float *y_out) {
    const std::vector<ScriptedFrame> &script = input_script();
    if (script.empty()) {
        return false;
    }
    const unsigned long now = (unsigned long)n64b::frames_drawn();
    const ScriptedFrame *current = nullptr;
    for (const ScriptedFrame &frame : script) {
        if (frame.at > now) {
            break;
        }
        current = &frame;
    }
    *buttons_out = current != nullptr ? current->buttons : uint16_t(0);
    *x_out = current != nullptr ? current->x : 0.0f;
    *y_out = current != nullptr ? current->y : 0.0f;
    return true;
}

bool get_input(int controller, uint16_t *buttons_out, float *x_out, float *y_out) {
    std::lock_guard<std::mutex> lock(pads_mutex);
    if (controller < 0 || controller >= kMaxControllers) {
        return false;
    }

    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;

    // A script holds port one and nothing else, so a second pad still works
    // beside it and a game that reads four ports still sees three empty.
    if (controller == 0 && scripted_input(buttons_out, x_out, y_out)) {
        return true;
    }

    SDL_GameController *pad = pads[size_t(controller)];
    if (pad == nullptr) {
        if (controller != 0 || !keyboard_only()) {
            return false;
        }
        read_keyboard(buttons, x, y);
        *buttons_out = buttons;
        *x_out = x;
        *y_out = y;
        return true;
    }

    auto down = [&](SDL_GameControllerButton which, uint16_t bit) {
        if (SDL_GameControllerGetButton(pad, which) != 0) {
            buttons |= bit;
        }
    };
    // A and B are the N64's two big buttons; on a modern pad the bottom and
    // left face buttons are where a thumb expects them.
    down(SDL_CONTROLLER_BUTTON_A, kA);
    down(SDL_CONTROLLER_BUTTON_X, kB);
    down(SDL_CONTROLLER_BUTTON_START, kStart);
    down(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL);
    down(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR);
    down(SDL_CONTROLLER_BUTTON_DPAD_UP, kDUp);
    down(SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDDown);
    down(SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDLeft);
    down(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDRight);

    // Z on either trigger: the left one is where the N64's Z was, and the
    // right one is where a player used to a modern shooter will reach.
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold ||
        SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold) {
        buttons |= kZ;
    }

    x = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    y = -axis(pad, SDL_CONTROLLER_AXIS_LEFTY); // SDL's Y grows downward

    const float cx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    const float cy = -axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    if (cy > kCButtonThreshold) buttons |= kCUp;
    if (cy < -kCButtonThreshold) buttons |= kCDown;
    if (cx < -kCButtonThreshold) buttons |= kCLeft;
    if (cx > kCButtonThreshold) buttons |= kCRight;

    // Y and B as the C-up/C-down a game uses for its camera, for pads whose
    // right stick is being used for something else.
    down(SDL_CONTROLLER_BUTTON_Y, kCUp);
    down(SDL_CONTROLLER_BUTTON_B, kCDown);

    *buttons_out = buttons;
    *x_out = x;
    *y_out = y;
    return true;
}

void set_rumble(int controller, bool on) {
    std::lock_guard<std::mutex> lock(pads_mutex);
    if (controller < 0 || controller >= kMaxControllers) {
        return;
    }
    SDL_GameController *pad = pads[size_t(controller)];
    if (pad == nullptr) {
        return;
    }
    // The Rumble Pak had one motor and one speed. Duration is generous
    // because the game re-asserts it every frame it wants to keep going.
    SDL_GameControllerRumble(pad, on ? 0xFFFF : 0, on ? 0xFFFF : 0, on ? 200 : 0);
}

ultramodern::input::connected_device_info_t connected_device(int controller) {
    std::lock_guard<std::mutex> lock(pads_mutex);
    if (controller < 0 || controller >= kMaxControllers) {
        return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
    }

    // Port one always answers. A game that finds no controller at all in port
    // one usually sits on a "please connect a controller" screen, and someone
    // running this for the first time on a laptop deserves to see the game.
    if (pads[size_t(controller)] == nullptr) {
        if (controller == 0 && keyboard_only()) {
            return {ultramodern::input::Device::Controller, ultramodern::input::Pak::None};
        }
        return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
    }
    return {ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak};
}

} // namespace

void init_input() {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0 &&
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "no controller support: %s\n", SDL_GetError());
        return;
    }

    std::lock_guard<std::mutex> lock(pads_mutex);
    for (SDL_GameController *&pad : pads) {
        if (pad != nullptr && SDL_GameControllerGetAttached(pad) == SDL_FALSE) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    }

    size_t port = 0;
    for (int joystick = 0; joystick < SDL_NumJoysticks() && port < pads.size(); joystick++) {
        if (!SDL_IsGameController(joystick)) {
            continue;
        }
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(joystick);
        bool already_open = false;
        for (SDL_GameController *pad : pads) {
            if (pad != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) {
                already_open = true;
                break;
            }
        }
        if (already_open) {
            continue;
        }
        while (port < pads.size() && pads[port] != nullptr) {
            port++;
        }
        if (port >= pads.size()) {
            break;
        }
        pads[port] = SDL_GameControllerOpen(joystick);
    }
}

ultramodern::input::callbacks_t input_callbacks() {
    return ultramodern::input::callbacks_t{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = connected_device,
    };
}

} // namespace n64b
