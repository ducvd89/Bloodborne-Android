// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: short spin, then block. The GPU threads used to wait for each other with pause/yield
// loops, which kept the cores (and their clocks) at full load while doing nothing. A wait here
// spins for a short budget (BB_SPIN_US, default 20 us) with the CPU's spin hint, then sleeps on
// a futex until the thread that advances the waited-on state calls Event::Notify().
// BB_SPIN_WAITS=1 restores the old spin/yield loops (A/B).
//
// Lost wake-ups: the waiter registers itself (waiters) and then re-checks its condition; the
// notifier publishes its state change and then reads waiters. A seq_cst fence on each side
// between the two steps guarantees that either the waiter sees the change or the notifier sees
// the waiter. The futex word (seq) is read before registering, so a notification between the
// registration and the futex call makes FUTEX_WAIT return at once.

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bbport_cpu.h"

namespace BbWait {

/// BB_SPIN_WAITS=1: the old spinning waits.
inline bool Spinning() {
    static const bool spinning = [] {
        const char* env = std::getenv("BB_SPIN_WAITS");
        return env && env[0] == '1';
    }();
    return spinning;
}

/// Spin budget before a wait blocks, in microseconds (BB_SPIN_US, default 20).
inline unsigned SpinMicros() {
    static const unsigned us = [] {
        if (const char* env = std::getenv("BB_SPIN_US")) {
            return static_cast<unsigned>(std::max(0, std::atoi(env)));
        }
        return 20u;
    }();
    return us;
}

/// The spin budget in BbCpu::Cycles() ticks.
inline std::uint64_t SpinCycles() {
    static const std::uint64_t cycles = BbCpu::MicrosToCycles(SpinMicros());
    return cycles;
}

/// Spins with the CPU's spin hint until done() or `cycles` ticks passed; returns done().
template <typename Done>
inline bool SpinFor(std::uint64_t cycles, Done&& done) {
    if (done()) {
        return true;
    }
    if (cycles == 0) {
        return false;
    }
    const std::uint64_t until = BbCpu::Cycles() + cycles;
    for (unsigned spins = 1;; ++spins) {
        BbCpu::Pause();
        if (done()) {
            return true;
        }
        if (!(spins & 63) && BbCpu::Cycles() >= until) {
            return false;
        }
    }
}

/// A futex a waiter sleeps on until a notifier changes the state it waits for.
class Event {
public:
    /// Blocks until done() (no spin; see SpinThenWait). `timeout_ns` > 0: returns after that
    /// long at the latest (done() may still be false then).
    template <typename Done>
    bool Wait(Done&& done, std::int64_t timeout_ns = 0) {
        timespec timeout{};
        if (timeout_ns > 0) {
            timeout.tv_sec = static_cast<time_t>(timeout_ns / 1000000000);
            timeout.tv_nsec = static_cast<long>(timeout_ns % 1000000000);
        }
        while (!done()) {
            const std::uint32_t seen = seq.load(std::memory_order_acquire);
            waiters.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            bool timed_out = false;
            if (!done()) {
                const long r = syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&seq),
                                       FUTEX_WAIT_PRIVATE, seen,
                                       timeout_ns > 0 ? &timeout : nullptr, nullptr, 0);
                timed_out = r != 0 && errno == ETIMEDOUT;
            }
            waiters.fetch_sub(1, std::memory_order_release);
            if (timed_out) {
                return done();
            }
        }
        return true;
    }

    /// Call after the store that may make a waiter's condition true.
    void Notify() noexcept {
        if (Spinning()) {
            return; // nobody blocks
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_acquire) != 0) {
            seq.fetch_add(1, std::memory_order_release);
            syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&seq), FUTEX_WAKE_PRIVATE, INT_MAX,
                    nullptr, nullptr, 0);
        }
    }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free &&
                  sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    alignas(64) std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint32_t> waiters{0};
};

/// The default wait: spins for the budget, then blocks on `event` until done().
template <typename Done>
inline void SpinThenWait(Event& event, Done&& done) {
    if (!SpinFor(SpinCycles(), done)) {
        event.Wait(done);
    }
}

} // namespace BbWait
