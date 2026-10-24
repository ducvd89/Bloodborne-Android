// SPDX-License-Identifier: GPL-2.0-or-later
// Compile-time and start-up check that cpu/guest_state.h's GuestState has the layout of the x86-64 front end's CPUState, which the Rebuild Tool
// (tools/recomp) compiles the units against. The runtime itself contains no FEX code: this check needs a FEX source tree (the tool's, which
// fetches it: tools/recomp/fetch-fex.sh) and is run by tools/check_recomp_abi.sh when BB_FEX_DIR points at one:
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
SAME(_pad1); SAME(mm); SAME(segment_arrays); SAME(FCW); SAME(AbridgedFTW); SAME(private_gdt);
static_assert(sizeof(Fex) == sizeof(Rep) && alignof(Fex) == alignof(Rep));
int main() {
    Fex f; Rep r;
    replica::bbaot::ResetGuestState(r);
    // The constructor leaves the same flags, mxcsr and pf_raw; the segment pointers point into each object's own GDT.
    const bool flags = !std::memcmp(f.flags, r.flags, sizeof(f.flags)) && f.mxcsr == r.mxcsr && f.pf_raw == r.pf_raw && f.FCW == r.FCW;
    std::printf("sizeof %zu, flags/mxcsr/pf %s\n", sizeof(Rep), flags ? "equal" : "DIFFER");
    return flags ? 0 : 1;
}
