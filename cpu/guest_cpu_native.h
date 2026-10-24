// SPDX-License-Identifier: GPL-2.0-or-later
// The platform services of the ahead-of-time engine (cpu/guest_cpu_aot.cpp), AArch64 hosts: cpu/guest_cpu_native.cpp. Host-function stubs, the
// sentinel, CPUID / XGETBV, the TSC, the unaligned-atomic fault handler. The unit tests (x86-64 hosts) define their own versions.
#ifndef BB_GUEST_CPU_NATIVE_H
#define BB_GUEST_CPU_NATIVE_H
#include <cstddef>
#include <cstdint>
namespace bbnative {
void Init();                                          // TSC scale, core count, MIDR: idempotent
unsigned TscShift();
uint64_t Tsc();                                       // the cycle counter scaled to at least 1 GHz
uint64_t TscFrequency();
void Cpuid(uint64_t function, uint64_t leaf, uint32_t out[4]);   // cpu/guest_cpu_cpuid.h
uint64_t Xgetbv(uint64_t function);
bool HandleUnalignedAtomic(void* ucontext);           // SIGBUS BUS_ADRALN at an LSE atomic: emulated, pc advanced (cpu/guest_cpu_atomics.h)

// Host-function stubs: 16-byte slots in a region below 1 TiB. The guest calls a host function by calling its slot's address; nothing ever
// executes the slots (the engine's dispatcher recognises the address and calls the host function).
void InitStubs(void* (*map)(size_t size, int prot));
uintptr_t HostFunction(void* fn);                     // the slot address of a host function (made on first use)
void* const* Stubs(uintptr_t* base, size_t* bytes);   // the host function of the slot at base + 16 * i is table[i] (NULL: none)
uint64_t Sentinel();                                  // the guest address a bottom guest call returns to

// The host program counter and stack pointer at a signal.
uintptr_t HostPc(const void* ucontext);
uintptr_t HostSp(const void* ucontext);
}  // namespace bbnative
#endif
