// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: observers in a game's own code (bbport_guest_code_hooks.h). The trampoline follows
// shadPS4's InstallGuestCodeHook (core/cpu_patches.cpp of the shadp2p/BBSC forks, GPL-2.0+) and the
// port's own upstream version of it (Xbyak); the machine code is emitted here byte by byte (no code
// generator library), so the Android build, where FEXCore runs the game's code, needs nothing more
// than Zydis (the check that a span is whole position-independent instructions).
//
// What differs from a native x86-64 host: the game's code is not run by the host CPU, so the
// trampoline cannot `call` the hook (arm64 code). It calls the guest-callable stub of one host
// function (guest_cpu_host_function) with the hook as a third argument; that function calls the
// hook. FEXCore has no page tracking of the game's code here, so every write of code is followed by
// guest_cpu_invalidate of the range (GuestCodeWritten).
#include "bbport_guest_code_hooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include <Zydis/Zydis.h>

#include "../../src/guest_cpu.h"

// Whether the trampoline reaches the hook through a guest-callable host function stub: always where
// the guest CPU is FEXCore; a test on an x86-64 host builds it both ways.
#ifndef BB_GUEST_HOOK_THUNK
#ifdef GUEST_CPU_NATIVE
#define BB_GUEST_HOOK_THUNK 0
#else
#define BB_GUEST_HOOK_THUNK 1
#endif
#endif

namespace Core {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t NearJumpBytes = 5;
constexpr std::size_t GuestRedZoneBytes = 128;
constexpr std::size_t AreaSize = 512u << 10;      // trampolines (~0.9 KiB each)
constexpr std::int64_t JumpReach = 0x7000'0000;   // inside a rel32 jump's 2 GiB
constexpr std::size_t MaxSpan = 64;               // the longest run of instructions a hook replaces

// The frame the trampoline builds below the guest's stack pointer: past the red zone, the
// snapshot (16-aligned), then the guest's r11 and flags (they are needed before the snapshot's
// base is known).
constexpr std::size_t SnapshotOffset = 0;
constexpr std::size_t ScratchR11Offset = sizeof(GuestRegisterSnapshot);
constexpr std::size_t ScratchRflagsOffset = ScratchR11Offset + sizeof(u64);
constexpr std::size_t FrameReserve =
    (ScratchRflagsOffset + sizeof(u64) + GuestRedZoneBytes + 15) & ~std::size_t(15);
static_assert(sizeof(GuestRegisterSnapshot) % 16 == 0);

// x86-64 register numbers (ModRM encoding).
enum Reg : int {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};

/// The bytes of the trampoline.
class Code {
public:
    std::vector<u8> bytes;
    void Byte(u8 b) { bytes.push_back(b); }
    void Bytes(std::initializer_list<u8> list) { bytes.insert(bytes.end(), list); }
    void U32(u32 v) {
        u8 b[4];
        std::memcpy(b, &v, 4);
        bytes.insert(bytes.end(), b, b + 4);
    }
    void U64(u64 v) {
        u8 b[8];
        std::memcpy(b, &v, 8);
        bytes.insert(bytes.end(), b, b + 8);
    }
    void Raw(const u8* data, std::size_t size) { bytes.insert(bytes.end(), data, data + size); }

    /// mov [rsp + disp32], reg  /  mov reg, [rsp + disp32]
    void StoreRsp(Reg reg, std::size_t disp) { MemRsp(0x89, reg, disp); }
    void LoadRsp(Reg reg, std::size_t disp) { MemRsp(0x8b, reg, disp); }
    /// mov rax, [r11 + disp32] / lea rax, [r11 + disp32]
    void LoadRaxFromR11(std::size_t disp) { Bytes({0x49, 0x8b, 0x83}); U32(u32(disp)); }
    void LeaRaxFromR11(std::size_t disp) { Bytes({0x49, 0x8d, 0x83}); U32(u32(disp)); }
    /// vmovdqu [rsp + disp32], ymmN  /  vmovdqu ymmN, [rsp + disp32]  (VEX.256.F3.0F 7F / 6F)
    void YmmStore(int n, std::size_t disp) { Ymm(0x7f, n, disp); }
    void YmmLoad(int n, std::size_t disp) { Ymm(0x6f, n, disp); }
    /// movabs reg, imm64 (rax, rdx, rdi)
    void MovImm(Reg reg, u64 value) {
        Bytes({0x48, u8(0xb8 + reg)});
        U64(value);
    }

private:
    void MemRsp(u8 opcode, Reg reg, std::size_t disp) {
        Byte(u8(0x48 | (reg >= R8 ? 4 : 0))); // REX.W, REX.R for r8-r15
        Byte(opcode);
        Byte(u8(0x84 | ((reg & 7) << 3))); // ModRM: [SIB + disp32], reg
        Byte(0x24);                        // SIB: rsp
        U32(u32(disp));
    }
    void Ymm(u8 opcode, int n, std::size_t disp) {
        Byte(0xc5);                          // two-byte VEX
        Byte(n >= 8 ? 0x7e : 0xfe);          // ~R, vvvv = 1111, L = 1 (256 bits), pp = F3
        Byte(opcode);
        Byte(u8(0x84 | ((n & 7) << 3)));
        Byte(0x24);
        U32(u32(disp));
    }
};

std::mutex hook_mutex;
std::map<std::uintptr_t, std::uintptr_t> hooked_sites; // [first byte, end) of installed hooks
unsigned installed_count = 0;
unsigned refused_count = 0;
GuestCodeMismatchExplainer explainer = nullptr;

/// An executable area within reach of its code, mapped on first use.
struct Area {
    std::uintptr_t start = 0;
    std::size_t used = 0;
};
std::vector<Area> areas;

std::uintptr_t MapNear(std::uintptr_t code) {
    const long page = sysconf(_SC_PAGESIZE);
    // Below the code first (the port maps the game's image at 0x800000000 with room under it),
    // then above, in 64 MiB steps.
    for (std::int64_t step = 1; step * (64ll << 20) < JumpReach; ++step) {
        for (const std::int64_t sign : {-1ll, 1ll}) {
            const std::int64_t at = std::int64_t(code) + sign * step * (64ll << 20);
            if (at <= 0) {
                continue;
            }
            void* p = mmap(reinterpret_cast<void*>(at & ~std::int64_t(page - 1)), AreaSize,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return reinterpret_cast<std::uintptr_t>(p);
            }
        }
    }
    return 0;
}

/// The area with room for `size` bytes within a jump's reach of `code`, else a new one.
Area* AreaFor(std::uintptr_t code, std::size_t size) {
    for (auto& area : areas) {
        const std::int64_t distance = std::int64_t(area.start) - std::int64_t(code);
        if (distance > -JumpReach && distance < JumpReach && area.used + size + 16 <= AreaSize) {
            return &area;
        }
    }
    const std::uintptr_t start = MapNear(code);
    if (!start) {
        return nullptr;
    }
    // The area is guest code (the guest CPU translates what the jumps lead to there).
    guest_cpu_code(start, AreaSize);
    areas.push_back({start, 0});
    return &areas.back();
}

std::string Hex(const u8* data, std::size_t size) {
    std::string text;
    char two[4];
    for (std::size_t i = 0; i < size && i < 16; ++i) {
        std::snprintf(two, sizeof(two), "%02x", data[i]);
        text += two;
        if (i + 1 < size) {
            text += ' ';
        }
    }
    if (size > 16) {
        text += " ...";
    }
    return text;
}

/// Whole instructions, none addressing relative to the instruction pointer (they move to the
/// trampoline).
bool WholeAndAbsolute(const u8* code, std::size_t size, const char** why) {
    static const ZydisDecoder decoder = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return d;
    }();
    std::size_t offset = 0;
    while (offset < size) {
        ZydisDecodedInstruction instruction{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const auto status = ZydisDecoderDecodeFull(&decoder, code + offset, size - offset,
                                                   &instruction, operands.data());
        if (!ZYAN_SUCCESS(status) || instruction.length == 0 ||
            instruction.length > size - offset) {
            *why = "not whole instructions";
            return false;
        }
        if (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            *why = "relative addressing";
            return false;
        }
        for (u8 i = 0; i < instruction.operand_count; ++i) {
            const auto& operand = operands[i];
            if ((operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) ||
                (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 (operand.mem.base == ZYDIS_REGISTER_RIP ||
                  operand.mem.base == ZYDIS_REGISTER_EIP))) {
                *why = "relative addressing";
                return false;
            }
        }
        offset += instruction.length;
    }
    return true;
}

std::atomic<bool> code_writable{false};

/// What each installed site does for BB_GUEST_HOOK_STATS=1: its calls, told every 20 s (the guest CPU pays a host
/// call for every hook run, so a hot site shows).
struct SiteStats {
    std::atomic<u64> calls{0};
    u64 reported = 0;
    u64 site = 0;
    u64 tag = 0;
};
std::vector<SiteStats*> all_stats;

bool StatsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_GUEST_HOOK_STATS");
        return env && env[0] == '1';
    }();
    return enabled;
}

void ReportStats() {
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(20) || !mutex.try_lock()) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    std::vector<std::pair<u64, SiteStats*>> delta;
    {
        std::scoped_lock lock{hook_mutex};
        for (SiteStats* s : all_stats) {
            const u64 calls = s->calls.load(std::memory_order_relaxed);
            if (calls != s->reported) {
                delta.emplace_back(calls - s->reported, s);
                s->reported = calls;
            }
        }
    }
    std::sort(delta.begin(), delta.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    u64 total = 0;
    for (const auto& d : delta) total += d.first;
    std::fprintf(stderr, "Guest code hooks: %llu runs in %.0f s (%.0f/s) over %zu sites; busiest:",
                 static_cast<unsigned long long>(total), seconds, double(total) / seconds, delta.size());
    for (std::size_t i = 0; i < delta.size() && i < 8; ++i) {
        std::fprintf(stderr, " %#llx(tag %llu)=%.0f/s", static_cast<unsigned long long>(delta[i].second->site),
                     static_cast<unsigned long long>(delta[i].second->tag), double(delta[i].first) / seconds);
    }
    std::fprintf(stderr, "\n");
    mutex.unlock();
}

/// Host function the trampoline calls under a guest CPU: passes the call on to the hook (`stats`: the site's
/// counters).
[[maybe_unused]] void HookDispatch(u64 tag, const GuestRegisterSnapshot* registers, GuestCodeHook hook,
                                   SiteStats* stats) {
    if (stats) {
        stats->calls.fetch_add(1, std::memory_order_relaxed);
        ReportStats();
    }
    try {
        hook(tag, registers);
    } catch (const std::exception& e) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::fprintf(stderr, "Guest code hook %#llx: C++ exception (%s); hooks go on\n",
                         static_cast<unsigned long long>(tag), e.what());
        }
    } catch (...) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::fprintf(stderr, "Guest code hook %#llx: C++ exception; hooks go on\n",
                         static_cast<unsigned long long>(tag));
        }
    }
}

/// The trampoline for the site at `site` whose `expected` bytes it runs after the hook.
Code BuildTrampoline(std::uintptr_t site, std::span<const u8> expected, u64 tag, GuestCodeHook hook,
                     const u8* skip_flag, std::uintptr_t at, SiteStats* stats) {
    Code t;
    const auto field = [](std::size_t f) { return SnapshotOffset + f; };
    t.Bytes({0x48, 0x8d, 0xa4, 0x24});                  // lea rsp, [rsp - FrameReserve]: past the red zone
    t.U32(u32(-std::int32_t(FrameReserve)));
    t.StoreRsp(R11, ScratchR11Offset);                  // mov [rsp + scratch], r11
    t.Byte(0x9c);                                       // pushfq
    t.Bytes({0x41, 0x5b});                              // pop r11
    t.StoreRsp(R11, ScratchRflagsOffset);               // mov [rsp + scratch + 8], r11
    t.Bytes({0x49, 0x89, 0xe3});                        // mov r11, rsp
    t.Bytes({0x48, 0x83, 0xe4, 0xf0});                  // and rsp, -16
    t.StoreRsp(RAX, field(offsetof(GuestRegisterSnapshot, rax)));
    t.StoreRsp(RBX, field(offsetof(GuestRegisterSnapshot, rbx)));
    t.StoreRsp(RCX, field(offsetof(GuestRegisterSnapshot, rcx)));
    t.StoreRsp(RDX, field(offsetof(GuestRegisterSnapshot, rdx)));
    t.StoreRsp(RSI, field(offsetof(GuestRegisterSnapshot, rsi)));
    t.StoreRsp(RDI, field(offsetof(GuestRegisterSnapshot, rdi)));
    t.StoreRsp(RBP, field(offsetof(GuestRegisterSnapshot, rbp)));
    t.StoreRsp(R8, field(offsetof(GuestRegisterSnapshot, r8)));
    t.StoreRsp(R9, field(offsetof(GuestRegisterSnapshot, r9)));
    t.StoreRsp(R10, field(offsetof(GuestRegisterSnapshot, r10)));
    t.StoreRsp(R12, field(offsetof(GuestRegisterSnapshot, r12)));
    t.StoreRsp(R13, field(offsetof(GuestRegisterSnapshot, r13)));
    t.StoreRsp(R14, field(offsetof(GuestRegisterSnapshot, r14)));
    t.StoreRsp(R15, field(offsetof(GuestRegisterSnapshot, r15)));
    t.LoadRaxFromR11(ScratchR11Offset);                 // the guest's r11 and flags, from the scratch
    t.StoreRsp(RAX, field(offsetof(GuestRegisterSnapshot, r11)));
    t.LoadRaxFromR11(ScratchRflagsOffset);
    t.StoreRsp(RAX, field(offsetof(GuestRegisterSnapshot, rflags)));
    t.LeaRaxFromR11(FrameReserve);                      // the guest's rsp
    t.StoreRsp(RAX, field(offsetof(GuestRegisterSnapshot, rsp)));
    t.Bytes({0x0f, 0xae, 0x9c, 0x24});                  // stmxcsr [rsp + mxcsr]
    t.U32(u32(field(offsetof(GuestRegisterSnapshot, mxcsr))));
    for (int i = 0; i < 16; ++i) {
        t.YmmStore(i, field(offsetof(GuestRegisterSnapshot, ymm)) + std::size_t(i) * 32);
    }
    // The aligned snapshot in a callee-saved register across the call.
    t.Bytes({0x49, 0x89, 0xe4});                        // mov r12, rsp
    t.MovImm(RDI, tag);                                 // mov rdi, tag
    t.Bytes({0x4c, 0x89, 0xe6});                        // mov rsi, r12 (+ SnapshotOffset = 0)
    static_assert(SnapshotOffset == 0);
#if BB_GUEST_HOOK_THUNK
    // The hook is host code the guest cannot call: its guest-callable stub gets it as a third argument (and the
    // site's counters, or 0, as a fourth) and calls it.
    t.MovImm(RDX, reinterpret_cast<u64>(hook));
    t.MovImm(RCX, reinterpret_cast<u64>(stats));
    t.MovImm(RAX, u64(guest_cpu_host_function(reinterpret_cast<void*>(&HookDispatch))));
#else
    (void)stats;
    t.MovImm(RAX, reinterpret_cast<u64>(hook));
#endif
    t.Bytes({0xff, 0xd0});                              // call rax
    t.Bytes({0x4c, 0x89, 0xe4});                        // mov rsp, r12
    for (int i = 0; i < 16; ++i) {
        t.YmmLoad(i, field(offsetof(GuestRegisterSnapshot, ymm)) + std::size_t(i) * 32);
    }
    t.Bytes({0x0f, 0xae, 0x94, 0x24});                  // ldmxcsr [rsp + mxcsr]
    t.U32(u32(field(offsetof(GuestRegisterSnapshot, mxcsr))));
    t.LoadRsp(RBX, field(offsetof(GuestRegisterSnapshot, rbx)));
    t.LoadRsp(RCX, field(offsetof(GuestRegisterSnapshot, rcx)));
    t.LoadRsp(RDX, field(offsetof(GuestRegisterSnapshot, rdx)));
    t.LoadRsp(RSI, field(offsetof(GuestRegisterSnapshot, rsi)));
    t.LoadRsp(RDI, field(offsetof(GuestRegisterSnapshot, rdi)));
    t.LoadRsp(RBP, field(offsetof(GuestRegisterSnapshot, rbp)));
    t.LoadRsp(R8, field(offsetof(GuestRegisterSnapshot, r8)));
    t.LoadRsp(R9, field(offsetof(GuestRegisterSnapshot, r9)));
    t.LoadRsp(R10, field(offsetof(GuestRegisterSnapshot, r10)));
    t.LoadRsp(R11, field(offsetof(GuestRegisterSnapshot, r11)));
    t.LoadRsp(R12, field(offsetof(GuestRegisterSnapshot, r12)));
    t.LoadRsp(R13, field(offsetof(GuestRegisterSnapshot, r13)));
    t.LoadRsp(R14, field(offsetof(GuestRegisterSnapshot, r14)));
    t.LoadRsp(R15, field(offsetof(GuestRegisterSnapshot, r15)));
    t.LoadRsp(RAX, field(offsetof(GuestRegisterSnapshot, rflags)));
    t.Bytes({0x50, 0x9d});                              // push rax; popfq
    t.LoadRsp(RAX, field(offsetof(GuestRegisterSnapshot, rax)));
    t.LoadRsp(RSP, field(offsetof(GuestRegisterSnapshot, rsp)));
    std::size_t je_patch = 0;
    if (skip_flag) {
        // Function entries only: [rsp] is the return address, flags are not live across a call; a
        // set flag (cleared here) returns from the game's function at once.
        t.Byte(0x50);                                   // push rax
        t.MovImm(RAX, reinterpret_cast<u64>(skip_flag));
        t.Bytes({0x80, 0x38, 0x00});                    // cmp byte [rax], 0
        t.Bytes({0xc6, 0x00, 0x00});                    // mov byte [rax], 0
        t.Byte(0x58);                                   // pop rax
        t.Bytes({0x0f, 0x84});                          // je run_original
        je_patch = t.bytes.size();
        t.U32(0);
        t.Byte(0xc3);                                   // ret
        const std::int32_t rel = std::int32_t(t.bytes.size() - (je_patch + 4));
        std::memcpy(t.bytes.data() + je_patch, &rel, 4);
    }
    t.Raw(expected.data(), expected.size());            // the replaced instructions
    t.Byte(0xe9);                                       // jmp back
    const std::int64_t back = std::int64_t(site + expected.size()) -
                              std::int64_t(at + t.bytes.size() + 4);
    t.U32(u32(std::int32_t(back)));
    return t;
}

/// Writes `size` bytes over the game's code under a write permission it may not have, then drops
/// the guest CPU's translation of the range.
void WriteCode(u8* code, const u8* bytes, std::size_t size) {
    const long page = sysconf(_SC_PAGESIZE);
    auto* first = reinterpret_cast<u8*>(reinterpret_cast<std::uintptr_t>(code) &
                                        ~std::uintptr_t(page - 1));
    const std::size_t span = (code + size) - first;
    const bool keep = code_writable.load(std::memory_order_acquire);
    if (!keep) {
        mprotect(first, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    // The jump's last four bytes first, its opcode last: a thread running here meanwhile meets
    // either the old instruction or the whole jump.
    std::memcpy(code + 1, bytes + 1, size - 1);
    __atomic_store_n(code, bytes[0], __ATOMIC_SEQ_CST);
    if (!keep) {
        mprotect(first, span, GuestCodeProtection());
    }
    GuestCodeWritten(code, size);
}

/// The guest stub of CallGuestWithFloats: rdi points at the argument block
///   [0] function, [8..56) rdi rsi rdx rcx r8 r9, [56..72) two stack arguments, [72..104) xmm0-7 (floats).
std::uintptr_t FloatCallStub() {
    static const std::uintptr_t stub = [] {
        Code c;
        c.Byte(0x53);                                       // push rbx
        c.Bytes({0x48, 0x89, 0xfb});                        // mov rbx, rdi
        for (int i = 0; i < 8; ++i) {                       // movss xmm_i, [rbx + 0x48 + 4 i]
            c.Bytes({0xf3, 0x0f, 0x10, u8(0x43 | (i << 3)), u8(0x48 + 4 * i)});
        }
        c.Bytes({0xff, 0x73, 0x40});                        // push qword [rbx + 0x40]: argument 8
        c.Bytes({0xff, 0x73, 0x38});                        // push qword [rbx + 0x38]: argument 7
        c.Bytes({0x48, 0x8b, 0x7b, 0x08});                  // mov rdi, [rbx + 8]
        c.Bytes({0x48, 0x8b, 0x73, 0x10});                  // mov rsi, [rbx + 0x10]
        c.Bytes({0x48, 0x8b, 0x53, 0x18});                  // mov rdx, [rbx + 0x18]
        c.Bytes({0x48, 0x8b, 0x4b, 0x20});                  // mov rcx, [rbx + 0x20]
        c.Bytes({0x4c, 0x8b, 0x43, 0x28});                  // mov r8, [rbx + 0x28]
        c.Bytes({0x4c, 0x8b, 0x4b, 0x30});                  // mov r9, [rbx + 0x30]
        c.Bytes({0xff, 0x13});                              // call [rbx]
        c.Bytes({0x48, 0x83, 0xc4, 0x10});                  // add rsp, 16
        c.Byte(0x5b);                                       // pop rbx
        c.Byte(0xc3);                                       // ret
        const long page = sysconf(_SC_PAGESIZE);
        void* memory = mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            return std::uintptr_t(0);
        }
        std::memcpy(memory, c.bytes.data(), c.bytes.size());
        guest_cpu_code(reinterpret_cast<std::uintptr_t>(memory), std::size_t(page));
        guest_cpu_invalidate(reinterpret_cast<std::uintptr_t>(memory), c.bytes.size());
        return reinterpret_cast<std::uintptr_t>(memory);
    }();
    return stub;
}

} // namespace

std::uint64_t CallGuestWithFloats(std::uintptr_t fn, std::span<const std::uint64_t> ints,
                                  std::span<const float> floats) {
    const std::uintptr_t stub = FloatCallStub();
    if (!stub || ints.size() > 8 || floats.size() > 8) {
        std::fprintf(stderr, "STOP: CallGuestWithFloats(%#llx): %s\n", static_cast<unsigned long long>(fn),
                     stub ? "too many arguments" : "no memory for the call stub");
        std::abort();
    }
    alignas(16) std::array<u64, 13> block{}; // fn, 8 integers, 4 words of floats
    block[0] = fn;
    for (std::size_t i = 0; i < ints.size(); ++i) {
        block[1 + i] = ints[i];
    }
    std::memcpy(&block[9], floats.data(), floats.size() * sizeof(float));
    const u64 argument = reinterpret_cast<u64>(block.data());
    return guest_cpu_call(stub, 1, &argument);
}

int GuestCodeProtection() {
    return code_writable.load(std::memory_order_acquire) ? PROT_READ | PROT_WRITE | PROT_EXEC
                                                         : PROT_READ | PROT_EXEC;
}

void KeepGuestCodeWritable() {
    code_writable.store(true, std::memory_order_release);
}

void GuestCodeWritten(const void* address, std::size_t size) {
    guest_cpu_invalidate(reinterpret_cast<std::uintptr_t>(address), size);
}

void WriteGuestCode(void* address, const void* bytes, std::size_t size) {
    std::memcpy(address, bytes, size);
    GuestCodeWritten(address, size);
}

void SetGuestCodeMismatchExplainer(GuestCodeMismatchExplainer function) {
    std::scoped_lock lock{hook_mutex};
    explainer = function;
}

unsigned GuestCodeHooksInstalled() {
    std::scoped_lock lock{hook_mutex};
    return installed_count;
}

unsigned GuestCodeHooksRefused() {
    std::scoped_lock lock{hook_mutex};
    return refused_count;
}

bool InstallGuestCodeHook(void* address, std::span<const std::uint8_t> expected, std::uint64_t tag,
                          GuestCodeHook hook, const std::uint8_t* skip_flag) {
    auto* code = static_cast<u8*>(address);
    if (!hook || !code || expected.size() < NearJumpBytes || expected.size() > MaxSpan) {
        return false;
    }
    std::scoped_lock lock{hook_mutex};
    const auto refuse = [&](const char* why, const std::string& detail = {}) {
        ++refused_count;
        std::fprintf(stderr, "Guest code hook at %p refused: %s%s%s\n", address, why,
                     detail.empty() ? "" : "; ", detail.c_str());
        return false;
    };
    if (std::memcmp(code, expected.data(), expected.size()) != 0) {
        std::string detail = "expected " + Hex(expected.data(), expected.size()) + ", found " +
                             Hex(code, expected.size());
        if (explainer) {
            const std::string more = explainer(reinterpret_cast<std::uintptr_t>(code), expected.size());
            if (!more.empty()) {
                detail += "; " + more;
            }
        }
        return refuse("the code differs (another build of the game, or a port patch there)", detail);
    }
    const char* why = nullptr;
    if (!WholeAndAbsolute(code, expected.size(), &why)) {
        return refuse(why);
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(code);
    const auto end = begin + expected.size();
    // A hook before the first one that ends after `begin`.
    auto it = hooked_sites.lower_bound(begin);
    if (it != hooked_sites.begin() && std::prev(it)->second > begin) {
        return refuse("it overlaps another hook");
    }
    if (it != hooked_sites.end() && it->first < end) {
        return refuse("it overlaps another hook");
    }
    // Room for the trampoline: at most ~1.1 KiB.
    Area* area = AreaFor(begin, 1280);
    if (!area) {
        return refuse("no trampoline memory in reach");
    }
    const std::uintptr_t at = (area->start + area->used + 15) & ~std::uintptr_t(15);
    SiteStats* stats = nullptr;
    if (StatsEnabled()) {
        stats = new SiteStats{};
        stats->site = begin;
        stats->tag = tag;
    }
    const Code trampoline = BuildTrampoline(begin, expected, tag, hook, skip_flag, at, stats);
    const std::int64_t rel = std::int64_t(at) - std::int64_t(begin + NearJumpBytes);
    if (rel != std::int32_t(rel) || at + trampoline.bytes.size() > area->start + AreaSize) {
        return refuse("no trampoline memory in reach");
    }
    std::memcpy(reinterpret_cast<void*>(at), trampoline.bytes.data(), trampoline.bytes.size());
    area->used = (at - area->start) + trampoline.bytes.size();
    guest_cpu_invalidate(at, trampoline.bytes.size());
    // The site: a jump to the trampoline, the rest of the span filled with nops.
    std::array<u8, MaxSpan> patch;
    patch.fill(0x90);
    const std::int32_t rel32 = std::int32_t(rel);
    patch[0] = 0xe9;
    std::memcpy(patch.data() + 1, &rel32, 4);
    WriteCode(code, patch.data(), expected.size());
    hooked_sites.emplace(begin, end);
    if (stats) {
        all_stats.push_back(stats);
    }
    ++installed_count;
    return true;
}

} // namespace Core
