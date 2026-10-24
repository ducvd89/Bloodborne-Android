// SPDX-License-Identifier: GPL-2.0-or-later
// Native replacements for what the ahead-of-time engine (cpu/guest_cpu_aot.cpp) otherwise asks FEX for (namespace bbnative: CPUID, XGETBV, the TSC, the
// unaligned-atomic fault handler; used in strict mode by every AArch64 build), and, in the BB_AOT_ONLY build (libbbcpu without FEXCore), the few
// bb_fb_* services of "the fallback CPU" when there is none.
// Everything a fallback would run is a bug in the unit library there (strict mode is always on), so what is left is bookkeeping:
//  - host-function stubs: addresses the guest calls to reach a host function. Nothing ever executes them (the engine's dispatcher recognises the
//    address), the region is plain memory below 1 TiB like FEX's stub region;
//  - the sentinel, the guest address a bottom guest call returns to;
//  - CPUID / XGETBV (guest_cpu_cpuid.h: FEX's CPUID model with fixed host features), the TSC (the cycle counter scaled like FEX's), and the handler
//    for unaligned `lock` operations (guest_cpu_atomics.h).
#if defined(__aarch64__)
#include "guest_cpu_native.h"
#include "guest_cpu_fallback.h"
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
#ifdef BB_AOT_ONLY
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
#endif
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
        uint64_t frequency = CounterFrequency();
        while (frequency && (frequency << tsc_shift) < 1'000'000'000) ++tsc_shift;   // SmallTSCScale (FEX's default): at least 1 GHz
        cpuid_host.cores = unsigned(sysconf(_SC_NPROCESSORS_CONF) > 0 ? sysconf(_SC_NPROCESSORS_CONF) : 1);
        cpuid_host.tsc_hz = uint32_t(frequency << tsc_shift);
        cpuid_host.tsc_shift = tsc_shift;
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

#ifdef BB_AOT_ONLY
extern "C" {
void bb_fb_init(void* (*map)(size_t size, int prot)) {
    stubs = static_cast<unsigned char*>(map(StubBytes, PROT_READ | PROT_WRITE));
    if (!stubs) Fatal("cannot allocate the host function stubs");
    std::memset(stubs, 0xcc, StubBytes);
    stub_count = 2;   // slot 0: the sentinel; slot 1 unused
    bbnative::Init();
    std::printf("Guest CPU: ahead-of-time units only (no FEXCore in this build), TSC x%u\n", 1u << tsc_shift);
}
void bb_fb_code(uintptr_t, size_t) {}
uintptr_t bb_fb_host_function(void* fn) {
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
uint64_t bb_fb_call(uintptr_t, unsigned, const uint64_t*) { Fatal("a guest call without BB_AOT: this runtime runs ahead-of-time units only"); }
void bb_fb_thread_stack(void*, size_t) {}
void bb_fb_set_gs(void*) {}
void bb_fb_abandon(void) {}
void bb_fb_thread_end(void) {}
uintptr_t bb_fb_return_address(uintptr_t) { return 0; }
uintptr_t bb_fb_stack_pointer(void) { return 0; }
int bb_fb_signal_regs(const void* ucontext, GuestRegs* regs) {
    std::memset(regs, 0, sizeof(*regs));
#if defined(__aarch64__)
    const auto& m = static_cast<const ucontext_t*>(ucontext)->uc_mcontext;
    regs->rip = m.pc;
    regs->gpr[GUEST_RSP] = m.sp;
#else
    (void)ucontext;
#endif
    return 0;
}
uintptr_t bb_fb_host_pc(const void* ucontext) {
#if defined(__aarch64__)
    return static_cast<const ucontext_t*>(ucontext)->uc_mcontext.pc;
#else
    (void)ucontext;
    return 0;
#endif
}
int bb_fb_handle_fault(int, siginfo_t*, void*) { return 0; }
// Guest code is never written into: a hook is a native slot or nothing (guest_cpu.h: the caller then hooks the old way, which makes the function
// patched and, with no variant, a miss).
int bb_fb_hook(uintptr_t, size_t, GuestHook) { return 0; }
void bb_fb_invalidate(uintptr_t, size_t) {}
uint64_t bb_fb_tsc(void) { return bbnative::Tsc(); }
uint64_t bb_fb_tsc_frequency(void) { return bbnative::TscFrequency(); }

uint64_t bb_fb_sentinel(void) { return reinterpret_cast<uint64_t>(stubs); }
void* const* bb_fb_stubs(uintptr_t* base, size_t* bytes) {
    *base = reinterpret_cast<uintptr_t>(stubs);
    *bytes = StubBytes;
    return stub_functions;
}
void bb_fb_cpuid(uint64_t function, uint64_t leaf, uint32_t out[4]) { bbnative::Cpuid(function, leaf, out); }
uint64_t bb_fb_xgetbv(uint64_t function) { return bbnative::Xgetbv(function); }
int bb_fb_handle_unaligned(void* ucontext) { return bbnative::HandleUnalignedAtomic(ucontext) ? 1 : 0; }
}  // extern "C"
#endif
#endif
