// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's frame rate set free by hooks in its code (bbport_frame_rate.h).
//
// The game's main loop (0x2018d20) runs its frame pacer (0x2034770: it waits for the frame time
// its mode asks, 1/30 s, and measures the frame at [pacer + 0x264]) and then steps the game by a
// fixed 1/30 s (0x2018e36). The hooks keep the pacer's mode at one vblank and no frame time to
// wait for beyond the shortest frame, and step the game by the measured frame time (1/480 to
// 1/30 s: below 30 FPS the game slows down, as it always did).
//
// What the game ties to a 30 FPS frame besides (found by the frame time each function is called
// with, BB_RE_ENTRY, and by what its code does with the constant): subsystems stepped by a fixed
// 1/30 s, per-frame steps and approaches, per-frame damping. Their instructions read the constant
// from a page next to the image instead (the instruction's own address operand moved; the game's
// code is otherwise as it was), where FrameStep writes the value for this frame's time: a step of
// 1/30 s becomes the frame's time, a per-frame factor f becomes f^(frames) for the frame's length
// in 30 FPS frames. Durations the game converts from 30 FPS frames into seconds stay as they are:
// what counts them down uses the real time.
//
// Ported from deadinside28/bloodborne_pc (testing, 72e5c8c "0.6-pre1") to the Bionic build, where
// the game's code runs in FEXCore (guest_cpu): the stubs call the native FrameStep through its
// guest-callable address (guest_cpu_host_function); the stub page is registered as guest code;
// every site is checked before the first byte is written (a mismatch leaves the game untouched:
// bbgpu_frame_hooks_installed() is false and the launcher's fallback patches are applied); the step's
// upper bound is configurable (BB_FRAME_DT_MAX_FPS: 30 as upstream, 20 keeps the game's speed
// down to 20 FPS).
#include "common/types.h"
#include "bbport_frame_rate.h"
#include "bbport_guest_code.h"
#include "bbport_hook_sites.h"
#include "bbport_patch_notes.h"
#include "../../src/guest_cpu.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace BbFrameRate {
namespace {
using BbGuestCode::Emitter;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

std::atomic<bool> unlocked{false};
u64 image_base = 0;
/// Nothing is written while this is false (every site is checked before the first write).
bool committing = false;
/// What the check phase has claimed so far: [start, end) of the bytes the hooks change.
std::vector<std::pair<u64, u64>> claimed;

constexpr float Frame30 = 1.0f / 30.0f;
/// The highest frame rate the pacer allows ("uncap", and the most BB_FPS_LIMIT can ask for). The
/// game itself breaks above it: on loading screens at 880-1000 FPS its main loop outran its worker
/// threads and a pool allocator popped a freed node (guest fault at +0x263b8e7, 2 of 2 loads);
/// at 480 all loads went through (4 of 4). The cap was 240 before.
constexpr int MaxRate = 480;
constexpr float ShortestFrame = 1.0f / MaxRate;
/// The longest step the game gets (FrameStep): 1/30 s upstream, the game's own frame time, below
/// which the game slows down. BB_FRAME_DT_MAX_FPS lowers the bound (20: real time down to 20 FPS,
/// a step of up to 50 ms, 1.5 of the game's frames).
float longest_frame = Frame30;

/// What the redirected instructions read, rewritten by FrameStep for each frame.
struct alignas(64) Live {
    float dt = Frame30;               ///< the frame's time: the game steps by it
    float shortest = ShortestFrame;   ///< the pacer's frame time target
    float pad[2] = {};
    float decay95[4] = {0.95f, 0.95f, 0.95f, 0.95f}; ///< 0.95 per 30 FPS frame (read as four)
    float approach30 = Frame30;       ///< a 1/30 approach per 30 FPS frame
    float approach10 = 0.1f;          ///< a 0.1 approach per 30 FPS frame
    float step_down = -0.1f;          ///< -0.1 per 30 FPS frame
    float step_up = 0.1f;             ///< +0.1 per 30 FPS frame
    float decay08 = 0.8f;             ///< 0.8 per 30 FPS frame
    float cloth = 1.0f;               ///< cloth constraints' stiffness
};
Live* live = nullptr;

/// The pacer at the main loop's step (rdi: it): this frame's values. `frames` below is the frame's
/// length in the game's 30 FPS frames: up to 30 * longest_frame (1.5 with a 20 FPS bound).
void PS4_SYSV_ABI FrameStep(u64 pacer) {
    const float measured = *reinterpret_cast<const float*>(pacer + 0x264);
    float dt = std::clamp(std::isfinite(measured) ? measured : Frame30, live->shortest, longest_frame);
    // BB_FRAME_DT_SMOOTH=a (diagnostics, 0 < a < 1): the step follows the measured frame time by
    // that fraction a frame, so frame-to-frame jitter of the measurement does not reach the
    // simulation (time stays right on average).
    static const float smooth = [] {
        const char* env = std::getenv("BB_FRAME_DT_SMOOTH");
        const float a = env && *env ? std::strtof(env, nullptr) : 0.0f;
        return a > 0.0f && a < 1.0f ? a : 0.0f;
    }();
    if (smooth > 0.0f) {
        static float smoothed = 0.0f;
        smoothed = smoothed > 0.0f ? smoothed + smooth * (dt - smoothed) : dt;
        dt = std::clamp(smoothed, live->shortest, longest_frame);
    }
    const float frames = dt * 30.0f; // the frame's length in 30 FPS frames
    live->dt = dt;
    const float decay95 = std::pow(0.95f, frames);
    for (float& d : live->decay95) d = decay95;
    live->approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    live->approach10 = 1.0f - std::pow(0.9f, frames);
    live->step_down = -0.1f * frames;
    live->step_up = 0.1f * frames;
    live->decay08 = std::pow(0.8f, frames);
    // Cloth is stiffer the more often it is solved: the game's 1.0 at 30 FPS, 0.5 from 60 FPS up
    // (the value the community's 60 FPS patches settled on). It is the upper bound the cloth
    // setups clamp their Stiffness, Rigid Factor and Allowed Compression to when they load. Below
    // 30 FPS (frames > 1) it stays at the game's own 1.0: never stiffer than the game made it.
    // BB_CLOTH_STIFFNESS=value (diagnostics): that bound at any frame rate.
    static const float cloth_override = [] {
        const char* env = std::getenv("BB_CLOTH_STIFFNESS");
        return env && *env ? std::strtof(env, nullptr) : -1.0f;
    }();
    live->cloth = cloth_override > 0.0f ? cloth_override : std::clamp(frames, 0.5f, 1.0f);
}

// ---- Redirected constants: instruction (its RIP-relative operand), the constant, the value ----
struct Redirect {
    u64 site;
    u64 constant;
    std::size_t value; ///< offsetof(Live, ..)
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A fixed 1/30 s step or "one frame" (the patches' lists, checked one by one):
    {0xab9f0d, 0x47efdcc, LIVE(dt)},   // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)},   // a per-frame budget
    {0xee596a, 0x48ad65c, LIVE(dt)},   // effects: a fade of at least one frame (three releases)
    {0xee5a8a, 0x48ad660, LIVE(dt)},
    {0xee5baa, 0x48ad664, LIVE(dt)},
    {0xef06f1, 0x48adc08, LIVE(dt)},   // at least one frame
    {0xef6992, 0x48add28, LIVE(dt)},
    {0x1044943, 0x48cc3f4, LIVE(dt)},  // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)},  // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)},  // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)},  // frames counted at one frame's time
    {0x1f77d4a, 0x4929170, LIVE(dt)},  // an update by one frame (0 after a reset)
    {0x227784b, 0x4929b80, LIVE(dt)},  // timers advanced by one frame (0xebbda0)
    {0x2277892, 0x4929b80, LIVE(dt)},
    {0x2313dad, 0x4929e70, LIVE(dt)},  // a simulation step: 1/60 in the game's 60 Hz mode,
    {0x2313db7, 0x4929e74, LIVE(dt)},  // 1/30 otherwise
    {0x2315d71, 0x4929e74, LIVE(dt)},
    // Body parts the physics moves (0x1515ec0, each part every frame): their timers advanced by the
    // step they are given (0x180df90 at +0xf0, 0x1811650 at +0xd0), 1/30 s; not in the patches.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Steps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping per frame:
    {0x1a4abca, 0x490b5b0, LIVE(decay95)}, // a character's motion (0x1a4a730, x0.95 a frame)
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check (x0.8 a frame while blocked)
    // Cloth constraints' stiffness:
    {0x47c6dc, 0x47693f0, LIVE(cloth)},
    {0x7e45be, 0x47b63e4, LIVE(cloth)},
    {0x7e4642, 0x47b63e4, LIVE(cloth)},
    {0x7e6239, 0x47b63e4, LIVE(cloth)},
    {0x7f706d, 0x47b8474, LIVE(cloth)},
    {0x7f70ad, 0x47b8474, LIVE(cloth)},
    {0x7f79e5, 0x47b8474, LIVE(cloth)},
    {0x7f7be8, 0x47b8474, LIVE(cloth)},
    {0x7f7dbd, 0x47b8474, LIVE(cloth)},
    {0x805898, 0x47b91c0, LIVE(cloth)},
    {0x805d18, 0x47b91c0, LIVE(cloth)},
    {0x806269, 0x47b91c0, LIVE(cloth)},
    {0x8062f0, 0x47b91c0, LIVE(cloth)},
    {0x809d62, 0x47b98b8, LIVE(cloth)},
    {0x80a4c9, 0x47b98b8, LIVE(cloth)},
    {0x80a876, 0x47b98b8, LIVE(cloth)},
};
#undef LIVE

/// [address, address + size) writable (the loader sets the image's protection after
/// bbgpu_patch_image; hooks installed earlier, BbGuestHooks, may have set a page's already).
void Writable(u64 address, std::size_t size) {
    BbPatchNote(address, size, "frame-rate hooks (bbport_frame_rate)");
    const u64 page = u64(sysconf(_SC_PAGESIZE));
    const u64 start = address & ~(page - 1);
    mprotect(reinterpret_cast<void*>(start), address + size - start, PROT_READ | PROT_WRITE | PROT_EXEC);
}

/// The bytes [address, address + size) are the hooks' from now on; false when another hook of the
/// check phase has them (nothing is written then, but a table error must not go unseen).
bool Claim(u64 address, std::size_t size) {
    for (const auto& [start, end] : claimed) {
        if (address < end && start < address + size) return false;
    }
    claimed.emplace_back(address, address + size);
    return true;
}

/// The instruction at `r.site` reads `r.constant` RIP-relative (a VEX instruction, no immediate
/// after the operand): it reads the live value instead.
bool RedirectOperand(const Redirect& r) {
    u8* code = reinterpret_cast<u8*>(image_base + r.site);
    std::size_t disp_at = 0, length = 0;
    if (code[0] == 0xc5 && (code[3] & 0xc7) == 0x05) {
        disp_at = 4, length = 8;
    } else if (code[0] == 0xc4 && (code[4] & 0xc7) == 0x05) {
        disp_at = 5, length = 9;
    } else {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + disp_at, 4);
    const u64 next = image_base + r.site + length;
    if (next + std::int64_t(disp) != image_base + r.constant) return false;
    const std::int64_t to = std::int64_t(reinterpret_cast<u64>(live) + r.value) - std::int64_t(next);
    if (to != std::int32_t(to)) return false;
    if (!committing) return Claim(image_base + r.site + disp_at, 4);
    const std::int32_t to32 = std::int32_t(to);
    Writable(reinterpret_cast<u64>(code + disp_at), 4);
    std::memcpy(code + disp_at, &to32, 4);
    return true;
}

// ---- Detours: the instructions at a site replaced by a jump to a stub ----
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// `e` into the stub page; a jump to it at `site` over `length` bytes (the rest int3) if the site
/// holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (length < 5 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > StubPageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(image_base + site + 5);
    if (rel != std::int32_t(rel)) return false;
    stub_used += (e.size + 15) & ~std::size_t(15);
    if (!committing) return Claim(image_base + site, length);
    std::memcpy(stub, e.code.data(), e.size);
    const std::int32_t rel32 = std::int32_t(rel);
    Writable(reinterpret_cast<u64>(code), length);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) code[i] = 0xcc;
    return true;
}

/// The pacer's mode, after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the
/// pacer): one vblank (+0x10), no flip skipping (+0x14), the shortest frame as the frame time
/// target (+0x18), no vblank counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.Bytes({0x50, 0x48, 0xb8});                                       // push rax; movabs rax,
    e.U64(reinterpret_cast<u64>(&live->shortest));                     //   &shortest
    e.Bytes({0x8b, 0x00});                                             // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58});                     // mov [r12 + 0x18], eax; pop rax
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [r12 + 0x268], 0
    for (const u8 b : code) e.Bytes({b});
    e.JmpAbs(image_base + 0x203493d + sizeof(code));
    return Detour(0x203493d, code, sizeof(code), e);
}

/// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
/// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(image_base + 0x2034d9f);
    return Detour(0x2034d96, code, sizeof(code), e);
}

/// The main loop's step (0x2018e36: `mov dword [rbp - 0x38], 1/30`, its time; rbx the pacer):
/// FrameStep, then the frame's time. The guest cannot call FrameStep (a host function) itself
/// under FEXCore: it calls the guest-callable stub of it (guest_cpu_host_function, SysV ABI; the
/// host CPU's own address on x86-64).
bool HookStep() {
    constexpr u8 code[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx
    e.CallAbs(reinterpret_cast<const void*>(guest_cpu_host_function(reinterpret_cast<void*>(&FrameStep))));
    e.RestoreVolatile();
    e.StoreToFrame(&live->dt, -0x38);
    e.JmpAbs(image_base + 0x2018e36 + sizeof(code));
    return Detour(0x2018e36, code, sizeof(code), e);
}

/// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + offset], 1/30` at `site`): by the
/// frame's time.
bool HookFixedStep(u64 site, std::int8_t offset) {
    const u8 code[] = {0xc7, 0x45, u8(offset), 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.StoreToFrame(&live->dt, offset);
    e.JmpAbs(image_base + site + sizeof(code));
    return Detour(site, code, sizeof(code), e);
}

/// The run's wall check (0x1514c24: the distance moved this frame x 30 = a speed at 30 FPS): the
/// distance divided by the frame's time ([rbp - 0x74], the character update's), a speed at any
/// frame rate. Above ~90 FPS the per-frame distance fell under the check's threshold and the run
/// dropped to half speed.
/// 1: done by the hook, 2: the community's Sprint Fix patch already did it, 0: other code.
int HookRunSpeed() {
    constexpr u64 site = 0x1514c24;
    const u8* code = reinterpret_cast<const u8*>(image_base + site);
    constexpr u8 patched[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss; nop
    if (std::memcmp(code, patched, sizeof(patched)) == 0) {
        return 2;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 4, 4);
    if (code[0] != 0xc5 || code[1] != 0xfa || code[2] != 0x59 || code[3] != 0x05 ||
        image_base + site + 8 + std::int64_t(disp) != image_base + 0x492632c) {
        return 0;
    }
    u8 expected[8];
    std::memcpy(expected, code, 8);
    Emitter e;
    e.Bytes({0xc5, 0xfa, 0x5e, 0x45, 0x8c}); // vdivss xmm0, xmm0, [rbp - 0x74]
    e.JmpAbs(image_base + site + 8);
    return Detour(site, expected, 8, e) ? 1 : 0;
}

/// A mapping within +-2 GiB of the image (below it), for rel32 jumps and RIP-relative operands.
u8* MapNear(int prot) {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = image_base - k * (8ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, prot,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return static_cast<u8*>(p);
    }
    return nullptr;
}

struct Counts {
    int redirects = 0;
    int detours = 0;
    int run = 0; ///< HookRunSpeed's answer
};

/// All the hooks, in one pass: written when `committing`, else only checked. False: a site is not
/// the expected code (`what` names it); the game's code is unchanged then, as the check pass
/// writes nothing.
bool InstallAll(Counts& c, std::string& what) {
    c = {};
    stub_used = 0;
    claimed.clear();
    char text[96];
    const auto fail = [&](const char* name, u64 site) {
        std::snprintf(text, sizeof(text), "%s (%#llx) is not the expected code", name, (unsigned long long)site);
        what = text;
        return false;
    };
    // The core first: without all three the game keeps its own frame rate.
    if (!HookPacerMode()) return fail("the pacer's mode", 0x203493d);
    if (!HookPacerLate()) return fail("the pacer's late frame", 0x2034d96);
    if (!HookStep()) return fail("the main loop's step", 0x2018e36);
    c.detours = 3;
    if (!HookFixedStep(0x17f9b71, -0x48)) return fail("a fixed step", 0x17f9b71);
    if (!HookFixedStep(0x1572a7d, -0x30)) return fail("a fixed step", 0x1572a7d);
    c.detours += 2;
    c.run = HookRunSpeed();
    if (c.run == 0) return fail("the run's wall check", 0x1514c24);
    c.detours += c.run == 1;
    for (const Redirect& r : Redirects) {
        if (!RedirectOperand(r)) return fail("a per-frame value", r.site);
        ++c.redirects;
    }
    return true;
}


// ---- Native hook slots (the game's code runs as ahead-of-time recompiled units, docs/RECOMP_HOOKS.md) ----
// The detours become C++ callbacks of hook slots: no stub page, no jump written. `live` moves to the unused end of the image's data segment's last
// page (as the cheats' flags do, tools/android/build_patch_parts.py), so that the operand redirects' displacements are the same bytes in every run and
// the recompiler can build the redirected functions ahead of time (tools/recomp/variants/gen_patchers.cpp reads them from this code). Every callback
// does what its stub did.
constexpr u64 LiveImageOffset = 0x56d3f40; // data segment end 0x56d30f4, its page ends at 0x56d4000; the cheats' flags are at 0x56d3f00
static_assert(sizeof(Live) <= 0xc0);
bool slot_mode = false;

/// The pacer's mode (0x203493d, before `lea r15, [r12 + 0x268]`).
std::uint32_t PacerModeSlot(void*, GuestRegsEx* r) {
    u8* pacer = reinterpret_cast<u8*>(r->gpr[GUEST_R12]);
    const std::uint32_t one = 1, shortest = *reinterpret_cast<const std::uint32_t*>(&live->shortest);
    std::memcpy(pacer + 0x10, &one, 4);
    pacer[0x14] = 0;
    std::memcpy(pacer + 0x18, &shortest, 4);
    const u64 zero = 0;
    std::memcpy(pacer + 0x268, &zero, 8);
    return GUEST_HOOK_CONTINUE; // the game's `lea` runs
}
/// The main loop's step (0x2018e36, instead of `mov dword [rbp - 0x38], 1/30`; rbx the pacer).
std::uint32_t StepSlot(void*, GuestRegsEx* r) {
    FrameStep(r->gpr[GUEST_RBX]);
    std::memcpy(reinterpret_cast<u8*>(r->gpr[GUEST_RBP]) - 0x38, &live->dt, 4);
    r->rip += 7;
    return GUEST_HOOK_JUMP;
}
/// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + offset], 1/30`, the offset is the context).
std::uint32_t FixedStepSlot(void* offset, GuestRegsEx* r) {
    std::memcpy(reinterpret_cast<u8*>(r->gpr[GUEST_RBP]) + std::int64_t(reinterpret_cast<std::intptr_t>(offset)), &live->dt, 4);
    r->rip += 7;
    return GUEST_HOOK_JUMP;
}

/// `size` bytes at `site` are `expected`: they become `bytes` (checked in the first pass, written in the second).
bool PatchBytes(u64 site, const u8* expected, const u8* bytes, std::size_t size) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (std::memcmp(code, expected, size) != 0) return false;
    if (!committing) return Claim(image_base + site, size);
    Writable(reinterpret_cast<u64>(code), size);
    std::memcpy(code, bytes, size);
    return true;
}

/// The hooks as slots (every site checked first, as InstallAll). The detours that only drop or replace instructions are byte patches instead
/// (the pacer's late-frame `je`, the run's wall check): variants of the functions, the same bytes in every run.
bool InstallAllSlots(Counts& c, std::string& what) {
    c = {};
    claimed.clear();
    char text[96];
    const auto fail = [&](const char* name, u64 site) {
        std::snprintf(text, sizeof(text), "%s (%#llx) is not the expected code", name, (unsigned long long)site);
        what = text;
        return false;
    };
    const auto slot = [&](u64 site, const u8* expected, std::size_t length, GuestHookEx fn, void* context,
                          std::initializer_list<u64> resume) {
        if (std::memcmp(reinterpret_cast<const u8*>(image_base + site), expected, length) != 0) return false;
        if (!committing) return Claim(image_base + site, length) && BbHookSites::Available(image_base + site);
        return BbHookSites::Slot(image_base + site, image_base, length, fn, context, resume);
    };
    {
        constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
        if (!slot(0x203493d, code, sizeof(code), PacerModeSlot, nullptr, {})) return fail("the pacer's mode", 0x203493d);
    }
    {
        // test dl, dl; je +5; mov eax, [r12 + 0x10]: the stub ran the mov always: the `je` becomes two nops.
        constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
        constexpr u8 patched[] = {0x84, 0xd2, 0x90, 0x90, 0x41, 0x8b, 0x44, 0x24, 0x10};
        if (!PatchBytes(0x2034d96, code, patched, sizeof(code))) return fail("the pacer's late frame", 0x2034d96);
    }
    {
        constexpr u8 code[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
        if (!slot(0x2018e36, code, sizeof(code), StepSlot, nullptr, {image_base + 0x2018e36 + 7}))
            return fail("the main loop's step", 0x2018e36);
    }
    c.detours = 3;
    for (const auto& [site, offset] : {std::pair<u64, std::int8_t>{0x17f9b71, -0x48}, {0x1572a7d, -0x30}}) {
        const u8 code[] = {0xc7, 0x45, u8(offset), 0x89, 0x88, 0x08, 0x3d};
        if (!slot(site, code, sizeof(code), FixedStepSlot, reinterpret_cast<void*>(std::intptr_t(offset)), {image_base + site + 7}))
            return fail("a fixed step", site);
    }
    c.detours += 2;
    {
        constexpr u64 site = 0x1514c24;
        const u8* code = reinterpret_cast<const u8*>(image_base + site);
        constexpr u8 patched[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss xmm0, xmm0, [rbp - 0x74]; nop
        if (std::memcmp(code, patched, sizeof(patched)) == 0) {
            c.run = 2;
        } else {
            std::int32_t disp = 0;
            std::memcpy(&disp, code + 4, 4);
            if (code[0] != 0xc5 || code[1] != 0xfa || code[2] != 0x59 || code[3] != 0x05 ||
                image_base + site + 8 + std::int64_t(disp) != image_base + 0x492632c) {
                return fail("the run's wall check", site);
            }
            u8 expected[8];
            std::memcpy(expected, code, 8);
            if (!PatchBytes(site, expected, patched, 8)) return fail("the run's wall check", site);
            c.run = 1;
        }
    }
    c.detours += c.run == 1;
    for (const Redirect& r : Redirects) {
        if (!RedirectOperand(r)) return fail("a per-frame value", r.site);
        ++c.redirects;
    }
    return true;
}

void Disabled(const char* why) {
    std::printf("Frame rate: hooks off (%s)\n", why);
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t image_size) {
    // BB_FPS (run.sh, the launcher's frame rate): uncap (default), 60 or 90 (the same, at most that
    // many frames a second), 30 (the game's own). BB_FRAME_HOOKS=0: the community's frame rate
    // patches instead (scripts/patches.py, tools/android/run-bionic.sh; comparisons).
    const char* fps = std::getenv("BB_FPS");
    const char* hooks = std::getenv("BB_FRAME_HOOKS");
    const int mode = !fps || std::strcmp(fps, "uncap") == 0 ? MaxRate : std::atoi(fps);
    if (hooks && hooks[0] == '0') {
        return Disabled("BB_FRAME_HOOKS=0");
    }
    if (mode <= 30) {
        return Disabled("BB_FPS=30: the game's own frame rate");
    }
    image_base = reinterpret_cast<u64>(image);
    // Recompiled game code with native hook slots at every detour site: callbacks instead of stubs (InstallAllSlots).
    slot_mode = image_size >= LiveImageOffset + sizeof(Live) && BbHookSites::Available(image_base + 0x203493d) &&
                BbHookSites::Available(image_base + 0x2018e36) && BbHookSites::Available(image_base + 0x17f9b71) &&
                BbHookSites::Available(image_base + 0x1572a7d);
    u8* data = nullptr;
    const auto release = [&] {
        if (data) munmap(data, StubPageSize);
        if (stub_page) munmap(stub_page, StubPageSize);
        data = stub_page = nullptr;
        live = nullptr;
    };
    if (slot_mode) {
        live = new (image + LiveImageOffset) Live{};
    } else {
        data = MapNear(PROT_READ | PROT_WRITE);
        stub_page = MapNear(PROT_READ | PROT_WRITE);
        if (!data || !stub_page) {
            release();
            return Disabled("no memory next to the game's code");
        }
        // The stub page is guest code (FEXCore translates what the jumps lead to there).
        guest_cpu_code(reinterpret_cast<uintptr_t>(stub_page), StubPageSize);
        live = new (data) Live{};
    }
    // The upper bound of a step: BB_FRAME_DT_MAX_FPS, 30 (the game's own frame time) by default.
    // 20 keeps real time down to 20 FPS; below that the game slows down.
    if (const char* env = std::getenv("BB_FRAME_DT_MAX_FPS"); env && *env) {
        const float lowest = std::strtof(env, nullptr);
        if (lowest > 0.0f) {
            longest_frame = 1.0f / std::clamp(lowest, 10.0f, 30.0f);
        }
    }
    // The mode's rate; BB_FPS_LIMIT (the launcher's frame rate limit) is the present side's here
    // (the video out holds finished frames back), and the game's pacer waits for it itself too
    // only with BB_FRAME_PACER_LIMIT=1 (before the frame's simulation: upstream's way, lower
    // latency than holding finished frames back at the present).
    int rate = std::clamp(mode, 30, MaxRate);
    if (const char* pacer = std::getenv("BB_FRAME_PACER_LIMIT"); pacer && pacer[0] == '1') {
        if (const char* limit = std::getenv("BB_FPS_LIMIT"); limit && std::atoi(limit) > 0) {
            rate = std::min(rate, std::clamp(std::atoi(limit), 30, MaxRate));
        }
    }
    live->shortest = 1.0f / float(rate);
    // Every site is checked before the first write: a game whose code differs (another build, a
    // port patch there) keeps its own frame rate untouched.
    Counts counts;
    std::string what;
    committing = false;
    const auto install = [&] { return slot_mode ? InstallAllSlots(counts, what) : InstallAll(counts, what); };
    if (!install()) {
        release();
        return Disabled(what.c_str());
    }
    committing = true;
    if (!install()) {
        // Not expected: the check pass found every site. The game's code may be half changed.
        std::fprintf(stderr, "STOP: Frame rate: %s after the check\n", what.c_str());
        std::abort();
    }
    if (stub_page) mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
    unlocked = true;
    std::printf("Frame rate: hooks on (%d redirects, %d detours%s), step 1/%.0f..1/%.0f s%s\n",
                counts.redirects, counts.detours, slot_mode ? ", native hook slots" : "", 1.0f / live->shortest, 1.0f / longest_frame,
                counts.run == 2 ? "; the run's wall check was fixed by a Sprint Fix patch already" : "");
}

bool Unlocked() {
    return unlocked.load(std::memory_order_relaxed);
}
} // namespace BbFrameRate
