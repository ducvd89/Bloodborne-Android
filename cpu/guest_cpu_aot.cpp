// SPDX-License-Identifier: GPL-2.0-or-later
// guest_cpu.h with ahead-of-time recompiled code (bb-recomp units, docs/RECOMP_ABI.md) and a fallback CPU.
//
// BB_AOT=<file.so> opts in. The .so holds one host function per guest function (a "unit") and a table of them. The
// game's x86-64 code then runs as those units; whatever has no unit (code the tool did not reach, the linked
// libraries, code written at run time, patched code) runs on the FALLBACK CPU on the same register state:
//   - x86-64 hosts: the host CPU, natively, on the guest stack (what src/runtime_cpu.c does without AOT);
//   - ARM64 hosts: FEXCore's JIT (cpu/guest_cpu_fex.cpp), whose thread state IS the units' GuestState.
// Without BB_AOT (or when the .so is refused) every guest_cpu_* call goes straight to the fallback: nothing changes.
//
// This file exports guest_cpu.h. The fallback's own functions are compiled under the names bb_fb_*
// (guest_cpu_rename.h) and are called from here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_cpu_fallback.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#include <x86intrin.h>
#endif

#define BBR_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
using namespace bbaot;
using UnitFn = void (*)(GuestState*);
struct UnitEntry {
    uint64_t vaddr;
    UnitFn fn;
};

constexpr uint64_t TrapUnsupported = 1ull << 63;
constexpr uint64_t TrapUnliftable = 1ull << 62;
constexpr size_t DefaultStack = 4u << 20;        // guest stack of a thread nobody gave one to (x86-64 hosts)
constexpr size_t NestedStack = 1u << 20;         // x86-64: a call into the game made by host code that native code called
constexpr size_t StackHeadroom = 256;
constexpr unsigned HostStackSlots = 10;          // guest stack slots copied for a host function: stack arguments

[[noreturn]] void Fatal(const char* message, uint64_t a = 0, uint64_t b = 0) {
    std::fprintf(stderr, "STOP: AOT: %s (%#llx, %#llx)\n", message, (unsigned long long)a, (unsigned long long)b);
    std::fflush(nullptr);
    _exit(21);
}

// ------------------------------------------------------------------------------------------------ state ----
struct Thread {
    GuestState* s = nullptr;
    uintptr_t stack_top = 0;        // given by guest_cpu_thread_stack, or our own allocation
    uintptr_t own_stack = 0;
    size_t own_stack_size = 0;
    unsigned depth = 0;             // guest calls made by the host on this thread, nested
    unsigned native = 0;            // x86-64: inside native fallback execution (host code runs on the guest stack)
    std::vector<uintptr_t> nested;  // x86-64: guest stacks for calls made inside native execution
    unsigned nested_used = 0;
    // BB_AOT_TRACE: the last events of this thread, for the crash report.
    struct Event { uint32_t kind; uint64_t a, b, c; };
    static constexpr unsigned TraceSize = 64;
    Event trace[TraceSize];
    unsigned trace_n = 0;
    // Statistics (only with BB_AOT_STATS).
    uint64_t unit_dispatch = 0, host_calls = 0, misses = 0, dirty_runs = 0, fb_cycles = 0;
    Thread* next_registered = nullptr;
};
thread_local Thread* self;
extern "C" {
#if defined(__x86_64__) && !defined(BB_AOT_FEX_STATE)
// Read by the asm below (local-exec TLS): the thread's state for the native return path, and the guest rsp of the host
// function being called (written by the host-function stubs and by call_host).
__thread GuestState* bbaot_state_ptr;
__thread uint64_t bbaot_guest_rsp;
__thread uint64_t bbaot_target;
#endif
}

struct Engine {
    bool on = false;                // BB_AOT given and accepted
    bool ready = false;             // image checked, dirty units marked
    std::once_flag once;
    void* so = nullptr;
    const UnitEntry* units = nullptr;
    uint64_t count = 0;
    const uint64_t* hashes = nullptr;
    const uint8_t* extra = nullptr;   // per unit: a switch case body (belongs to the preceding unit's function)
    uint64_t text_end = 0;
    uintptr_t base = 0;
    uint64_t size = 0;
    // vaddr -> unit: open addressing, key = vaddr + 1 (0: empty, ~0: removed).
    struct Slot { std::atomic<uint32_t> key; uint32_t index; };
    Slot* table = nullptr;
    uint32_t mask = 0, shift = 0;
    // units sorted by host address, for guest_cpu_signal_regs.
    struct Where { uintptr_t fn; uint32_t vaddr; };
    std::vector<Where> by_address;
    uintptr_t so_lo = ~uintptr_t(0), so_hi = 0;
    std::vector<uint8_t> dirty;     // per unit
    std::atomic<uint64_t> dirty_count {0}, dirty_patched {0}, dirty_unpatchable {0};
    std::mutex lock;
    // Host-function stubs (x86-64: ours; ARM64: FEX's).
    uintptr_t stub_base = 0;
    size_t stub_bytes = 0;
    size_t stub_stride = 16;
    void* const* stub_fns = nullptr;
    // Code the guest may run besides the image (guest_cpu_code ranges).
    std::vector<std::pair<uintptr_t, size_t>> ranges;
    bool stats = false;
    bool trace = false;
    void* (*low_map)(size_t, int) = nullptr;
    std::atomic<Thread*> threads {nullptr};
    uint64_t sentinel = 0;
} E;

#if defined(__x86_64__) && !defined(BB_AOT_FEX_STATE)
constexpr bool NativeFallback = true;
#else
constexpr bool NativeFallback = false;
#endif

uint64_t ReadGuest(uintptr_t address) {
    uint64_t value = 0;
    iovec local {&value, sizeof(value)}, remote {reinterpret_cast<void*>(address), sizeof(value)};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(value) ? value : 0;
}

enum : uint32_t { EvMiss = 1, EvFallbackEnd, EvHost, EvDirty, EvFree, EvUnwind };
inline void Trace(Thread* t, uint32_t kind, uint64_t a, uint64_t b = 0, uint64_t c = 0) {
    if (E.trace) t->trace[t->trace_n++ % Thread::TraceSize] = {kind, a, b, c};
}

// ------------------------------------------------------------------------------------- miss statistics ----
struct MissSlot { std::atomic<uint64_t> addr; std::atomic<uint64_t> count; std::atomic<uint64_t> dirty; };
constexpr size_t MissSlots = 1 << 16;
MissSlot miss_table[MissSlots];
std::atomic<uint64_t> miss_dropped {0};
std::atomic<uint64_t> mid_unit_jumps {0};
std::atomic<uint64_t> plt_hops {0};

void CountMiss(uint64_t address, bool dirty_unit) {
    const uint64_t key = address | 1ull << 63;  // address 0 stays distinguishable
    for (size_t i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 48; i < MissSlots; ++i) {
        MissSlot& m = miss_table[(slot + i) % MissSlots];
        uint64_t expected = 0;
        if (m.addr.load(std::memory_order_relaxed) == key || m.addr.compare_exchange_strong(expected, key)) {
            m.count.fetch_add(1, std::memory_order_relaxed);
            if (dirty_unit) m.dirty.store(1, std::memory_order_relaxed);
            return;
        }
    }
    miss_dropped.fetch_add(1, std::memory_order_relaxed);
}

void RegisterThread(Thread* t) {
    Thread* head = E.threads.load();
    do { t->next_registered = head; } while (!E.threads.compare_exchange_weak(head, t));
}

void Report(const char* why) {
    uint64_t unit = 0, host = 0, misses = 0, dirty_runs = 0, cycles = 0;
    for (Thread* t = E.threads.load(); t; t = t->next_registered) {
        unit += t->unit_dispatch; host += t->host_calls; misses += t->misses; dirty_runs += t->dirty_runs; cycles += t->fb_cycles;
    }
    std::fprintf(stderr, "AOT stats (%s): %llu PLT thunks followed; %llu mid-unit jumps run to the end of the stack on the fallback\n", why,
                 (unsigned long long)plt_hops.load(), (unsigned long long)mid_unit_jumps.load());
    std::fprintf(stderr, "AOT stats (%s): %llu units, %llu dirty (%llu entries patched, %llu not patchable)\n"
                 "  via the dispatcher (indirect calls, tail jumps; direct unit-to-unit calls are not counted): %llu unit entries, "
                 "%llu host function calls\n"
                 "  fallback runs: %llu misses (%llu of them dirty units), %.3f Gcycles on the fallback\n",
                 why, (unsigned long long)E.count, (unsigned long long)E.dirty_count.load(), (unsigned long long)E.dirty_patched.load(),
                 (unsigned long long)E.dirty_unpatchable.load(), (unsigned long long)unit, (unsigned long long)host,
                 (unsigned long long)misses, (unsigned long long)dirty_runs, double(cycles) / 1e9);
    struct Row { uint64_t addr, count, dirty; };
    std::vector<Row> rows;
    for (auto& m : miss_table) {
        const uint64_t a = m.addr.load(std::memory_order_relaxed);
        if (a) rows.push_back({a & ~(1ull << 63), m.count.load(std::memory_order_relaxed), m.dirty.load(std::memory_order_relaxed)});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
    std::fprintf(stderr, "  %zu distinct miss addresses%s; top (image offset, calls):\n", rows.size(),
                 miss_dropped.load() ? " (table full: some dropped)" : "");
    for (size_t i = 0; i < rows.size() && i < 25; ++i) {
        const uint64_t a = rows[i].addr;
        const bool inside = a - E.base < E.size;
        if (inside) std::fprintf(stderr, "    +%#llx  %llu%s\n", (unsigned long long)(a - E.base), (unsigned long long)rows[i].count, rows[i].dirty ? "  (patched unit)" : "");
        else std::fprintf(stderr, "    %#llx (outside the image)  %llu\n", (unsigned long long)a, (unsigned long long)rows[i].count);
    }
    std::fflush(stderr);
}

void StatsThread() {
    const char* sec = std::getenv("BB_AOT_STATS_SEC");
    const unsigned every = sec ? std::max(1, std::atoi(sec)) : 20;
    pthread_setname_np(pthread_self(), "bb:aot-stats");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(every));
        Report("periodic");
    }
}

// ---------------------------------------------------------------------------------------------- table ----
inline UnitFn Lookup(uint64_t offset) {
    const uint32_t key = uint32_t(offset) + 1;
    uint32_t h = (key * 0x9E3779B1u) >> E.shift;
    for (;;) {
        const uint32_t k = E.table[h].key.load(std::memory_order_relaxed);
        if (k == key) return E.units[E.table[h].index].fn;
        if (k == 0) return nullptr;
        h = (h + 1) & E.mask;
    }
}

bool BuildTable() {
    uint32_t bits = 4;
    while ((1ull << bits) < E.count * 2) ++bits;
    E.mask = (1u << bits) - 1;
    E.shift = 32 - bits;
    E.table = static_cast<Engine::Slot*>(std::calloc(size_t(1) << bits, sizeof(Engine::Slot)));
    if (!E.table) return false;
    for (uint64_t i = 0; i < E.count; ++i) {
        if (E.units[i].vaddr >= 0xFFFFFFFEull) return false;
        const uint32_t key = uint32_t(E.units[i].vaddr) + 1;
        uint32_t h = (key * 0x9E3779B1u) >> E.shift;
        while (E.table[h].key.load(std::memory_order_relaxed)) h = (h + 1) & E.mask;
        E.table[h].key.store(key, std::memory_order_relaxed);
        E.table[h].index = uint32_t(i);
    }
    E.by_address.reserve(E.count);
    for (uint64_t i = 0; i < E.count; ++i) {
        const auto fn = reinterpret_cast<uintptr_t>(E.units[i].fn);
        E.by_address.push_back({fn, uint32_t(E.units[i].vaddr)});
        E.so_lo = std::min(E.so_lo, fn);
        E.so_hi = std::max(E.so_hi, fn);
    }
    std::sort(E.by_address.begin(), E.by_address.end(), [](const Engine::Where& a, const Engine::Where& b) { return a.fn < b.fn; });
    E.so_hi += 1 << 20; // the last function's body (no unit is anywhere near a megabyte past the previous entry)
    E.dirty.assign(E.count, 0);
    return true;
}

// ------------------------------------------------------------------------------------ fallback CPU run ----
extern "C" void bbr_dirty_unit(GuestState* s, uint64_t vaddr);

#if defined(__x86_64__) && !defined(BB_AOT_FEX_STATE)
// Native execution on the host CPU. bbaot_native_enter loads the guest registers from the state, switches to the guest
// stack and jumps to the target. The guest returns to bbaot_native_ret_a (the return address we put on its stack, so the
// callee returns to us) or bbaot_native_ret_s (the sentinel at the bottom of the guest stack: it unwound past us, by a
// longjmp or an exception); both store the registers to the state, switch back to the host stack and return from
// bbaot_native_enter, with 0 or 1 in the state's R11 slot.
static_assert(offsetof(GuestState, InlineJITBlockHeader) == 0 && offsetof(GuestState, gregs) == 32 && offsetof(GuestState, xmm) == 448,
              "the asm below uses GuestState offsets");
__asm__(R"(
    .text
    .p2align 4
    .globl bbaot_native_enter
    .type bbaot_native_enter,@function
bbaot_native_enter:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, 0(%rdi)
    mov %rsi, %fs:bbaot_target@tpoff
    movups 448(%rdi), %xmm0
    movups 464(%rdi), %xmm1
    movups 480(%rdi), %xmm2
    movups 496(%rdi), %xmm3
    movups 512(%rdi), %xmm4
    movups 528(%rdi), %xmm5
    movups 544(%rdi), %xmm6
    movups 560(%rdi), %xmm7
    movups 576(%rdi), %xmm8
    movups 592(%rdi), %xmm9
    movups 608(%rdi), %xmm10
    movups 624(%rdi), %xmm11
    movups 640(%rdi), %xmm12
    movups 656(%rdi), %xmm13
    movups 672(%rdi), %xmm14
    movups 688(%rdi), %xmm15
    mov 32(%rdi), %rax
    mov 40(%rdi), %rcx
    mov 48(%rdi), %rdx
    mov 56(%rdi), %rbx
    mov 72(%rdi), %rbp
    mov 80(%rdi), %rsi
    mov 96(%rdi), %r8
    mov 104(%rdi), %r9
    mov 112(%rdi), %r10
    mov 120(%rdi), %r11
    mov 128(%rdi), %r12
    mov 136(%rdi), %r13
    mov 144(%rdi), %r14
    mov 152(%rdi), %r15
    mov 64(%rdi), %rsp
    mov 88(%rdi), %rdi
    cld
    jmp *%fs:bbaot_target@tpoff
    .size bbaot_native_enter, .-bbaot_native_enter

    .p2align 4
    .globl bbaot_native_ret_a
    .type bbaot_native_ret_a,@function
bbaot_native_ret_a:
    xor %r11d, %r11d
    jmp bbaot_native_ret_common
    .p2align 4
    .globl bbaot_native_ret_s
    .type bbaot_native_ret_s,@function
bbaot_native_ret_s:
    mov $1, %r11d
bbaot_native_ret_common:
    push %rax
    mov %fs:bbaot_state_ptr@tpoff, %rax
    mov %rcx, 40(%rax)
    mov %rdx, 48(%rax)
    mov %rbx, 56(%rax)
    mov %rbp, 72(%rax)
    mov %rsi, 80(%rax)
    mov %rdi, 88(%rax)
    mov %r8, 96(%rax)
    mov %r9, 104(%rax)
    mov %r10, 112(%rax)
    mov %r11, 120(%rax)
    mov %r12, 128(%rax)
    mov %r13, 136(%rax)
    mov %r14, 144(%rax)
    mov %r15, 152(%rax)
    pop %rcx
    mov %rcx, 32(%rax)
    mov %rsp, 64(%rax)
    movups %xmm0, 448(%rax)
    movups %xmm1, 464(%rax)
    movups %xmm2, 480(%rax)
    movups %xmm3, 496(%rax)
    movups %xmm4, 512(%rax)
    movups %xmm5, 528(%rax)
    movups %xmm6, 544(%rax)
    movups %xmm7, 560(%rax)
    movups %xmm8, 576(%rax)
    movups %xmm9, 592(%rax)
    movups %xmm10, 608(%rax)
    movups %xmm11, 624(%rax)
    movups %xmm12, 640(%rax)
    movups %xmm13, 656(%rax)
    movups %xmm14, 672(%rax)
    movups %xmm15, 688(%rax)
    mov 0(%rax), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size bbaot_native_ret_a, .-bbaot_native_ret_a

    .p2align 4
    .globl bbaot_host_entry
    .type bbaot_host_entry,@function
bbaot_host_entry:
    mov %rsp, %fs:bbaot_guest_rsp@tpoff
    jmp *%r11
    .size bbaot_host_entry, .-bbaot_host_entry

    // bbaot_host_call(fn, HostArgs*, HostResult*): a SysV call with the guest's argument registers, vector registers
    // and stack slots; the results rax, rdx, xmm0, xmm1 go to the result.
    .p2align 4
    .globl bbaot_host_call
    .type bbaot_host_call,@function
bbaot_host_call:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    mov %rdx, %rbx
    mov %rdi, %r11
    mov %rsi, %r12
    mov 48(%r12), %rcx
    mov 56(%r12), %rsi
    lea 15(,%rcx,8), %rax
    and $-16, %rax
    sub %rax, %rsp
    xor %rax, %rax
1:  cmp %rcx, %rax
    jae 2f
    mov (%rsi,%rax,8), %rdx
    mov %rdx, (%rsp,%rax,8)
    inc %rax
    jmp 1b
2:  movups 64(%r12), %xmm0
    movups 80(%r12), %xmm1
    movups 96(%r12), %xmm2
    movups 112(%r12), %xmm3
    movups 128(%r12), %xmm4
    movups 144(%r12), %xmm5
    movups 160(%r12), %xmm6
    movups 176(%r12), %xmm7
    mov 0(%r12), %rdi
    mov 8(%r12), %rsi
    mov 16(%r12), %rdx
    mov 24(%r12), %rcx
    mov 32(%r12), %r8
    mov 40(%r12), %r9
    mov $8, %eax
    call *%r11
    mov %rax, 0(%rbx)
    mov %rdx, 8(%rbx)
    movups %xmm0, 16(%rbx)
    movups %xmm1, 32(%rbx)
    lea -16(%rbp), %rsp
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size bbaot_host_call, .-bbaot_host_call
)");
extern "C" {
void bbaot_native_enter(GuestState* s, uint64_t target);
void bbaot_native_ret_a(void);
void bbaot_native_ret_s(void);
void bbaot_host_entry(void);
}
struct HostArgs {
    uint64_t x[6];
    uint64_t stack_count;
    const uint64_t* stack;
    __uint128_t v[8];
};
struct HostResult {
    uint64_t x[2];
    __uint128_t v[2];
};
extern "C" void bbaot_host_call(void* fn, const HostArgs* args, HostResult* result);
static_assert(offsetof(HostArgs, stack_count) == 48 && offsetof(HostArgs, stack) == 56 && offsetof(HostArgs, v) == 64);
static_assert(offsetof(HostResult, v) == 16);

uint64_t Sentinel() { return reinterpret_cast<uint64_t>(&bbaot_native_ret_s); }

// Runs the native code at `target` on this state. `call`: the guest stack's top is the return address of the call
// that reached target (a miss from a unit): it is replaced so the callee returns here, and rip ends as that address.
// Not `call` (after unwinding): the code runs to the end of the guest stack, and rip ends as the sentinel.
void FallbackRun(GuestState* s, uint64_t target, bool call, Thread* t) {
    uint64_t original = 0;
    if (call) {
        auto* slot = reinterpret_cast<uint64_t*>(s->gregs[RSP]);
        original = *slot;
        *slot = reinterpret_cast<uint64_t>(&bbaot_native_ret_a);
    }
    const uint64_t saved_sp = s->InlineJITBlockHeader;
    bbaot_state_ptr = s;
    ++t->native;
    const uint64_t t0 = E.stats ? __rdtsc() : 0;
    bbaot_native_enter(s, target);
    if (E.stats) t->fb_cycles += __rdtsc() - t0;
    --t->native;
    s->InlineJITBlockHeader = saved_sp;
    const bool returned_here = s->gregs[R11] == 0;
    s->gregs[R11] = 0;
    s->rip = (call && returned_here) ? original : Sentinel();
}
void CallHostFn(GuestState* s, void* fn) {
    HostArgs a;
    a.x[0] = s->gregs[RDI]; a.x[1] = s->gregs[RSI]; a.x[2] = s->gregs[RDX];
    a.x[3] = s->gregs[RCX]; a.x[4] = s->gregs[R8]; a.x[5] = s->gregs[R9];
    a.stack_count = HostStackSlots;
    a.stack = reinterpret_cast<const uint64_t*>(s->gregs[RSP] + 8);
    for (unsigned i = 0; i < 8; ++i) std::memcpy(&a.v[i], s->xmm.sse.data[i], 16);
    HostResult r;
    bbaot_host_call(fn, &a, &r);
    s->gregs[RAX] = r.x[0];
    s->gregs[RDX] = r.x[1];
    std::memcpy(s->xmm.sse.data[0], &r.v[0], 16);
    std::memcpy(s->xmm.sse.data[1], &r.v[1], 16);
}
inline void NoteGuestRsp(uint64_t rsp) { bbaot_guest_rsp = rsp; }
inline uint64_t GuestRspAtHostCall() { return bbaot_guest_rsp; }
#else // ARM64: FEX is the fallback
struct HostArgs {
    uint64_t x[8];
    uint64_t stack_count;
    const uint64_t* stack;
    __uint128_t v[8];
};
struct HostResult {
    uint64_t x[2];
    __uint128_t v[2];
};
extern "C" void bbcpu_host_call(void* fn, const HostArgs* args, HostResult* result);
static_assert(offsetof(HostArgs, stack_count) == 64 && offsetof(HostArgs, stack) == 72 && offsetof(HostArgs, v) == 80);
static_assert(offsetof(HostResult, v) == 16);
thread_local uint64_t guest_rsp_at_host_call;

uint64_t Sentinel() { return bb_fb_sentinel(); }
void FallbackRun(GuestState* s, uint64_t target, bool call, Thread* t) {
    (void)t;
    bb_fb_run(s, target, call);
}
void CallHostFn(GuestState* s, void* fn) {
    HostArgs a;
    a.x[0] = s->gregs[RDI]; a.x[1] = s->gregs[RSI]; a.x[2] = s->gregs[RDX];
    a.x[3] = s->gregs[RCX]; a.x[4] = s->gregs[R8]; a.x[5] = s->gregs[R9];
    const auto* stack = reinterpret_cast<const uint64_t*>(s->gregs[RSP] + 8);
    a.x[6] = stack[0];
    a.x[7] = stack[1];
    a.stack_count = HostStackSlots - 2;
    a.stack = stack + 2;
    for (unsigned i = 0; i < 8; ++i) std::memcpy(&a.v[i], s->xmm.sse.data[i], 16);
    HostResult r;
    bbcpu_host_call(fn, &a, &r);
    s->gregs[RAX] = r.x[0];
    s->gregs[RDX] = r.x[1];
    std::memcpy(s->xmm.sse.data[0], &r.v[0], 16);
    std::memcpy(s->xmm.sse.data[1], &r.v[1], 16);
}
inline void NoteGuestRsp(uint64_t rsp) { guest_rsp_at_host_call = rsp; }
inline uint64_t GuestRspAtHostCall() { return guest_rsp_at_host_call; }
#endif

// ---------------------------------------------------------------------------------------------- threads ----
Thread* Current() {
    if (self) return self;
    auto* t = new Thread;
#if defined(BB_AOT_FEX_STATE)
    t->s = bb_fb_state();
#else
    t->s = static_cast<GuestState*>(std::aligned_alloc(64, (sizeof(GuestState) + 63) & ~size_t(63)));
    if (!t->s) Fatal("cannot allocate a guest state");
    new (t->s) GuestState();
    ResetGuestState(*t->s);
    bbaot_state_ptr = t->s;
#endif
    if (E.stats) RegisterThread(t);
    self = t;
    return t;
}

// ------------------------------------------------------------------------------------------ dispatching ----
inline void HostCall(GuestState* s, void* fn, Thread* t) {
    if (E.stats) ++t->host_calls;
    const uint64_t sp = s->gregs[RSP];
    const uint64_t ret = *reinterpret_cast<const uint64_t*>(sp);
    Trace(t, EvHost, reinterpret_cast<uint64_t>(fn), sp, ret);
    const uint64_t saved = GuestRspAtHostCall();
    NoteGuestRsp(sp);
    CallHostFn(s, fn);
    NoteGuestRsp(saved);
    s->gregs[RSP] = sp + 8;
    s->rip = ret;
}


void* StubFunction(uint64_t target) {
    const uint64_t offset = target - E.stub_base;
    if (offset < E.stub_bytes && offset % E.stub_stride == 0 && E.stub_fns) return E.stub_fns[offset / E.stub_stride];
    return nullptr;
}

// A call or jump to an address that is not a unit entry: a host function (import), or code for the fallback CPU.
void Miss(GuestState* s, uint64_t target, Thread* t, bool dirty_unit) {
    Trace(t, dirty_unit ? EvDirty : EvMiss, target, s->gregs[RSP], *reinterpret_cast<uint64_t*>(s->gregs[RSP]));
    if (E.stats) {
        ++t->misses;
        if (dirty_unit) ++t->dirty_runs;
        CountMiss(target, dirty_unit);
    }
    FallbackRun(s, target, true, t);
    Trace(t, EvFallbackEnd, s->rip, s->gregs[RSP], target);
}
bool IsUnitEntry(uint64_t offset) {
    const UnitEntry* end = E.units + E.count;
    const UnitEntry* at = std::lower_bound(E.units, end, offset, [](const UnitEntry& u, uint64_t v) { return u.vaddr < v; });
    return at != end && at->vaddr == offset;
}

// A jump (not a call) into the middle of a unit, which no unit starts: the case of a switch the tool did not resolve whose
// target lies inside another case body. The code runs natively from there, but nothing says where its function's return
// address is on the guest stack (the frame is mid-function), so it cannot return to the unit: it runs to the end of the
// guest stack, which makes everything above it native too. Counted in the statistics (mid-unit jumps).
[[gnu::noinline]] void MidUnitJump(GuestState* s, uint64_t target, Thread* t) {
    mid_unit_jumps.fetch_add(1, std::memory_order_relaxed);
    if (E.stats) { ++t->misses; CountMiss(target, false); }
    Trace(t, EvFree, target, s->gregs[RSP]);
    FallbackRun(s, target, false, t);
}

[[gnu::noinline]] void Slow(GuestState* s, uint64_t target, bool tail) {
    Thread* t = Current();
    for (unsigned hops = 0;; ++hops) {
        if (void* fn = StubFunction(target)) {
            HostCall(s, fn, t);
            return;
        }
        const uint64_t offset = target - E.base;
        if (offset < E.size) {
            if (hops) {
                if (UnitFn unit = Lookup(offset)) {
                    s->rip = target;
                    unit(s);
                    return;
                }
            }
            // The import thunks of the game's PLT (`jmp *[rip + disp]`, outside every function): the slot holds the host
            // function's stub or an unresolved-import trap. Followed here, an import call costs no fallback run.
            const unsigned char* code = reinterpret_cast<const unsigned char*>(target);
            if (hops < 4 && code[0] == 0xff && code[1] == 0x25) {
                int32_t disp;
                std::memcpy(&disp, code + 2, 4);
                target = *reinterpret_cast<const uint64_t*>(target + 6 + disp);
                s->rip = target;
                plt_hops.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        break;
    }
    if (tail && target - E.base < E.text_end && !IsUnitEntry(target - E.base)) {
        MidUnitJump(s, target, t);
        return;
    }
    Miss(s, target, t, false);
}

// ---------------------------------------------------------------------------------------- dirty units ----
// Units whose code differs from the image the .so was built from (patches, hooks, code the runtime wrote) leave the
// table, and their entry is overwritten with a jump to bbr_dirty_unit, so that direct calls from other units reach the
// fallback too (they bypass the table). The entry patch needs the .so's text writable for a moment (mprotect); where
// that is refused (Android's W^X policy) only the table is changed and RECOMP_ABI.md asks for a flag the units check.
#if defined(__x86_64__)
constexpr size_t PatchBytes = 23;
void WritePatch(unsigned char* at, uint64_t vaddr) {
    const uint64_t handler = reinterpret_cast<uint64_t>(&bbr_dirty_unit);
    at[0] = 0x48; at[1] = 0xbe; std::memcpy(at + 2, &vaddr, 8);       // mov rsi, vaddr
    at[10] = 0x49; at[11] = 0xbb; std::memcpy(at + 12, &handler, 8);  // mov r11, handler
    at[20] = 0x41; at[21] = 0xff; at[22] = 0xe3;                      // jmp r11
}
#else
constexpr size_t PatchBytes = 24;
void WritePatch(unsigned char* at, uint64_t vaddr) {
    const uint64_t handler = reinterpret_cast<uint64_t>(&bbr_dirty_unit);
    const uint32_t code[4] = {0x52800001u | uint32_t((vaddr & 0xffff) << 5),          // movz w1, #lo
                              0x72a00001u | uint32_t(((vaddr >> 16) & 0xffff) << 5),  // movk w1, #hi, lsl #16
                              0x58000050u,                                            // ldr x16, #8 (the literal at +16)
                              0xd61f0200u};                                           // br x16
    std::memcpy(at, code, sizeof(code));
    std::memcpy(at + 16, &handler, 8);
}
#endif

void DirtyOne(uint64_t i);
// A function's units go together: the case bodies are entered by jumps from the function's own code, with no return address
// on top of the guest stack, so they cannot run on the fallback alone.
void DirtyUnit(uint64_t i) {  // E.lock held
    uint64_t first = i;
    while (first > 0 && E.extra[first]) --first;
    DirtyOne(first);
    for (uint64_t j = first + 1; j < E.count && E.extra[j]; ++j) DirtyOne(j);
}
void DirtyOne(uint64_t i) {
    if (E.dirty[i]) return;
    E.dirty[i] = 1;
    E.dirty_count.fetch_add(1);
    const uint32_t key = uint32_t(E.units[i].vaddr) + 1;
    for (uint32_t h = (key * 0x9E3779B1u) >> E.shift;; h = (h + 1) & E.mask) {
        const uint32_t k = E.table[h].key.load(std::memory_order_relaxed);
        if (k == key) { E.table[h].key.store(0xFFFFFFFFu, std::memory_order_release); break; }
        if (k == 0) break;
    }
    const uintptr_t fn = reinterpret_cast<uintptr_t>(E.units[i].fn);
    auto at = std::lower_bound(E.by_address.begin(), E.by_address.end(), fn, [](const Engine::Where& w, uintptr_t v) { return w.fn < v; });
    const uintptr_t next = (at + 1 != E.by_address.end()) ? (at + 1)->fn : fn + 4096;
    if (next - fn < PatchBytes) { E.dirty_unpatchable.fetch_add(1); return; }
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    const uintptr_t lo = fn & ~(page - 1), hi = (fn + PatchBytes + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        E.dirty_unpatchable.fetch_add(1);
        return;
    }
    WritePatch(reinterpret_cast<unsigned char*>(fn), E.units[i].vaddr);
    __builtin___clear_cache(reinterpret_cast<char*>(fn), reinterpret_cast<char*>(fn + PatchBytes));
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);
    E.dirty_patched.fetch_add(1);
}

void MarkDirty(uintptr_t address, size_t size) {
    if (!E.on || !E.base || !size || address + size <= E.base || address >= E.base + E.size) return;
    const uint64_t lo = std::max<uintptr_t>(address, E.base) - E.base;
    const uint64_t hi = std::min<uintptr_t>(address + size, E.base + E.size) - E.base;
    std::lock_guard<std::mutex> guard(E.lock);
    uint64_t first = std::upper_bound(E.units, E.units + E.count, lo, [](uint64_t v, const UnitEntry& u) { return v < u.vaddr; }) - E.units;
    if (first) --first;
    for (uint64_t i = first; i < E.count && E.units[i].vaddr < hi; ++i) {
        const uint64_t end = i + 1 < E.count ? E.units[i + 1].vaddr : E.text_end;
        if (end > lo) DirtyUnit(i);
    }
}

uint64_t Fnv(const unsigned char* p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; ++i) {
        unsigned char b = p[i];
        if (b == 0x65) b = 0x64;  // bb-prepare turns fs: into gs: in the image; the tool hashes both the same
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

// Once, before the first guest call (every patch and hook of the start-up is in the image by then): compare each unit's
// bytes with what the .so was built from.
void PrepareImage() {
    if (!E.base) { E.on = false; std::fprintf(stderr, "AOT: no guest image was registered: the fallback CPU only\n"); return; }
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t bad = 0;
    std::vector<uint64_t> bad_units;
    for (uint64_t i = 0; i < E.count; ++i) {
        const uint64_t lo = E.units[i].vaddr, hi = i + 1 < E.count ? E.units[i + 1].vaddr : E.text_end;
        if (hi > E.size || lo >= hi) { ++bad; continue; }
        if (Fnv(reinterpret_cast<const unsigned char*>(E.base + lo), hi - lo) != E.hashes[i]) { ++bad; bad_units.push_back(i); }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (bad * 4 > E.count) {
        E.on = false;
        std::fprintf(stderr, "AOT: %llu of %llu units differ from the image: this .so was built from another image; the fallback CPU only\n",
                     (unsigned long long)bad, (unsigned long long)E.count);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(E.lock);
        for (uint64_t i : bad_units) DirtyUnit(i);
    }
    std::printf("AOT: image checked in %.0f ms: %llu units; %llu differ (patched or hooked), %llu units with their functions' other units "
                "run on the fallback CPU\n", ms, (unsigned long long)E.count, (unsigned long long)bad, (unsigned long long)E.dirty_count.load());
    if (std::getenv("BB_AOT_DEBUG"))
        for (uint64_t i : bad_units) std::printf("AOT:   patched unit +%#llx\n", (unsigned long long)E.units[i].vaddr);
    E.ready = true;
}

// ------------------------------------------------------------------------------------------------ loading ----
uint64_t FnvFile(const char* path, bool* ok) {
    FILE* f = std::fopen(path, "rb");
    *ok = f != nullptr;
    uint64_t h = 1469598103934665603ull;
    if (!f) return 0;
    static unsigned char buffer[1 << 20];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
        for (size_t i = 0; i < n; ++i) { h ^= buffer[i]; h *= 1099511628211ull; }
    std::fclose(f);
    return h;
}

void Load(const char* path) {
    void* so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!so) { std::fprintf(stderr, "AOT: cannot load %s: %s\n", path, dlerror()); return; }
    const auto* abi = static_cast<const uint32_t*>(dlsym(so, "bbr_abi_version"));
    const auto* count = static_cast<const uint64_t*>(dlsym(so, "bbr_unit_count"));
    const auto* units = static_cast<const UnitEntry*>(dlsym(so, "bbr_units"));
    const auto* hashes = static_cast<const uint64_t*>(dlsym(so, "bbr_unit_hash"));
    const auto* extra = static_cast<const uint8_t*>(dlsym(so, "bbr_unit_extra"));
    const auto* text_end = static_cast<const uint64_t*>(dlsym(so, "bbr_text_end"));
    auto* image_base = static_cast<uint64_t*>(dlsym(so, "bbr_image_base"));
    const auto* image_hash = static_cast<const uint64_t*>(dlsym(so, "bbr_image_hash"));
    if (!abi || !count || !units || !image_base || !hashes || !extra || !text_end || !image_hash) {
        std::fprintf(stderr, "AOT: %s is not a bb-recomp unit library (missing bbr_* symbols; unit hashes need a newer tool)\n", path);
        return;
    }
    if (*abi != 1) { std::fprintf(stderr, "AOT: %s has ABI version %u, this runtime runs 1\n", path, *abi); return; }
    if (const char* elf = std::getenv("BB_AOT_ELF")) {  // optional: the whole-file check the tool's bbr_image_hash allows
        bool ok;
        const uint64_t h = FnvFile(elf, &ok);
        if (!ok || h != *image_hash) {
            std::fprintf(stderr, "AOT: %s was not built from %s (image hash %#llx, file %#llx)\n", path, elf, (unsigned long long)*image_hash, (unsigned long long)h);
            return;
        }
    }
#ifdef BB_AOT_FEX_STATE
    if (bb_fb_wide_xmm()) { std::fprintf(stderr, "AOT: this host keeps SVE256 vector registers; the units use the 128-bit layout\n"); return; }
#endif
    E.so = so;
    E.units = units;
    E.count = *count;
    E.hashes = hashes;
    E.extra = extra;
    E.text_end = *text_end;
    E.stats = std::getenv("BB_AOT_STATS") != nullptr;
    E.trace = std::getenv("BB_AOT_TRACE") != nullptr;
    if (!BuildTable()) { std::fprintf(stderr, "AOT: cannot build the dispatch table\n"); return; }
    E.so_lo &= ~uintptr_t(0xfff);
    E.on = true;
    std::printf("AOT: %s: %llu units\n", path, (unsigned long long)E.count);
}

void OpenImage(uintptr_t base, size_t size) {
    E.base = base;
    E.size = std::min<size_t>(size, 0xFFFFFFF0u);
    *static_cast<uint64_t*>(dlsym(E.so, "bbr_image_base")) = base;
}
}  // namespace

// ============================================================================================ bbr_* helpers ====
BBR_EXPORT void bbr_dispatch(GuestState* s, uint64_t target) {
    const uint64_t offset = target - E.base;
    if (__builtin_expect(offset < E.size, 1)) {
        if (UnitFn fn = Lookup(offset)) {
            if (E.stats) ++Current()->unit_dispatch;
            fn(s);
            return;
        }
    }
    Slow(s, target, false);
}
BBR_EXPORT void bbr_dispatch_tail(GuestState* s) {
    const uint64_t target = s->rip;
    const uint64_t offset = target - E.base;
    if (__builtin_expect(offset < E.size, 1)) {
        if (UnitFn fn = Lookup(offset)) {
            if (E.stats) ++Current()->unit_dispatch;
            fn(s);
            return;
        }
    }
    Slow(s, target, true);
}
extern "C" void bbr_dirty_unit(GuestState* s, uint64_t vaddr) {
    Miss(s, E.base + vaddr, Current(), true);
}
BBR_EXPORT void bbr_trap(GuestState* s, uint64_t kind) {
    if (kind & TrapUnliftable) {
        // A function the tool could not lift: the fallback runs it (entered by a call: the return address is on top).
        Miss(s, E.base + (kind & 0xFFFFFFFFull), Current(), true);
        return;
    }
    if (kind & TrapUnsupported)
        Fatal("a unit contains an IR op the tool does not lower (FEX IR op index, rip offset)", kind & ~TrapUnsupported, s->rip - E.base);
    Fatal("guest trap (signal | trap number << 8 | si_code << 16 | error << 32, rip offset)", kind, s->rip - E.base);
}
BBR_EXPORT void bbr_syscall(GuestState* s) {
    Fatal("the game executed a syscall instruction (rip offset, rax)", s->rip - E.base, s->gregs[RAX]);
}
struct CpuidResult { uint64_t lo, hi; };
BBR_EXPORT CpuidResult bbr_cpuid(GuestState*, uint64_t leaf, uint64_t subleaf) {
    uint32_t r[4];
#if defined(BB_AOT_FEX_STATE)
    bb_fb_cpuid(leaf, subleaf, r);   // FEX's own CPUID model: the values the game sees today
#else
    __cpuid_count(uint32_t(leaf), uint32_t(subleaf), r[0], r[1], r[2], r[3]);  // as the native PC build runs it
#endif
    return {uint64_t(r[0]) | uint64_t(r[1]) << 32, uint64_t(r[2]) | uint64_t(r[3]) << 32};
}
BBR_EXPORT uint64_t bbr_rdtsc(void) {
    return bb_fb_tsc();
}
BBR_EXPORT uint64_t bbr_xgetbv(uint64_t function) {
#if defined(BB_AOT_FEX_STATE)
    return bb_fb_xgetbv(function);
#else
    uint32_t eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(uint32_t(function)));
    return uint64_t(eax) | uint64_t(edx) << 32;
#endif
}
BBR_EXPORT uint64_t bbr_processor_id(void) {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) cpu = node = 0;
    return uint64_t(node) << 12 | cpu;
}
#if defined(__x86_64__)
// FEX's round mode value: x86 MXCSR.RC in bits 1:0, flush-to-zero in bit 2.
BBR_EXPORT void bbr_set_rounding(uint64_t mode, uint64_t mxcsr, uint64_t set_daz) {
    uint32_t m = _mm_getcsr();
    m = (m & ~(3u << 13)) | uint32_t(mode & 3) << 13;
    m = (m & ~(1u << 15)) | uint32_t((mode >> 2) & 1) << 15;
    if (set_daz) m = (m & ~(1u << 6)) | (uint32_t(mxcsr) & (1u << 6));
    _mm_setcsr(m);
}
BBR_EXPORT uint64_t bbr_get_rounding(void) {
    const uint32_t m = _mm_getcsr();
    return ((m >> 13) & 3) | uint64_t((m >> 15) & 1) << 2;
}
BBR_EXPORT uint64_t bbr_push_rounding(uint64_t mode) {
    const uint32_t old = _mm_getcsr();
    _mm_setcsr((old & ~(3u << 13)) | uint32_t(mode & 3) << 13);
    return old;
}
BBR_EXPORT void bbr_pop_rounding(uint64_t saved) {
    _mm_setcsr(uint32_t(saved));
}
#else
namespace {
uint64_t ReadFpcr() { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
void WriteFpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
// x86 rounding mode order is nearest, down, up, zero; FPCR.RMode has nearest, up, down, zero.
uint64_t SwapRc(uint64_t rc) { return rc == 1 ? 2 : rc == 2 ? 1 : rc; }
}  // namespace
BBR_EXPORT void bbr_set_rounding(uint64_t mode, uint64_t mxcsr, uint64_t set_daz) {
    (void)mxcsr; (void)set_daz;  // FPCR.FIZ (denormals are zero) needs FEAT_AFP, which the Thor lacks, as FEX's own JIT
    uint64_t v = ReadFpcr();
    v = (v & ~(3ull << 22)) | SwapRc(mode & 3) << 22;
    v = (v & ~(1ull << 24)) | ((mode >> 2) & 1) << 24;
    WriteFpcr(v);
}
BBR_EXPORT uint64_t bbr_get_rounding(void) {
    const uint64_t v = ReadFpcr();
    return SwapRc((v >> 22) & 3) | ((v >> 24) & 1) << 2;
}
BBR_EXPORT uint64_t bbr_push_rounding(uint64_t mode) {
    const uint64_t old = ReadFpcr();
    WriteFpcr((old & ~(3ull << 22)) | SwapRc(mode & 3) << 22);
    return old;
}
BBR_EXPORT void bbr_pop_rounding(uint64_t saved) {
    WriteFpcr(saved);
}
#endif

// ============================================================================================ guest_cpu.h ====
namespace {
// Host functions get a stub on x86-64 hosts when AOT is on (they are plain function pointers without it).
#if defined(__x86_64__) && !defined(BB_AOT_FEX_STATE)
constexpr size_t StubBytesTotal = 1 << 20;
constexpr size_t StubSize = 32;
void* x86_stub_fns[StubBytesTotal / StubSize];
size_t x86_stub_count = 1;  // slot 0 unused
std::mutex stub_lock;
std::vector<std::pair<void*, uintptr_t>> x86_stub_of;

void InitStubs(void* (*map)(size_t, int)) {
    auto* stubs = static_cast<unsigned char*>(map(StubBytesTotal, PROT_READ | PROT_WRITE | PROT_EXEC));
    if (!stubs) Fatal("cannot allocate host function stubs");
    std::memset(stubs, 0xcc, StubBytesTotal);
    E.stub_base = reinterpret_cast<uintptr_t>(stubs);
    E.stub_bytes = StubBytesTotal;
    E.stub_stride = StubSize;
    E.stub_fns = x86_stub_fns;
    E.ranges.emplace_back(E.stub_base, StubBytesTotal);
}
uintptr_t MakeStub(void* fn) {
    std::lock_guard<std::mutex> guard(stub_lock);
    for (auto& [f, a] : x86_stub_of) if (f == fn) return a;
    if (x86_stub_count >= StubBytesTotal / StubSize) Fatal("too many host functions");
    unsigned char* stub = reinterpret_cast<unsigned char*>(E.stub_base) + x86_stub_count * StubSize;
    const uint64_t target = reinterpret_cast<uint64_t>(fn), entry = reinterpret_cast<uint64_t>(&bbaot_host_entry);
    stub[0] = 0x49; stub[1] = 0xbb; std::memcpy(stub + 2, &target, 8);   // mov r11, fn
    stub[10] = 0x49; stub[11] = 0xba; std::memcpy(stub + 12, &entry, 8); // mov r10, bbaot_host_entry
    stub[20] = 0x41; stub[21] = 0xff; stub[22] = 0xe2;                   // jmp r10
    x86_stub_fns[x86_stub_count] = fn;
    const uintptr_t address = reinterpret_cast<uintptr_t>(stub);
    x86_stub_of.emplace_back(fn, address);
    ++x86_stub_count;
    return address;
}
#endif

uintptr_t AllocateStack(size_t size) {
    if (!E.low_map) Fatal("guest stacks before guest_cpu_init");
    void* p = E.low_map(size, PROT_READ | PROT_WRITE);
    if (!p) Fatal("cannot allocate a guest stack");
    return reinterpret_cast<uintptr_t>(p) + size;
}

bool InCode(uintptr_t pc) {
    for (const auto& [base, size] : E.ranges) if (pc - base < size) return true;
    return false;
}
}  // namespace

extern "C" {
void guest_cpu_init(void* (*map)(size_t size, int prot)) {
    bb_fb_init(map);
    E.low_map = map;
    const char* path = std::getenv("BB_AOT");
    if (!path || !*path) return;
    Load(path);
    if (!E.on) return;
#if defined(BB_AOT_FEX_STATE)
    E.stub_fns = bb_fb_stubs(&E.stub_base, &E.stub_bytes);
    E.stub_stride = 16;
#elif defined(__x86_64__)
    InitStubs(map);
#endif
    if (E.stats) {
        std::atexit([] { Report("exit"); });
        std::thread(StatsThread).detach();
    }
}

void guest_cpu_code(uintptr_t base, size_t size) {
    bb_fb_code(base, size);
    if (!E.on) return;
    if (E.ranges.size() < 64) E.ranges.emplace_back(base, size);
    if (!E.base) OpenImage(base, size);  // the first range is the game's image (src/probe.c)
}

uintptr_t guest_cpu_host_function(void* fn) {
#if defined(__x86_64__) && !defined(BB_AOT_FEX_STATE)
    if (E.on) return MakeStub(fn);
#endif
    return bb_fb_host_function(fn);
}

uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t* args) {
    static constexpr unsigned arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    if (!E.on) return bb_fb_call(fn, count, args);
    if (!E.ready) {
        static std::once_flag once;
        std::call_once(once, PrepareImage);
        if (!E.on) return bb_fb_call(fn, count, args);
    }
    Thread* t = Current();
    GuestState& s = *t->s;
    uint64_t saved[16];
    std::memcpy(saved, s.gregs, sizeof(saved));
    const uint64_t saved_rip = s.rip;
    const uint64_t saved_host_rsp = GuestRspAtHostCall();
    uintptr_t sp;
    bool nested_stack = false;
    if (NativeFallback && t->native) {
        // Host code that native guest code called, calling back into the game: this thread's stack is the guest stack
        // the native code runs on, and the units' own frames would meet the guest's pushes there, so the call gets a
        // guest stack of its own.
        if (t->nested_used == t->nested.size()) t->nested.push_back(AllocateStack(NestedStack));
        sp = t->nested[t->nested_used++] - StackHeadroom;
        nested_stack = true;
    } else if (t->depth) {
        sp = s.gregs[RSP] - 128;  // below the red zone of the guest code that called the host
    } else {
        if (!t->stack_top) {
            t->stack_top = t->own_stack = AllocateStack(DefaultStack);
            t->own_stack_size = DefaultStack;
        }
        sp = t->stack_top - StackHeadroom;
    }
    const unsigned stack_args = count > 6 ? count - 6 : 0;
    sp = (sp - stack_args * 8) & ~uintptr_t(15);
    for (unsigned i = 0; i < stack_args; ++i) reinterpret_cast<uint64_t*>(sp)[i] = args[6 + i];
    const uint64_t sentinel = Sentinel();
    s.gregs[RSP] = sp - 8;  // as after a call: the return address at the top, the stack arguments above it
    *reinterpret_cast<uint64_t*>(sp - 8) = sentinel;
    for (unsigned i = 0; i < count && i < 6; ++i) s.gregs[arg_regs[i]] = args[i];
    s.gregs[RAX] = 0;
    ++t->depth;
    s.rip = fn;
    bbr_dispatch(&s, fn);
    while (s.rip != sentinel) {
        // The guest returned somewhere else than the caller expected (longjmp, an exception, a return address used as a
        // jump): continue from there, on a unit if it starts one, else on the fallback to the end of the stack.
        const uint64_t rip = s.rip;
        const uint64_t offset = rip - E.base;
        Trace(t, EvUnwind, rip, s.gregs[RSP]);
        UnitFn unit = offset < E.size ? Lookup(offset) : nullptr;
        if (unit) unit(&s);
        else if (void* host = StubFunction(rip)) HostCall(&s, host, t);
        else {
            if (E.stats) { ++t->misses; CountMiss(rip, false); }
            Trace(t, EvFree, rip, s.gregs[RSP]);
            FallbackRun(&s, rip, false, t);
        }
    }
    --t->depth;
    if (nested_stack) --t->nested_used;
    NoteGuestRsp(saved_host_rsp);
    const uint64_t result = s.gregs[RAX];
    std::memcpy(s.gregs, saved, sizeof(saved));
    s.rip = saved_rip;
    return result;
}

void guest_cpu_thread_stack(void* top, size_t size) {
    bb_fb_thread_stack(top, size);
    if (E.on) Current()->stack_top = reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15);
}

void guest_cpu_set_gs(void* base) {
    bb_fb_set_gs(base);  // on x86-64 the host's GS (the fallback runs the guest natively), on ARM64 FEX's state
    if (E.on) {
        GuestState& s = *Current()->s;
        s.gs_cached = reinterpret_cast<uint64_t>(base);
        s.fs_cached = reinterpret_cast<uint64_t>(base);
    }
}

void guest_cpu_abandon(void) {
    bb_fb_abandon();
    if (self) {
        self->depth = 0;
        self->native = 0;
        self->nested_used = 0;
    }
}

void guest_cpu_thread_end(void) {
    Thread* t = self;
    if (E.on && t) {
        self = nullptr;
        if (NativeFallback) {
            std::free(t->s);
            if (t->own_stack) munmap(reinterpret_cast<void*>(t->own_stack - t->own_stack_size), t->own_stack_size);
            for (uintptr_t top : t->nested) munmap(reinterpret_cast<void*>(top - NestedStack), NestedStack);
            t->s = nullptr;
        }
        if (!E.stats) delete t;  // with statistics the thread stays on the list
    }
    bb_fb_thread_end();
}

uintptr_t guest_cpu_return_address(uintptr_t native) {
    if (!E.on) return bb_fb_return_address(native);
    if (!self || !self->depth) return NativeFallback ? native : 0;
    return ReadGuest(self->native || NativeFallback ? GuestRspAtHostCall() : self->s->gregs[RSP]);
}

uintptr_t guest_cpu_stack_pointer(void) {
    if (!E.on) return bb_fb_stack_pointer();
    if (self && self->depth) return NativeFallback ? GuestRspAtHostCall() : self->s->gregs[RSP];
    return NativeFallback ? reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) : 0;
}

int guest_cpu_signal_regs(const void* ucontext, GuestRegs* regs) {
    if (!E.on) return bb_fb_signal_regs(ucontext, regs);
    const uintptr_t pc = bb_fb_host_pc(ucontext);
    Thread* t = self;
    if (t && pc >= E.so_lo && pc < E.so_hi) {
        // Inside a unit: the registers are those the unit last stored (stale, but valid), rip the unit's entry.
        std::memcpy(regs->gpr, t->s->gregs, sizeof(regs->gpr));
        auto at = std::upper_bound(E.by_address.begin(), E.by_address.end(), pc, [](uintptr_t v, const Engine::Where& w) { return v < w.fn; });
        regs->rip = at == E.by_address.begin() ? 0 : E.base + (at - 1)->vaddr;
        return GUEST_CPU_IN_GUEST;
    }
    if (NativeFallback && InCode(pc)) return bb_fb_signal_regs(ucontext, regs);  // native guest code: the registers are the signal's
    if (!NativeFallback) {
        const int fex = bb_fb_signal_regs(ucontext, regs);  // FEX's own code buffer
        if (fex == GUEST_CPU_IN_GUEST) return fex;
    }
    if (t && t->depth) {
        std::memcpy(regs->gpr, t->s->gregs, sizeof(regs->gpr));
        const uint64_t rsp = NativeFallback ? GuestRspAtHostCall() : t->s->gregs[RSP];
        regs->gpr[GUEST_RSP] = rsp;
        regs->rip = ReadGuest(rsp);
        return GUEST_CPU_IN_HOST_CALL;
    }
    return NativeFallback ? bb_fb_signal_regs(ucontext, regs) : 0;
}

// Called by the crash report (src/probe.c, weak): the faulting thread's last transitions between units, fallback and host.
__attribute__((visibility("default"))) void bbaot_crash_report(const void* ucontext) {
    if (!E.on || !self) return;
    Thread* t = self;
    const uintptr_t pc = bb_fb_host_pc(ucontext);
    std::fprintf(stderr, "AOT: fault at host pc %p (%s), guest rsp %#llx, rip %#llx, depth %u, native %u\n", reinterpret_cast<void*>(pc),
                 pc >= E.so_lo && pc < E.so_hi ? "in a unit" : InCode(pc) ? "in native guest code" : "elsewhere",
                 (unsigned long long)t->s->gregs[RSP], (unsigned long long)t->s->rip, t->depth, t->native);
    if (pc >= E.so_lo && pc < E.so_hi) {
        auto at = std::upper_bound(E.by_address.begin(), E.by_address.end(), pc, [](uintptr_t v, const Engine::Where& w) { return v < w.fn; });
        if (at != E.by_address.begin()) std::fprintf(stderr, "AOT: unit +%#x, offset %#llx in it\n", (at - 1)->vaddr, (unsigned long long)(pc - (at - 1)->fn));
    }
    if (E.trace) {
        static const char* names[] = {"?", "miss", "fallback returned", "host call", "dirty unit", "free run", "unwind"};
        const unsigned n = t->trace_n, first = n > Thread::TraceSize ? n - Thread::TraceSize : 0;
        for (unsigned i = first; i < n; ++i) {
            const auto& e = t->trace[i % Thread::TraceSize];
            std::fprintf(stderr, "AOT:   %-18s a=%#llx b=%#llx c=%#llx (image offsets: %#llx %#llx)\n", names[e.kind], (unsigned long long)e.a, (unsigned long long)e.b,
                         (unsigned long long)e.c, (unsigned long long)(e.a - E.base), (unsigned long long)(e.c - E.base));
        }
    }
    std::fflush(stderr);
}

uintptr_t guest_cpu_host_pc(const void* ucontext) { return bb_fb_host_pc(ucontext); }
int guest_cpu_handle_fault(int sig, siginfo_t* info, void* ucontext) { return bb_fb_handle_fault(sig, info, ucontext); }

int guest_cpu_is_native(void) { return NativeFallback && !E.on; }

int guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook) {
    MarkDirty(address, size);
    return bb_fb_hook(address, size, hook);
}
void guest_cpu_invalidate(uintptr_t address, size_t size) {
    MarkDirty(address, size);
    bb_fb_invalidate(address, size);
}
uint64_t guest_cpu_tsc(void) { return bb_fb_tsc(); }
uint64_t guest_cpu_tsc_frequency(void) { return bb_fb_tsc_frequency(); }
}  // extern "C"
