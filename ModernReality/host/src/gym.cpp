// SPDX-License-Identifier: GPL-3.0-or-later
//
// A game something else is playing.
//
// `--gym <name>` runs the console for a caller in another process instead of
// for a person: the game advances one frame when it is asked to, at whatever
// speed the machine manages, and stops between frames with everything parked.
// The caller reads the game's memory directly -- it is mapped into both
// processes -- and writes the controller into a block it shares with this one.
// `include/modernreality/gym.h` is the whole of what passes between them.
//
// Three things here are not what a player's console does.
//
// The retrace is the caller's. ultramodern's own VI thread is a clock and a
// clock is exactly what an environment must not have: sixty frames a second is
// slower than anything learning to play wants, and a frame that ends because
// time passed is one whose result may not be there yet. So the host takes the
// retrace (`ultramodern::host_retrace`) and then waits for the console to go
// quiet, which is a fact about the game rather than about the clock.
//
// The renderer is optional. Nothing about a game needs a picture, and drawing
// one costs more than everything else here put together, so headless swaps
// RT64 for a renderer that counts frames and discards them. The game still
// builds its display lists -- that is its own code and it is not ours to skip.
//
// And it can be put back where it was. A savestate here is the console's eight
// megabytes plus the registers of every thread, taken while the game is quiet
// and put back into a game that is quiet in the same shape, which is what makes
// an episode start at the same moment every time without watching the intro to
// get there. See `load_state` for what "the same shape" has to mean and why it
// is checked rather than assumed.

#include "host.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <librecomp/game.hpp>

#include "modernreality/gym.h"

namespace n64b {

/// Frames the game has drawn, kept by whichever renderer is in use. The
/// headless one below is the other writer of it; see renderer.cpp.
extern std::atomic<uint64_t> drawn_frames;

namespace {

/// How long a single retrace is allowed to take before the game is called
/// stuck. This is a watchdog rather than a budget: a headless frame takes under
/// a millisecond, and the number is large because a drawn one can take seconds
/// the first time RT64 meets a game and compiles its shaders.
constexpr uint32_t kRetraceTimeoutMs = 30000;

/// How long to let the console settle before reading or replacing its state.
constexpr uint32_t kSettleTimeoutMs = 30000;

/// How many retraces one frame of the game may cost before the same is said. A
/// game that draws thirty frames a second spends two; one loading a level can
/// spend many more, and one waiting for something that is never coming spends
/// all of them.
constexpr uint32_t kMaxRetracesPerFrame = 900;

struct Gym {
    bool active = false;
    bool headless = false;
    int shared_fd = -1;
    int socket_fd = -1;
    n64b_gym_block *block = nullptr;
    /// The console's memory, once the game has been given some. Set from
    /// `map_memory` on the game's own thread, read by the control thread.
    std::atomic<uint8_t *> rdram{nullptr};
    std::thread control;
    std::string name;
} gym;

/// A pointer into the console's memory, for a structure the game owns.
///
/// Words are stored so that an aligned load is a load, which is why this is a
/// subtraction and not a translation. Halfwords and bytes are not, and nothing
/// here reads one: everything below is either a 32-bit field or a block copy.
template <typename T>
T *at(uint8_t *rdram, uint32_t address) {
    return reinterpret_cast<T *>(rdram + (address - N64B_GYM_RDRAM_BASE));
}

// --- a renderer for nobody --------------------------------------------------

class Headless final : public ultramodern::renderer::RendererContext {
public:
    Headless() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Metal;
    }

    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig &,
                       const ultramodern::renderer::GraphicsConfig &) override {
        return true;
    }
    void enable_instant_present() override {}

    /// The game's frame, counted and dropped.
    ///
    /// Counting is not incidental: a display list is what a frame of the game
    /// *is* here, so this is the clock a step is measured in. The work of
    /// building the list has already happened in the game's own code by the
    /// time we are called, and that is the half that could not be skipped
    /// anyway.
    void send_dl(const OSTask *) override {
        drawn_frames.fetch_add(1, std::memory_order_relaxed);
    }

    void send_dummy_workload(uint32_t) override {}
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_headless(
    uint8_t *, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<Headless>();
}

// --- the console, put down and picked up again -------------------------------

constexpr uint64_t kStateMagic = 0x4E36425453544154ull; // "N6BTSTAT"
/// 2 added the video interface. A state without it restores a game that runs
/// and cannot be seen, so version 1 is refused rather than read.
constexpr uint32_t kStateVersion = 2;

struct StateHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t thread_count;
    uint64_t rdram_bytes;
    /// What the game had drawn when this was taken. Carried for the caller's
    /// benefit; nothing here depends on it.
    uint64_t frames;
    char game_id[8];
    /// What the console was set up to put on a screen. Not in the memory below
    /// and not recoverable from it; see `ultramodern::ViSnapshot`.
    ultramodern::ViSnapshot vi;
};

/// One thread of the game, as a state file holds it.
///
/// `thread` is the address of its OSThread in the console's memory, which is
/// the only name for a thread that means the same thing in two processes -- a
/// game puts its thread structures in the same place every time it boots.
/// `queue` is the message queue it was parked on, and it is here to be checked
/// rather than restored: see `load_state`.
struct StateThread {
    uint32_t thread;
    uint32_t queue;
    recomp_context registers;
};

struct Collected {
    std::vector<StateThread> threads;
    uint8_t *rdram;
};

void collect_thread(PTR(OSThread) thread_, void *registers, void *user) {
    Collected *collected = static_cast<Collected *>(user);
    const OSThread *thread = at<OSThread>(collected->rdram, uint32_t(thread_));
    StateThread entry{};
    entry.thread = uint32_t(thread_);
    entry.queue = uint32_t(thread->queue);
    entry.registers = *static_cast<const recomp_context *>(registers);
    // A pointer into the register file it came from, which means nothing to
    // whoever reads this file. It is rebuilt on the way back in.
    entry.registers.f_odd = nullptr;
    collected->threads.push_back(entry);
}

/// Point a restored register file's odd-float pointer back into itself.
///
/// The recompiler reaches an odd single-precision register through this, and
/// which half of which register it names depends on the mode bit the game set
/// -- see cop0_status_write. Both live in the file, so the answer does too;
/// what cannot travel is the address.
void relink_odd_floats(recomp_context *registers) {
    registers->f_odd = registers->mips3_float_mode ? &registers->f1.u32l : &registers->f0.u32h;
}

bool write_all(int fd, const void *data, size_t bytes);

bool save_state(const char *path, std::string &error) {
    uint8_t *rdram = gym.rdram.load();
    if (rdram == nullptr) {
        error = "the game has no memory yet";
        return false;
    }
    // Wait for the frame in progress to end rather than refusing: the caller
    // asked between frames, but a device it cannot see may still be working --
    // RT64 compiling the shaders for a game's first display list takes seconds.
    if (!ultramodern::wait_until_quiescent(0, kSettleTimeoutMs)) {
        char why[384];
        ultramodern::describe_quiescence(rdram, why, sizeof(why));
        error = std::string("the game never came to rest: ") + why;
        return false;
    }

    Collected collected{{}, rdram};
    ultramodern::for_each_thread_registers(collect_thread, &collected);
    if (collected.threads.empty()) {
        error = "the game has no threads to save";
        return false;
    }

    StateHeader header{};
    header.magic = kStateMagic;
    header.version = kStateVersion;
    header.thread_count = uint32_t(collected.threads.size());
    header.rdram_bytes = N64B_GYM_RDRAM_BYTES;
    header.frames = drawn_frames.load();
    const std::u8string game_id = recomp::current_game_id();
    std::memcpy(header.game_id, game_id.data(), std::min(sizeof(header.game_id), game_id.size()));
    ultramodern::save_vi(rdram, &header.vi);

    // Written beside the file and renamed over it, so that a state killed
    // halfway through writing leaves the last good one rather than eight
    // megabytes of nothing.
    const std::string temporary = std::string(path) + ".writing";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = std::string("could not write ") + temporary + ": " + std::strerror(errno);
        return false;
    }
    const bool written = write_all(fd, &header, sizeof(header)) &&
                         write_all(fd, collected.threads.data(),
                                   collected.threads.size() * sizeof(StateThread)) &&
                         write_all(fd, rdram, N64B_GYM_RDRAM_BYTES);
    const bool closed = ::close(fd) == 0;
    if (!written || !closed) {
        error = std::string("could not write all of ") + temporary;
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path) != 0) {
        error = std::string("could not put ") + path + " in place: " + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

/// What a restore has to know about the game it is going into: which threads
/// there are, where each one is waiting, and what belongs to this process.
struct Restoring {
    const StateThread *threads;
    uint32_t count;
    uint8_t *rdram;
    /// What the live process owns inside each OSThread and the file must not be
    /// allowed to overwrite: the pointer to the host thread that is parked
    /// there. The file's copy names a structure in another process.
    std::vector<std::pair<uint32_t, UltraThreadContext *>> hosts;
    bool matched = true;
    std::string complaint;
};

void check_thread(PTR(OSThread) thread_, void *registers, void *user) {
    (void)registers;
    Restoring *restoring = static_cast<Restoring *>(user);
    const uint32_t address = uint32_t(thread_);
    OSThread *thread = at<OSThread>(restoring->rdram, address);
    restoring->hosts.emplace_back(address, thread->context);

    for (uint32_t i = 0; i < restoring->count; i++) {
        if (restoring->threads[i].thread != address) {
            continue;
        }
        if (restoring->threads[i].queue != uint32_t(thread->queue)) {
            restoring->matched = false;
            restoring->complaint = "thread " + std::to_string(thread->id) +
                                   " is waiting somewhere else than it was in the state";
        }
        return;
    }
    restoring->matched = false;
    restoring->complaint = "thread " + std::to_string(thread->id) + " is not in the state";
}

bool read_all(int fd, void *data, size_t bytes);

/// Put a saved console back, and say no if it would not be the same console.
///
/// The memory is the easy half. The hard half is that a thread of a recompiled
/// game is a host thread parked inside a real call stack -- `osRecvMesg` inside
/// `display_and_vsync` inside the game loop -- and that stack is not in the file
/// and cannot be. What the file has is the registers; what the process has is
/// the stacks. Putting one into the other is only correct when the two agree
/// about where every thread is.
///
/// They do agree, and not by luck. This runs only when the game is quiet, and a
/// game is quiet in one shape: every thread parked on the message queue it waits
/// on every frame, which is the same queue in the same function whatever is
/// happening in the game -- which is why a state taken in the middle of a level
/// restores into a game that has only just booted. That is what the queue in
/// each thread's record is for: it is checked against the live one, and a
/// mismatch is refused rather than resumed, because resuming it would return a
/// thread into a function that is not the one it left.
bool load_state(const char *path, std::string &error) {
    uint8_t *rdram = gym.rdram.load();
    if (rdram == nullptr) {
        error = "the game has no memory yet";
        return false;
    }
    // Wait for the frame in progress to end rather than refusing: the caller
    // asked between frames, but a device it cannot see may still be working --
    // RT64 compiling the shaders for a game's first display list takes seconds.
    if (!ultramodern::wait_until_quiescent(0, kSettleTimeoutMs)) {
        char why[384];
        ultramodern::describe_quiescence(rdram, why, sizeof(why));
        error = std::string("the game never came to rest: ") + why;
        return false;
    }

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        error = std::string("could not read ") + path + ": " + std::strerror(errno);
        return false;
    }
    StateHeader header{};
    if (!read_all(fd, &header, sizeof(header))) {
        ::close(fd);
        error = std::string(path) + " is too short to be a state";
        return false;
    }
    if (header.magic != kStateMagic) {
        ::close(fd);
        error = std::string(path) + " is not a state this host wrote";
        return false;
    }
    if (header.version != kStateVersion) {
        ::close(fd);
        error = std::string(path) + " was written by an older host (version " +
                std::to_string(header.version) + ", this one writes " +
                std::to_string(kStateVersion) + "); make it again";
        return false;
    }
    if (header.rdram_bytes != N64B_GYM_RDRAM_BYTES) {
        ::close(fd);
        error = std::string(path) + " holds a console with a different amount of memory";
        return false;
    }
    const std::u8string game_id = recomp::current_game_id();
    if (std::strncmp(header.game_id, reinterpret_cast<const char *>(game_id.c_str()),
                     sizeof(header.game_id)) != 0) {
        ::close(fd);
        error = std::string(path) + " is a state for a different game";
        return false;
    }

    // A game has a handful of threads; a number far past that is a corrupt file
    // rather than a console, and this is read before anything is allocated for it.
    if (header.thread_count == 0 || header.thread_count > 64) {
        ::close(fd);
        error = std::string(path) + " claims " + std::to_string(header.thread_count) +
                " threads, which is not a console";
        return false;
    }
    std::vector<StateThread> threads(header.thread_count);
    if (!read_all(fd, threads.data(), threads.size() * sizeof(StateThread))) {
        ::close(fd);
        error = std::string(path) + " is missing its threads";
        return false;
    }

    Restoring restoring{threads.data(), header.thread_count, rdram, {}, true, {}};
    ultramodern::for_each_thread_registers(check_thread, &restoring);
    if (restoring.hosts.size() != threads.size()) {
        restoring.matched = false;
        restoring.complaint = "the game has " + std::to_string(restoring.hosts.size()) +
                              " threads and the state has " + std::to_string(threads.size());
    }
    if (!restoring.matched) {
        ::close(fd);
        error = "this game is not in the shape the state was taken in: " + restoring.complaint;
        return false;
    }

    // Past here it has to work, because half a restore is a game that cannot be
    // recovered by anything short of booting again.
    const bool read = read_all(fd, rdram, N64B_GYM_RDRAM_BYTES);
    ::close(fd);
    if (!read) {
        error = std::string(path) + " is missing the console's memory, which is now half put back";
        return false;
    }

    // The console's memory is back; so is the console's picture. The video
    // interface is state the game set through libultra and then forgot, so a
    // game restored without it keeps whatever the *booting* console was left in
    // -- which is a blanked raster, since every game blanks the screen on its
    // way up and unblanks it on the frame it first has something to show. That
    // frame is inside the state rather than ahead of it, so it never comes
    // again, and the game runs and plays and cannot be seen.
    ultramodern::load_vi(rdram, &header.vi);

    // Give each thread structure back the host thread that is actually parked
    // in it. Everything else in there -- the queue it is on, the next thread in
    // that queue, its stack pointer, its priority -- came out of the file and
    // is the game's own.
    for (const auto &[address, context] : restoring.hosts) {
        at<OSThread>(rdram, address)->context = context;
    }

    for (const StateThread &saved : threads) {
        struct Handing {
            const StateThread *saved;
        } handing{&saved};
        ultramodern::for_each_thread_registers(
            [](PTR(OSThread) thread_, void *registers, void *user) {
                const StateThread *saved = static_cast<Handing *>(user)->saved;
                if (uint32_t(thread_) != saved->thread) {
                    return;
                }
                recomp_context *live = static_cast<recomp_context *>(registers);
                *live = saved->registers;
                relink_odd_floats(live);
            },
            &handing);
    }
    return true;
}

// --- reading and writing without partial results -----------------------------

bool write_all(int fd, const void *data, size_t bytes) {
    const uint8_t *from = static_cast<const uint8_t *>(data);
    while (bytes > 0) {
        const ssize_t wrote = ::write(fd, from, bytes);
        if (wrote <= 0) {
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        from += wrote;
        bytes -= size_t(wrote);
    }
    return true;
}

bool read_all(int fd, void *data, size_t bytes) {
    uint8_t *into = static_cast<uint8_t *>(data);
    while (bytes > 0) {
        const ssize_t got = ::read(fd, into, bytes);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        into += got;
        bytes -= size_t(got);
    }
    return true;
}

// --- the clock, and what one turn of it means --------------------------------

std::atomic<uint64_t> retraces_given{0};

/// One interrupt of the video interface, and then wait for the game to finish
/// reacting to it. False if it did not.
bool one_retrace() {
    const uint64_t epoch = ultramodern::quiescent_epoch();
    ultramodern::host_retrace();
    retraces_given.fetch_add(1, std::memory_order_relaxed);
    if (ultramodern::wait_until_quiescent(epoch, kRetraceTimeoutMs)) {
        return true;
    }
    char why[384];
    ultramodern::describe_quiescence(gym.rdram.load(), why, sizeof(why));
    std::fprintf(stderr, "gym: the game did not finish a retrace in %u ms: %s\n",
                 kRetraceTimeoutMs, why);
    return false;
}

/// Advance the game by frames of its own.
///
/// A frame is a display list handed over, because that is the thing a game does
/// once per frame whatever else it is doing -- and unlike a retrace it is the
/// game's own clock rather than the television's. Super Mario 64 spends two
/// retraces on each, which is why it plays at thirty frames a second; a game
/// that misses its target spends three, and nothing here has to know that.
uint32_t advance(uint32_t frames, uint32_t *retraces_taken) {
    *retraces_taken = 0;
    for (uint32_t frame = 0; frame < frames; frame++) {
        const uint64_t target = drawn_frames.load() + 1;
        uint32_t spent = 0;
        while (drawn_frames.load() < target) {
            if (!one_retrace()) {
                return N64B_GYM_STALLED;
            }
            *retraces_taken += 1;
            if (++spent >= kMaxRetracesPerFrame) {
                return N64B_GYM_STALLED;
            }
        }
    }
    return N64B_GYM_OK;
}

/// Run the console until it first goes quiet, which is a game that has booted:
/// its threads made, its video interface configured, everything waiting for the
/// next retrace. Nothing can be asked of it before this.
bool boot_until_quiet() {
    constexpr int kGivingUpAfter = 20000;
    for (int tries = 0; tries < kGivingUpAfter; tries++) {
        // A short wait rather than a long one: before the game's threads exist
        // there is nobody to go quiet, and the answer is simply to keep the
        // retraces coming until there is.
        if (ultramodern::wait_until_quiescent(0, 2)) {
            return true;
        }
        // Never hand the console a retrace while a device still owes it an
        // answer. The game cannot act on it, and the work only piles up: the
        // first display list of a game holds the renderer for as long as it
        // takes to compile its shaders, and a retrace every two milliseconds
        // through that is thousands of screen updates queued behind it.
        if (ultramodern::hardware_busy()) {
            continue;
        }
        ultramodern::host_retrace();
        retraces_given.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void say(uint32_t status, const std::string &message) {
    gym.block->status = status;
    std::snprintf(gym.block->message, sizeof(gym.block->message), "%s", message.c_str());
}

void control_thread() {
    ultramodern::set_native_thread_name("Gym");

    // The game is started by librecomp on its own thread, and its memory
    // arrives with it. Nothing can be driven until both have happened.
    for (int tries = 0; gym.rdram.load() == nullptr && tries < 10000; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (gym.rdram.load() == nullptr) {
        std::fprintf(stderr, "gym: the game never started, so there is nothing to drive.\n");
        return;
    }

    if (!boot_until_quiet()) {
        std::fprintf(stderr, "gym: the game never settled after booting.\n");
        return;
    }

    // The doorbell, rung once to say the game is up and waiting.
    gym.block->frames = drawn_frames.load();
    gym.block->retraces = retraces_given.load();
    say(N64B_GYM_OK, "ready");
    const uint8_t ready = 1;
    if (!write_all(gym.socket_fd, &ready, 1)) {
        return;
    }

    while (true) {
        uint8_t doorbell = 0;
        if (!read_all(gym.socket_fd, &doorbell, 1)) {
            // The other end is gone. So is the reason for this process.
            ultramodern::quit();
            return;
        }

        const uint32_t command = gym.block->command;
        const uint32_t count = gym.block->count;
        gym.block->retraces_taken = 0;
        say(N64B_GYM_OK, "");

        switch (command) {
            case N64B_GYM_STEP: {
                uint32_t retraces_taken = 0;
                const uint32_t status = advance(count == 0 ? 1 : count, &retraces_taken);
                gym.block->retraces_taken = retraces_taken;
                if (status != N64B_GYM_OK) {
                    say(status, "the game stopped answering the video interface");
                }
                break;
            }
            case N64B_GYM_SAVE_STATE: {
                std::string error;
                if (!save_state(gym.block->path, error)) {
                    say(N64B_GYM_REFUSED, error);
                }
                break;
            }
            case N64B_GYM_LOAD_STATE: {
                std::string error;
                if (!load_state(gym.block->path, error)) {
                    say(N64B_GYM_REFUSED, error);
                }
                break;
            }
            case N64B_GYM_QUIT:
                gym.block->frames = drawn_frames.load();
                write_all(gym.socket_fd, &doorbell, 1);
                ultramodern::quit();
                return;
            case N64B_GYM_NOTHING:
                break;
            default:
                say(N64B_GYM_REFUSED, "no such command");
                break;
        }

        gym.block->frames = drawn_frames.load();
        gym.block->retraces = retraces_given.load();
        if (!write_all(gym.socket_fd, &doorbell, 1)) {
            ultramodern::quit();
            return;
        }
    }
}

} // namespace

bool gym_open(const std::string &name, bool headless, std::string &error) {
    gym.shared_fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (gym.shared_fd < 0) {
        error = "could not open the shared block " + name + ": " + std::strerror(errno);
        return false;
    }
    void *mapped = ::mmap(nullptr, N64B_GYM_RDRAM_OFFSET, PROT_READ | PROT_WRITE, MAP_SHARED,
                          gym.shared_fd, 0);
    if (mapped == MAP_FAILED) {
        error = std::string("could not map the shared block: ") + std::strerror(errno);
        return false;
    }
    gym.block = static_cast<n64b_gym_block *>(mapped);
    if (gym.block->magic != N64B_GYM_MAGIC || gym.block->abi != N64B_GYM_ABI) {
        error = "the shared block is not one this host understands";
        return false;
    }
    gym.block->rdram_offset = N64B_GYM_RDRAM_OFFSET;
    gym.block->rdram_bytes = N64B_GYM_RDRAM_BYTES;

    gym.socket_fd = N64B_GYM_SOCKET_FD;
    // The caller is meant to have handed us one end of a socket. Saying so here
    // is better than a first write failing somewhere inside the game.
    int kind = 0;
    socklen_t kind_size = sizeof(kind);
    if (::getsockopt(gym.socket_fd, SOL_SOCKET, SO_TYPE, &kind, &kind_size) != 0) {
        error = "there is no control socket on file descriptor " +
                std::to_string(N64B_GYM_SOCKET_FD);
        return false;
    }

    gym.name = name;
    gym.headless = headless;
    gym.active = true;
    return true;
}

bool gym_running() { return gym.active; }

bool gym_headless() { return gym.active && gym.headless; }

ultramodern::renderer::callbacks_t headless_renderer_callbacks() {
    return ultramodern::renderer::callbacks_t{
        .create_render_context = create_headless,
        .get_graphics_api_name = nullptr,
    };
}

/// Make the console's memory the memory the caller can see.
///
/// The runtime has already allocated it and put the first megabyte of the
/// cartridge in it by the time this runs, so the eight megabytes are carried
/// out, the shared object is mapped over the top of them, and they are carried
/// back in. From here the two processes are reading and writing the same pages
/// -- the recompiled code included, which is what makes reading the game's
/// variables free rather than a request.
///
/// This has to happen before the register window is aliased over it, which is
/// why it is the first thing `place_sections` does: that alias is a second view
/// of these same pages, and making it first would leave it pointing at the
/// pages this replaces.
void gym_map_memory(uint8_t *rdram) {
    if (!gym.active) {
        return;
    }
    std::vector<uint8_t> carried(N64B_GYM_RDRAM_BYTES);
    std::memcpy(carried.data(), rdram, N64B_GYM_RDRAM_BYTES);
    void *mapped = ::mmap(rdram, N64B_GYM_RDRAM_BYTES, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED, gym.shared_fd, N64B_GYM_RDRAM_OFFSET);
    if (mapped != rdram) {
        std::fprintf(stderr,
                     "gym: the console's memory could not be shared (%s); the caller will read "
                     "an empty game.\n",
                     std::strerror(errno));
        return;
    }
    std::memcpy(rdram, carried.data(), N64B_GYM_RDRAM_BYTES);
    gym.rdram.store(rdram);
}

void gym_start() {
    if (!gym.active) {
        return;
    }
    gym.control = std::thread{control_thread};
    gym.control.detach();
}

bool gym_input(int controller, uint16_t *buttons, float *x, float *y) {
    if (!gym.active || controller != 0 || gym.block == nullptr) {
        return false;
    }
    const n64b_gym_pad &pad = gym.block->pad[0];
    *buttons = pad.buttons;
    *x = pad.stick_x;
    *y = pad.stick_y;
    return true;
}

} // namespace n64b
