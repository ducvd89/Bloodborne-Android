// SPDX-License-Identifier: GPL-2.0-or-later
// The FEX-free pieces of the native-only runtime (BB_AOT_ONLY), on an x86-64 PC: the AArch64 LSE atomic emulator for unaligned x86 `lock` operations
// (cpu/guest_cpu_atomics.h, run against instruction words) and the CPUID/XGETBV model (cpu/guest_cpu_cpuid.h).
//   c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Icpu tests/test_aot_native.cpp -o out/aot-native-test
#undef NDEBUG
#include "../cpu/guest_cpu_atomics.h"
#include "../cpu/guest_cpu_cpuid.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace bbaot;

// Encoders of the instructions the units use (Arm ARM, "Atomic memory operations", "Compare and swap").
static uint32_t Ld(unsigned size, unsigned opc, unsigned o3, unsigned rs, unsigned rn, unsigned rt) {   // LD<op>AL / SWPAL: A = R = 1
    return (size << 30) | 0x38200000u | (1u << 23) | (1u << 22) | (rs << 16) | (o3 << 15) | (opc << 12) | (rn << 5) | rt;
}
static uint32_t Cas(unsigned size, unsigned rs, unsigned rn, unsigned rt) { return (size << 30) | 0x08A00000u | (1u << 22) | (1u << 15) | 0x7C00u | (rs << 16) | (rn << 5) | rt; }   // CASAL
static uint32_t Casp(unsigned sz, unsigned rs, unsigned rn, unsigned rt) { return (sz << 30) | 0x08200000u | (1u << 22) | (1u << 15) | 0x7C00u | (rs << 16) | (rn << 5) | rt; }   // CASPAL

int main() {
    alignas(16) uint8_t mem[64] = {};
    uint64_t x[31] = {};
    auto at = [&](unsigned off) { return reinterpret_cast<uint64_t>(mem + off); };
    auto rd = [&](unsigned off, unsigned n) { uint64_t v = 0; std::memcpy(&v, mem + off, n); return v; };
    auto wr = [&](unsigned off, uint64_t v, unsigned n) { std::memcpy(mem + off, &v, n); };

    // LDADDAL w1, w2, [x3] across a 16-byte boundary (offset 14, four bytes): old value in w2, memory += w1
    wr(14, 0xFFFFFFF0u, 4); x[1] = 0x20; x[3] = at(14); x[2] = 0x1111111111111111ull;
    assert(EmulateAtomic(Ld(2, 0, 0, 1, 3, 2), x, 0));
    assert(x[2] == 0xFFFFFFF0u && rd(14, 4) == 0x10);                       // 32-bit wrap, the old value zero-extended
    // LDADDAL x1, xzr, [x3] (STADD: the old value is discarded)
    wr(8, 5, 8); x[1] = 7; x[3] = at(8);
    assert(EmulateAtomic(Ld(3, 0, 0, 1, 3, 31), x, 0) && rd(8, 8) == 12);
    // LDCLR / LDEOR / LDSET (byte)
    wr(0, 0xF0, 1); x[1] = 0x30; x[3] = at(0);
    assert(EmulateAtomic(Ld(0, 1, 0, 1, 3, 2), x, 0) && x[2] == 0xF0 && rd(0, 1) == 0xC0);        // clear
    assert(EmulateAtomic(Ld(0, 2, 0, 1, 3, 2), x, 0) && rd(0, 1) == 0xF0);                          // eor 0x30
    assert(EmulateAtomic(Ld(0, 3, 0, 1, 3, 2), x, 0) && rd(0, 1) == 0xF0);                          // set 0x30: already set
    // signed and unsigned max/min (halfword)
    wr(2, 0xFFFE, 2); x[1] = 3; x[3] = at(2);                                                       // -2 against 3
    assert(EmulateAtomic(Ld(1, 4, 0, 1, 3, 2), x, 0) && rd(2, 2) == 3);                            // smax -> 3
    wr(2, 0xFFFE, 2);
    assert(EmulateAtomic(Ld(1, 6, 0, 1, 3, 2), x, 0) && rd(2, 2) == 0xFFFE);                       // umax stays
    assert(EmulateAtomic(Ld(1, 7, 0, 1, 3, 2), x, 0) && rd(2, 2) == 3);                            // umin -> 3
    assert(EmulateAtomic(Ld(1, 5, 0, 1, 3, 2), x, 0) && rd(2, 2) == 3);                            // smin 3, 3
    // SWPAL x1, x2, [x3] across a cache line (offset 60 of an 8-byte value needs 68: use 56..63 and a 4-byte one at 62)
    wr(30, 0x1122334455667788ull, 8); x[1] = 0xAABBCCDDEEFF0011ull; x[3] = at(30);
    assert(EmulateAtomic(Ld(3, 0, 1, 1, 3, 2), x, 0) && x[2] == 0x1122334455667788ull && rd(30, 8) == 0xAABBCCDDEEFF0011ull);
    // CASAL w1, w2, [x3]: success and failure
    wr(13, 0xCAFE, 4); x[1] = 0xCAFE; x[2] = 0xBEEF; x[3] = at(13);
    assert(EmulateAtomic(Cas(2, 1, 3, 2), x, 0) && x[1] == 0xCAFE && rd(13, 4) == 0xBEEF);          // compare equal: stored, Rs = old
    x[1] = 0x1234; x[2] = 0x9999;
    assert(EmulateAtomic(Cas(2, 1, 3, 2), x, 0) && x[1] == 0xBEEF && rd(13, 4) == 0xBEEF);          // not equal: unchanged, Rs = current
    // CASPAL x2, x3, x4, x5, [x6]: cmpxchg16b
    wr(24, 1, 8); wr(32, 2, 8); x[2] = 1; x[3] = 2; x[4] = 10; x[5] = 20; x[6] = at(24);
    assert(EmulateAtomic(Casp(1, 2, 6, 4), x, 0) && rd(24, 8) == 10 && rd(32, 8) == 20 && x[2] == 1 && x[3] == 2);
    x[2] = 99; x[3] = 98; x[4] = 7; x[5] = 8;
    assert(EmulateAtomic(Casp(1, 2, 6, 4), x, 0) && rd(24, 8) == 10 && x[2] == 10 && x[3] == 20);   // mismatch: the old pair comes back
    // The stack pointer as a base register (Rn = 31)
    wr(40, 100, 8); x[1] = 1;
    assert(EmulateAtomic(Ld(3, 0, 0, 1, 31, 2), x, at(40)) && rd(40, 8) == 101 && x[2] == 100);
    // Not ours: a plain load, and LDAPR (o3 = 1, opc != 0)
    assert(!EmulateAtomic(0xF9400020u, x, 0));
    assert(!EmulateAtomic(Ld(3, 4, 1, 1, 3, 2), x, 0));

    // ---- CPUID ----
    CpuidHost h; h.cores = 8; h.cpu = 3; h.tsc_hz = 19200000; h.tsc_shift = 6; h.midr = 0x411fd461;   // the Thor: core 0 is a Cortex-A510 r1p1
    auto leaf0 = Cpuid(h, 0, 0);
    char vendor[13] = {}; std::memcpy(vendor, &leaf0.ebx, 4); std::memcpy(vendor + 4, &leaf0.edx, 4); std::memcpy(vendor + 8, &leaf0.ecx, 4);
    assert(!std::strcmp(vendor, "GenuineIntel") && leaf0.eax == 0x16);
    auto l1 = Cpuid(h, 1, 0);
    assert((l1.ebx >> 24) == 3 && ((l1.ebx >> 16) & 0xff) == 8);            // APIC id, logical cores
    assert((l1.ecx & (1u << 28)) && (l1.ecx & (1u << 27)) && (l1.ecx & (1u << 26)) && (l1.ecx & (1u << 25)) && (l1.ecx & (1u << 20)) && (l1.ecx & 1));   // AVX OSXSAVE XSAVE AES SSE4.2 SSE3
    assert(!(l1.ecx & (1u << 30)));                                         // no RDRAND on the Thor
    auto l7 = Cpuid(h, 7, 0);
    assert((l7.ebx & (1u << 5)) && (l7.ebx & (1u << 3)) && (l7.ebx & (1u << 8)));   // AVX2, BMI1, BMI2 (FEX reports them with AVX)
    assert(Cpuid(h, 7, 1).eax == 0 && Cpuid(h, 0x12345678, 0).eax == 0);   // reserved leaves answer zeros
    auto e1 = Cpuid(h, 0x80000001, 0);
    assert((e1.ecx & (1u << 5)) && !(e1.ecx & (1u << 6)) && (e1.edx & (1u << 29)) && (e1.edx & (1u << 27)));   // LZCNT, no SSE4a (FEX does not report it on ARM hosts), long mode, RDTSCP
    auto d0 = Cpuid(h, 0xD, 0);
    assert(d0.eax == 7 && d0.ebx == 0x340);
    auto t15 = Cpuid(h, 0x15, 0);
    assert(t15.eax == 1 && t15.ebx == 64 && t15.ecx == 19200000);   // FEX: ECX is the counter's own frequency, EBX the scale; TSC = ECX * EBX / EAX = 1.2288 GHz
    assert(Xgetbv(h, 0) == 7 && Xgetbv(h, 1) == 0);
    CpuidHost noavx = h; noavx.avx = false;
    assert(Xgetbv(noavx, 0) == 3 && !(Cpuid(noavx, 1, 0).ecx & (1u << 28)));

    // ---- the values FEX gives on the Thor (measured with BB_AOT_CPUID_LOG on the device and read from FEXCore/Source/Interface/Core/CPUID.cpp, FEX 2609.1;
    //      bb-recomp-asmtest --cpuid-diff compares the whole model against FEXCore's own on the PC) ----
    // SSE4a (0x80000001 ECX bit 6): FEX 0. CLZERO (0x80000008 EBX bit 0): FEX 1.
    assert(e1.eax == 0x000A0661 && e1.ecx == 0x0002012B && e1.edx == 0xEFD3FBFF);   // family 6 model 0xA6 stepping 1; LAHF|CMP|ABM|PREFETCHW|TCE; NX, MMXExt, 1G pages, RDTSCP, LM, 3DNow
    auto e8 = Cpuid(h, 0x80000008, 0);
    assert(e8.eax == 0x3030 && e8.ebx == 1 && e8.ecx == ((8u << 12) | 7) && e8.edx == 0);
    CpuidHost noclz = h; noclz.clzero = false;
    assert(Cpuid(noclz, 0x80000008, 0).ebx == 0);
    CpuidHost sse4a = h; sse4a.sse4a = true;
    assert(Cpuid(sse4a, 0x80000001, 0).ecx == 0x0002016B);   // the switch still works, it is off by default
    // Brand string: the product name FEX derives from the MIDR of core 0 (HideHybrid), 16 bytes per leaf, NUL padded.
    auto brand = [](const CpuidHost& host) {
        char b[49] = {};
        for (uint32_t i = 0; i < 3; ++i) { const auto r = Cpuid(host, 0x80000002 + i, 0); std::memcpy(b + 16 * i, &r, 16); }
        return std::string(b, 48);
    };
    auto expect = [](const char* s) { std::string e(s); e.resize(48, '\0'); return e; };
    assert(brand(h) == expect("Cortex-A510"));
    assert(Cpuid(h, 0x80000002, 0).eax == 0x74726F43 && Cpuid(h, 0x80000002, 0).ebx == 0x412D7865 && Cpuid(h, 0x80000002, 0).ecx == 0x00303135 && Cpuid(h, 0x80000002, 0).edx == 0);   // "Cort" "ex-A" "510\0"
    assert(Cpuid(h, 0x80000003, 0).eax == 0 && Cpuid(h, 0x80000004, 0).eax == 0);
    CpuidHost core; core.midr = 0x411fd4e0;  assert(brand(core) == expect("Cortex-X3"));                  // 8 Gen 2 prime core
    core.midr = 0x411fd4d0;                  assert(brand(core) == expect("Cortex-A715"));
    core.midr = 0x411fd470;                  assert(brand(core) == expect("Cortex-A710"));
    core.midr = 0x511f0010;                  assert(brand(core) == expect("Oryon-1"));                      // Qualcomm Oryon: implementer 0x51, part 0x001
    core.midr = 0x610f0230;                  assert(brand(core) == expect("Apple Firestorm (M1)"));
    core.midr = 0x610f0000;                  assert(brand(core) == expect("Apple Silicon"));              // implementer 0x61, part 0: the table's QEmu entry
    core.midr = 0x610f1230;                  assert(brand(core) == expect("Unknown ARM CPU"));            // an Apple part the table does not know
    core.midr = 0x410fd000;                  assert(brand(core) == expect("Unknown ARM CPU"));            // implementer 0x41, part 0
    core.midr = 0;                           assert(brand(core) == expect("Unknown ARM CPU"));            // sysfs unreadable
    core.midr = 0x7f00ffff;                  assert(brand(core) == expect("Unknown ARM CPU"));            // not in the table
    assert(std::string(ProductName(0x411fd900)) == "C1-Premium" && std::string(ProductName(0x411fd8e0)) == "Neoverse N3");
    assert(std::string(ProductName(0x410fd0e0)) == "Cortex-A76AE");
    {   // the table is FEX's: 68 entries, the unknown-CPU catch-all last
        constexpr size_t n = sizeof(kMidrNames) / sizeof(kMidrNames[0]);
        assert(n == 68 && kMidrNames[n - 1].implementer == 0 && kMidrNames[n - 1].part == 0 && std::string(kMidrNames[n - 1].name) == "Unknown ARM CPU");
    }
    // MIDR as sysfs prints it: "0x" + 16 hex digits + newline (FEX reads 18 bytes); only the low 32 bits count
    assert(ParseMidr("0x00000000411fd461\n", 18) == 0x411fd461);
    assert(ParseMidr("0xFFFFFFFF410FD461\n", 18) == 0x410FD461);
    assert(ParseMidr("", 0) == 0 && ParseMidr("0x", 2) == 0 && ParseMidr("zz", 2) == 0 && ParseMidr("0xg", 3) == 0);
    // Hypervisor leaves: the FEX identity and its version string (GIT_DESCRIBE_STRING, 16 bytes per sub-leaf)
    auto hv = Cpuid(h, 0x40000000, 0);
    char hid[13] = {}; std::memcpy(hid, &hv.ebx, 4); std::memcpy(hid + 4, &hv.ecx, 4); std::memcpy(hid + 8, &hv.edx, 4);
    assert(hv.eax == 0x40000001 && !std::strcmp(hid, "FEXIFEXIEMU"));
    assert(Cpuid(h, 0x40000001, 0).eax == (2u | (2u << 16)));
    {
        auto v1 = Cpuid(h, 0x40000001, 1); char v[17] = {}; std::memcpy(v, &v1, 16);
        assert(!std::strcmp(v, "FEX-2609.1") && v1.edx == 0);
        auto v2 = Cpuid(h, 0x40000001, 2);
        assert(v2.eax == 0 && v2.ebx == 0 && v2.ecx == 0 && v2.edx == 0 && Cpuid(h, 0x40000001, 3).eax == 0 && Cpuid(h, 0x40000002, 0).eax == 0);
    }
    // 1 GB page TLBs (0x80000019). The AMD-only leaves 0x80000005 (L1 TLBs and caches) and 0x8000001D (cache parameters) are zero: FEX has the Intel identity.
    auto e19 = Cpuid(h, 0x80000019, 0);
    assert(e19.eax == 0xF040F040 && e19.ebx == 0 && e19.ecx == 0 && e19.edx == 0);
    for (uint32_t l = 0; l < 5; ++l)
        for (uint32_t f : {0x80000005u, 0x8000001Du, 0x8000001Eu, 0x80000020u}) assert(Cpuid(h, f, l).eax == 0 && Cpuid(h, f, l).ebx == 0 && Cpuid(h, f, l).ecx == 0 && Cpuid(h, f, l).edx == 0);
    // Leaves FEX answers with zeros: hybrid (0x1A: hidden), 0x24, the topology and the AMD feature leaves
    for (uint32_t f : {0x3u, 0x5u, 0xAu, 0xBu, 0x10u, 0x14u, 0x16u, 0x1Au, 0x1Fu, 0x24u, 0x80000009u, 0x8000000Au, 0x80000021u})
        assert(Cpuid(h, f, 0).eax == 0 && Cpuid(h, f, 0).ebx == 0 && Cpuid(h, f, 0).ecx == 0 && Cpuid(h, f, 0).edx == 0);
    assert(Cpuid(h, 7, 0).edx == (1u << 4) && !(Cpuid(h, 7, 0).edx & (1u << 15)));   // fast short rep mov; the hybrid bit stays clear
    // The rest of the Thor profile, exactly (the sums of FEX's bit lists with AVX, AES, PMULL, CRC, SHA on and RDRAND, WFxT off)
    assert(Cpuid(h, 1, 0).ecx == 0xBED8330Fu && Cpuid(h, 1, 0).edx == 0x278BFBFFu);
    assert(l7.ebx == 0x218827E9u && l7.ecx == 0x01400601u && l7.edx == 0x10u && l7.eax == 0);
    std::puts("aot native test: ok");
    return 0;
}
