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
#include "common/types.h"
#include "bbport_guest_guards.h"
#include "bbport_guest_code.h"
#include "bbport_patch_notes.h"
#include "../../src/guest_cpu.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <array>
#include <atomic>
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

bool clamp_on = false, gx_guard_on = false, fx_clear_on = false;
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

bool InstallKernelClamp() {
    if (const char* env = std::getenv("BB_KERNEL_SAMPLE_CLAMP"); env && env[0] == '0') {
        std::printf("Guest guards: blur-kernel sample clamp off (BB_KERNEL_SAMPLE_CLAMP=0)\n");
        return false;
    }
    if (!Expect(KernelSite, KernelEntry.data(), KernelEntry.size(), "blur-kernel sample clamp")) {
        return false;
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

bool InstallGxNodeGuard() {
    if (!Expect(GxNodeSite, GxNodeCode.data(), GxNodeCode.size(), "GX command node guard")) {
        return false;
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
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    image_base = reinterpret_cast<u64>(image);
    if (size < 0x2200000) {
        return;
    }
    stub_page = MapNear();
    if (!stub_page) {
        std::printf("Guest guards: not installed: no memory next to the game's code\n");
        return;
    }
    clamp_on = InstallKernelClamp();
    gx_guard_on = InstallGxNodeGuard();
    fx_clear_on = InstallFxInitClear();
    if (stub_used == 0) {
        munmap(stub_page, PageSize);
        stub_page = nullptr;
        return;
    }
    // The stub page is guest code (FEXCore translates what the jumps lead to there).
    guest_cpu_code(reinterpret_cast<uintptr_t>(stub_page), PageSize);
    mprotect(stub_page, PageSize, PROT_READ | PROT_EXEC);
    std::printf("Guest guards: blur-kernel sample clamp %s, GX command node guard %s, effect "
                "initialization by the port %s\n",
                clamp_on ? "on" : "off", gx_guard_on ? "on" : "off", fx_clear_on ? "on" : "off");
}

void Report() {
    if (clamp_on || gx_guard_on || fx_clear_on) {
        std::printf("Guest guards: %llu blur-kernel clamps, %llu GX commands without a node, %llu "
                    "effect targets cleared by the port\n",
                    static_cast<unsigned long long>(clamps.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(gx_null_nodes.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(fx_clears.load(std::memory_order_relaxed)));
    }
}
} // namespace BbGuestGuards
