// SPDX-License-Identifier: GPL-2.0-or-later
// The platform services (cpu/guest_cpu_native.h) for the unit tests, which include cpu/guest_cpu_aot.cpp on an x86-64 host: nothing runs on a
// guest CPU here, so most are empty. Include after cpu/guest_cpu_aot.cpp.
#pragma once
namespace bbnative {
void Init() {}
unsigned TscShift() { return 0; }
uint64_t Tsc() { return 0; }
uint64_t TscFrequency() { return 1; }
void Cpuid(uint64_t, uint64_t, uint32_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
uint64_t Xgetbv(uint64_t) { return 0; }
bool HandleUnalignedAtomic(void*) { return false; }
void InitStubs(void* (*)(size_t, int)) {}
uintptr_t HostFunction(void* fn) { return uintptr_t(fn); }
void* const* Stubs(uintptr_t* base, size_t* bytes) { *base = 0; *bytes = 0; return nullptr; }
uint64_t Sentinel() { return 0x7fff00000000ull; }
uintptr_t HostPc(const void*) { return 0; }
uintptr_t HostSp(const void*) { return 0; }
}  // namespace bbnative
