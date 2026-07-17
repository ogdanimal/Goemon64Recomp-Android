// Android crash diagnostics + lifecycle observability.
//
// RETAINED PERMANENTLY (not investigation scaffolding). Originally added to
// root-cause the background/resume audio-DSP SIGSEGV (Bug 6), it proved its
// worth and stays as a standing asset: any future fault on a new GPU / OS /
// lifecycle edge is classified instantly instead of needing a fresh repro.
//
// Installs an SA_SIGINFO handler for SIGSEGV/SIGBUS that, before chaining to
// the previously-installed handler (so the normal Android tombstone still
// prints), logs:
//   - the faulting address
//   - the RDRAM base and, if the fault is inside the 4 GB RDRAM reservation,
//     the N64-space offset + whether it is past the 512 MB committed guard
//     boundary (i.e. an out-of-bounds recompiled pointer -- how the Bug-6
//     audio fault was classified: a corrupted N64 pointer, not unmapped memory)
//   - the faulting thread name
//   - the current app lifecycle phase (foreground / backgrounded / resuming)
//
// set_phase() is also the lifecycle-transition log the pause gate relies on for
// observability (see input.cpp's SDL_APP_* handlers); it is load-bearing for
// diagnosability, not debug-only. The handler is cheap and __ANDROID__-guarded.

#if defined(__ANDROID__)

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include "librecomp/addresses.hpp"

#define DIAG_TAG "Goemon64-diag"
#define DIAGE(...) __android_log_print(ANDROID_LOG_ERROR, DIAG_TAG, __VA_ARGS__)
#define DIAGI(...) __android_log_print(ANDROID_LOG_INFO,  DIAG_TAG, __VA_ARGS__)

namespace goemon64::diag {

    // App lifecycle phase, set from the SDL event pump (main thread) and read
    // from the (async-signal) crash handler. Plain int is lock-free and
    // async-signal-safe to read.
    enum Phase : int { Foreground = 0, Background = 1, Resuming = 2 };
    static std::atomic<int> g_phase{Foreground};

    // RDRAM base, published once the runtime has allocated it. Read in the
    // signal handler to compute the N64-space offset of a fault.
    static std::atomic<uintptr_t> g_rdram_base{0};

    void set_phase(int phase) {
        g_phase.store(phase, std::memory_order_relaxed);
        DIAGI("lifecycle phase -> %s",
              phase == Foreground ? "FOREGROUND" :
              phase == Background ? "BACKGROUND" : "RESUMING");
    }

    void set_rdram_base(const void* base) {
        g_rdram_base.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
        DIAGI("rdram base registered = %p (committed guard boundary at +0x%zx, reservation ends +0x%zx)",
              base, static_cast<size_t>(recomp::mem_size), static_cast<size_t>(recomp::allocation_size));
    }

    namespace {
        struct sigaction g_prev_segv{};
        struct sigaction g_prev_bus{};
        std::atomic<bool> g_installed{false};

        void crash_handler(int sig, siginfo_t* info, void* ucontext) {
            const uintptr_t fault = reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr);
            const uintptr_t base = g_rdram_base.load(std::memory_order_relaxed);
            const int phase = g_phase.load(std::memory_order_relaxed);

            char tname[32] = {0};
            pthread_getname_np(pthread_self(), tname, sizeof(tname));

            DIAGE("==== SIGNAL %d (%s) fault_addr=0x%zx thread='%s' phase=%s ====",
                  sig, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "?",
                  static_cast<size_t>(fault), tname,
                  phase == Foreground ? "FOREGROUND" :
                  phase == Background ? "BACKGROUND" : "RESUMING");

            if (base != 0 && fault >= base && fault < base + recomp::allocation_size) {
                const uintptr_t off = fault - base;
                const bool past_guard = off >= recomp::mem_size;
                DIAGE("  fault is INSIDE rdram reservation: n64_offset=0x%zx %s",
                      static_cast<size_t>(off),
                      past_guard ? "-> PAST 512MB COMMITTED GUARD (out-of-bounds recompiled pointer)"
                                 : "-> within committed 512MB (unexpected: valid region faulted)");
            } else if (base != 0) {
                DIAGE("  fault is OUTSIDE rdram reservation (base=0x%zx). Not an RDRAM-offset fault.",
                      static_cast<size_t>(base));
            } else {
                DIAGE("  rdram base not yet registered; cannot classify fault.");
            }

            // Chain to the previously-installed handler so the normal Android
            // tombstone/debuggerd path still runs.
            const struct sigaction& prev = (sig == SIGBUS) ? g_prev_bus : g_prev_segv;
            if (prev.sa_flags & SA_SIGINFO) {
                if (prev.sa_sigaction) {
                    prev.sa_sigaction(sig, info, ucontext);
                    return;
                }
            } else if (prev.sa_handler == SIG_IGN) {
                return;
            } else if (prev.sa_handler && prev.sa_handler != SIG_DFL) {
                prev.sa_handler(sig);
                return;
            }
            // Fall back to default disposition.
            signal(sig, SIG_DFL);
            raise(sig);
        }
    } // namespace

    void install_crash_handler() {
        if (g_installed.exchange(true)) {
            return;
        }
        struct sigaction sa{};
        sa.sa_sigaction = &crash_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_prev_segv);
        sigaction(SIGBUS, &sa, &g_prev_bus);
        DIAGI("crash diagnostics handler installed");
    }

} // namespace goemon64::diag

#endif // __ANDROID__
