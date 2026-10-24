// SPDX-License-Identifier: GPL-2.0-or-later
// CPUID and XGETBV for ahead-of-time code without FEX (BB_AOT_ONLY): the values FEXCore's CPUID model (FEXCore/Source/Interface/Core/CPUID.cpp,
// the Intel/Skylake identity, FEX 2609.1) gives the game today on the Thor, with the host features fixed to the profile the units are lifted for
// (tools/recomp/src/lifter.cpp makeHost: AVX through FEX's AVX128 path, AES, PMULL, CRC32, SHA, SSE4a, 3DNow, no RNG, no SVE256). Leaves FEX answers
// with zeros stay zero. Pure C++: tests/test_aot_cpuid.cpp checks it on the PC.
//
// Not reproduced: the per-CPU product name (FEX reads the MIDR of the core: "Unknown ARM CPU" here) and the hybrid-core leaf 0x1A (FEX hides hybrid).
#ifndef BB_GUEST_CPU_CPUID_H
#define BB_GUEST_CPU_CPUID_H
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace bbaot {

struct CpuidHost {
    uint32_t cores = 8;          // logical CPUs of the host (FEX: HostFeatures.CPUMIDRs.size())
    uint32_t cpu = 0;            // the CPU the code runs on now (the local APIC id)
    uint32_t tsc_hz = 0;         // cycle counter frequency in Hz (leaf 0x15 ECX), 0: not reported
    uint32_t tsc_shift = 0;      // the TSC scale (leaf 0x15 EBX = 1 << tsc_shift)
    bool avx = true, aes = true, aes256 = true, pmull = true, crc = true, sha = true, rand = false, sse4a = true, now3d = true, wfxt = false;
};

struct CpuidResult { uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0; };

inline CpuidResult Cpuid(const CpuidHost& h, uint32_t fn, uint32_t leaf) {
    CpuidResult r;
    auto bit = [](bool v, unsigned n) { return uint32_t(v ? 1u : 0u) << n; };
    constexpr uint32_t Family = 1 | (6u << 4) | (6u << 8) | (0xAu << 16);   // Intel family 6 model 0xA6 stepping 1 (FAMILY_IDENTIFIER)
    switch (fn) {
    case 0x0:
        r.eax = 0x16;
        std::memcpy(&r.ebx, "Genu", 4); std::memcpy(&r.edx, "ineI", 4); std::memcpy(&r.ecx, "ntel", 4);
        break;
    case 0x1:
        r.eax = Family;
        r.ebx = (8u << 8) | (h.cores << 16) | (h.cpu << 24);
        r.ecx = 1u | bit(h.pmull, 1) | (1u << 2) | (1u << 3) | (1u << 8) | (1u << 9) | bit(h.avx, 12) | (1u << 13) | (1u << 19) | bit(h.crc, 20) | (1u << 22) |
                (1u << 23) | bit(h.aes, 25) | bit(h.avx, 26) | bit(h.avx, 27) | bit(h.avx, 28) | bit(h.avx, 29) | bit(h.rand, 30) | (1u << 31);  // (hypervisor bit set: HideHypervisorBit is off)
        r.edx = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x200u | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) |
                (1u << 16) | (1u << 17) | (1u << 19) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 29);
        break;
    case 0x2:
        r.eax = 0x1 | (0x5Au << 8) | (0x03u << 16) | (0x55u << 24);
        r.ebx = 0xE4u | (0xB2u << 8) | (0xF0u << 16);
        r.edx = 0x2Cu | (0x21u << 8) | (0xCAu << 16) | (0x09u << 24);
        break;
    case 0x4: {
        const uint32_t cc = h.cores - 1;
        if (leaf == 0)      { r.eax = 1u | (1u << 5) | (1u << 8) | (cc << 26); r.ebx = 63u | (7u << 22); r.ecx = 63; }               // L1D 32 KB
        else if (leaf == 1) { r.eax = 2u | (1u << 5) | (1u << 8) | (cc << 26); r.ebx = 63u | (7u << 22); r.ecx = 63; }               // L1I 32 KB
        else if (leaf == 2) { r.eax = 3u | (2u << 5) | (1u << 8) | (cc << 26); r.ebx = 63u | (7u << 22); r.ecx = 0x3FF; }            // L2 512 KB
        else if (leaf == 3) { r.eax = 3u | (3u << 5) | (1u << 8) | (cc << 14) | (cc << 26); r.ebx = 63u | (7u << 22); r.ecx = 0x4000; r.edx = 1u << 2; }  // L3 8 MB
        break;
    }
    case 0x6: r.eax = 1u << 2; break;
    case 0x7:
        if (leaf == 0) {
            r.ebx = 1u | bit(h.avx, 3) | bit(h.avx, 5) | (1u << 6) | (1u << 7) | bit(h.avx, 8) | (1u << 9) | (1u << 10) | (1u << 13) | bit(h.rand, 18) | (1u << 19) |
                    (1u << 23) | (1u << 24) | bit(h.sha, 29);
            r.ecx = 1u | bit(h.wfxt, 5) | bit(h.aes256, 9) | bit(h.pmull && h.avx, 10) | (1u << 22) | (1u << 24);
            r.edx = 1u << 4;   // fast short rep mov (hybrid hidden)
        }
        break;
    case 0xD: {
        const uint32_t size = h.avx ? 0x340 : 0x240;
        if (leaf == 0)      { r.eax = 1u | 2u | bit(h.avx, 2); r.ebx = size; r.ecx = size; }
        else if (leaf == 1) { r.eax = 1u; r.ebx = size; }
        else if (leaf == 2) { r.eax = h.avx ? 0x100 : 0; r.ebx = h.avx ? 0x240 : 0; }
        break;
    }
    case 0x15:
        if (h.tsc_hz) { r.eax = 1; r.ebx = 1u << h.tsc_shift; r.ecx = h.tsc_hz; }
        break;
    case 0x40000000:
        r.eax = 0x40000001;
        { static const char id[12] = {'F','E','X','I','F','E','X','I','E','M','U',0}; std::memcpy(&r.ebx, id, 12); }   // EBX, ECX, EDX as FEX copies it
        break;
    case 0x40000001:
        if (leaf == 0) r.eax = 0b0010 | (2u << 16);   // AArch64 host, two sub-leaves
        break;
    case 0x80000000:
        r.eax = 0x8000001F;
        std::memcpy(&r.ebx, "Genu", 4); std::memcpy(&r.edx, "ineI", 4); std::memcpy(&r.ecx, "ntel", 4);
        break;
    case 0x80000001:
        r.eax = Family;
        r.ecx = 1u | (1u << 1) | (1u << 3) | (1u << 5) | bit(h.sse4a, 6) | (1u << 8) | (1u << 17);
        r.edx = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x200u | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) |
                (1u << 16) | (1u << 17) | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 29) |
                bit(h.now3d, 30) | bit(h.now3d, 31);
        break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        static const char name[48] = "Unknown ARM CPU";
        std::memcpy(&r, name + 16 * (fn - 0x80000002), 16);
        break;
    }
    case 0x80000005:
        r.eax = 64u | (255u << 8) | (64u << 16) | (255u << 24);
        r.ebx = r.eax;
        r.ecx = 64u | (1u << 8) | (8u << 16) | (32u << 24);
        r.edx = 64u | (1u << 8) | (4u << 16) | (64u << 24);
        break;
    case 0x80000006:
        r.eax = 1024u | (6u << 12) | (1536u << 16) | (3u << 28);
        r.ebx = 1024u | (6u << 12) | (1536u << 16) | (5u << 28);
        r.ecx = 64u | (1u << 8) | (6u << 12) | (512u << 16);
        r.edx = 64u | (1u << 8) | (6u << 12) | (16u << 18);
        break;
    case 0x80000007: r.eax = 1u << 2; r.edx = 1u << 8; break;
    case 0x80000008:
        r.eax = 48u | (48u << 8);
        r.ecx = (std::bit_ceil(h.cores) << 12) | (h.cores - 1);
        break;
    default: break;   // reserved leaves answer zeros
    }
    return r;
}

// XGETBV with ECX = 0: XCR0 = x87 | SSE (| AVX), as FEX's XCRFunction_0h.
inline uint64_t Xgetbv(const CpuidHost& h, uint64_t function) { return function == 0 ? (h.avx ? 7u : 3u) : 0u; }

}  // namespace bbaot
#endif
