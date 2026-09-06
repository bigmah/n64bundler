// SPDX-License-Identifier: GPL-3.0-or-later
//
// RT64, wearing ultramodern's renderer interface.
//
// ultramodern owns a graphics thread and hands it three kinds of work: a
// display list the game submitted, a screen update carrying the VI registers
// at scanout, and a change of graphics settings. RT64 wants exactly those
// three things, so most of this file is translation rather than logic.
//
// The one piece that is not translation is the Core structure. RT64 was
// written against an emulator plugin API, so it expects pointers to the
// console's registers and calls back for interrupts. Here there is no
// emulator: ultramodern models the VI itself and librecomp raises the
// interrupts. So the registers RT64 reads for scanout point into ultramodern's
// own VI state, and the ones it would raise an interrupt through point at
// storage nothing reads.

#include "host.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <librecomp/game.hpp>

extern "C" PTR(void) osViGetCurrentFramebuffer();
#include <librecomp/rsp.hpp>

#include "hle/rt64_application.h"

namespace n64b {

/// Frames the game has drawn, for the parts of the host that are counted in
/// them. Written by the renderer on the graphics thread and read by the input
/// script on a game thread, which is one writer and one reader of a word.
std::atomic<uint64_t> drawn_frames{0};

uint64_t frames_drawn() { return drawn_frames.load(std::memory_order_relaxed); }

namespace {

/// The console registers RT64 wants that nothing here models.
///
/// The RDP registers exist because a real plugin would drive the RDP through
/// them; RT64 renders in HLE and librecomp signals DP completion itself, so
/// these are written and never read. Keeping them as real storage rather than
/// null pointers means RT64's own bookkeeping works unchanged.
struct DeadRegisters {
    uint32_t mi_intr = 0;
    uint32_t dpc_start = 0;
    uint32_t dpc_end = 0;
    uint32_t dpc_current = 0;
    uint32_t dpc_status = 0;
    uint32_t dpc_clock = 0;
    uint32_t dpc_bufbusy = 0;
    uint32_t dpc_pipebusy = 0;
    uint32_t dpc_tmem = 0;
};

/// RT64 also reads instruction memory. Nothing in this runtime has one -- the
/// RSP is either recompiled microcode or the renderer -- so it gets a blank
/// one of the right size rather than a null pointer to trip over.
uint8_t imem[0x1000] = {};

void no_interrupts() {
    // librecomp raises SP and DP completion itself, on the graphics thread,
    // around the call that got us here. There is nothing for RT64 to signal.
}

RT64::UserConfiguration::Antialiasing map_antialiasing(ultramodern::renderer::Antialiasing msaa) {
    switch (msaa) {
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        default: return RT64::UserConfiguration::Antialiasing::None;
    }
}

RT64::UserConfiguration::AspectRatio map_aspect(ultramodern::renderer::AspectRatio ratio) {
    switch (ratio) {
        case ultramodern::renderer::AspectRatio::Expand: return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual: return RT64::UserConfiguration::AspectRatio::Manual;
        default: return RT64::UserConfiguration::AspectRatio::Original;
    }
}

RT64::UserConfiguration::RefreshRate map_refresh_rate(ultramodern::renderer::RefreshRate rate) {
    switch (rate) {
        case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Manual: return RT64::UserConfiguration::RefreshRate::Manual;
        default: return RT64::UserConfiguration::RefreshRate::Display;
    }
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult result) {
    switch (result) {
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        default:
            return ultramodern::renderer::SetupResult::Success;
    }
}

/// Where RT64 keeps its shader cache and its own configuration file. Under our
/// own directory rather than its default so that two frontends on one machine
/// do not fight over one cache.
std::filesystem::path rt64_data_path() {
    const char *home = std::getenv("HOME");
    std::filesystem::path base = home != nullptr ? std::filesystem::path(home) : std::filesystem::path(".");
    return base / "Library" / "Application Support" / "N64Bundler" / "rt64";
}

class RT64Context final : public ultramodern::renderer::RendererContext {
public:
    RT64Context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle,
                bool developer_mode)
        : developer_(developer_mode) {
        RT64::Application::Core core{};
        core.window.window = window_handle.window;
        core.window.view = window_handle.view;

        // The ROM header, which RT64 hashes to pick up any per-game
        // configuration it ships. librecomp has the image in memory by now
        // because the game thread loaded it before it started.
        std::span<const uint8_t> rom = recomp::get_rom();
        header_.fill(0);
        if (rom.size() >= header_.size()) {
            std::memcpy(header_.data(), rom.data(), header_.size());
        }
        core.HEADER = header_.data();

        core.RDRAM = rdram;
        core.DMEM = dmem;
        core.IMEM = imem;

        core.MI_INTR_REG = &dead_.mi_intr;
        core.DPC_START_REG = &dead_.dpc_start;
        core.DPC_END_REG = &dead_.dpc_end;
        core.DPC_CURRENT_REG = &dead_.dpc_current;
        core.DPC_STATUS_REG = &dead_.dpc_status;
        core.DPC_CLOCK_REG = &dead_.dpc_clock;
        core.DPC_BUFBUSY_REG = &dead_.dpc_bufbusy;
        core.DPC_PIPEBUSY_REG = &dead_.dpc_pipebusy;
        core.DPC_TMEM_REG = &dead_.dpc_tmem;

        // The VI registers RT64 scans out from are ultramodern's, updated on
        // every screen update just before we are called.
        ultramodern::renderer::ViRegs *vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        core.checkInterrupts = no_interrupts;

        RT64::ApplicationConfiguration app_config{};
        app_config.appId = "N64Bundler";
        app_config.dataPath = rt64_data_path();
        app_config.detectDataPath = false;
        app_config.useConfigurationFile = true;

        app_ = std::make_unique<RT64::Application>(core, app_config);
        app_->userConfig.developerMode = developer_mode;
        apply_config(ultramodern::renderer::get_graphics_config());

        uint64_t thread_id = 0;
        pthread_threadid_np(nullptr, &thread_id);
        const RT64::Application::SetupResult result = app_->setup(uint32_t(thread_id));
        setup_result = map_setup_result(result);
        chosen_api = ultramodern::renderer::GraphicsApi::Metal;
        if (result != RT64::Application::SetupResult::Success) {
            app_.reset();
            return;
        }

        // Again, because `setup` read RT64's own configuration file over the
        // top of what we just asked for.
        //
        // `useConfigurationFile` is on so that RT64's developer tools have
        // somewhere to keep their state, and the first thing `setup` does is
        // load that file into `userConfig` -- so a machine that has ever run
        // this once is a machine where the host's settings are read, replaced,
        // and never used. The window's aspect ratio, its antialiasing and its
        // refresh rate all came from a file written months ago rather than from
        // the game being launched. The first call still has to happen, because
        // `setup` picks a graphics API and a sample count out of the config on
        // its way through; this one is what makes those choices stick.
        apply_config(ultramodern::renderer::get_graphics_config());
        app_->updateUserConfig(true);
    }

    bool valid() override { return app_ != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig &old_config,
                       const ultramodern::renderer::GraphicsConfig &new_config) override {
        if (app_ == nullptr) {
            return false;
        }
        apply_config(new_config);
        // Framebuffers are only worth throwing away when what they hold has
        // changed shape; a refresh rate change leaves them valid.
        const bool discard = old_config.res_option != new_config.res_option ||
                             old_config.msaa_option != new_config.msaa_option ||
                             old_config.hpfb_option != new_config.hpfb_option;
        app_->updateUserConfig(discard);
        return true;
    }

    void enable_instant_present() override {
        // A newer RT64 has a present mode for this; this one does not, and
        // presenting a frame early is an optimisation rather than a
        // correctness requirement.
    }

    void send_dl(const OSTask *task) override {
        if (app_ == nullptr) {
            return;
        }
        // Everything in a task is a KSEG0 address; RT64 indexes RDRAM
        // directly, so the segment bits come off.
        constexpr uint32_t physical = 0x03FFFFFFu;
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(uint32_t(task->t.ucode) & physical,
                                        uint32_t(task->t.ucode_data) & physical, true);
        frames_++;
        drawn_frames.store(frames_, std::memory_order_relaxed);
        // Whether the renderer knows this game's microcode at all is the other
        // half of a black window, and it is knowable exactly once.
        static bool first = true;
        if (first) {
            first = false;
            std::fprintf(stderr, "note: the game submitted its first display list, and RT64 %s "
                                 "its microcode.\n",
                         app_->interpreter->hleGBI != nullptr ? "recognises" : "does NOT recognise");
        }
        last_display_list_ = uint32_t(task->t.data_ptr);
        const auto started = std::chrono::steady_clock::now();
        app_->processDisplayLists(app_->core.RDRAM, uint32_t(task->t.data_ptr) & physical, 0, true);
        dl_time_ += std::chrono::steady_clock::now() - started;
    }

    void send_dummy_workload(uint32_t fb_address) override {
        // What this exists for is the window between the process starting and
        // the game submitting its first display list, so that the window is
        // not simply undefined. This RT64 has no way to enqueue an empty
        // workload, and the swap chain clears, so leaving it is honest.
    }

    /// Write what the video interface is scanning out, as a PPM.
    ///
    /// The window is the real answer to "does this game draw", but a window
    /// cannot be looked at from a script, on a locked screen, or in a log. The
    /// renderer copies each finished frame back into the console's own memory
    /// in the console's own format, which is what a real video interface would
    /// be reading, so that copy is the frame -- and turning it into a file
    /// costs nothing and needs nothing from the graphics API.
    ///
    /// What that copy is not is what the window shows, because the video
    /// interface is not only a reader: with VI_CTRL_GAMMA_ON it raises every
    /// channel to a power on the way out, and RT64 does the same to what it
    /// presents. A frame written straight out of memory is therefore several
    /// stops darker than the game, and darker than any emulator's screenshot of
    /// the same moment -- which turns every comparison against the reference
    /// console into an argument about brightness. So the gamma goes on here
    /// too, with RT64's own exponent, and the picture is the window's.
    void write_screenshot(const char *path) const {
        const ultramodern::renderer::ViRegs *vi = ultramodern::renderer::get_vi_regs();
        const unsigned width = vi->VI_WIDTH_REG;
        // The origin is a physical address, and the frame is as tall as the
        // vertical active window, which the VI counts in half-lines.
        const unsigned origin = vi->VI_ORIGIN_REG & 0x00FFFFFFu;
        const unsigned start = (vi->VI_V_START_REG >> 16) & 0x3FFu;
        const unsigned end = vi->VI_V_START_REG & 0x3FFu;
        const unsigned height = (end > start) ? (end - start) / 2 : 240;
        if (width == 0 || origin == 0 || width > 1280 || height > 720) {
            std::fprintf(stderr, "note: no frame to write: origin 0x%08X width %u\n", origin, width);
            return;
        }
        // Every N64 pixel format the video interface can scan out.
        const unsigned depth = vi->VI_STATUS_REG & 3u;
        if (depth != 2 && depth != 3) {
            std::fprintf(stderr, "note: the video interface is blanked (status 0x%08X)\n",
                         vi->VI_STATUS_REG);
            return;
        }
        std::FILE *out = std::fopen(path, "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "note: could not write %s\n", path);
            return;
        }
        // The exponent RT64 presents with, and the table that saves doing it
        // per channel per pixel.
        const bool gamma_on = (vi->VI_STATUS_REG & 0x8u) != 0;
        unsigned char gamma[256];
        for (int i = 0; i < 256; i++) {
            gamma[i] = gamma_on
                           ? (unsigned char)std::lround(255.0 * std::pow(i / 255.0, 1.0 / 2.2))
                           : (unsigned char)i;
        }
        std::fprintf(out, "P6\n%u %u\n255\n", width, height);
        const uint8_t *rdram = app_->core.RDRAM;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                unsigned r = 0, g = 0, b = 0;
                if (depth == 2) {
                    // 16-bit, five bits each and one of coverage. A word holds
                    // two pixels, and a word is native here.
                    const unsigned at = origin + (y * width + x) * 2u;
                    unsigned word = 0;
                    __builtin_memcpy(&word, rdram + (at & ~3u), sizeof(word));
                    const unsigned pixel = (at & 2u) ? (word & 0xFFFFu) : (word >> 16);
                    r = ((pixel >> 11) & 31) * 255 / 31;
                    g = ((pixel >> 6) & 31) * 255 / 31;
                    b = ((pixel >> 1) & 31) * 255 / 31;
                } else {
                    const unsigned at = origin + (y * width + x) * 4u;
                    unsigned word = 0;
                    __builtin_memcpy(&word, rdram + at, sizeof(word));
                    r = (word >> 24) & 0xFF;
                    g = (word >> 16) & 0xFF;
                    b = (word >> 8) & 0xFF;
                }
                const unsigned char rgb[3] = {gamma[r & 0xFF], gamma[g & 0xFF], gamma[b & 0xFF]};
                std::fwrite(rgb, 1, 3, out);
            }
        }
        std::fclose(out);
        std::fprintf(stderr, "note: wrote %s, %u by %u, from the frame at 0x%08X, vi status 0x%08X%s\n",
                     path, width, height, origin, vi->VI_STATUS_REG,
                     gamma_on ? ", with the video interface's gamma" : "");
    }

    /// The console's eight megabytes at one of the frames a screenshot names.
    ///
    /// A picture says what came out; this says what the game thought. The two
    /// together are what a comparison against the reference console is made
    /// of, and the frame number is what makes them the same moment: wall clock
    /// cannot line up a cached interpreter with compiled code, and a vertical
    /// interrupt is a thing both consoles count the same way.
    void write_memory_image(unsigned frame) const {
        const char *path = std::getenv("N64B_SCREENSHOT_RAM");
        if (path == nullptr) {
            return;
        }
        char named[512];
        std::snprintf(named, sizeof(named), "%s.%u.bin", path, frame);
        std::FILE *out = std::fopen(named, "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "note: could not write %s\n", named);
            return;
        }
        std::fwrite(app_->core.RDRAM, 1, 8u * 1024u * 1024u, out);
        std::fclose(out);
        // The address the last display list started at, because that is what
        // reading one back out of this image needs and nothing in the image
        // says it: `n64dl.py <image> <address>`.
        std::fprintf(stderr, "note: wrote %s, the console's memory at frame %u, "
                             "whose display list began at 0x%08X.\n",
                     named, frame, last_display_list_);
    }

    void update_screen() override {
        if (app_ == nullptr) {
            return;
        }
        // A frame on disk, for when the window cannot be looked at.
        //
        // `N64B_SCREENSHOT_AFTER` may name several frames, because what a game
        // does is a sequence and one still of it is a poor account: a title, a
        // menu and a level are three runs of a minute each otherwise. With more
        // than one frame asked for, each file carries the frame it came from.
        //
        // A frame here is a display list the game submitted, which is one frame
        // of the *game*, and deliberately not one interrupt of the video
        // interface. The two are different clocks and nothing lines them up: the
        // interface scans out sixty times a second whatever the game is doing,
        // and Banjo-Tooie draws twenty. The reason it has to be the game's clock
        // is `refshot` on the other console, whose frame is `M64CMD_ADVANCE_FRAME`
        // -- and mupen64plus advances that in `new_frame()`, which its RSP calls
        // once per graphics task. Counting anything else here makes two pictures
        // that are numbered the same and are minutes apart in the game.
        if (const char *path = std::getenv("N64B_SCREENSHOT")) {
            static const std::vector<unsigned> at = [] {
                std::vector<unsigned> frames;
                const char *after = std::getenv("N64B_SCREENSHOT_AFTER");
                for (const char *scan = after; scan != nullptr && *scan != '\0';) {
                    char *end = nullptr;
                    frames.push_back(unsigned(std::strtoul(scan, &end, 10)));
                    scan = (*end == ',') ? end + 1 : end;
                    if (end == scan) {
                        break;
                    }
                }
                if (frames.empty()) {
                    frames.push_back(600);
                }
                // In order, because the shot for each is taken on the first
                // screen update past it and the count of what has been taken
                // only goes forwards.
                std::sort(frames.begin(), frames.end());
                return frames;
            }();
            // The screen update that follows the display list is the one that
            // has it in memory to photograph, so the shot is taken on the first
            // update after the game's frame count reaches the one asked for.
            static uint64_t shot_through = 0;
            const uint64_t drawn = frames_;
            for (unsigned frame : at) {
                if (frame > drawn || frame <= shot_through) {
                    continue;
                }
                shot_through = frame;
                std::string named = path;
                if (at.size() > 1) {
                    const size_t dot = named.find_last_of('.');
                    const std::string stem = dot == std::string::npos ? named : named.substr(0, dot);
                    const std::string suffix = dot == std::string::npos ? "" : named.substr(dot);
                    named = stem + "." + std::to_string(frame) + suffix;
                }
                // A picture of the window if the window server will give one,
                // because that is what the game looks like. The frame in the
                // console's own memory is the fallback and not the same thing:
                // RT64 writes it back a framebuffer pair at a time and a scene
                // built out of several leaves parts of itself out of it.
                if (!capture_window(named.c_str())) {
                    write_screenshot(named.c_str());
                }
                write_memory_image(frame);
            }
        }
        // What the VI is scanning out, once a second, in developer mode. It is
        // the first thing to look at when a game runs and the window stays
        // black: a width and an origin mean the game configured the video
        // interface, and a framebuffer of zero means it never handed one over.
        if (developer_) {
            static int frames = 0;
            if (frames % 60 == 0) {
                const ultramodern::renderer::ViRegs *vi = ultramodern::renderer::get_vi_regs();
                // Where the graphics thread's second went. It has one queue and
                // two kinds of work in it -- a display list the game submitted,
                // and a screen update the VI thread posts sixty times a second
                // -- so a game that draws twenty frames a second is either a
                // game that submitted twenty lists or a thread that had no room
                // for more. These two numbers say which.
                using ms = std::chrono::duration<double, std::milli>;
                std::fprintf(stderr,
                             "vi: origin 0x%08X width %u, game framebuffer 0x%08X, "
                             "%llu display lists so far (%llu this second), "
                             "%.1f ms in display lists, %.1f ms presenting\n",
                             vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG,
                             uint32_t(osViGetCurrentFramebuffer()),
                             (unsigned long long)frames_,
                             (unsigned long long)(frames_ - last_frames_),
                             ms(dl_time_).count(), ms(present_time_).count());
                last_frames_ = frames_;
                dl_time_ = {};
                present_time_ = {};
            }
            frames++;
        }
        const auto started = std::chrono::steady_clock::now();
        app_->updateScreen();
        present_time_ += std::chrono::steady_clock::now() - started;
    }

    void shutdown() override {
        if (app_ != nullptr) {
            app_->end();
            app_.reset();
        }
    }

    uint32_t get_display_framerate() const override {
        if (app_ == nullptr || app_->appWindow == nullptr) {
            return 60;
        }
        return app_->appWindow->getRefreshRate();
    }

    float get_resolution_scale() const override {
        if (app_ == nullptr) {
            return 1.0f;
        }
        // The scale ultramodern reports to the game, so that anything drawn
        // at native resolution knows how much bigger the target is.
        return float(app_->sharedQueueResources->resolutionScale.x);
    }

private:
    void apply_config(const ultramodern::renderer::GraphicsConfig &config) {
        RT64::UserConfiguration &user = app_->userConfig;
        // Metal is the only backend that exists on this platform, and picking
        // it outright means a misconfigured "Auto" cannot land on a backend
        // that is not there.
        user.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
        user.antialiasing = map_antialiasing(config.msaa_option);
        user.aspectRatio = map_aspect(config.ar_option);
        user.refreshRate = map_refresh_rate(config.rr_option);
        user.refreshRateTarget = config.rr_manual_value > 0 ? config.rr_manual_value : 60;
        user.downsampleMultiplier = config.ds_option > 0 ? config.ds_option : 1;
        user.developerMode = config.developer_mode;

        switch (config.res_option) {
            case ultramodern::renderer::Resolution::Original:
                user.resolution = RT64::UserConfiguration::Resolution::Manual;
                user.resolutionMultiplier = 1.0;
                break;
            case ultramodern::renderer::Resolution::Original2x:
                user.resolution = RT64::UserConfiguration::Resolution::Manual;
                user.resolutionMultiplier = 2.0;
                break;
            default:
                // Match the window, which is what "Auto" means to everyone who
                // has not read the enum.
                user.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
                break;
        }

        switch (config.hpfb_option) {
            case ultramodern::renderer::HighPrecisionFramebuffer::On:
                user.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::High;
                break;
            case ultramodern::renderer::HighPrecisionFramebuffer::Off:
                user.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
                break;
            default:
                user.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Automatic;
                break;
        }

        user.validate();
    }

    std::unique_ptr<RT64::Application> app_;
    std::chrono::steady_clock::duration dl_time_{};
    std::chrono::steady_clock::duration present_time_{};
    uint64_t last_frames_ = 0;
    uint32_t last_display_list_ = 0;
    bool developer_ = false;
    /// Display lists submitted, which is the game's own frame count.
    uint64_t frames_ = 0;
    DeadRegisters dead_{};
    std::array<uint8_t, 0x40> header_{};
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_context(
    uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

std::string api_name(ultramodern::renderer::GraphicsApi api) {
    return api == ultramodern::renderer::GraphicsApi::Metal ? "Metal" : "Metal (the only one here)";
}

} // namespace

ultramodern::renderer::callbacks_t renderer_callbacks() {
    return ultramodern::renderer::callbacks_t{
        .create_render_context = create_context,
        .get_graphics_api_name = api_name,
    };
}

} // namespace n64b
