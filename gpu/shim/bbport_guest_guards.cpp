// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guards in the game's code against its own failure paths (upstream bloodborne_pc 0.6,
// bbport_guest_hooks.cpp there), as x86-64 stubs on a page next to the image registered as guest
// code (FEXCore runs them; bbport_frame_rate.cpp and bbport_display.cpp do the same). A trap-based
// hook (guest_cpu_hook) is not used: it leaves RCX undefined, and RCX is an argument at two of the
// sites. Installed at load, before the game's first instruction; each site's bytes are checked
// first, and a site that differs is left alone (logged).
//
// - Blur-kernel sample count (#207): the game's post-processing middleware (Yebis) computes blur
//   kernel parameters in GPUTexUtil_GetRecursiveSampleParameters (0xbbc3e0; called from the motion
//   blur's camera rotation parameters, 0xc33f37). Its first argument (edi), the base sample count,
//   must be at least 3: the function asserts "GPUMath.cpp(3199): nBaseSamples >= 3" through
//   sceKernelDebugRaiseException (code 0xa0020007, raised at +0xbbc429), which the runtime ends with
//   exit code 23. Game data now and then gives 2 while an area streams in (minutes into play; seen
//   on the Thor too). Ignoring the assertion is not enough: with 2 the function runs on into a state
//   that never ends (a black screen). So the count is raised to 3 at the function's entry.
//   BB_KERNEL_SAMPLE_CLAMP=0: not installed.
// - GX per-frame command node (#162): the GX swap chain's per-frame render command takes its 16-byte
//   node from a per-frame bump allocator that returns null once the frame's heap is used up; the
//   game writes through it unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the
//   render thread): a guest fault at address 8. Without a node the post is skipped (to 0x219934e,
//   past the post and the clear of the pending flag, this + 0x3ab): the command goes out with the
//   next frame. The heap itself is 1 MiB instead of 256 KiB (gx-heap.bin, run-bionic.sh).
// - Effect target initialization (Bad PM4 at save load, code 23): the effect's initialization
//   (0xebb960) on the game's main thread clears three small targets through 0x2168630 into the main
//   graphics context, whose command buffer the game's GXRenderThread is building at the same time
//   (the cursor is not atomic): a command buffer with a hole of zeros or a packet over another,
//   submitted as it is. Those three calls go to FxInitClear: for the checked 2x2 RG16F layout and
//   colour (0, 0, 0, 1) a WRITE_DATA of zeros is submitted to the graphics queue in order, without
//   touching the game's context; anything else (another layout, colour or thread) calls the game's
//   function as before. BB_FX_INIT_CLEAR=0: not installed.
// - GX occlusion queries without a handle (#336, upstream 0.7): a query whose creation failed (the
//   device's heap full late in a long session) has a null handle, and the render command that
//   begins it reads the handle: a guest fault at 0x2169edc on GXRenderThread. Described at
//   GxQuery below. Only as native hook slots (three sites that tool 1.2.x libraries have no slot
//   for: without them the guard is off, one line says so). BB_GX_QUERY_GUARD=0: not installed.
#include "common/types.h"
#include "bbport_guest_guards.h"
#include "bbport_guest_code.h"
#include "bbport_hook_sites.h"
#include "bbport_patch_notes.h"
#include "../../src/guest_cpu.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
// gnmdriver.cpp: a WRITE_DATA of zeros over [address, address + bytes) in the graphics queue.
bool BbSubmitFxInitZero(std::uint64_t address, std::uint32_t bytes);

namespace BbGuestGuards {
namespace {
using BbGuestCode::Emitter;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

constexpr std::size_t PageSize = 4096;
u64 image_base = 0;
u8* stub_page = nullptr;
std::size_t stub_used = 0;

bool clamp_on = false, gx_guard_on = false, fx_clear_on = false, gx_query_on = false;
std::atomic<u64> clamps{0}, gx_null_nodes{0}, fx_clears{0};

/// Rate-limited logging: the first five and then every power of two.
bool Tell(u64 n) {
    return n <= 5 || (n & (n - 1)) == 0;
}

/// A page within +-2 GiB of the image (below it), away from the frame rate's and the display's.
u8* MapNear() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = image_base - (128 + k) * (8ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), PageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return static_cast<u8*>(p);
    }
    return nullptr;
}

void Writable(u64 address, std::size_t size) {
    BbPatchNote(address, size, "guest guards (bbport_guest_guards)");
    const u64 page = u64(sysconf(_SC_PAGESIZE));
    const u64 start = address & ~(page - 1);
    mprotect(reinterpret_cast<void*>(start), address + size - start, PROT_READ | PROT_WRITE | PROT_EXEC);
}

/// `e` onto the stub page: its address, or 0 when it does not fit.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page) {
        stub_page = MapNear();
        if (!stub_page) std::printf("Guest guards: no memory next to the game's code for a stub\n");
    }
    if (!stub_page || e.overflow || stub_used + e.size > PageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32(u64 from_next, u64 to, std::int32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d != std::int32_t(d)) return false;
    rel = std::int32_t(d);
    return true;
}

/// A `jmp stub` over `length` bytes at `site` (the rest int3).
bool JumpTo(u64 site, std::size_t length, u64 stub) {
    std::int32_t rel = 0;
    if (!stub || !Rel32(site + 5, stub, rel)) return false;
    u8* code = reinterpret_cast<u8*>(site);
    Writable(site, length);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, 4);
    for (std::size_t i = 5; i < length; ++i) code[i] = 0xcc;
    return true;
}

/// The bytes at `offset` of the image are `expected`; else logged under `what`.
bool Expect(u64 offset, const u8* expected, std::size_t size, const char* what) {
    const u8* code = reinterpret_cast<const u8*>(image_base + offset);
    if (std::memcmp(code, expected, size) == 0) return true;
    char found[3 * 16 + 1] = {};
    for (std::size_t i = 0; i < size && i < 16; ++i) {
        std::snprintf(found + 3 * i, 4, "%02x ", code[i]);
    }
    std::printf("Guest guards: %s not installed: +%#llx is not the expected code (%s)\n", what,
                static_cast<unsigned long long>(offset), found);
    return false;
}

/// Saves the caller-saved registers around a call of host function `fn` (its guest-callable
/// address), which gets the registers' values as its arguments.
void CallHost(Emitter& e, void* fn) {
    e.SaveVolatile();
    e.SaveXmm();
    e.CallAbs(reinterpret_cast<const void*>(guest_cpu_host_function(fn)));
    e.RestoreXmm();
    e.RestoreVolatile();
}

// ---- The blur-kernel sample count ----
constexpr u64 KernelSite = 0xbbc3e0;
constexpr std::array<u8, 6> KernelEntry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15

/// The game asked for fewer than 3 base samples (`samples`, as it passed them).
void PS4_SYSV_ABI OnClamp(std::int32_t samples) {
    const u64 n = clamps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Tell(n)) {
        std::printf("Guest guards: the game asked for %d blur-kernel base samples, raised to 3 "
                    "(%llu so far)\n",
                    samples, static_cast<unsigned long long>(n));
    }
}

/// The clamp as a native hook slot: below 3 the game's value is told, then edi = 3 (the stub's work; the entry's own instructions run next).
std::uint32_t ClampSlot(void*, GuestRegsEx* r) {
    const auto samples = static_cast<std::int32_t>(r->gpr[GUEST_RDI]);
    if (samples < 3) {
        OnClamp(samples);
        r->gpr[GUEST_RDI] = 3; // mov edi, 3
    }
    return GUEST_HOOK_CONTINUE;
}

bool InstallKernelClamp() {
    if (const char* env = std::getenv("BB_KERNEL_SAMPLE_CLAMP"); env && env[0] == '0') {
        std::printf("Guest guards: blur-kernel sample clamp off (BB_KERNEL_SAMPLE_CLAMP=0)\n");
        return false;
    }
    if (!Expect(KernelSite, KernelEntry.data(), KernelEntry.size(), "blur-kernel sample clamp")) {
        return false;
    }
    if (BbHookSites::Available(image_base + KernelSite)) {
        return BbHookSites::Slot(image_base + KernelSite, image_base, KernelEntry.size(), ClampSlot, nullptr);
    }
    // Below 3: OnClamp told (with the game's value), then edi = 3.
    Emitter clamp;
    CallHost(clamp, reinterpret_cast<void*>(&OnClamp));
    clamp.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00}); // mov edi, 3
    Emitter e;
    e.Bytes({0x83, 0xff, 0x03}); // cmp edi, 3
    e.Bytes({0x0f, 0x8d});       // jge past the clamp (rel32: the clamp is longer than 127 bytes)
    e.U32(u32(clamp.size));
    for (std::size_t i = 0; i < clamp.size && i < clamp.code.size(); ++i) e.Bytes({clamp.code[i]});
    for (const u8 b : KernelEntry) e.Bytes({b}); // the instructions the jump replaces
    e.JmpAbs(image_base + KernelSite + KernelEntry.size());
    if (clamp.overflow) return false;
    return JumpTo(image_base + KernelSite, KernelEntry.size(), PlaceStub(e));
}

// ---- The GX per-frame command node ----
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the pending flag's clear
constexpr std::array<u8, 9> GxNodeCode = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                          0x49, 0x8b, 0x87, 0xb0};       // mov rax, [r15 + 0xb0]

void PS4_SYSV_ABI OnNullNode() {
    const u64 n = gx_null_nodes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Tell(n)) {
        std::printf("Guest guards: the GX per-frame command heap was full: a render command waits "
                    "for the next frame (%llu so far)\n",
                    static_cast<unsigned long long>(n));
    }
}

/// The GX node guard as a native hook slot: instead of `mov [r12 + 8], r13`: with a node, the store, then on; without, told, then past the post.
std::uint32_t GxNodeSlot(void*, GuestRegsEx* r) {
    if (r->gpr[GUEST_R12]) {
        std::memcpy(reinterpret_cast<u8*>(r->gpr[GUEST_R12]) + 8, &r->gpr[GUEST_R13], 8);
        r->rip = image_base + GxNodeSite + 5;
    } else {
        OnNullNode();
        r->rip = image_base + GxNodeSkip;
    }
    return GUEST_HOOK_JUMP;
}

bool InstallGxNodeGuard() {
    if (!Expect(GxNodeSite, GxNodeCode.data(), GxNodeCode.size(), "GX command node guard")) {
        return false;
    }
    if (BbHookSites::Available(image_base + GxNodeSite)) {
        return BbHookSites::Slot(image_base + GxNodeSite, image_base, 5, GxNodeSlot, nullptr,
                                 {image_base + GxNodeSite + 5, image_base + GxNodeSkip});
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});             // test r12, r12
    e.Bytes({0x74, 0x13});                   // jz null (past the next 19 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08}); // mov [r12 + 8], r13
    e.JmpAbs(image_base + GxNodeSite + 5);   // 14 bytes
    // null: told, then past the post.
    CallHost(e, reinterpret_cast<void*>(&OnNullNode));
    e.JmpAbs(image_base + GxNodeSkip);
    return JumpTo(image_base + GxNodeSite, 5, PlaceStub(e));
}

// ---- The effect targets' initialization ----
constexpr u64 FxClearFunction = 0x2168630;
struct CallSite {
    u64 offset;
    std::array<u8, 5> code;
};
constexpr std::array<CallSite, 3> FxInitSites = {{
    {0xebbc86, {0xe8, 0xa5, 0xc9, 0x2a, 0x01}},
    {0xebbca4, {0xe8, 0x87, 0xc9, 0x2a, 0x01}},
    {0xebbcc2, {0xe8, 0x69, 0xc9, 0x2a, 0x01}},
}};

struct ZeroClear {
    u64 address;
    u32 bytes;
};
/// The three 2x2 RG16F history targets 0xebb960 initializes (a Gnm render target from +0x10 of
/// `texture`: 11 words). A uniform zero has the same representation in every tile; the requested
/// alpha is not stored by this two-component target. Other clears and layouts: none.
bool PlanClear(const u32* target, const float* color, ZeroClear& out) {
    if (color[0] != 0 || color[1] != 0 || color[2] != 0 || color[3] != 1 || target[0] == 0 ||
        target[3] != 0 || target[6] != 0x00020002 || target[7] || target[8] || target[9] ||
        target[10]) {
        return false;
    }
    const u32 info = target[4], attrib = target[5];
    // RG16, FLOAT, standard component mapping, no compression, fast clear or Neo mode.
    if (((info >> 2) & 31) != 5 || ((info >> 8) & 7) != 7 ||
        (info & (3u | (1u << 7) | (3u << 11) | (3u << 13) | (1u << 28) | (1u << 31))) ||
        (attrib & 31) != 14 || (attrib & (31u << 12))) {
        return false;
    }
    const u64 pitch = ((target[1] & 0x7ff) + 1) * 8;
    const u64 texels = ((target[2] & 0x3fffff) + 1) * 64;
    const u64 bytes = texels * 4;
    const u64 address = u64(target[0]) << 8;
    if (pitch < 2 || texels / pitch < 2 || texels % pitch || bytes > 32768 ||
        address + bytes > (1ull << 40)) {
        return false;
    }
    out = {address, u32(bytes)};
    return true;
}

void PS4_SYSV_ABI FxInitClear(void* context, void* state, void* texture, const float* color) {
    // The main thread only (the game's GXRenderThread builds that context's commands itself).
    if (syscall(SYS_gettid) == getpid() && texture && color &&
        runtime_memory_is_mapped(reinterpret_cast<uintptr_t>(texture) + 0x10, 11 * sizeof(u32))) {
        u32 target[11];
        float rgba[4];
        std::memcpy(target, static_cast<const u8*>(texture) + 0x10, sizeof(target));
        std::memcpy(rgba, color, sizeof(rgba));
        ZeroClear clear{};
        if (PlanClear(target, rgba, clear) && runtime_memory_is_mapped(clear.address, clear.bytes) &&
            BbSubmitFxInitZero(clear.address, clear.bytes)) {
            const u64 n = fx_clears.fetch_add(1, std::memory_order_relaxed) + 1;
            if (Tell(n)) {
                std::printf("Guest guards: effect initialization: %llu targets cleared by the "
                            "port's own commands (%#llx +%u bytes)\n",
                            static_cast<unsigned long long>(n),
                            static_cast<unsigned long long>(clear.address), clear.bytes);
            }
            return;
        }
    }
    // The game's own clear, as the call did.
    const u64 args[4] = {reinterpret_cast<u64>(context), reinterpret_cast<u64>(state),
                         reinterpret_cast<u64>(texture), reinterpret_cast<u64>(color)};
    guest_cpu_call(uintptr_t(image_base + FxClearFunction), 4, args);
}

/// The three calls as native hook slots: instead of `call 0x2168630`, FxInitClear with the call's arguments, then on after the call.
std::uint32_t FxInitSlot(void*, GuestRegsEx* r) {
    FxInitClear(reinterpret_cast<void*>(r->gpr[GUEST_RDI]), reinterpret_cast<void*>(r->gpr[GUEST_RSI]),
                reinterpret_cast<void*>(r->gpr[GUEST_RDX]), reinterpret_cast<const float*>(r->gpr[GUEST_RCX]));
    r->rip += 5;
    return GUEST_HOOK_JUMP;
}

bool InstallFxInitClear() {
    if (const char* env = std::getenv("BB_FX_INIT_CLEAR"); env && env[0] == '0') {
        std::printf("Guest guards: effect initialization as the game does it (BB_FX_INIT_CLEAR=0)\n");
        return false;
    }
    for (const auto& site : FxInitSites) {
        if (!Expect(site.offset, site.code.data(), site.code.size(), "effect initialization")) {
            return false;
        }
    }
    bool all_slots = true;
    for (const auto& site : FxInitSites) all_slots = all_slots && BbHookSites::Available(image_base + site.offset);
    if (all_slots) {
        for (const auto& site : FxInitSites) {
            if (!BbHookSites::Slot(image_base + site.offset, image_base, 5, FxInitSlot, nullptr, {image_base + site.offset + 5})) return false;
        }
        return true;
    }
    Emitter e;
    e.JmpAbs(guest_cpu_host_function(reinterpret_cast<void*>(&FxInitClear)));
    const u64 stub = PlaceStub(e);
    std::array<std::int32_t, FxInitSites.size()> rels{};
    for (std::size_t i = 0; i < FxInitSites.size(); ++i) {
        if (!stub || !Rel32(image_base + FxInitSites[i].offset + 5, stub, rels[i])) return false;
    }
    for (std::size_t i = 0; i < FxInitSites.size(); ++i) {
        // The call's target only: `call stub` (the stub jumps on to FxInitClear).
        u8* code = reinterpret_cast<u8*>(image_base + FxInitSites[i].offset);
        Writable(reinterpret_cast<u64>(code), 5);
        std::memcpy(code + 1, &rels[i], 4);
    }
    return true;
}
// ---- GX occlusion queries without a handle ----
// A query (0x30 bytes, made by its pool, 0xec6820 and 0xec68d0, with 0x2288d00) gets its handle, its
// first field, from 0xe2f080(this, 1) -> 0x21674f0, which allocates a 0x18-byte handle (the device's
// allocator at +0xdc10) and its data (+0xdc18). On a failure it returns 8 or 9 and the handle stays
// null; the pool does not look. The render command 0x2285e10 then begins the query (0xe2f130: inner
// state [this + 8] 0 -> 2, then 0x2169ec0(device, device + 0xb730, handle), which reads the handle:
// the fault) and ends it (0xe2f160: state 1, 0x216a070, which reads it too). The guard: at the
// begin, a query whose creation failed (GxQueryCreated keeps their addresses) is created again; one
// still without a handle is not begun, and its end is skipped. Its state stays 0, so the result
// getter (0xe2f190) gives its stored result, 0 (nothing visible). A null handle the creation did not
// leave (a query released while the render thread holds it) is only skipped: creating one would
// write into a freed object. The pool creates on the main thread, the retry runs on GXRenderThread:
// both allocators (0x1fc5f30, 0x1fc5ed0) hold their lock (+0x468) around the game's TLSF heap
// (0x2085a60), and the rest of the creation touches only the query. The cause is a guess; the
// observer logs each failed creation once a minute.
// BB_RE_GX_QUERY=1: the first creations and guard events are logged with their thread.
// BB_GX_QUERY_FAIL=N (diagnostics): every N-th creation (retries too) fails as with a full heap
// (the game's release 0xe2f020 takes the new handle back), so the guard's paths run in any
// session. The pool makes 8192 at startup and hands out the last made first: N = 2 reaches a scene.
constexpr u64 GxQueryBeginSite = 0xe2f134; // cmp dword [rdi + 8], 0; jne 0xe2f157 (begun already)
constexpr u64 GxQueryReturn = 0xe2f157;    // mov al, 1; pop rbp; ret
constexpr std::array<u8, 6> GxQueryBeginCode = {0x83, 0x7f, 0x08, 0x00, 0x75, 0x1d};
constexpr u64 GxQueryEndSite = 0xe2f164;   // mov dword [rdi + 8], 1
constexpr u64 GxQueryEndReturn = 0xe2f181; // mov al, 1; pop rbp; ret
constexpr std::array<u8, 7> GxQueryEndCode = {0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00};
constexpr u64 GxQueryCreate = 0xe2f080;
constexpr u64 GxQueryRelease = 0xe2f020;
/// The creation's common exit (`mov rcx, [r15]; cmp rcx, [rbp - 0x20]`, the stack check): eax the
/// result, r14 where the handle goes (the query), rbx null when the handle's allocation failed.
constexpr u64 GxQueryCreateExit = 0x21675b0;
constexpr std::array<u8, 7> GxQueryCreateExitCode = {0x49, 0x8b, 0x0f, 0x48, 0x3b, 0x4d, 0xe0};
std::array<std::atomic<u64>, 256> gx_query_failed{}; ///< queries whose creation failed (0: free)
std::atomic<u64> gx_query_failures{0}, gx_query_recreated{0}, gx_query_skipped{0};
std::atomic<long long> gx_query_failure_told{0}, gx_query_guard_told{0};

/// True the first time and then at most once a minute (`last`: steady clock, ms).
bool OncePerMinute(std::atomic<long long>& last) {
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
    long long seen = last.load(std::memory_order_relaxed);
    return (seen == 0 || now - seen >= 60000) && last.compare_exchange_strong(seen, now);
}

std::array<char, 32> ThreadName() {
    std::array<char, 32> name{};
    if (pthread_getname_np(pthread_self(), name.data(), name.size()) != 0 || !name[0]) name = {"?"};
    return name;
}

/// BB_RE_GX_QUERY=1: true for the first 16 events counted in `traced`.
bool GxQueryTrace(std::atomic<int>& traced) {
    static const bool trace = [] {
        const char* env = std::getenv("BB_RE_GX_QUERY");
        return env && env[0] == '1';
    }();
    return trace && traced.fetch_add(1) < 16;
}

/// The creation's exit as a native hook slot (observer: the game's instructions run next): rax the result, r14 the query.
std::uint32_t GxQueryCreatedSlot(void*, GuestRegsEx* r) {
    auto status = std::uint32_t(r->gpr[GUEST_RAX]);
    const u64 query = r->gpr[GUEST_R14];
    static const long fail_every = [] {
        const char* env = std::getenv("BB_GX_QUERY_FAIL");
        return env ? std::strtol(env, nullptr, 10) : 0l;
    }();
    static std::atomic<u64> creations{0};
    const bool simulated = status == 0 && fail_every > 0 &&
                           (creations.fetch_add(1) + 1) % u64(fail_every) == 0;
    if (simulated) {
        const u64 args[1] = {query};
        guest_cpu_call(uintptr_t(image_base + GxQueryRelease), 1, args);
        r->gpr[GUEST_RAX] = status = 9;
    }
    static std::atomic<int> traced{0};
    if (GxQueryTrace(traced)) {
        std::printf("Guest guards: GX query %#llx created, result %u%s (thread %s)\n",
                    static_cast<unsigned long long>(query), status, simulated ? " (BB_GX_QUERY_FAIL)" : "",
                    ThreadName().data());
    }
    if (status == 0) {
        for (auto& slot : gx_query_failed) {
            u64 expected = query;
            if (slot.load(std::memory_order_relaxed) == query) slot.compare_exchange_strong(expected, 0);
        }
        return GUEST_HOOK_CONTINUE;
    }
    bool known = false;
    for (const auto& slot : gx_query_failed) known = known || slot.load(std::memory_order_relaxed) == query;
    for (auto& slot : gx_query_failed) {
        u64 expected = 0;
        if (known || slot.compare_exchange_strong(expected, query)) break;
    }
    const u64 n = gx_query_failures.fetch_add(1) + 1;
    if (!OncePerMinute(gx_query_failure_told)) return GUEST_HOOK_CONTINUE;
    std::printf("Guest guards: a GX occlusion query could not be created (error %u: %s), %llu so far "
                "(thread %s); the query guard creates it again before it begins\n", status,
                simulated     ? "BB_GX_QUERY_FAIL"
                : status == 9 ? (r->gpr[GUEST_RBX] ? "no memory for its data" : "no memory for its handle")
                              : "the device or its data not ready",
                static_cast<unsigned long long>(n), ThreadName().data());
    return GUEST_HOOK_CONTINUE;
}

/// Called for a query without a handle at its begin. A query whose creation failed is created again
/// (the game's own 0xe2f080, as the pool does); the caller then looks again.
void GxQueryWithoutHandle(u64 query) {
    bool failed = false;
    for (const auto& slot : gx_query_failed) failed = failed || slot.load(std::memory_order_relaxed) == query;
    if (failed) {
        const u64 args[2] = {query, 1};
        guest_cpu_call(uintptr_t(image_base + GxQueryCreate), 2, args); // GxQueryCreatedSlot keeps or forgets it
    }
    const bool created = *reinterpret_cast<const volatile u64*>(query) != 0;
    const u64 recreated = gx_query_recreated.fetch_add(created) + created;
    const u64 skipped = gx_query_skipped.fetch_add(!created) + !created;
    static std::atomic<int> traced{0};
    if (!OncePerMinute(gx_query_guard_told) && !GxQueryTrace(traced)) return;
    std::printf("Guest guards: GX occlusion query %#llx had no handle at its begin (%s): %llu created "
                "again, %llu not begun (no fault; nothing visible) so far (thread %s)\n",
                static_cast<unsigned long long>(query),
                failed ? "its creation had failed" : "no failed creation seen: released?",
                static_cast<unsigned long long>(recreated), static_cast<unsigned long long>(skipped),
                ThreadName().data());
}

/// The begin (instead of `cmp dword [rdi + 8], 0; jne done`): the game's state check, then the handle's:
/// begun or ended already, or not begun without a handle (the state stays 0): to the return; else the body.
std::uint32_t GxQueryBeginSlot(void*, GuestRegsEx* r) {
    const u64 query = r->gpr[GUEST_RDI];
    const bool done = *reinterpret_cast<const volatile std::uint32_t*>(query + 8) != 0;
    bool handle = done || *reinterpret_cast<const volatile u64*>(query) != 0;
    if (!handle) {
        GxQueryWithoutHandle(query);
        handle = *reinterpret_cast<const volatile u64*>(query) != 0;
    }
    r->rip = image_base + (!done && handle ? GxQueryBeginSite + GxQueryBeginCode.size() : GxQueryReturn);
    return GUEST_HOOK_JUMP;
}

/// The end (instead of `mov dword [rdi + 8], 1`): only a query with a handle (begun) is ended.
std::uint32_t GxQueryEndSlot(void*, GuestRegsEx* r) {
    const u64 query = r->gpr[GUEST_RDI];
    if (*reinterpret_cast<const volatile u64*>(query) != 0) {
        const std::uint32_t ended = 1;
        std::memcpy(reinterpret_cast<u8*>(query) + 8, &ended, sizeof(ended)); // mov dword [rdi + 8], 1
        r->rip = image_base + GxQueryEndSite + GxQueryEndCode.size();
    } else {
        r->rip = image_base + GxQueryEndReturn; // not ended, the state stays 0
    }
    return GUEST_HOOK_JUMP;
}

bool InstallGxQueryGuard() {
    if (const char* env = std::getenv("BB_GX_QUERY_GUARD"); env && env[0] == '0') {
        std::printf("Guest guards: GX occlusion query guard off (BB_GX_QUERY_GUARD=0)\n");
        return false;
    }
    if (!Expect(GxQueryBeginSite, GxQueryBeginCode.data(), GxQueryBeginCode.size(), "GX occlusion query guard") ||
        !Expect(GxQueryEndSite, GxQueryEndCode.data(), GxQueryEndCode.size(), "GX occlusion query guard") ||
        !Expect(GxQueryCreateExit, GxQueryCreateExitCode.data(), GxQueryCreateExitCode.size(), "GX occlusion query guard")) {
        return false;
    }
    // Slots only: a libbbaot.so built before these sites were in the catalog (Rebuild Tool 1.2.x) has none, and runs as before.
    if (!BbHookSites::Available(image_base + GxQueryBeginSite) || !BbHookSites::Available(image_base + GxQueryEndSite) ||
        !BbHookSites::Available(image_base + GxQueryCreateExit)) {
        std::printf("Guest guards: GX occlusion query guard inactive: the libbbaot.so has no hook slots there "
                    "(rebuild it with a newer Rebuild Tool)\n");
        return false;
    }
    // The creation observer first: without it a query without a handle is only skipped.
    return BbHookSites::Slot(image_base + GxQueryCreateExit, image_base, GxQueryCreateExitCode.size(), GxQueryCreatedSlot, nullptr) &&
           BbHookSites::Slot(image_base + GxQueryBeginSite, image_base, GxQueryBeginCode.size(), GxQueryBeginSlot, nullptr,
                             {image_base + GxQueryBeginSite + GxQueryBeginCode.size(), image_base + GxQueryReturn}) &&
           BbHookSites::Slot(image_base + GxQueryEndSite, image_base, GxQueryEndCode.size(), GxQueryEndSlot, nullptr,
                             {image_base + GxQueryEndSite + GxQueryEndCode.size(), image_base + GxQueryEndReturn});
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    image_base = reinterpret_cast<u64>(image);
    if (size < 0x2200000) {
        return;
    }
    clamp_on = InstallKernelClamp();
    gx_guard_on = InstallGxNodeGuard();
    fx_clear_on = InstallFxInitClear();
    gx_query_on = InstallGxQueryGuard();
    if (stub_page && stub_used == 0) {
        munmap(stub_page, PageSize);
        stub_page = nullptr;
    }
    if (stub_page) {
        // The stub page is guest code (registered with guest_cpu_code).
        guest_cpu_code(reinterpret_cast<uintptr_t>(stub_page), PageSize);
        mprotect(stub_page, PageSize, PROT_READ | PROT_EXEC);
    }
    if (!clamp_on && !gx_guard_on && !fx_clear_on && !gx_query_on) return;
    std::printf("Guest guards: blur-kernel sample clamp %s, GX command node guard %s, effect "
                "initialization by the port %s, GX query guard %s\n",
                clamp_on ? "on" : "off", gx_guard_on ? "on" : "off", fx_clear_on ? "on" : "off",
                gx_query_on ? "on" : "off");
}

void Report() {
    if (clamp_on || gx_guard_on || fx_clear_on || gx_query_on) {
        std::printf("Guest guards: %llu blur-kernel clamps, %llu GX commands without a node, %llu "
                    "effect targets cleared by the port, %llu GX queries recreated, %llu not begun\n",
                    static_cast<unsigned long long>(clamps.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(gx_null_nodes.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(fx_clears.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(gx_query_recreated.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(gx_query_skipped.load(std::memory_order_relaxed)));
    }
}
} // namespace BbGuestGuards
