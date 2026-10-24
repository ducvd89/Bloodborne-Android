// SPDX-License-Identifier: GPL-2.0-or-later
// Native (FEX-free) services of the ahead-of-time engine, AArch64 hosts: cpu/guest_cpu_native.cpp. Strict mode uses them in every build (so that it
// rehearses the native-only runtime); the BB_AOT_ONLY build has nothing else.
#ifndef BB_GUEST_CPU_NATIVE_H
#define BB_GUEST_CPU_NATIVE_H
#include <cstdint>
namespace bbnative {
void Init();                                          // TSC scale and the core count; idempotent
unsigned TscShift();
uint64_t Tsc();                                       // the cycle counter scaled to at least 1 GHz, as FEX's SmallTSCScale
uint64_t TscFrequency();
void Cpuid(uint64_t function, uint64_t leaf, uint32_t out[4]);   // cpu/guest_cpu_cpuid.h
uint64_t Xgetbv(uint64_t function);
bool HandleUnalignedAtomic(void* ucontext);           // SIGBUS BUS_ADRALN at an LSE atomic: emulated, pc advanced (cpu/guest_cpu_atomics.h)
}  // namespace bbnative
#endif
