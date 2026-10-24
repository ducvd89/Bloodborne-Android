// SPDX-License-Identifier: GPL-2.0-or-later
// Compile-time and start-up check that cpu/guest_state.h's replica of FEX's CPUState has the same layout:
//   clang++ -std=c++20 -I<FEX>/FEXCore/include -I<FEX> -I<FEX>/External/fmt/include ... tests/guest_state_check.cpp
#include <FEXCore/Core/CoreState.h>
#include <cstdio>
#include <cstring>
namespace replica {
#include "../cpu/guest_state.h"
}
using Fex = FEXCore::Core::CPUState;
using Rep = replica::GuestState;
#define SAME(m) static_assert(offsetof(Fex, m) == offsetof(Rep, m), #m)
SAME(pf_raw); SAME(af_raw); SAME(rip); SAME(gregs); SAME(L1Pointer); SAME(L1Mask); SAME(callret_sp); SAME(avx_high); SAME(xmm);
SAME(es_idx); SAME(cs_idx); SAME(gs_idx); SAME(fs_idx); SAME(mxcsr); SAME(es_cached); SAME(gs_cached); SAME(fs_cached); SAME(flags);
SAME(mm); SAME(segment_arrays); SAME(FCW); SAME(AbridgedFTW); SAME(private_gdt);
static_assert(sizeof(Fex) == sizeof(Rep) && alignof(Fex) == alignof(Rep));
int main() {
    Fex f; Rep r;
    replica::bbaot::ResetGuestState(r);
    // The constructor leaves the same flags, mxcsr and pf_raw; the segment pointers point into each object's own GDT.
    const bool flags = !std::memcmp(f.flags, r.flags, sizeof(f.flags)) && f.mxcsr == r.mxcsr && f.pf_raw == r.pf_raw && f.FCW == r.FCW;
    std::printf("sizeof %zu, flags/mxcsr/pf %s\n", sizeof(Rep), flags ? "equal" : "DIFFER");
    return flags ? 0 : 1;
}
