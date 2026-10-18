// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace BbThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}

/// The calling thread's pin (PinFromEnv), if any.
struct Pin {
    cpu_set_t set;
    bool on;
    const char* what;
};
inline Pin& Pinned() {
    static thread_local Pin pin{};
    return pin;
}

/// The calling thread runs only on the CPUs the environment variable `env` lists (taskset style:
/// "7", "3-7", "6,7"); unset or unparsable: unchanged. For a frame-critical thread that is light
/// once it no longer spins (shadPS4:GpuComm): on the prime core it finishes its bursts sooner and
/// sleeps in between, where the scheduler would put it on a slower core.
inline void PinFromEnv(const char* env, const char* what) {
    const char* list = std::getenv(env);
    if (!list || !*list) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const char* p = list; *p;) {
        char* end = nullptr;
        const long first = std::strtol(p, &end, 10);
        if (end == p || first < 0 || first >= CPU_SETSIZE) {
            std::printf("Threads: %s=%s is not a CPU list; %s keeps its CPUs\n", env, list, what);
            return;
        }
        long last = first;
        if (*end == '-') {
            p = end + 1;
            last = std::strtol(p, &end, 10);
            if (end == p || last < first || last >= CPU_SETSIZE) {
                std::printf("Threads: %s=%s is not a CPU list; %s keeps its CPUs\n", env, list, what);
                return;
            }
        }
        for (long cpu = first; cpu <= last; ++cpu) {
            CPU_SET(cpu, &set);
        }
        p = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') {
            break;
        }
    }
    const int result = sched_setaffinity(0, sizeof(set), &set);
    std::printf("Threads: %s on CPUs %s (%s)%s\n", what, list, env, result == 0 ? "" : ": refused");
    if (result == 0) {
        Pinned() = {set, true, what};
    }
}

/// Puts a pinned thread back on its CPUs when Android reset them: moving the app between cpusets
/// (foreground / top-app), and the Thor's performance service, give every thread the cpuset's CPUs again, many times a
/// second. Checked at most once a millisecond (a coarse clock read otherwise); call it from the thread's loop.
inline void KeepPinned() {
    Pin& pin = Pinned();
    if (!pin.on) {
        return;
    }
    static thread_local long long next_check_ns = 0;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    const long long now_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
    if (now_ns < next_check_ns) {
        return;
    }
    next_check_ns = now_ns + 1000000LL; // 1 ms: Android resets it within fractions of a second
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(0, sizeof(current), &current) == 0 && !CPU_EQUAL(&current, &pin.set)) {
        static thread_local unsigned reapplied = 0;
        if (sched_setaffinity(0, sizeof(pin.set), &pin.set) == 0 && reapplied++ < 4) {
            std::printf("Threads: %s put back on its CPUs (Android reset them)\n", pin.what);
        }
    }
}

} // namespace BbThreads
