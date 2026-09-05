// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ring buffer of the last functions a game entered.
//
// When a recompiled game goes wrong it goes wrong quietly: a pointer read out
// of the wrong place, a function that returns a value nothing set, and the
// first visible sign is an indirect call to an address that is plainly text
// rather than code. By then the function that actually broke has returned.
//
// The recompiler emits `TRACE_ENTRY()` at the top of every function when its
// trace mode is on, and `n64b-port --trace` points that macro here. Nothing is
// formatted while the game runs -- the names are string literals in the module
// and only the pointers are stored -- so the cost is one store and an
// increment per call, and the buffer is dumped when the process exits however
// it exits.

#include <atomic>
#include <execinfo.h>

#include "host.hpp"
#include <csignal>
#include <thread>
#include <chrono>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/// The word the watched address holds, in the game's byte order.
uint8_t *watched_rdram = nullptr;

unsigned watch_value(unsigned address) {
    if (watched_rdram == nullptr) {
        return 0;
    }
    // The runtime keeps RDRAM so that an aligned word is a native word --
    // that is what `MEM_W` reads -- and only byte and halfword accesses swap.
    unsigned word = 0;
    __builtin_memcpy(&word, watched_rdram + (address - 0x80000000u), sizeof(word));
    return word;
}


/// Print the words at each `address:count` in `N64B_DUMP`.
///
/// When a game has gone wrong the question is usually what a particular table
/// or global holds now, as against what the cartridge put there. The addresses
/// come from the disassembly, so they belong on the command line rather than
/// in this file.
void dump_memory(const char *why) {
    const char *spec = std::getenv("N64B_DUMP");
    if (spec == nullptr || watched_rdram == nullptr) {
        return;
    }
    std::fprintf(stderr, "\n--- memory, %s ---\n", why);
    while (*spec != '\0') {
        char *after = nullptr;
        const unsigned long address = std::strtoul(spec, &after, 0);
        unsigned long count = 1;
        if (*after == ':') {
            count = std::strtoul(after + 1, &after, 0);
        }
        for (unsigned long i = 0; i < count; i++) {
            const unsigned at = unsigned(address) + unsigned(i) * 4u;
            std::fprintf(stderr, "%s%08X: %08X", i % 4 == 0 ? "" : "  ", at, watch_value(at));
            if (i % 4 == 3 || i + 1 == count) {
                std::fprintf(stderr, "\n");
            }
        }
        if (*after != ',') {
            break;
        }
        spec = after + 1;
    }
}

// Enough to see how the game got where it got, small enough to stay in cache.
constexpr size_t kEntries = 512;

const char *names[kEntries];
std::atomic<size_t> next{0};

// How often each function was entered. Keyed on the name pointer, which is a
// string literal in the module and therefore stable and unique per function.
// A ring shows how the game got somewhere; this shows where it is stuck.
constexpr size_t kSlots = 8192;
struct Count {
    const char *name;
    uint64_t hits;
    /// The trace index this was last entered at. When a game ends up spinning
    /// in two functions, the ring buffer fills with those two and says nothing
    /// about how it got there; the most recently entered functions that are
    /// not the spin are what does.
    uint64_t last_seen;
};
Count counts[kSlots];

/// After this many entries into one function, print a native backtrace once.
///
/// A function that runs billions of times is being called from a loop that has
/// stopped making progress, and the loop is in the caller. The recompiled
/// caller calls it as an ordinary C function, so the native stack is the
/// game's stack and naming it is enough to find the loop. Off unless
/// `N64B_TRACE_STOP` is set.
uint64_t backtrace_at() {
    static const uint64_t at = [] {
        const char *set = std::getenv("N64B_TRACE_STOP");
        return set != nullptr ? std::strtoull(set, nullptr, 0) : 0ull;
    }();
    return at;
}

void print_registers(const void *ctx) {
    static const char *kNames[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                     "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                     "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    if (ctx == nullptr) {
        return;
    }
    // The register file is thirty-two 64-bit words at the top of the context.
    const uint64_t *gpr = static_cast<const uint64_t *>(ctx);
    std::fprintf(stderr, "--- its registers ---\n");
    for (size_t i = 0; i < 32; i++) {
        std::fprintf(stderr, "%s %08X%s", kNames[i], unsigned(gpr[i]), (i % 8 == 7) ? "\n" : "  ");
    }
}

void print_backtrace(const char *name, const void *ctx) {
    void *frames[24];
    const int depth = backtrace(frames, 24);
    std::fprintf(stderr, "\n--- %s has run this many times; who is calling it ---\n", name);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, depth, 2);
    print_registers(ctx);
    dump_memory("where it stopped");
}

void record(const char *name, const void *ctx) {
    size_t slot = (reinterpret_cast<uintptr_t>(name) >> 4) % kSlots;
    for (size_t probe = 0; probe < 64; probe++) {
        Count &entry = counts[(slot + probe) % kSlots];
        if (entry.name == name) {
            entry.hits++;
            entry.last_seen = next.load(std::memory_order_relaxed);
            if (entry.hits == backtrace_at()) {
                print_backtrace(name, ctx);
            }
            return;
        }
        if (entry.name == nullptr) {
            entry.name = name;
            entry.hits = 1;
            entry.last_seen = next.load(std::memory_order_relaxed);
            return;
        }
    }
}

void dump_counts() {
    Count *top[60] = {};
    size_t found = 0;
    for (Count &entry : counts) {
        if (entry.name == nullptr) {
            continue;
        }
        size_t at = found < 60 ? found++ : 60;
        while (at > 0 && (at == 60 || top[at - 1]->hits < entry.hits)) {
            if (at < 60) {
                top[at] = top[at - 1];
            }
            at--;
        }
        if (at < 60) {
            top[at] = &entry;
        }
    }
    if (found == 0) {
        return;
    }
    if (std::getenv("N64B_TRACE_ALL") != nullptr) {
        // Every function entered, however rarely. What the top twenty answer is
        // "where is it spending its time"; what this answers is "did it ever
        // get here at all", which is the question when something ran once and
        // did the wrong thing.
        std::fprintf(stderr, "\n--- every function this game entered ---\n");
        for (const Count &entry : counts) {
            if (entry.name != nullptr) {
                std::fprintf(stderr, "%llu %s\n", (unsigned long long)entry.hits, entry.name);
            }
        }
    }
    {
        // The last dozen distinct functions to run, newest first.
        Count *recent[12] = {};
        size_t kept = 0;
        for (Count &entry : counts) {
            if (entry.name == nullptr) {
                continue;
            }
            size_t at = kept < 12 ? kept++ : 12;
            while (at > 0 && (at == 12 || recent[at - 1]->last_seen < entry.last_seen)) {
                if (at < 12) {
                    recent[at] = recent[at - 1];
                }
                at--;
            }
            if (at < 12) {
                recent[at] = &entry;
            }
        }
        std::fprintf(stderr, "\n--- the last distinct functions to run, newest first ---\n");
        for (size_t i = 0; i < kept; i++) {
            std::fprintf(stderr, "  %s (%llu times)\n", recent[i]->name,
                         (unsigned long long)recent[i]->hits);
        }
    }
    std::fprintf(stderr, "\n--- the functions this game spent its time in ---\n");
    for (size_t i = 0; i < found; i++) {
        std::fprintf(stderr, "%12llu  %s\n", (unsigned long long)top[i]->hits, top[i]->name);
    }
}

// The same, per thread.
//
// A game runs several: the scheduler, the audio, the game loop, and libultra's
// idle thread, which is an empty `while (1)` and enters a function tens of
// millions of times a second. In one shared ring that idle spin is all there
// is, and what the game thread was doing when it stopped -- the only
// interesting thing -- is nowhere. Kept separately, it is the first line of
// its own list.
constexpr size_t kThreads = 16;
constexpr size_t kPerThread = 96;

struct ThreadTail {
    std::atomic<bool> claimed{false};
    const char *names[kPerThread];
    uint64_t repeats[kPerThread];
    size_t next;
};

ThreadTail tails[kThreads];
std::atomic<size_t> thread_count{0};

size_t my_slot() {
    thread_local size_t slot = [] {
        const size_t index = thread_count.fetch_add(1);
        return index < kThreads ? index : kThreads - 1;
    }();
    return slot;
}

void remember_per_thread(const char *name) {
    ThreadTail &tail = tails[my_slot()];
    tail.claimed.store(true, std::memory_order_relaxed);
    // A thread that has stopped is usually spinning between two functions, and
    // an honest tail of the last ninety-six entries is those two functions
    // ninety-six times, which says nothing. Fold a repeat of either of the last
    // two entries into a count, so the tail keeps what ran before the spin.
    for (size_t back = 1; back <= 2 && back <= tail.next; back++) {
        const size_t at = (tail.next - back) % kPerThread;
        if (tail.names[at] == name) {
            tail.repeats[at]++;
            return;
        }
    }
    const size_t at = tail.next % kPerThread;
    tail.names[at] = name;
    tail.repeats[at] = 0;
    tail.next++;
}

void dump_threads() {
    for (size_t i = 0; i < kThreads; i++) {
        ThreadTail &tail = tails[i];
        if (!tail.claimed.load(std::memory_order_relaxed)) {
            continue;
        }
        std::fprintf(stderr, "\n--- thread %zu, last %zu functions, oldest first ---\n", i,
                     tail.next < kPerThread ? tail.next : kPerThread);
        const size_t shown = tail.next < kPerThread ? tail.next : kPerThread;
        for (size_t k = 0; k < shown; k++) {
            const size_t at = (tail.next - shown + k) % kPerThread;
            char with_count[64];
            if (tail.repeats[at] != 0) {
                std::snprintf(with_count, sizeof(with_count), "%s x%llu",
                              tail.names[at] != nullptr ? tail.names[at] : "?",
                              (unsigned long long)(tail.repeats[at] + 1));
            } else {
                std::snprintf(with_count, sizeof(with_count), "%s",
                              tail.names[at] != nullptr ? tail.names[at] : "?");
            }
            std::fprintf(stderr, "%-26s%s", with_count, (k + 1) % 4 == 0 ? "\n" : "  ");
        }
        std::fprintf(stderr, "\n");
    }
}

void dump() {
    // Said however the game ends, and whether or not it was built for tracing:
    // a game that had to have code translated while it ran is a game whose
    // analysis did not have all of it, and that is worth knowing even when
    // everything worked.
    n64b::report_recompiled();

    // The runtime ends a game by calling `exit` from wherever it noticed --
    // an indirect call to an address no function covers, most often -- and
    // this runs on that same stack, so a backtrace here names the recompiled
    // function that made the call.
    void *frames[16];
    const int depth = backtrace(frames, 16);
    std::fprintf(stderr, "\n--- where the game was when it ended ---\n");
    std::fflush(stderr);
    backtrace_symbols_fd(frames, depth, 2);
    // A frame with no symbol may be code the runtime generated while the game
    // ran, which has no symbol to have. Name those by the address they were
    // translated from, which is the only name they have ever had.
    for (int i = 0; i < depth; i++) {
        const unsigned owner = n64b::generated_owner(frames[i]);
        if (owner != 0) {
            std::fprintf(stderr, "%-4d(recompiled while running: the function at 0x%08X)\n", i,
                         owner);
        }
    }
    dump_memory("at exit");
    const size_t total = next.load();
    if (total == 0) {
        return;
    }
    const size_t shown = total < kEntries ? total : kEntries;
    std::fprintf(stderr, "\n--- the last %zu functions this game entered, oldest first ---\n",
                 shown);
    for (size_t i = 0; i < shown; i++) {
        const size_t index = (total - shown + i) % kEntries;
        std::fprintf(stderr, "%s%s", names[index] != nullptr ? names[index] : "?",
                     (i + 1) % 6 == 0 ? "\n" : "  ");
    }
    std::fprintf(stderr, "\n");
    dump_counts();
    dump_threads();
}

} // namespace

/// Called from a module built with `--watch` every time the watched address is
/// accessed. Each distinct function is reported once: what is wanted is the
/// set of places that touch a global, not a log of every access.
extern "C" void n64b_watch(unsigned address, const char *where) {
    // The first few accesses in order, with what the address holds when each
    // one happens. A store shows its old value, so a pointer being written and
    // then cleared reads as the value appearing and then going back to zero --
    // which is the shape of the bug this exists to find.
    static size_t reported = 0;
    if (reported++ >= 120) {
        return;
    }
    std::fprintf(stderr, "watch: %2zu  0x%08X = 0x%08X, in %s\n", reported, address,
                 watch_value(address), where);
}

/// Called from every recompiled function when the module was built with
/// tracing. Deliberately not thread-safe beyond the atomic counter: a torn
/// read across the game's threads costs one wrong name in a log, and a lock
/// here would change the timing of the thing being debugged.
/// The arguments of one named function, every time it runs.
///
/// A trace says a function ran and a watch says an address was touched. What
/// neither says is what a function was *asked* for, and that is the question
/// as soon as a game has several objects of the same kind: the interesting
/// thing about "the routine that puts a character in the player's hands ran
/// twice" is which character, and whether the second call was handing it back.
///
/// `N64B_TRACE_ARGS=<name>` prints the four argument registers on every entry
/// to that function. It needs a `--trace` module, like everything else here.
extern "C" void n64b_trace(const char *name, const void *ctx) {
    static const char *watched = std::getenv("N64B_TRACE_ARGS");
    if (watched != nullptr && ctx != nullptr && std::strcmp(watched, name) == 0) {
        const uint64_t *r = static_cast<const uint64_t *>(ctx);
        std::fprintf(stderr, "call: %s(a0=%08X a1=%08X a2=%08X a3=%08X)\n", name,
                     unsigned(r[4]), unsigned(r[5]), unsigned(r[6]), unsigned(r[7]));
        std::fflush(stderr);
    }
    names[next.fetch_add(1, std::memory_order_relaxed) % kEntries] = name;
    record(name, ctx);
    remember_per_thread(name);
}

/// Arrange for the buffer to be printed however the process ends. A game that
/// has gone wrong exits in whichever way the runtime noticed first -- a failed
/// function lookup calls exit, an unrecoverable one aborts, and a bad pointer
/// takes a signal -- so all three are covered.
namespace n64b {
void install_trace(bool catch_signals);
void set_watch_memory(uint8_t *rdram);
}

/// Snapshots of the console's memory while the game runs.
///
/// The question a running game raises that none of the tools above answers is
/// "what is it changing?" -- a game that draws a frame, plays its music and
/// counts its frames while nothing in its world moves is not stopped anywhere
/// a trace or a backtrace can point at, and the difference between two
/// snapshots a minute apart is what says which of its state is alive.
///
/// `N64B_RAMDUMP=<path>` writes `<path>.NN.bin` every ten seconds. Each one is
/// the eight megabytes the console has, in the order the runtime holds it: a
/// word is a word this machine can load, and a halfword or a byte is at its
/// address exclusive-ored with two or three.
///
/// The images are a derived work of the player's own cartridge in the same way
/// the unpacked one is. Nothing writes one unless it is asked to.
void n64b::set_watch_memory(uint8_t *rdram) {
    watched_rdram = rdram;
    const char *path = std::getenv("N64B_RAMDUMP");
    if (path == nullptr) {
        return;
    }
    static std::thread dumper([path] {
        for (unsigned i = 0; i < 99; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (watched_rdram == nullptr) {
                continue;
            }
            char named[512];
            std::snprintf(named, sizeof(named), "%s.%02u.bin", path, i);
            std::FILE *out = std::fopen(named, "wb");
            if (out == nullptr) {
                continue;
            }
            std::fwrite(watched_rdram, 1, 8u * 1024u * 1024u, out);
            std::fclose(out);
            std::fprintf(stderr, "note: wrote %s, the console's memory as it stands.\n", named);
            std::fflush(stderr);
        }
    });
    dumper.detach();
}

void n64b::install_trace(bool catch_signals) {
    std::atexit(dump);
    if (!catch_signals) {
        // Only a developer run takes the signals. Swallowing a segfault in an
        // ordinary one would hide a crash behind a tidy message, and there is
        // nothing to print anyway unless the module was built with --trace.
        return;
    }
    // SIGTERM and SIGINT as well: a game that runs is one you stop
    // rather than one that stops itself, and the trace is wanted either way.
    for (int signal_number : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTERM, SIGINT}) {
        std::signal(signal_number, [](int number) {
            dump();
            std::fprintf(stderr, "(the game took signal %d)\n", number);
            std::_Exit(EXIT_FAILURE);
        });
    }
}
