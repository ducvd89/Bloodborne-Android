// SPDX-License-Identifier: GPL-2.0-or-later
// gpu/shim/bbport_guest_code_hooks.cpp on an x86-64 host: the trampoline is emitted next to a copy
// of small fake "game" functions and RUN, so the context it builds (every register, flags, MXCSR,
// YMM), what a hook's changes do, the displaced instructions, the skip flag, the refusals and the
// invalidation calls are all checked. Built twice: the hook called directly (the x86-64 runtime)
// and through a host-function thunk (-DBB_GUEST_HOOK_THUNK=1, the FEXCore build's call path, here
// with a guest_cpu_host_function that is the identity, as on an x86-64 host).
//   clang++ -std=c++20 -O1 -g -I src -I gpu/shim tests/test_guest_code_hooks.cpp \
//       gpu/shim/bbport_guest_code_hooks.cpp -lZydis -lZycore -o out/guest-code-hooks-test
#undef NDEBUG
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/mman.h>

#include <Zydis/Zydis.h>

#include "bbport_guest_code_hooks.h"
#include "guest_cpu.h"

#if !defined(__x86_64__)
int main() {
    std::puts("guest code hooks test: not an x86-64 host (nothing to run)");
    return 0;
}
#else

// ---- The guest CPU as seen by the hooks: recorded ----
static std::vector<std::pair<std::uintptr_t, std::size_t>> g_invalidated;
static std::vector<std::pair<std::uintptr_t, std::size_t>> g_code;
static int g_thunks = 0;
extern "C" {
void guest_cpu_code(uintptr_t base, size_t size) { g_code.emplace_back(base, size); }
uintptr_t guest_cpu_host_function(void* fn) {
    ++g_thunks;
    return reinterpret_cast<uintptr_t>(fn);
}
void guest_cpu_invalidate(uintptr_t address, size_t size) { g_invalidated.emplace_back(address, size); }
// An x86-64 host calls the "guest" function itself (src/runtime_cpu.c).
uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t* args) {
    using Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t a[6] = {};
    assert(count <= 6);
    for (unsigned i = 0; i < count; ++i) a[i] = args[i];
    return reinterpret_cast<Fn>(fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
}
}

// ---- Fake game code ----
asm(R"(
.text
.intel_syntax noprefix
.globl t_regs_begin
.globl t_regs_site
.globl t_regs_end
t_regs_begin:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov r15, rdi
    mov [r15 + 0x98], rsp
    movabs rbx, 0x1111111111111111
    movabs r12, 0x2222222222222222
    movabs r13, 0x3333333333333333
    movabs r14, 0x4444444444444444
    movabs rbp, 0x5555555555555555
    movabs rcx, 0x6666666666666666
    movabs rdx, 0x7777777777777777
    movabs rsi, 0x8888888888888888
    movabs rdi, 0x9999999999999999
    movabs r9,  0xbbbbbbbbbbbbbbbb
    movabs r10, 0xcccccccccccccccc
    movabs r11, 0xdddddddddddddddd
    movabs rax, 0xeeeeeeeeeeeeeeee
    vpcmpeqd ymm1, ymm1, ymm1
    vpcmpeqd ymm3, ymm3, ymm3
    stc
t_regs_site:
    lea r8, [rbx + 0x12345678]
    mov [r15 + 0x00], rax
    mov [r15 + 0x08], rcx
    mov [r15 + 0x10], rdx
    mov [r15 + 0x18], rbx
    mov [r15 + 0x28], rbp
    mov [r15 + 0x30], rsi
    mov [r15 + 0x38], rdi
    mov [r15 + 0x40], r8
    mov [r15 + 0x48], r9
    mov [r15 + 0x50], r10
    mov [r15 + 0x58], r11
    mov [r15 + 0x60], r12
    mov [r15 + 0x68], r13
    mov [r15 + 0x70], r14
    mov [r15 + 0xa8], rsp
    stmxcsr [r15 + 0xa0]
    vmovdqu [r15 + 0xc0], ymm1
    vmovdqu [r15 + 0xe0], ymm3
    pushfq
    pop qword ptr [r15 + 0x90]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
t_regs_end:

.globl t_entry_begin
.globl t_entry_end
t_entry_begin:
    push rbp
    mov rbp, rsp
    push r15
    mov eax, 0x11111111
    lea rax, [rax + rdi*2 + 3]
    pop r15
    pop rbp
    ret
t_entry_end:

.globl t_wide_begin
.globl t_wide_end
t_wide_begin:
    movabs rax, 0x0123456789abcdef
    ret
t_wide_end:

.globl t_rel_begin
.globl t_rel_end
t_rel_begin:
    lea rax, [rip + 0x10]
    nop
    ret
t_rel_end:

.globl t_float_begin
.globl t_float_end
t_float_begin:
    lea rax, [rsp + 8]
    test al, 8                  # entry: rsp + 8 is 16-aligned
    jz 1f
    mov eax, 0xdead
    ret
1:  mov rax, rdi
    imul rax, rax, 37
    add rax, rsi
    imul rax, rax, 37
    add rax, rdx
    imul rax, rax, 37
    add rax, rcx
    imul rax, rax, 37
    add rax, r8
    imul rax, rax, 37
    add rax, r9
    imul rax, rax, 37
    add rax, [rsp + 8]
    imul rax, rax, 37
    add rax, [rsp + 16]
    cvttss2si r10, xmm0
    imul rax, rax, 37
    add rax, r10
    cvttss2si r10, xmm1
    imul rax, rax, 37
    add rax, r10
    cvttss2si r10, xmm2
    imul rax, rax, 37
    add rax, r10
    cvttss2si r10, xmm3
    imul rax, rax, 37
    add rax, r10
    cvttss2si r10, xmm4
    imul rax, rax, 37
    add rax, r10
    cvttss2si r10, xmm7
    imul rax, rax, 37
    add rax, r10
    ret
t_float_end:

.globl t_call_begin
.globl t_call_end
t_call_begin:
    call t_call_begin
    ret
t_call_end:
.att_syntax prefix
)");
extern "C" {
void t_regs_begin();
void t_regs_site();
void t_regs_end();
void t_entry_begin();
void t_entry_end();
void t_wide_begin();
void t_wide_end();
void t_rel_begin();
void t_rel_end();
void t_float_begin();
void t_float_end();
void t_call_begin();
void t_call_end();
}

namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// A copy of [begin, end) in a page of its own (a hook sets its page's protection back to read and
/// execute afterwards).
u8* Copy(void (*begin)(), void (*end)()) {
    const std::size_t size = reinterpret_cast<u8*>(end) - reinterpret_cast<u8*>(begin);
    auto* page = static_cast<u8*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED && size < 2048);
    std::memcpy(page + 64, reinterpret_cast<void*>(begin), size);
    return page + 64;
}

struct Seen {
    int calls = 0;
    u64 tag = 0;
    Core::GuestRegisterSnapshot regs{};
    bool aligned = false;
    bool site_untouched_yet = false;
} seen;

constexpr u64 RegsTag = 0x1234567890abcdefull;

// Changes some registers, as a hook may: rbx (the displaced `lea r8, [rbx + ..]` must see it), r13,
// one YMM byte.
void BB_GUEST_HOOK_ABI RegsHook(u64 tag, const Core::GuestRegisterSnapshot* registers) {
    ++seen.calls;
    seen.tag = tag;
    seen.regs = *registers;
    seen.aligned = reinterpret_cast<std::uintptr_t>(registers) % 16 == 0;
    auto* writable = const_cast<Core::GuestRegisterSnapshot*>(registers);
    writable->rbx += 1;
    writable->r13 = 0x3333333333333334ull;
    writable->ymm[3][5] = 0x5a;
}

bool g_skip_next = false;
u8 g_skip_flag = 0;
int g_entry_calls = 0;
void BB_GUEST_HOOK_ABI EntryHook(u64 tag, const Core::GuestRegisterSnapshot* registers) {
    ++g_entry_calls;
    assert(tag == 7);
    if (g_skip_next) {
        const_cast<Core::GuestRegisterSnapshot*>(registers)->rax = 0x77;
        g_skip_flag = 1;
    }
}

void BB_GUEST_HOOK_ABI NoHook(u64, const Core::GuestRegisterSnapshot*) { assert(!"called"); }

// What the trampoline is: follow the jump at the site and decode it to the jump back.
void CheckTrampolineDecodes(const u8* site, std::size_t span, bool with_skip) {
    assert(site[0] == 0xe9);
    std::int32_t rel;
    std::memcpy(&rel, site + 1, 4);
    const u8* t = site + 5 + rel;
    for (std::size_t i = 5; i < span; ++i) {
        assert(site[i] == 0x90); // the rest of the span is nops
    }
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    std::size_t offset = 0;
    int calls = 0, rets = 0, vmovs = 0;
    bool back = false;
    while (offset < 4096 && !back) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status = ZydisDecoderDecodeFull(&decoder, t + offset, 64, &instruction, operands);
        assert(ZYAN_SUCCESS(status)); // the whole trampoline decodes
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
            ++calls;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) {
            ++rets;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_VMOVDQU) {
            ++vmovs;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP && instruction.length == 5 &&
                   operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            ZyanU64 target = 0;
            ZydisCalcAbsoluteAddress(&instruction, &operands[0],
                                     reinterpret_cast<ZyanU64>(t + offset), &target);
            assert(target == reinterpret_cast<ZyanU64>(site) + span); // back after the span
            back = true;
        }
        offset += instruction.length;
    }
    assert(back);
    assert(calls == 1);
    assert(vmovs == 32);          // 16 YMM registers saved, 16 restored
    assert(rets == (with_skip ? 1 : 0));
}

bool Refused(const u8* before, const u8* after, std::size_t size) {
    return std::memcmp(before, after, size) == 0;
}
} // namespace

int main() {
    using namespace Core;
    std::printf("guest code hooks test: hook %s\n", BB_GUEST_HOOK_THUNK ? "through the host-function thunk"
                                                                       : "called directly");
    assert(GuestCodeProtection() == (PROT_READ | PROT_EXEC));

    // ---- A hook in the middle of a function, every register live ----
    u8* regs_fn = Copy(t_regs_begin, t_regs_end);
    const std::size_t site_offset = reinterpret_cast<u8*>(t_regs_site) - reinterpret_cast<u8*>(t_regs_begin);
    u8* site = regs_fn + site_offset;
    const std::array<u8, 7> site_bytes{0x4c, 0x8d, 0x83, 0x78, 0x56, 0x34, 0x12}; // lea r8, [rbx + 0x12345678]
    assert(std::memcmp(site, site_bytes.data(), 7) == 0);
    using RegsFn = void (*)(u64*);
    std::array<u64, 0x40> out{};

    // Refusals leave the code as it was.
    {
        std::array<u8, 7> wrong = site_bytes;
        wrong[3] ^= 1;
        u8 before[7];
        std::memcpy(before, site, 7);
        assert(!InstallGuestCodeHook(site, wrong, 1, RegsHook));
        assert(Refused(before, site, 7));
        assert(!InstallGuestCodeHook(site, std::span<const u8>{site_bytes.data(), 4}, 1, RegsHook)); // too short
        assert(!InstallGuestCodeHook(site, site_bytes, 1, nullptr));
        assert(GuestCodeHooksInstalled() == 0 && GuestCodeHooksRefused() == 1);
        assert(g_invalidated.empty() && g_code.empty()); // nothing written, nothing mapped
    }
    // A mismatch asks the explainer.
    {
        SetGuestCodeMismatchExplainer([](std::uintptr_t, std::size_t) -> std::string {
            return "explained: patch part test";
        });
        std::array<u8, 7> wrong = site_bytes;
        wrong[0] ^= 0xff;
        assert(!InstallGuestCodeHook(site, wrong, 1, RegsHook));
        SetGuestCodeMismatchExplainer(nullptr);
    }
    // Position-dependent instructions are refused.
    {
        u8* rel_fn = Copy(t_rel_begin, t_rel_end);
        u8 lea_rip[9];
        std::memcpy(lea_rip, rel_fn, 9);
        u8 before[9];
        std::memcpy(before, rel_fn, 9);
        assert(!InstallGuestCodeHook(rel_fn, std::span<const u8>{lea_rip, 9}, 1, NoHook));
        assert(Refused(before, rel_fn, 9));
        u8* call_fn = Copy(t_call_begin, t_call_end);
        u8 call_bytes[6];
        std::memcpy(call_bytes, call_fn, 6);
        assert(!InstallGuestCodeHook(call_fn, std::span<const u8>{call_bytes, 6}, 1, NoHook));
        // A span that ends inside an instruction.
        assert(!InstallGuestCodeHook(rel_fn, std::span<const u8>{lea_rip, 5}, 1, NoHook));
    }
    assert(GuestCodeHooksInstalled() == 0);

    assert(InstallGuestCodeHook(site, site_bytes, RegsTag, RegsHook));
    assert(GuestCodeHooksInstalled() == 1);
    // The invalidation: the site's span and the trampoline.
    {
        bool site_seen = false;
        for (const auto& [address, size] : g_invalidated) {
            site_seen = site_seen || (address == reinterpret_cast<std::uintptr_t>(site) && size == 7);
        }
        assert(site_seen);
        assert(g_code.size() == 1); // the trampoline area is registered as guest code
        assert(BB_GUEST_HOOK_THUNK ? g_thunks == 1 : g_thunks == 0);
    }
    assert(site[0] == 0xe9);
    CheckTrampolineDecodes(site, 7, false);
    // The same site again is a hook on a hook: refused (its bytes are the jump now).
    assert(!InstallGuestCodeHook(site, site_bytes, 1, RegsHook));

    reinterpret_cast<RegsFn>(regs_fn)(out.data());
    assert(seen.calls == 1 && seen.tag == RegsTag && seen.aligned);
    const auto& r = seen.regs;
    assert(r.rax == 0xeeeeeeeeeeeeeeeeull);
    assert(r.rbx == 0x1111111111111111ull);
    assert(r.rcx == 0x6666666666666666ull);
    assert(r.rdx == 0x7777777777777777ull);
    assert(r.rsi == 0x8888888888888888ull);
    assert(r.rdi == 0x9999999999999999ull);
    assert(r.rbp == 0x5555555555555555ull);
    assert(r.r9 == 0xbbbbbbbbbbbbbbbbull);
    assert(r.r10 == 0xccccccccccccccccull);
    assert(r.r11 == 0xddddddddddddddddull);
    assert(r.r12 == 0x2222222222222222ull);
    assert(r.r13 == 0x3333333333333333ull);
    assert(r.r14 == 0x4444444444444444ull);
    assert(r.r15 == reinterpret_cast<u64>(out.data()));
    assert(r.rsp == out[0x98 / 8]);   // the stack pointer the game had at the site
    assert(out[0xa8 / 8] == out[0x98 / 8]); // and has again after it
    assert(r.rflags & 1);             // CF (stc) in the snapshot
    assert(out[0x90 / 8] & 1);        // and after the hook
    assert(r.mxcsr == 0x1f80);
    {
        std::uint32_t mxcsr;
        std::memcpy(&mxcsr, &out[0xa0 / 8], 4);
        assert(mxcsr == 0x1f80);
    }
    for (int i = 0; i < 32; ++i) {
        assert(r.ymm[1][i] == 0xff && r.ymm[3][i] == 0xff); // vpcmpeqd: all ones, upper halves too
    }
    // The hook's changes took effect; the displaced instruction ran on them.
    assert(out[0x18 / 8] == 0x1111111111111112ull);              // rbx
    assert(out[0x40 / 8] == 0x1111111111111112ull + 0x12345678); // r8 = rbx + 0x12345678
    assert(out[0x68 / 8] == 0x3333333333333334ull);              // r13
    {
        u8 y1[32], y3[32];
        std::memcpy(y1, &out[0xc0 / 8], 32);
        std::memcpy(y3, &out[0xe0 / 8], 32);
        for (int i = 0; i < 32; ++i) {
            assert(y1[i] == 0xff);
            assert(y3[i] == (i == 5 ? 0x5a : 0xff));
        }
    }
    // Everything else as the game left it.
    assert(out[0x00 / 8] == 0xeeeeeeeeeeeeeeeeull && out[0x08 / 8] == 0x6666666666666666ull &&
           out[0x10 / 8] == 0x7777777777777777ull && out[0x28 / 8] == 0x5555555555555555ull &&
           out[0x30 / 8] == 0x8888888888888888ull && out[0x38 / 8] == 0x9999999999999999ull &&
           out[0x48 / 8] == 0xbbbbbbbbbbbbbbbbull && out[0x50 / 8] == 0xccccccccccccccccull &&
           out[0x58 / 8] == 0xddddddddddddddddull && out[0x60 / 8] == 0x2222222222222222ull &&
           out[0x70 / 8] == 0x4444444444444444ull);
    // Again: the hook is called each time.
    out.fill(0);
    seen.calls = 0;
    reinterpret_cast<RegsFn>(regs_fn)(out.data());
    assert(seen.calls == 1);

    // ---- A hook at a function entry, with a skip flag ----
    u8* entry_fn = Copy(t_entry_begin, t_entry_end);
    const std::array<u8, 6> entry_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
    assert(std::memcmp(entry_fn, entry_bytes.data(), 6) == 0);
    using EntryFn = std::uint64_t (*)(std::uint64_t);
    assert(reinterpret_cast<EntryFn>(entry_fn)(5) == 0x11111111ull + 10 + 3);
    assert(InstallGuestCodeHook(entry_fn, entry_bytes, 7, EntryHook, &g_skip_flag));
    assert(GuestCodeHooksInstalled() == 2);
    CheckTrampolineDecodes(entry_fn, 6, true);
    // No skip: the original runs.
    assert(reinterpret_cast<EntryFn>(entry_fn)(5) == 0x11111111ull + 10 + 3);
    assert(g_entry_calls == 1);
    // Skip: the function returns at once with the hook's rax; the flag is cleared.
    g_skip_next = true;
    assert(reinterpret_cast<EntryFn>(entry_fn)(5) == 0x77);
    assert(g_skip_flag == 0 && g_entry_calls == 2);
    g_skip_next = false;
    assert(reinterpret_cast<EntryFn>(entry_fn)(100) == 0x11111111ull + 200 + 3);
    assert(g_entry_calls == 3);

    // ---- Overlapping hooks ----
    u8* wide_fn = Copy(t_wide_begin, t_wide_end);
    std::array<u8, 10> wide_bytes;
    std::memcpy(wide_bytes.data(), wide_fn, 10);
    assert(InstallGuestCodeHook(wide_fn, wide_bytes, 1, NoHook));
    const std::array<u8, 5> nops{0x90, 0x90, 0x90, 0x90, 0x90}; // the span's tail after the jump
    assert(std::memcmp(wide_fn + 5, nops.data(), 5) == 0);
    const unsigned refused = GuestCodeHooksRefused();
    assert(!InstallGuestCodeHook(wide_fn + 5, nops, 1, NoHook)); // inside a hook's span
    assert(GuestCodeHooksRefused() == refused + 1);
    assert(GuestCodeHooksInstalled() == 3);

    // ---- A call with float arguments ----
    {
        u8* float_fn = Copy(t_float_begin, t_float_end);
        const std::array<std::uint64_t, 8> ints{1, 2, 3, 4, 5, 6, 7, 8};
        const std::array<float, 6> floats{10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 0.0f};
        const std::uint64_t got = CallGuestWithFloats(reinterpret_cast<std::uintptr_t>(float_fn), ints, floats);
        std::uint64_t want = 0;
        for (const std::uint64_t v : ints) want = want * 37 + v;
        for (const int f : {10, 11, 12, 13, 14}) want = want * 37 + std::uint64_t(f); // xmm0-4 (truncated)
        want = want * 37 + 0; // xmm7: not given, zero
        if (got != want) std::fprintf(stderr, "float call: got %llx want %llx\n", (unsigned long long)got, (unsigned long long)want);
        assert(got == want);
        // Fewer arguments: the rest is zero.
        const std::array<std::uint64_t, 2> two{9, 9};
        const std::uint64_t small = CallGuestWithFloats(reinterpret_cast<std::uintptr_t>(float_fn), two, {});
        std::uint64_t want2 = 0;
        for (const std::uint64_t v : {9ull, 9ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull}) want2 = want2 * 37 + v;
        for (int i = 0; i < 6; ++i) want2 = want2 * 37;
        assert(small == want2);
    }

    // ---- Code writes and protection ----
    g_invalidated.clear();
    u8 patch[3] = {0x90, 0x90, 0x90};
    mprotect(wide_fn - 64, 4096, PROT_READ | PROT_WRITE | PROT_EXEC); // the hook left it read and execute
    WriteGuestCode(wide_fn + 20, patch, 3);
    assert(g_invalidated.size() == 1 && g_invalidated[0].first == reinterpret_cast<std::uintptr_t>(wide_fn + 20) &&
           g_invalidated[0].second == 3);
    GuestCodeWritten(wide_fn, 4);
    assert(g_invalidated.size() == 2);
    KeepGuestCodeWritable();
    assert(GuestCodeProtection() == (PROT_READ | PROT_WRITE | PROT_EXEC));

    std::puts("guest code hooks test: ok");
    return 0;
}
#endif
