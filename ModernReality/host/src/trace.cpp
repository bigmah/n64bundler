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
#include <csignal>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>

namespace {

/// The word the watched address holds, in the game's byte order.
uint8_t *watched_rdram = nullptr;

unsigned watch_value(unsigned address) {
    if (watched_rdram == nullptr) {
        return 0;
    }
    const uint8_t *at = watched_rdram + (address - 0x80000000u);
    return (unsigned(at[0]) << 24) | (unsigned(at[1]) << 16) | (unsigned(at[2]) << 8) | at[3];
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
};
Count counts[kSlots];

void record(const char *name) {
    size_t slot = (reinterpret_cast<uintptr_t>(name) >> 4) % kSlots;
    for (size_t probe = 0; probe < 64; probe++) {
        Count &entry = counts[(slot + probe) % kSlots];
        if (entry.name == name) {
            entry.hits++;
            return;
        }
        if (entry.name == nullptr) {
            entry.name = name;
            entry.hits = 1;
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
    std::fprintf(stderr, "\n--- the functions this game spent its time in ---\n");
    for (size_t i = 0; i < found; i++) {
        std::fprintf(stderr, "%12llu  %s\n", (unsigned long long)top[i]->hits, top[i]->name);
    }
}

void dump() {
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
    if (reported++ >= 40) {
        return;
    }
    std::fprintf(stderr, "watch: %2zu  0x%08X = 0x%08X, in %s\n", reported, address,
                 watch_value(address), where);
}

/// Called from every recompiled function when the module was built with
/// tracing. Deliberately not thread-safe beyond the atomic counter: a torn
/// read across the game's threads costs one wrong name in a log, and a lock
/// here would change the timing of the thing being debugged.
extern "C" void n64b_trace(const char *name) {
    names[next.fetch_add(1, std::memory_order_relaxed) % kEntries] = name;
    record(name);
}

/// Arrange for the buffer to be printed however the process ends. A game that
/// has gone wrong exits in whichever way the runtime noticed first -- a failed
/// function lookup calls exit, an unrecoverable one aborts, and a bad pointer
/// takes a signal -- so all three are covered.
namespace n64b {
void install_trace(bool catch_signals);
void set_watch_memory(uint8_t *rdram);
}

void n64b::set_watch_memory(uint8_t *rdram) {
    watched_rdram = rdram;
}

void n64b::install_trace(bool catch_signals) {
    std::atexit(dump);
    if (!catch_signals) {
        // Only a developer run takes the signals. Swallowing a segfault in an
        // ordinary one would hide a crash behind a tidy message, and there is
        // nothing to print anyway unless the module was built with --trace.
        return;
    }
    for (int signal_number : {SIGABRT, SIGSEGV, SIGBUS, SIGILL}) {
        std::signal(signal_number, [](int number) {
            dump();
            std::fprintf(stderr, "(the game took signal %d)\n", number);
            std::_Exit(EXIT_FAILURE);
        });
    }
}
