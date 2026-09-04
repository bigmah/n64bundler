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

// Enough to see how the game got where it got, small enough to stay in cache.
constexpr size_t kEntries = 512;

const char *names[kEntries];
std::atomic<size_t> next{0};

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
}

} // namespace

/// Called from every recompiled function when the module was built with
/// tracing. Deliberately not thread-safe beyond the atomic counter: a torn
/// read across the game's threads costs one wrong name in a log, and a lock
/// here would change the timing of the thing being debugged.
extern "C" void n64b_trace(const char *name) {
    names[next.fetch_add(1, std::memory_order_relaxed) % kEntries] = name;
}

/// Arrange for the buffer to be printed however the process ends. A game that
/// has gone wrong exits in whichever way the runtime noticed first -- a failed
/// function lookup calls exit, an unrecoverable one aborts, and a bad pointer
/// takes a signal -- so all three are covered.
namespace n64b {
void install_trace(bool catch_signals);
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
