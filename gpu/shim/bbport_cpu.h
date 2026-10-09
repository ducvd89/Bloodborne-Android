// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: host CPU cycle counter and spin-wait hint (x86-64: TSC and PAUSE; AArch64: the generic
// timer CNTVCT_EL0 and YIELD). The counter is only compared with itself (profiling, refresh
// intervals); its rate differs between hosts.
#pragma once

#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace BbCpu {

inline std::uint64_t Cycles() {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
#error "No cycle counter for this architecture"
#endif
}

inline void Pause() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

/// Counter ticks in `us` microseconds. x86-64 keeps the fixed ~3.2 GHz estimate the thresholds
/// were tuned with; AArch64 reads the timer frequency.
inline std::uint64_t MicrosToCycles(std::uint64_t us) {
#if defined(__aarch64__)
    std::uint64_t hz;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return us * hz / 1000000;
#else
    return us * 3200;
#endif
}

} // namespace BbCpu
