// SPDX-License-Identifier: GPL-2.0-or-later
// guest_cpu.h on ahead-of-time recompiled code (bb-recomp units, docs/RECOMP_ABI.md): the Android runtime's only guest CPU.
//
// BB_AOT=<libbbaot.so> names the unit library, which holds one host function per guest function (a "unit") and a table of them. The game's
// x86-64 code runs as those units and nothing else: there is no interpreter, JIT or other fallback CPU, so a guest address no unit covers (a gap
// in the library, code the game wrote at run time, a patched function without a variant) is a bug of the library, not a slow path. It is written
// to the misses log (BB_AOT_MISS_LOG, default logs/aot-misses.log, the format bb-recomp --extra-entries reads) and the game stops with a
// "STOP: AOT: ..." message ("strict mode", docs/RECOMP_ABI.md).
//
// cpu/guest_cpu_native.cpp provides the platform services (host-function stubs, CPUID, the TSC, the unaligned-atomic handler). On an x86-64
// host this file compiles for the unit tests only (tests/test_aot_*.cpp provide those services).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_state.h"
#include "guest_cpu_atomics.h"
#include "guest_cpu_native.h"
#include "guest_cpu.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

constexpr uint32_t BbrAbiVersion = 2;            // bbr_abi_version of the library (docs/RECOMP_ABI.md); the app's NativeCpu.ABI_VERSION and the tool's kAbiVersion say the same
constexpr uint64_t TrapUnsupported = 1ull << 63;
constexpr uint64_t TrapUnliftable = 1ull << 62;
constexpr size_t DefaultStack = 4u << 20;        // guest stack of a thread nobody gave one to
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
    // BB_AOT_TRACE: the last events of this thread, for the crash report.
    struct Event { uint32_t kind; uint64_t a, b, c; };
    static constexpr unsigned TraceSize = 64;
    Event trace[TraceSize];
    unsigned trace_n = 0;
    // Statistics (only with BB_AOT_STATS).
    uint64_t unit_dispatch = 0, host_calls = 0;
    uint64_t host_ticks = 0;        // time in host functions called from units (Ticks(): CNTVCT)
    uint64_t prev_host = 0, prev_units = 0;  // the previous report's values
    unsigned host_depth = 0;
    unsigned unwound_depth = 0;     // bbr_unwound runs nested on this thread
    pid_t tid = 0;
    Thread* next_registered = nullptr;
};
thread_local Thread* self;

struct Engine {
    bool on = false;                // the unit library is loaded and accepted
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
    std::vector<UnitFn> fns;        // per unit: what the dispatch table runs (patch variants replace entries; guest_cpu_aot_hooks.inc)
    std::atomic<uint64_t> dirty_count {0}, dirty_unchecked {0};
    uint8_t* dirty_flags = nullptr;       // the .so's bbr_dirty[] (units check it at entry), or null
    const uint64_t* ends = nullptr;       // bbr_unit_end[] or null
    const uint64_t* ehashes = nullptr;    // bbr_unit_ehash[] or null
    uint64_t trap_units = 0;              // units the tool started dirty (unsupported IR ops)
    std::mutex lock;
    // Host-function stubs (cpu/guest_cpu_native.cpp): addresses the guest calls to reach a host function; nothing ever executes them.
    uintptr_t stub_base = 0;
    size_t stub_bytes = 0;
    size_t stub_stride = 16;
    void* const* stub_fns = nullptr;
    // Code the guest may run besides the image (guest_cpu_code ranges).
    std::vector<std::pair<uintptr_t, size_t>> ranges;
    bool stats = false;
    bool trace = false;
    const char* miss_log = "logs/aot-misses.log";   // BB_AOT_MISS_LOG overrides
    void* (*low_map)(size_t, int) = nullptr;
    size_t (*panic_text)(char*, size_t) = nullptr;   // guest_cpu_set_panic_text
    std::atomic<Thread*> threads {nullptr};
    uint64_t sentinel = 0;
} E;

uint64_t ReadGuest(uintptr_t address) {
    uint64_t value = 0;
    iovec local {&value, sizeof(value)}, remote {reinterpret_cast<void*>(address), sizeof(value)};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(value) ? value : 0;
}

enum : uint32_t { EvMiss = 1, EvHost, EvUnwind };
const char* const EventNames[] = {"?", "miss", "host call", "unwind"};
inline void Trace(Thread* t, uint32_t kind, uint64_t a, uint64_t b = 0, uint64_t c = 0) {
    if (E.trace) t->trace[t->trace_n++ % Thread::TraceSize] = {kind, a, b, c};
}

inline uint64_t Ticks() {
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return __rdtsc();
#endif
}
inline double TicksPerSecond() {
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return double(f);
#else
    return double(bbnative::TscFrequency());
#endif
}

// ----------------------------------------------------------------------------------------------- statistics ----
std::atomic<uint64_t> plt_hops {0};
std::atomic<uint64_t> unaligned_atomics {0};
std::atomic<uint64_t> unwound_runs {0};

void RegisterThread(Thread* t) {
    Thread* head = E.threads.load();
    do { t->next_registered = head; } while (!E.threads.compare_exchange_weak(head, t));
}

// BB_AOT_STATS=1: every BB_AOT_STATS_SEC seconds (default 20) and at exit, the engine's counters and, per thread over the window since the last
// report, the share of time spent in host functions called by units (the rest is units, or waiting inside host functions, counted as host).
void Report(const char* why) {
    uint64_t unit = 0, host = 0;
    for (Thread* t = E.threads.load(); t; t = t->next_registered) { unit += t->unit_dispatch; host += t->host_calls; }
    std::fprintf(stderr, "AOT stats (%s): %llu units, %llu dirty (%llu without an entry check: direct calls still run them)\n"
                 "  via the dispatcher (indirect calls, tail jumps; direct unit-to-unit calls are not counted): %llu unit entries, %llu host function calls\n"
                 "  %llu PLT thunks followed, %llu unaligned atomics emulated, %llu unwound returns resumed on units\n", why,
                 (unsigned long long)E.count, (unsigned long long)E.dirty_count.load(), (unsigned long long)E.dirty_unchecked.load(),
                 (unsigned long long)unit, (unsigned long long)host, (unsigned long long)plt_hops.load(),
                 (unsigned long long)unaligned_atomics.load(), (unsigned long long)unwound_runs.load());
    static uint64_t last = 0;
    const uint64_t now = Ticks();
    const double window = last ? double(now - last) : 0;
    last = now;
    if (window > 0) {
        std::fprintf(stderr, "  per thread over %.1f s (host = host functions called by units):\n", window / TicksPerSecond());
        for (Thread* t = E.threads.load(); t; t = t->next_registered) {
            const uint64_t dhost = t->host_ticks - t->prev_host, dunits = t->unit_dispatch - t->prev_units;
            t->prev_host = t->host_ticks; t->prev_units = t->unit_dispatch;
            if (!dhost && !dunits) continue;
            char name[32] = "?", path[64];
            std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(t->tid));
            if (FILE* f = std::fopen(path, "r")) {
                if (std::fgets(name, sizeof(name), f)) name[std::strcspn(name, "\n")] = 0;
                std::fclose(f);
            }
            std::fprintf(stderr, "    %-15s host %5.1f%%, %llu unit entries\n", name, 100.0 * double(dhost) / window, (unsigned long long)dunits);
        }
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
        if (k == key) return E.fns[E.table[h].index];
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
    E.fns.resize(E.count);
    for (uint64_t i = 0; i < E.count; ++i) E.fns[i] = E.units[i].fn;
    return true;
}

// ----------------------------------------------------------------------------------------- host functions ----
// A guest call of a host function (an address of the stub region) runs the host function with the guest's argument registers and stack slots, on
// the host stack (cpu/host_call_arm64.S). AArch64 only: the x86-64 build of this file is for the unit tests, which call no host functions.
#if defined(__aarch64__)
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
#else
[[noreturn]] void CallHostFn(GuestState*, void*) { Fatal("host function calls need an AArch64 host (this x86-64 build is for the unit tests)"); }
#endif
thread_local uint64_t guest_rsp_at_host_call;
inline void NoteGuestRsp(uint64_t rsp) { guest_rsp_at_host_call = rsp; }
inline uint64_t GuestRspAtHostCall() { return guest_rsp_at_host_call; }
inline uint64_t Sentinel() { return bbnative::Sentinel(); }

// ---------------------------------------------------------------------------------------------- threads ----
Thread* Current() {
    if (self) return self;
    auto* t = new Thread;
    t->s = static_cast<GuestState*>(std::aligned_alloc(64, (sizeof(GuestState) + 63) & ~size_t(63)));
    if (!t->s) Fatal("cannot allocate a guest state");
    new (t->s) GuestState();
    ResetGuestState(*t->s);
    t->tid = pid_t(syscall(SYS_gettid));
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
    const uint64_t h0 = E.stats && !t->host_depth ? Ticks() : 0;
    ++t->host_depth;
    CallHostFn(s, fn);
    --t->host_depth;
    if (h0) t->host_ticks += Ticks() - h0;
    NoteGuestRsp(saved);
    s->gregs[RSP] = sp + 8;
    s->rip = ret;
}


void* StubFunction(uint64_t target) {
    const uint64_t offset = target - E.stub_base;
    if (offset < E.stub_bytes && offset % E.stub_stride == 0 && E.stub_fns) return E.stub_fns[offset / E.stub_stride];
    return nullptr;
}

// -------------------------------------------------------------------------- granular entries, misses, strict mode ----
// Granular entries (docs/RECOMP_ABI.md): the units of a function marked in bbr_func_granular[] can be entered at any instruction boundary they
// hold: a unit called with the resume flag set (GuestState::_pad1 = 1) and rip at the wanted address continues there and clears the flag, or
// returns at once with the flag still set when the address is not one of its boundaries. The dispatcher asks the function's units one by one.
struct GranRec { uint32_t start, end, first, count; };   // bbr_funcs[] (the same record guest_cpu_aot_hooks.inc calls FuncRec)
struct Gran {
    const GranRec* funcs = nullptr;
    uint64_t nfuncs = 0;
    const uint8_t* flags = nullptr;    // bbr_func_granular[]
    std::atomic<uint64_t> entries {0}, rejected {0};
} G;

bool GranularEntry(GuestState* s, uint64_t target, Thread* t) {
    if (!G.flags) return false;
    const uint64_t offset = target - E.base;
    if (offset >= E.size) return false;
    const GranRec* end = G.funcs + G.nfuncs;
    const GranRec* f = std::upper_bound(G.funcs, end, offset, [](uint64_t v, const GranRec& r) { return v < r.start; });
    if (f == G.funcs) return false;
    --f;
    if (offset >= f->end || !G.flags[f - G.funcs]) return false;
    for (uint32_t u = f->first; u < f->first + f->count; ++u) {
        UnitFn fn = __atomic_load_n(&E.fns[u], __ATOMIC_ACQUIRE);
        s->rip = target;
        s->_pad1 = 1;
        fn(s);
        if (s->_pad1 == 0) {                      // the unit took it and ran the code from there
            if (E.stats) { ++t->unit_dispatch; G.entries.fetch_add(1, std::memory_order_relaxed); }
            return true;
        }
    }
    s->_pad1 = 0;
    G.rejected.fetch_add(1, std::memory_order_relaxed);
    return false;
}

enum class MissKind { Call, Jump, Unwind, Patched, Outside };
const char* const MissKindNames[] = {"call", "jump", "unwind", "patched", "outside"};

struct MissLog {
    std::mutex lock;
    int fd = -1;
    bool tried = false;
} ML;

void MissLogWrite(const char* text, size_t n) {   // ML.lock held
    if (!ML.tried) {
        ML.tried = true;
        ML.fd = open(E.miss_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (ML.fd < 0) std::fprintf(stderr, "AOT: cannot open the misses log %s: %s\n", E.miss_log, std::strerror(errno));
        else {
            char head[256];
            const int m = std::snprintf(head, sizeof head, "# bb aot misses (image offsets; feed this file to bb-recomp --extra-entries), pid %d, image base %#llx\n", int(getpid()),
                                        (unsigned long long)E.base);
            if (write(ML.fd, head, size_t(m)) < 0) {}
        }
    }
    if (ML.fd >= 0 && write(ML.fd, text, n) < 0) {}
}

// The guest return addresses on the stack: [rsp], then the rbp chain (the game's code keeps frame pointers).
std::string GuestBacktrace(const GuestState* s) {
    std::string out;
    char b[32];
    auto add = [&](uint64_t a) {
        if (a - E.base >= E.size) return false;
        std::snprintf(b, sizeof b, "%s+%#llx", out.empty() ? "" : ",", (unsigned long long)(a - E.base));
        out += b;
        return true;
    };
    add(ReadGuest(s->gregs[RSP]));
    uint64_t rbp = s->gregs[RBP];
    for (unsigned i = 0; i < 24 && rbp && !(rbp & 7); ++i) {
        const uint64_t ret = ReadGuest(rbp + 8), next = ReadGuest(rbp);
        if (!ret || !add(ret)) break;
        if (next <= rbp) break;
        rbp = next;
    }
    return out;
}

// The game's deliberate crash after a panic: Dantelion2's DL_PANIC prints its message and then stores to address 0 (`movl $0xdeadba, 0x0`), right after the
// call of the print routine. `p` points at a store of a register or an immediate to the absolute address 0: [66] [REX] 88/89/C6/C7 /r with the SIB form of
// an absolute disp32 address (modrm mod 00 rm 100, SIB 0x25) and disp32 = 0.
bool StoresToAddressZero(const uint8_t* p) {
    if (*p == 0x66) ++p;
    if ((*p & 0xf0) == 0x40) ++p;
    const uint8_t op = *p++;
    if (op != 0x88 && op != 0x89 && op != 0xc6 && op != 0xc7) return false;
    uint32_t disp;
    std::memcpy(&disp, p + 2, 4);
    return (p[0] & 0xc7) == 0x04 && p[1] == 0x25 && disp == 0;
}
// `target` is the instruction after a call (a return address: an unwind, or a call-return address of a direct call) and a store to address 0: the game
// stopped itself. A native library built before the tool kept an access through a null pointer as such (LLVM treats it as undefined behaviour and deleted
// the call-return block before it), so a normal return from the print routine arrived here as an "unwind" to an address nothing could resume.
bool IsPanicTrap(uint64_t target, MissKind kind) {
    if (kind == MissKind::Patched || kind == MissKind::Outside || target - E.base >= E.size || target - E.base < 8 || target - E.base + 16 > E.size) return false;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(target);
    return StoresToAddressZero(code) && (kind == MissKind::Unwind || code[-5] == 0xe8);
}

// A guest address no unit and no granular entry covers (or a patched function with no variant): the game stops here. The miss is written to the
// misses log with the thread and a backtrace of guest return addresses, so that testers' reports find the gaps (docs/RECOMP_ABI.md, "Strict mode";
// bb-recomp --extra-entries turns the log into entries), and the STOP message says what to do. When the address is the game's own panic trap (above)
// it is no gap: the message says that the game stopped itself and quotes the panic text the runtime captured (guest_cpu_set_panic_text), and the log
// line says `panic` (not `miss`: --extra-entries must not turn it into an entry).
[[gnu::noinline, noreturn]] void NoteMiss(GuestState* s, uint64_t target, Thread* t, MissKind kind) {
    const bool inside = target - E.base < E.size;
    const bool panic = IsPanicTrap(target, kind);
    char name[32] = "?";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    char text[1024];
    text[0] = 0;
    if (panic && E.panic_text) E.panic_text(text, sizeof text);
    Trace(t, EvMiss, target, s->gregs[RSP], 0);
    {
        std::lock_guard<std::mutex> g(ML.lock);
        char line[2048];
        const std::string bt = GuestBacktrace(s);
        int m;
        if (panic)
            m = std::snprintf(line, sizeof line, "panic +%#llx kind=%s thread=%s tid=%d rsp=%#llx backtrace=%s text=\"%.1000s\"\n", (unsigned long long)(target - E.base), MissKindNames[int(kind)], name,
                              int(syscall(SYS_gettid)), (unsigned long long)s->gregs[RSP], bt.c_str(), text);
        else if (inside)
            m = std::snprintf(line, sizeof line, "miss +%#llx kind=%s thread=%s tid=%d rsp=%#llx backtrace=%s\n", (unsigned long long)(target - E.base), MissKindNames[int(kind)], name,
                              int(syscall(SYS_gettid)), (unsigned long long)s->gregs[RSP], bt.c_str());
        else
            m = std::snprintf(line, sizeof line, "outside %#llx kind=%s thread=%s tid=%d rsp=%#llx backtrace=%s\n", (unsigned long long)target, MissKindNames[int(kind)], name,
                              int(syscall(SYS_gettid)), (unsigned long long)s->gregs[RSP], bt.c_str());
        MissLogWrite(line, size_t(std::min<int>(m, int(sizeof line) - 1)));
    }
    if (panic) {
        if (text[0])
            std::fprintf(stderr, "STOP: the game stopped itself after an internal error (panic) at +%#llx: %s (no native code is missing; details: %s)\n",
                         (unsigned long long)(target - E.base), text, E.miss_log);
        else
            std::fprintf(stderr, "STOP: the game stopped itself after an internal error (panic) at +%#llx; its own message is above in the log (no native code is missing; details: %s)\n",
                         (unsigned long long)(target - E.base), E.miss_log);
    } else if (inside && kind != MissKind::Patched)
        std::fprintf(stderr, "STOP: AOT: native code is missing an entry at +%#llx; please report this (details: %s)\n", (unsigned long long)(target - E.base), E.miss_log);
    else if (inside)
        std::fprintf(stderr, "STOP: AOT: native code for a patched or hooked function has no variant at +%#llx; please report this (details: %s)\n",
                     (unsigned long long)(target - E.base), E.miss_log);
    else
        std::fprintf(stderr, "STOP: AOT: guest code ran at %#llx, outside the image, which native code cannot run; please report this (details: %s)\n",
                     (unsigned long long)target, E.miss_log);
    std::fflush(nullptr);
    _exit(22);
}

// The game's import traps for imports nothing resolves (src/probe.c): `mov rsi,rdi; mov edi,<index>; movabs rax,<handler stub>; jmp rax`, 32 bytes each,
// in a code range of their own. Recognised here instead of run: the handler (the host function `unresolved`) then gets (index, rdi) as it would.
uint64_t TrapHandler(GuestState* s, uint64_t target) {
    if (E.ranges.size() < 2) return 0;
    for (size_t i = 1; i < E.ranges.size(); ++i) {
        const auto [base, size] = E.ranges[i];
        if (target - base >= size || (target - base) % 32 || target - E.stub_base < E.stub_bytes) continue;
        const unsigned char* t = reinterpret_cast<const unsigned char*>(target);
        if (t[0] != 0x48 || t[1] != 0x89 || t[2] != 0xfe || t[3] != 0xbf || t[8] != 0x48 || t[9] != 0xb8 || t[18] != 0xff || t[19] != 0xe0) return 0;
        uint32_t index;
        uint64_t handler;
        std::memcpy(&index, t + 4, 4);
        std::memcpy(&handler, t + 10, 8);
        s->gregs[RSI] = s->gregs[RDI];
        s->gregs[RDI] = index;
        return handler;
    }
    return 0;
}

// A call or jump to an address that is not a unit entry, a host function (import) or an import thunk. A patched unit (dirty_unit) is a
// function whose bytes changed and for which the library has no variant.
[[noreturn]] void Miss(GuestState* s, uint64_t target, Thread* t, bool dirty_unit) {
    NoteMiss(s, target, t, dirty_unit ? MissKind::Patched : (target - E.base < E.size ? MissKind::Call : MissKind::Outside));
}
bool IsUnitEntry(uint64_t offset) {
    const UnitEntry* end = E.units + E.count;
    const UnitEntry* at = std::lower_bound(E.units, end, offset, [](const UnitEntry& u, uint64_t v) { return u.vaddr < v; });
    return at != end && at->vaddr == offset;
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
            // function's stub or an unresolved-import trap.
            const unsigned char* code = reinterpret_cast<const unsigned char*>(target);
            if (hops < 4 && code[0] == 0xff && code[1] == 0x25) {
                int32_t disp;
                std::memcpy(&disp, code + 2, 4);
                target = *reinterpret_cast<const uint64_t*>(target + 6 + disp);
                s->rip = target;
                plt_hops.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        } else if (hops < 8) {
            // An import trap of src/probe.c (an import nothing resolves): its handler is a host function stub.
            if (const uint64_t handler = TrapHandler(s, target)) {
                target = handler;
                s->rip = target;
                continue;
            }
        }
        break;
    }
    // An instruction boundary inside a function with granular entries (a computed jump the lifter could not resolve, a jump into the middle of a function).
    if (GranularEntry(s, target, t)) return;
    if (tail && target - E.base < E.text_end && !IsUnitEntry(target - E.base)) {
        // A jump (not a call) into the middle of a unit, which no unit starts: the case of a switch the tool did not resolve whose target lies
        // inside another case body.
        NoteMiss(s, target, t, MissKind::Jump);
    }
    Miss(s, target, t, false);
}

// ---------------------------------------------------------------------------------------- dirty units ----
// Units whose code differs from the image the .so was built from (patches, hooks, code the runtime wrote) must not run.
// Every unit tests its byte of the .so's writable bbr_dirty[] first and, when set, tail-calls bbr_dirty_entry: direct
// unit-to-unit calls and jumps reach it too, and the .so's text is never written (Android's SELinux refuses
// execmod on app files). bbr_dirty_entry runs the function's patch variant when the .so has one (guest_cpu_aot_hooks.inc) and
// is a miss (the game stops) when it has none. The tool also starts units it could not lower completely (an unsupported IR op)
// with the byte set. A .so without the check (bbr_abi_flags bit 1 clear, an older tool) only loses the dirty units from the
// dispatch table: direct calls from other units then still run the stale code.
void DirtyOne(uint64_t i);
// A function's units go together: the case bodies are entered by jumps from the function's own code, so the whole function is out.
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
    if (E.dirty_flags) {
        // The unit stays in the table: its own check sends every entry, through the table or a direct call, to bbr_dirty_entry.
        __atomic_store_n(&E.dirty_flags[i], uint8_t(1), __ATOMIC_RELEASE);
        return;
    }
    const uint32_t key = uint32_t(E.units[i].vaddr) + 1;
    for (uint32_t h = (key * 0x9E3779B1u) >> E.shift;; h = (h + 1) & E.mask) {
        const uint32_t k = E.table[h].key.load(std::memory_order_relaxed);
        if (k == key) { E.table[h].key.store(0xFFFFFFFFu, std::memory_order_release); break; }
        if (k == 0) break;
    }
    E.dirty_unchecked.fetch_add(1);
}

uint64_t UnitEnd(uint64_t i) {
    if (E.ends && E.ends[i] > E.units[i].vaddr) return E.ends[i];
    return i + 1 < E.count ? E.units[i + 1].vaddr : E.text_end;
}

#include "guest_cpu_aot_hooks.inc"  // patch variants and native hook slots (docs/RECOMP_ABI.md)

void MarkDirty(uintptr_t address, size_t size) {
    if (!E.on || !E.base || !size || address + size <= E.base || address >= E.base + E.size) return;
    const uint64_t lo = std::max<uintptr_t>(address, E.base) - E.base;
    const uint64_t hi = std::min<uintptr_t>(address + size, E.base + E.size) - E.base;
    std::lock_guard<std::mutex> guard(E.lock);
    uint64_t first = std::upper_bound(E.units, E.units + E.count, lo, [](uint64_t v, const UnitEntry& u) { return v < u.vaddr; }) - E.units;
    if (first) --first;
    // Units overlap (secondary entries repeat the code after them up to their function's end): units that start before lo
    // may still cover it.
    for (uint64_t back = 0; first > 0 && back < 4096; ++back) --first;
    NewResolveEpoch();
    for (uint64_t i = first; i < E.count && E.units[i].vaddr < hi; ++i)
        if (UnitEnd(i) > lo) ResolveUnit(i);  // (was DirtyUnit: the function is classified again from its bytes: original, variant or dirty)
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
    if (!E.base) Fatal("no guest image was registered; native code cannot run");
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t bad = 0;
    std::vector<uint64_t> bad_units;
    for (uint64_t i = 0; i < E.count; ++i) {
        // bbr_unit_ehash: the unit's exact bytes [vaddr, bbr_unit_end) (no false positives from a patched neighbour); older
        // tools: [vaddr, next unit).
        const uint64_t lo = E.units[i].vaddr, hi = E.ehashes ? UnitEnd(i) : (i + 1 < E.count ? E.units[i + 1].vaddr : E.text_end);
        if (hi > E.size || lo >= hi) { ++bad; bad_units.push_back(i); continue; }
        if (Fnv(reinterpret_cast<const unsigned char*>(E.base + lo), hi - lo) != (E.ehashes ? E.ehashes[i] : E.hashes[i])) { ++bad; bad_units.push_back(i); }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (bad * 4 > E.count) {
        std::fprintf(stderr, "AOT: %llu of %llu units differ from the image: this library was built from another build of the game\n",
                     (unsigned long long)bad, (unsigned long long)E.count);
        Fatal("the unit library was built from another build of the game; native code cannot run (rebuild it with the Bloodborne Android Rebuild Tool from the game folder this app uses)", bad, E.count);
    }
    {
        std::lock_guard<std::mutex> guard(E.lock);
        ResolveUnits(bad_units);
    }
    std::printf("AOT: image checked in %.0f ms: %llu units; %llu differ (patched or hooked), %llu units with their functions' other units "
                "have no native code\n", ms, (unsigned long long)E.count, (unsigned long long)bad, (unsigned long long)E.dirty_count.load());
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
    const auto* abi_flags = static_cast<const uint64_t*>(dlsym(so, "bbr_abi_flags"));
    if (!abi || !count || !units || !image_base || !hashes || !extra || !text_end || !image_hash) {
        std::fprintf(stderr, "AOT: %s is not a bb-recomp unit library (missing bbr_* symbols; unit hashes need a newer tool)\n", path);
        return;
    }
    if (*abi != BbrAbiVersion) { std::fprintf(stderr, "AOT: %s has ABI version %u, this runtime runs %u (build it again with the matching Rebuild Tool)\n", path, *abi, BbrAbiVersion); return; }
    if (const char* elf = std::getenv("BB_AOT_ELF")) {  // optional: the whole-file check the tool's bbr_image_hash allows
        bool ok;
        const uint64_t h = FnvFile(elf, &ok);
        if (!ok || h != *image_hash) {
            std::fprintf(stderr, "AOT: %s was not built from %s (image hash %#llx, file %#llx)\n", path, elf, (unsigned long long)*image_hash, (unsigned long long)h);
            return;
        }
    }
    E.so = so;
    E.units = units;
    E.count = *count;
    E.hashes = hashes;
    E.extra = extra;
    E.text_end = *text_end;
    E.ends = static_cast<const uint64_t*>(dlsym(so, "bbr_unit_end"));
    E.ehashes = E.ends ? static_cast<const uint64_t*>(dlsym(so, "bbr_unit_ehash")) : nullptr;
    if (abi_flags && (*abi_flags & 2)) E.dirty_flags = static_cast<uint8_t*>(dlsym(so, "bbr_dirty"));
    if (abi_flags && (*abi_flags & 1))
        std::fprintf(stderr, "AOT: %s has resume entries (bbr_abi_flags bit 0); this runtime does not use them\n", path);
    if (abi_flags && (*abi_flags & 16)) {   // granular entries: bbr_func_granular[] says which functions of bbr_funcs[] have them
        G.funcs = static_cast<const GranRec*>(dlsym(so, "bbr_funcs"));
        const auto* nf = static_cast<const uint64_t*>(dlsym(so, "bbr_func_count"));
        G.flags = static_cast<const uint8_t*>(dlsym(so, "bbr_func_granular"));
        G.nfuncs = nf ? *nf : 0;
        if (!G.funcs || !G.flags || !G.nfuncs) { G.flags = nullptr; std::fprintf(stderr, "AOT: %s: granular entries declared but their tables are missing\n", path); }
    }
    E.stats = std::getenv("BB_AOT_STATS") != nullptr;
    E.trace = std::getenv("BB_AOT_TRACE") != nullptr;
    if (!BuildTable()) { std::fprintf(stderr, "AOT: cannot build the dispatch table\n"); return; }
    if (E.dirty_flags)  // units the tool could not lower completely start dirty
        for (uint64_t i = 0; i < E.count; ++i)
            if (E.dirty_flags[i]) { E.dirty[i] = 1; ++E.trap_units; }
    LoadVariantTables(so, abi_flags ? *abi_flags : 0);
    E.so_lo &= ~uintptr_t(0xfff);
    E.on = true;
    std::printf("AOT: %s: %llu units; unit entry check %s, exact unit ranges %s; %llu units with unsupported ops have no native code\n", path,
                (unsigned long long)E.count, E.dirty_flags ? "on" : "off (old tool: dirty units leave the table only)", E.ehashes ? "yes" : "no",
                (unsigned long long)E.trap_units);
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
// A unit's callee came back with rip other than its return address (longjmp, an exception's landing pad, a context switch).
// The unit's state is all stored. When rip starts a unit, it runs from this host frame: the code stays on units, and when it
// returns, the rip check of a caller further up (whose host frame is still there) matches. Otherwise the unit returns and
// its callers unwind as before. Nesting is capped so repeated jumps cannot grow the host stack without end.
BBR_EXPORT void bbr_unwound(GuestState* s) {
    const uint64_t offset = s->rip - E.base;
    if (offset >= E.size) return;
    Thread* t = Current();
    if (t->unwound_depth >= 16) return;
    const UnitFn fn = Lookup(offset);
    if (!fn) {
        // (a granular function's instruction boundary: run from here, nested like the unit case)
        ++t->unwound_depth;
        const bool ran = GranularEntry(s, s->rip, t);
        --t->unwound_depth;
        if (ran) unwound_runs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    unwound_runs.fetch_add(1, std::memory_order_relaxed);
    ++t->unwound_depth;
    fn(s);
    --t->unwound_depth;
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
// A dirty unit's entry check (bbr_dirty[i] set): the patch variant of the function when the library has one, else the game stops.
BBR_EXPORT void bbr_dirty_entry(GuestState* s, uint64_t vaddr) {
    if (UnitFn variant = VariantRedirect(vaddr)) { variant(s); return; }  // a patch variant of this unit is active (guest_cpu_aot_hooks.inc)
    s->rip = E.base + vaddr;
    NoteMiss(s, E.base + vaddr, Current(), MissKind::Patched);
}
BBR_EXPORT void bbr_trap(GuestState* s, uint64_t kind) {
    if (kind & TrapUnliftable)  // a function the tool could not lift
        NoteMiss(s, E.base + (kind & 0xFFFFFFFFull), Current(), MissKind::Patched);
    if (kind & TrapUnsupported)  // the tool starts such units dirty (bbr_dirty[i] = 1): only an old tool's .so gets here
        Fatal("a unit contains an IR op the tool does not lower (IR op index, rip offset); rebuild the .so with a newer bb-recomp",
              kind & ~TrapUnsupported, s->rip - E.base);
    Fatal("guest trap (signal | trap number << 8 | si_code << 16 | error << 32, rip offset)", kind, s->rip - E.base);
}
BBR_EXPORT void bbr_syscall(GuestState* s) {
    Fatal("the game executed a syscall instruction (rip offset, rax)", s->rip - E.base, s->gregs[RAX]);
}
struct CpuidResult { uint64_t lo, hi; };
BBR_EXPORT CpuidResult bbr_cpuid(GuestState*, uint64_t leaf, uint64_t subleaf) {
    uint32_t r[4];
    bbnative::Cpuid(leaf, subleaf, r);   // the CPUID model of cpu/guest_cpu_cpuid.h with the host features fixed
    return {uint64_t(r[0]) | uint64_t(r[1]) << 32, uint64_t(r[2]) | uint64_t(r[3]) << 32};
}
BBR_EXPORT uint64_t bbr_rdtsc(void) { return bbnative::Tsc(); }
BBR_EXPORT uint64_t bbr_xgetbv(uint64_t function) { return bbnative::Xgetbv(function); }
BBR_EXPORT uint64_t bbr_processor_id(void) {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) cpu = node = 0;
    return uint64_t(node) << 12 | cpu;
}
#if defined(__x86_64__)
// The lowering's round mode value (the x86-64 front end's convention): x86 MXCSR.RC in bits 1:0, flush-to-zero in bit 2.
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
    (void)mxcsr; (void)set_daz;  // FPCR.FIZ (denormals are zero) needs FEAT_AFP, which the Thor lacks
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

// Hooks without a native slot are not available: the game's code is never rewritten (no trampolines). Logged, a few times.
void RefuseHook(uintptr_t address) {
    static std::atomic<unsigned> told {0};
    if (told.fetch_add(1) < 16)
        std::fprintf(stderr, "AOT: no native hook slot at +%#llx: hook refused (the native build only runs hooks the library was built with a slot for)\n",
                     (unsigned long long)(address - E.base));
}
}  // namespace

extern "C" {
void guest_cpu_init(void* (*map)(size_t size, int prot)) {
    E.low_map = map;
    const char* path = std::getenv("BB_AOT");
    if (!path || !*path || std::strcmp(path, "0") == 0)
        Fatal("BB_AOT=<libbbaot.so> is required: this runtime runs the game as native code only (build the library on a PC with the Bloodborne Android Rebuild Tool)");
    if (const char* ml = std::getenv("BB_AOT_MISS_LOG")) E.miss_log = ml;
    bbnative::InitStubs(map);
    Load(path);
    if (!E.on) Fatal("the unit library could not be loaded (see the lines above)");
    std::printf("AOT: native code only: a miss is logged to %s and stops the game\n", E.miss_log);
    // BB_AOT_CPUID_LOG=1: the CPUID leaves and XGETBV(0) the game can read (cpu/guest_cpu_cpuid.h).
    if (const char* cl = std::getenv("BB_AOT_CPUID_LOG"); cl && cl[0] == '1') {
        static const uint32_t leaves[][2] = {{0, 0}, {1, 0}, {2, 0}, {4, 0}, {4, 1}, {4, 2}, {4, 3}, {6, 0}, {7, 0}, {7, 1}, {0xd, 0}, {0xd, 1}, {0xd, 2}, {0x15, 0},
                                             {0x1a, 0}, {0x40000000u, 0}, {0x40000001u, 0}, {0x40000001u, 1}, {0x40000001u, 2}, {0x80000000u, 0}, {0x80000001u, 0},
                                             {0x80000002u, 0}, {0x80000003u, 0}, {0x80000004u, 0}, {0x80000005u, 0}, {0x80000006u, 0}, {0x80000007u, 0},
                                             {0x80000008u, 0}, {0x80000019u, 0}, {0x8000001du, 0}};
        for (const auto& l : leaves) {
            uint32_t nat[4];
            bbnative::Cpuid(l[0], l[1], nat);
            std::printf("AOT cpuid %08x.%u: %08x %08x %08x %08x\n", l[0], l[1], nat[0], nat[1], nat[2], nat[3]);
        }
        std::printf("AOT xgetbv(0): %#llx\n", (unsigned long long)bbnative::Xgetbv(0));
    }
    E.stub_fns = bbnative::Stubs(&E.stub_base, &E.stub_bytes);
    E.stub_stride = 16;
    if (E.stats) {
        std::atexit([] { Report("exit"); });
        std::thread(StatsThread).detach();
    }
}

void guest_cpu_set_panic_text(size_t (*get)(char* out, size_t size)) { E.panic_text = get; }

void guest_cpu_code(uintptr_t base, size_t size) {
    if (!E.on) return;
    if (E.ranges.size() < 64) E.ranges.emplace_back(base, size);
    if (!E.base) OpenImage(base, size);  // the first range is the game's image (src/probe.c)
}

uintptr_t guest_cpu_host_function(void* fn) { return bbnative::HostFunction(fn); }

uint64_t guest_cpu_call_floats(uintptr_t fn, unsigned count, const uint64_t* args, unsigned nfloats, const float* floats);
uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t* args) { return guest_cpu_call_floats(fn, count, args, 0, nullptr); }
uint64_t guest_cpu_call_floats(uintptr_t fn, unsigned count, const uint64_t* args, unsigned nfloats, const float* floats) {
    static constexpr unsigned arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    if (!E.ready) {
        static std::once_flag once;
        std::call_once(once, PrepareImage);
    }
    Thread* t = Current();
    GuestState& s = *t->s;
    uint64_t saved[16];
    std::memcpy(saved, s.gregs, sizeof(saved));
    const uint64_t saved_rip = s.rip;
    const uint64_t saved_host_rsp = GuestRspAtHostCall();
    uintptr_t sp;
    if (t->depth) {
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
    s.gregs[RAX] = nfloats;   // (the number of vector registers used, as the SysV ABI has it for a variadic callee)
    for (unsigned i = 0; i < nfloats && i < 8; ++i) {
        std::memset(s.xmm.sse.data[i], 0, 16);
        std::memcpy(s.xmm.sse.data[i], &floats[i], sizeof(float));
        std::memset(s.avx_high[i], 0, 16);
    }
    ++t->depth;
    s.rip = fn;
    bbr_dispatch(&s, fn);
    while (s.rip != sentinel) {
        // The guest returned somewhere else than the caller expected (longjmp, an exception, a return address used as a
        // jump): continue from there when a unit starts there, else the game stops.
        const uint64_t rip = s.rip;
        const uint64_t offset = rip - E.base;
        Trace(t, EvUnwind, rip, s.gregs[RSP]);
        UnitFn unit = offset < E.size ? Lookup(offset) : nullptr;
        if (unit) unit(&s);
        else if (void* host = StubFunction(rip)) HostCall(&s, host, t);
        else if (GranularEntry(&s, rip, t)) {}
        else NoteMiss(&s, rip, t, MissKind::Unwind);
    }
    --t->depth;
    NoteGuestRsp(saved_host_rsp);
    const uint64_t result = s.gregs[RAX];
    std::memcpy(s.gregs, saved, sizeof(saved));
    s.rip = saved_rip;
    return result;
}

void guest_cpu_thread_stack(void* top, size_t) {
    if (E.on) Current()->stack_top = reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15);
}

void guest_cpu_set_gs(void* base) {
    if (E.on) {
        GuestState& s = *Current()->s;
        s.gs_cached = reinterpret_cast<uint64_t>(base);
        s.fs_cached = reinterpret_cast<uint64_t>(base);
    }
}

void guest_cpu_abandon(void) {
    if (self) self->depth = 0;
}

void guest_cpu_thread_end(void) {
    Thread* t = self;
    if (E.on && t) {
        self = nullptr;
        std::free(t->s);
        if (t->own_stack) munmap(reinterpret_cast<void*>(t->own_stack - t->own_stack_size), t->own_stack_size);
        t->s = nullptr;
        if (!E.stats) delete t;  // with statistics the thread stays on the list
    }
}

uintptr_t guest_cpu_return_address(uintptr_t) {
    if (!E.on || !self || !self->depth) return 0;
    return ReadGuest(self->s->gregs[RSP]);
}

uintptr_t guest_cpu_stack_pointer(void) {
    if (E.on && self && self->depth) return self->s->gregs[RSP];
    return 0;
}

int guest_cpu_signal_regs(const void* ucontext, GuestRegs* regs) {
    const uintptr_t pc = bbnative::HostPc(ucontext);
    Thread* t = self;
    if (E.on && t && pc >= E.so_lo && pc < E.so_hi) {
        // Inside a unit: the registers are those the unit last stored (stale, but valid), rip the unit's entry.
        std::memcpy(regs->gpr, t->s->gregs, sizeof(regs->gpr));
        auto at = std::upper_bound(E.by_address.begin(), E.by_address.end(), pc, [](uintptr_t v, const Engine::Where& w) { return v < w.fn; });
        regs->rip = at == E.by_address.begin() ? 0 : E.base + (at - 1)->vaddr;
        return GUEST_CPU_IN_GUEST;
    }
    if (E.on && t && t->depth) {
        std::memcpy(regs->gpr, t->s->gregs, sizeof(regs->gpr));
        const uint64_t rsp = t->s->gregs[RSP];
        regs->gpr[GUEST_RSP] = rsp;
        regs->rip = ReadGuest(rsp);
        return GUEST_CPU_IN_HOST_CALL;
    }
    // No guest code below: the host pc and stack pointer.
    std::memset(regs, 0, sizeof(*regs));
    regs->rip = pc;
    regs->gpr[GUEST_RSP] = bbnative::HostSp(ucontext);
    return 0;
}

// Called by the crash report (src/probe.c, weak): the faulting thread's last transitions between units and host functions.
__attribute__((visibility("default"))) void bbaot_crash_report(const void* ucontext) {
    if (!E.on || !self) return;
    Thread* t = self;
    const uintptr_t pc = bbnative::HostPc(ucontext);
    std::fprintf(stderr, "AOT: fault at host pc %p (%s), guest rsp %#llx, rip %#llx, depth %u\n", reinterpret_cast<void*>(pc),
                 pc >= E.so_lo && pc < E.so_hi ? "in a unit" : InCode(pc) ? "in guest code" : "elsewhere",
                 (unsigned long long)t->s->gregs[RSP], (unsigned long long)t->s->rip, t->depth);
    if (pc >= E.so_lo && pc < E.so_hi) {
        auto at = std::upper_bound(E.by_address.begin(), E.by_address.end(), pc, [](uintptr_t v, const Engine::Where& w) { return v < w.fn; });
        if (at != E.by_address.begin()) std::fprintf(stderr, "AOT: unit +%#x, offset %#llx in it\n", (at - 1)->vaddr, (unsigned long long)(pc - (at - 1)->fn));
    }
    if (E.trace) {
        const unsigned n = t->trace_n, first = n > Thread::TraceSize ? n - Thread::TraceSize : 0;
        std::fprintf(stderr, "AOT: tid %d, last %u transitions:\n", int(syscall(SYS_gettid)), n - first);
        for (unsigned i = first; i < n; ++i) {
            const auto& e = t->trace[i % Thread::TraceSize];
            // host functions as module+offset (symbolize on the PC against the unstripped library)
            Dl_info info {};
            const bool host = e.kind == EvHost && dladdr(reinterpret_cast<void*>(e.a), &info) && info.dli_fname;
            const char* mod = host ? std::strrchr(info.dli_fname, '/') : nullptr;
            std::fprintf(stderr, "AOT:   %-18s a=%#llx b=%#llx c=%#llx (image offsets: %#llx %#llx)%s%s%s%#llx\n", EventNames[e.kind], (unsigned long long)e.a,
                         (unsigned long long)e.b, (unsigned long long)e.c, (unsigned long long)(e.a - E.base), (unsigned long long)(e.c - E.base),
                         host ? " " : "", host ? (mod ? mod + 1 : info.dli_fname) : "", host ? "+" : "",
                         host ? (unsigned long long)(e.a - reinterpret_cast<uintptr_t>(info.dli_fbase)) : 0ull);
        }
    }
    std::fflush(stderr);
}

uintptr_t guest_cpu_host_pc(const void* ucontext) { return bbnative::HostPc(ucontext); }
int guest_cpu_handle_fault(int sig, siginfo_t* info, void* ucontext) {
    // x86 `lock` operations may be unaligned; the units' LSE atomics fault when they cross 16 bytes: emulated, and the unit
    // goes on after the instruction.
    if (E.on && sig == SIGBUS && info->si_code == BUS_ADRALN) {
        const uintptr_t pc = bbnative::HostPc(ucontext);
        if (pc >= E.so_lo && pc < E.so_hi && bbnative::HandleUnalignedAtomic(ucontext)) {
            unaligned_atomics.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    return 0;
}

int guest_cpu_is_native(void) { return 0; }

int guest_cpu_hook(uintptr_t address, size_t, GuestHook hook) {
    if (InstallSlot(address, hook, nullptr, nullptr)) return 1;  // a native hook site: no code is written
    RefuseHook(address);
    return 0;
}
int guest_cpu_hook_slot(uintptr_t address, size_t size, GuestHookEx hook, void* context) {
    (void)size;
    if (!hook) return 0;
    if (InstallSlot(address, nullptr, hook, context)) return 1;
    RefuseHook(address);
    return 0;
}
int guest_cpu_has_hook_slot(uintptr_t address) {
    if (!E.on || !V.nsites) return 0;
    const int64_t k = SiteIndex(address);
    if (k < 0) return 0;
    std::lock_guard<std::mutex> guard(E.lock);
    return SiteRunsNatively(k) ? 1 : 0;
}
void guest_cpu_hook_slot_clear(uintptr_t address) {
    if (!E.on || !V.nsites) return;
    const int64_t k = SiteIndex(address);
    if (k < 0) return;
    __atomic_store_n(&V.slots[k], static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    V.recs[k].store(nullptr, std::memory_order_release);
}
void guest_cpu_invalidate(uintptr_t address, size_t size) { MarkDirty(address, size); }
uint64_t guest_cpu_tsc(void) { return bbnative::Tsc(); }
uint64_t guest_cpu_tsc_frequency(void) { return bbnative::TscFrequency(); }
}  // extern "C"
