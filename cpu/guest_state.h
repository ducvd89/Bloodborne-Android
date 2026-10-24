// SPDX-License-Identifier: GPL-2.0-or-later
// GuestState: the register file ahead-of-time recompiled code (bb-recomp units) runs on. It is FEXCore's
// CPUState layout (docs/RECOMP_ABI.md), so the offsets the lowering uses (tools/recomp/src/lower_llvm.cpp) apply
// unchanged, and on ARM64 hosts the very state of FEX's thread, which the fallback CPU shares.
//
// FEX builds (BB_AOT_FEX_STATE) alias the real type. Other hosts (the x86-64 PC, where FEX is not part of the
// runtime) use a replica; tests/guest_state_check.cpp compares the two against FEX's headers.
#ifndef BB_GUEST_STATE_H
#define BB_GUEST_STATE_H
#include <cstddef>
#include <cstdint>

#ifdef BB_AOT_FEX_STATE
#include <FEXCore/Core/CoreState.h>
using GuestState = FEXCore::Core::CPUState;
#else
struct alignas(64) GuestState {
    union XMMRegs {
        struct AVX { uint64_t data[16][4]; } avx;
        struct SSE { uint64_t data[16][2]; uint64_t pad[16][2]; } sse;
    };
    uint64_t InlineJITBlockHeader {};
    uint64_t DeferredSignalRefCount {};
    uint32_t pf_raw {1};
    uint32_t af_raw {};
    uint64_t rip {};
    uint64_t gregs[16] {};
    uint64_t L1Pointer {};
    uint64_t L1Mask {};
    uint64_t callret_sp {};
    uint64_t _pad1 {};
    uint64_t avx_high[16][2] {};
    XMMRegs xmm {};
    uint16_t es_idx {}, cs_idx {}, ss_idx {}, ds_idx {};
    uint16_t gs_idx {}, fs_idx {};
    uint32_t mxcsr {};
    uint32_t es_cached {}, cs_cached {}, ss_cached {}, ds_cached {};
    uint64_t gs_cached {};
    uint64_t fs_cached {};
    uint8_t flags[48] {};
    uint64_t mm[8][2] {};
    struct gdt_segment {
        uint16_t Limit0;
        uint16_t Base0;
        uint16_t Base1 : 8;
        uint16_t Type : 4;
        uint16_t S : 1;
        uint16_t DPL : 2;
        uint16_t P : 1;
        uint16_t Limit1 : 4;
        uint16_t AVL : 1;
        uint16_t L : 1;
        uint16_t D : 1;
        uint16_t G : 1;
        uint16_t Base2 : 8;
    };
    gdt_segment* segment_arrays[2] {};
    uint16_t FCW {0x37F};
    uint8_t AbridgedFTW {};
    uint8_t _pad2[5] {};
    gdt_segment private_gdt[32] {};
};
#endif

// x86 register numbers in gregs[] and the flag layout (FEXCore/Core/X86Enums.h).
namespace bbaot {
enum : unsigned { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
// NZCV word in flags[]: N<<31 | Z<<30 | C<<29 | V<<28 (bytes 24..27), DF raw at 10, reserved at 1, IF at 9.
constexpr unsigned FlagNZCV = 24;
// The reset state FEX gives a thread (CPUState constructor, which the replica does not have).
inline void ResetGuestState(GuestState& s) {
    s.flags[1] = 1;      // RFLAG_RESERVED_LOC
    s.flags[9] = 1;      // RFLAG_IF_LOC
    s.flags[10] = 1;     // RFLAG_DF_RAW_LOC: DF clear
    s.flags[27] = 1 << 5; // RFLAG_NZCV_3_LOC: bit 29, carry (inverted inside the JIT)
    s.mxcsr = 0x1F80;
    s.pf_raw = 1;
    s.segment_arrays[0] = s.private_gdt;
    s.segment_arrays[1] = s.private_gdt;
    s.cs_idx = 6 << 3;
    auto* code = &s.private_gdt[6];
    code->Limit0 = 0xFFFF;
    code->Limit1 = 0xF;
    code->L = 1;
    s.cs_cached = 0;
}
} // namespace bbaot
#endif
