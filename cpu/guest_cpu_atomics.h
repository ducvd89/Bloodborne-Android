// SPDX-License-Identifier: GPL-2.0-or-later
// Unaligned x86 `lock` operations in ahead-of-time code (the runtime has no JIT or interpreter to do it).
//
// x86 `lock add [mem]`, `lock cmpxchg`, `xchg [mem]`, `lock cmpxchg16b` accept any alignment. The units use AArch64 LSE atomics
// (LDADDAL, LDCLRAL, LDSETAL, LDEORAL, SWPAL, CASAL, CASPAL ...), which fault with SIGBUS when the access crosses a 16-byte granule
// (FEAT_LSE2 only covers an access inside one). The fault handler decodes the faulting instruction with EmulateAtomic, performs it
// byte-wise under one global lock (atomic against the other emulated accesses, not against aligned atomics on the same bytes: the
// same guarantee as any emulation can give), writes the result registers and advances the pc.
//
// Pure C++ without architecture-specific code: tests/test_aot_atomics.cpp runs it on the x86-64 PC against encoded instruction words.
#ifndef BB_GUEST_CPU_ATOMICS_H
#define BB_GUEST_CPU_ATOMICS_H
#include <atomic>
#include <cstdint>
#include <cstring>

namespace bbaot {

// x[0..30] are the general registers of the interrupted code, `sp` its stack pointer (register number 31 as a base address). Returns false when
// `insn` is not an atomic this knows (the fault is then not ours).
inline bool EmulateAtomic(uint32_t insn, uint64_t* x, uint64_t sp) {
    static std::atomic_flag lock = ATOMIC_FLAG_INIT;
    struct Guard {
        Guard() { while (lock.test_and_set(std::memory_order_acquire)) {} }
        ~Guard() { lock.clear(std::memory_order_release); }
    };
    auto reg = [&](unsigned r) -> uint64_t { return r == 31 ? 0 : x[r]; };           // XZR reads as zero
    auto setreg = [&](unsigned r, uint64_t v) { if (r != 31) x[r] = v; };              // writes to XZR vanish
    const unsigned rt = insn & 31, rn = (insn >> 5) & 31, rs = (insn >> 16) & 31;
    const uint64_t base = rn == 31 ? sp : x[rn];
    auto load = [&](uint64_t a, unsigned bytes) { uint64_t v = 0; std::memcpy(&v, reinterpret_cast<const void*>(a), bytes); return v; };
    auto store = [&](uint64_t a, uint64_t v, unsigned bytes) { std::memcpy(reinterpret_cast<void*>(a), &v, bytes); };
    auto trunc = [](uint64_t v, unsigned bytes) { return bytes == 8 ? v : v & ((uint64_t(1) << (bytes * 8)) - 1); };
    auto sext = [](uint64_t v, unsigned bytes) { const unsigned sh = 64 - bytes * 8; return int64_t(v << sh) >> sh; };

    // CAS, CASA, CASAL, CASL: size 0010 001L 1 Rs o0 11111 Rn Rt
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) {
        const unsigned bytes = 1u << (insn >> 30);
        Guard g;
        const uint64_t old = load(base, bytes), expect = trunc(reg(rs), bytes);
        if (old == expect) store(base, trunc(reg(rt), bytes), bytes);
        setreg(rs, old);
        return true;
    }
    // CASP, CASPA, CASPAL, CASPL: 0 sz 0010 000L 1 Rs o0 11111 Rn Rt (Rs and Rt even: register pairs)
    if ((insn & 0xBFA07C00u) == 0x08207C00u) {
        const unsigned bytes = (insn >> 30 & 1) ? 8 : 4;
        if ((rs & 1) || (rt & 1)) return false;
        Guard g;
        const uint64_t o0 = load(base, bytes), o1 = load(base + bytes, bytes);
        const uint64_t e0 = trunc(reg(rs), bytes), e1 = trunc(reg(rs + 1), bytes);
        if (o0 == e0 && o1 == e1) { store(base, trunc(reg(rt), bytes), bytes); store(base + bytes, trunc(reg(rt + 1), bytes), bytes); }
        setreg(rs, o0);
        setreg(rs + 1, o1);
        return true;
    }
    // LD<op>, ST<op> (Rt = XZR), SWP: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt
    if ((insn & 0x3F200C00u) == 0x38200000u) {
        const unsigned bytes = 1u << (insn >> 30);
        const unsigned o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        if (o3 && opc != 0) return false;   // LDAPR and the like
        Guard g;
        const uint64_t old = load(base, bytes), operand = trunc(reg(rs), bytes);
        uint64_t result;
        if (o3) result = operand;                                            // SWP
        else switch (opc) {
        case 0: result = old + operand; break;                               // LDADD
        case 1: result = old & ~operand; break;                              // LDCLR
        case 2: result = old ^ operand; break;                               // LDEOR
        case 3: result = old | operand; break;                               // LDSET
        case 4: result = sext(old, bytes) > sext(operand, bytes) ? old : operand; break;   // LDSMAX
        case 5: result = sext(old, bytes) < sext(operand, bytes) ? old : operand; break;   // LDSMIN
        case 6: result = old > operand ? old : operand; break;               // LDUMAX
        default: result = old < operand ? old : operand; break;              // LDUMIN
        }
        store(base, trunc(result, bytes), bytes);
        setreg(rt, old);
        return true;
    }
    return false;
}

}  // namespace bbaot
#endif
