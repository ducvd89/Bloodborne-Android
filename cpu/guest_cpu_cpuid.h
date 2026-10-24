// SPDX-License-Identifier: GPL-2.0-or-later
// CPUID and XGETBV for ahead-of-time code: exactly the values the FEX-Emu CPUID model gave the game on the Thor (the values the game was verified with)
// (FEXCore/Source/Interface/Core/CPUID.cpp, the Intel/Skylake identity, FEX 2609.1, default configuration: HideHybrid on, hypervisor bit shown, no atomic
// memcpy TSO), with the host features of the Thor build: AVX through FEX's AVX128 path, AES, PMULL, CRC32, SHA, 3DNow, no RNG, no SVE256, no WFxT; SSE4a
// is NOT reported (FEX's HostFeatures never sets it on ARM hosts) and CLZERO is (DC ZVA block of 64 bytes, set from DCZID_EL0). Every leaf FEX answers is
// here, with FEX's formulas: 0 1 2 4 6 7 0xD 0x15, the FEX hypervisor leaves 0x40000000 and 0x40000001 (sub-leaf 1 holds the FEX version string), and
// 0x80000000..4, 0x80000006..8 and 0x80000019. Everything else answers zeros, as in FEX: leaf 0x1A (hybrid cores are hidden), and the AMD-only leaves
// 0x80000005 and 0x8000001D (FEX is built with the Intel identity: CPUID_AMD is not defined).
// The brand string (0x80000002..4) is the product name FEX derives from the MIDR of core 0 (HideHybrid makes every core report core 0's name); the name
// table below is ported from FEX-Emu (MIT licence, Copyright (c) 2019 Ryan Houdek; notice in the app's THIRD_PARTY_LICENSES.txt). tests/test_aot_native.cpp pins all of it.
// Pure C++, no host calls.
#ifndef BB_GUEST_CPU_CPUID_H
#define BB_GUEST_CPU_CPUID_H
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bbaot {

struct CpuidHost {
    uint32_t cores = 8;          // logical CPUs of the host (FEX: HostFeatures.CPUMIDRs.size())
    uint32_t cpu = 0;            // the CPU the code runs on now (the local APIC id)
    uint32_t tsc_hz = 0;         // the cycle counter's own frequency in Hz (CNTFRQ_EL0, NOT scaled; leaf 0x15 ECX), 0: not reported
    uint32_t tsc_shift = 0;      // the TSC scale (leaf 0x15 EBX = 1 << tsc_shift): the counter is shifted left by it to reach 1 GHz
    uint32_t midr = 0;           // MIDR_EL1 of core 0 (sysfs; 0 when unreadable): the brand string
    bool avx = true, aes = true, aes256 = true, pmull = true, crc = true, sha = true, rand = false, sse4a = false, now3d = true, wfxt = false, clzero = true;
};

// FEX's CPUMIDRs table (CPUID.cpp SetupHostHybridFlag), in its order; the first entry matching implementer (MIDR[31:24]) and part (MIDR[15:4]) wins. The last
// entry matches an unreadable MIDR (0). Hybrid grouping is not modelled (FEX hides it by default).
struct MidrName { uint8_t implementer; uint16_t part; const char* name; };
inline constexpr MidrName kMidrNames[] = {
    {0x51, 0x002, "Oryon-3"},
    {0x51, 0x001, "Oryon-1"},
    {0x61, 0x039, "Apple Avalanche (M2 Max)"},
    {0x61, 0x035, "Apple Avalanche (M2 Pro)"},
    {0x61, 0x033, "Apple Avalanche (M2)"},
    {0x61, 0x029, "Apple Firestorm (M1 Max)"},
    {0x61, 0x025, "Apple Firestorm (M1 Pro)"},
    {0x61, 0x023, "Apple Firestorm (M1)"},
    {0x61, 0, "Apple Silicon"},
    {0x41, 0xd8c, "C1-Ultra"},
    {0x41, 0xd90, "C1-Premium"},
    {0x41, 0xd8b, "C1-Pro"},
    {0x41, 0xd85, "Cortex-X925"},
    {0x41, 0xd87, "Cortex-A725"},
    {0x41, 0xd84, "Neoverse V3"},
    {0x41, 0xd83, "Neoverse V3AE"},
    {0x41, 0xd8e, "Neoverse N3"},
    {0x41, 0xd82, "Cortex-X4"},
    {0x41, 0xd81, "Cortex-A720"},
    {0x41, 0xd4e, "Cortex-X3"},
    {0x41, 0xd4d, "Cortex-A715"},
    {0x41, 0xd4f, "Neoverse V2"},
    {0x41, 0xd4b, "Cortex-A78C"},
    {0x41, 0xd4a, "Neoverse E1"},
    {0x41, 0xd49, "Neoverse N2"},
    {0x41, 0xd48, "Cortex-X2"},
    {0x41, 0xd47, "Cortex-A710"},
    {0x41, 0xd4C, "Cortex-X1C"},
    {0x41, 0xd44, "Cortex-X1"},
    {0x41, 0xd42, "Cortex-A78AE"},
    {0x41, 0xd41, "Cortex-A78"},
    {0x41, 0xd40, "Neoverse V1"},
    {0x41, 0xd0e, "Cortex-A76AE"},
    {0x41, 0xd0d, "Cortex-A77"},
    {0x41, 0xd0c, "Neoverse N1"},
    {0x41, 0xd0b, "Cortex-A76"},
    {0x51, 0x804, "Kryo 4xx/5xx"},
    {0x41, 0xd0a, "Cortex-A75"},
    {0x51, 0x802, "Kryo 3xx"},
    {0x41, 0xd09, "Cortex-A73"},
    {0x51, 0x800, "Kryo 2xx"},
    {0x41, 0xd08, "Cortex-A72"},
    {0xc0, 0xac3, "AmpereOne"},
    {0xc0, 0xac4, "AmpereOneA"},
    {0xc0, 0xac5, "AmpereOneB"},
    {0xc0, 0xac7, "AmpereOneC"},
    {0x4e, 0x010, "Nvidia Olympus"},
    {0x4e, 0x004, "Nvidia Carmel"},
    {0x4e, 0x003, "Nvidia Denver"},
    {0x41, 0xd07, "Cortex-A57"},
    {0x61, 0x038, "Apple Blizzard (M2 Max)"},
    {0x61, 0x034, "Apple Blizzard (M2 Pro)"},
    {0x61, 0x032, "Apple Blizzard (M2)"},
    {0x61, 0x028, "Apple Icestorm (M1 Max)"},
    {0x61, 0x024, "Apple Icestorm (M1 Pro)"},
    {0x61, 0x022, "Apple Icestorm (M1)"},
    {0x41, 0xd8a, "C1-Nano"},
    {0x41, 0xd80, "Cortex-A520"},
    {0x41, 0xd46, "Cortex-A510"},
    {0x41, 0xd06, "Cortex-A65"},
    {0x41, 0xd05, "Cortex-A55"},
    {0x51, 0x805, "Kryo 4xx/5xx S"},
    {0x51, 0x803, "Kryo 3xx S"},
    {0x41, 0xd03, "Cortex-A53"},
    {0x51, 0x801, "Kryo 2xx S"},
    {0x41, 0xd04, "Cortex-A35"},
    {0x41, 0, "Unknown ARM CPU"},
    {0x0, 0, "Unknown ARM CPU"},
};

// MIDR_EL1 from the 18 bytes of sysfs ("0x" and 16 hex digits, FEX reads exactly that many): the low 32 bits, as FEX truncates them; 0 when it does not parse.
inline uint32_t ParseMidr(const char* data, size_t size) {
    if (size < 3) return 0;
    uint64_t v = 0;
    size_t digits = 0;
    for (size_t i = 2; i < size; ++i, ++digits) {
        const char c = data[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = (v << 4) | uint64_t(d);
    }
    return digits ? uint32_t(v) : 0;
}

inline const char* ProductName(uint32_t midr) {
    const uint8_t implementer = uint8_t(midr >> 24);
    const uint16_t part = uint16_t((midr >> 4) & 0xFFF);
    for (const MidrName& m : kMidrNames)
        if (m.implementer == implementer && m.part == part) return m.name;
    return "Unknown ARM CPU";
}

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
        else if (leaf == 1 || leaf == 2) {            // FEX's version string (GIT_DESCRIBE_STRING, NUL included), 16 bytes per sub-leaf
            static constexpr char version[] = "FEX-2609.1";
            const size_t size = sizeof(version), skip = leaf == 1 ? 0 : 16;
            if (size > skip) std::memcpy(&r, version + skip, std::min<size_t>(size - skip, sizeof(r)));
        }
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
        const char* name = ProductName(h.midr);
        const size_t length = std::strlen(name), skip = 16 * (fn - 0x80000002);
        if (length > skip) std::memcpy(&r, name + skip, std::min<size_t>(length - skip, sizeof(r)));
        break;
    }
    case 0x80000006:
        r.eax = 1024u | (6u << 12) | (1536u << 16) | (3u << 28);
        r.ebx = 1024u | (6u << 12) | (1536u << 16) | (5u << 28);
        r.ecx = 64u | (1u << 8) | (6u << 12) | (512u << 16);
        r.edx = 64u | (1u << 8) | (6u << 12) | (16u << 18);
        break;
    case 0x80000007: r.eax = 1u << 2; r.edx = 1u << 8; break;
    case 0x80000008:
        r.eax = 48u | (48u << 8);
        r.ebx = bit(h.clzero, 0);
        r.ecx = (std::bit_ceil(h.cores) << 12) | (h.cores - 1);
        break;
    case 0x80000019:   // 1 GB page TLBs
        r.eax = (0xFu << 28) | (64u << 16) | (0xFu << 12) | 64u;
        break;
    default: break;   // reserved leaves answer zeros
    }
    return r;
}

// XGETBV with ECX = 0: XCR0 = x87 | SSE (| AVX), as FEX's XCRFunction_0h.
inline uint64_t Xgetbv(const CpuidHost& h, uint64_t function) { return function == 0 ? (h.avx ? 7u : 3u) : 0u; }

}  // namespace bbaot
#endif
