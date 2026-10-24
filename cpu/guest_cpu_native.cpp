// SPDX-License-Identifier: GPL-2.0-or-later
// The platform services of the ahead-of-time engine (cpu/guest_cpu_aot.cpp), AArch64 hosts (namespace bbnative, cpu/guest_cpu_native.h):
//  - host-function stubs: addresses the guest calls to reach a host function. Nothing ever executes them (the engine's dispatcher recognises the
//    address), the region is plain memory below 1 TiB;
//  - the sentinel, the guest address a bottom guest call returns to;
//  - CPUID / XGETBV (guest_cpu_cpuid.h: the CPUID model the game sees; the host values it needs are read in Init), the TSC (the cycle counter
//    scaled to at least 1 GHz), and the handler for unaligned `lock` operations (guest_cpu_atomics.h).
#if defined(__aarch64__)
#include "guest_cpu_native.h"
#include "guest_cpu_cpuid.h"
#include "guest_cpu_atomics.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
constexpr size_t StubSize = 16;
constexpr size_t StubBytes = 1 << 20;
unsigned char* stubs;
void* stub_functions[StubBytes / StubSize];
std::atomic<size_t> stub_count;
std::mutex stub_lock;
std::unordered_map<void*, uintptr_t> stub_of;

[[noreturn]] void Fatal(const char* message) {
    std::fprintf(stderr, "STOP: native guest CPU: %s\n", message);
    std::fflush(nullptr);
    _exit(21);
}
unsigned tsc_shift;
bbaot::CpuidHost cpuid_host;
uint64_t CounterFrequency() {
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 1000000000;
#endif
}
}  // namespace

namespace bbnative {
void Init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const uint64_t frequency = CounterFrequency();
        uint64_t scaled = frequency;
        while (scaled && scaled < 1'000'000'000) { scaled <<= 1; ++tsc_shift; }   // SmallTSCScale (FEX's default): at least 1 GHz
        // The CPUID model, as FEX derives it (FEXCore CPUID.cpp / HostFeatures.cpp / CPUInfo.cpp):
        // - cores: the cpuN directories of /sys/devices/system/cpu, counted from cpu0 up to the first one that is missing
        unsigned cpus = 1;
        for (;; ++cpus) {
            char path[64];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u", cpus);
            if (access(path, F_OK) != 0) break;
        }
        cpuid_host.cores = cpus;
        // - leaf 0x15: the counter's own frequency (ECX) and the scale (EBX = 1 << shift); TSC frequency = ECX * EBX
        cpuid_host.tsc_hz = uint32_t(frequency);
        cpuid_host.tsc_shift = tsc_shift;
        // - brand string: the product name of core 0's MIDR (HideHybrid: every core reports core 0's name); 0 when unreadable
        char data[18] = {};
        if (FILE* f = std::fopen("/sys/devices/system/cpu/cpu0/regs/identification/midr_el1", "r")) {
            const size_t n = std::fread(data, 1, sizeof(data), f);
            std::fclose(f);
            if (n != sizeof(data)) std::memset(data, 0, sizeof(data));
        }
        cpuid_host.midr = bbaot::ParseMidr(data, sizeof(data));
        // - CLZERO: DC ZVA usable and its block exactly the emulated 64-byte cache line (DCZID_EL0: DZP bit 4, BS bits 3:0 = log2(words))
        uint64_t dczid = 0x10;
#if defined(__aarch64__)
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
#endif
        cpuid_host.clzero = !(dczid & 0x10) && (4u << (dczid & 0xF)) == 64;
    });
}
unsigned TscShift() { return tsc_shift; }
uint64_t Tsc() {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v << tsc_shift;
}
uint64_t TscFrequency() { return CounterFrequency() << tsc_shift; }
void Cpuid(uint64_t function, uint64_t leaf, uint32_t out[4]) {
    bbaot::CpuidHost h = cpuid_host;
    unsigned cpu = 0;
    if (syscall(SYS_getcpu, &cpu, nullptr, nullptr) == 0) h.cpu = cpu;
    const bbaot::CpuidResult r = bbaot::Cpuid(h, uint32_t(function), uint32_t(leaf));
    out[0] = r.eax; out[1] = r.ebx; out[2] = r.ecx; out[3] = r.edx;
}
uint64_t Xgetbv(uint64_t function) { return bbaot::Xgetbv(cpuid_host, function); }
bool HandleUnalignedAtomic(void* ucontext) {
    auto& m = static_cast<ucontext_t*>(ucontext)->uc_mcontext;
    uint32_t insn;
    std::memcpy(&insn, reinterpret_cast<const void*>(m.pc), 4);
    if (!bbaot::EmulateAtomic(insn, reinterpret_cast<uint64_t*>(m.regs), m.sp)) return false;
    m.pc += 4;
    return true;
}
}  // namespace bbnative

namespace bbnative {
void InitStubs(void* (*map)(size_t size, int prot)) {
    stubs = static_cast<unsigned char*>(map(StubBytes, PROT_READ | PROT_WRITE));
    if (!stubs) Fatal("cannot allocate the host function stubs");
    std::memset(stubs, 0xcc, StubBytes);
    stub_count = 2;   // slot 0: the sentinel; slot 1 unused
    Init();
    std::printf("Guest CPU: ahead-of-time units only, TSC x%u\n", 1u << tsc_shift);
}
uintptr_t HostFunction(void* fn) {
    std::lock_guard<std::mutex> g(stub_lock);
    if (auto it = stub_of.find(fn); it != stub_of.end()) return it->second;
    const size_t index = stub_count.load(std::memory_order_relaxed);
    if (index >= StubBytes / StubSize) Fatal("too many host functions");
    stub_functions[index] = fn;
    stub_count.store(index + 1, std::memory_order_release);
    const uintptr_t address = reinterpret_cast<uintptr_t>(stubs) + index * StubSize;
    stub_of.emplace(fn, address);
    return address;
}
void* const* Stubs(uintptr_t* base, size_t* bytes) {
    *base = reinterpret_cast<uintptr_t>(stubs);
    *bytes = StubBytes;
    return stub_functions;
}
uint64_t Sentinel() { return reinterpret_cast<uint64_t>(stubs); }
uintptr_t HostPc(const void* ucontext) { return static_cast<const ucontext_t*>(ucontext)->uc_mcontext.pc; }
uintptr_t HostSp(const void* ucontext) { return static_cast<const ucontext_t*>(ucontext)->uc_mcontext.sp; }
}  // namespace bbnative
#endif
