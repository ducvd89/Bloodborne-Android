/* guest_cpu.h crossings with hand-assembled x86-64 guest code (any host). */
#define _GNU_SOURCE
#include "guest_cpu.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void *map_low(size_t size, int prot) {
    static uintptr_t next = 0x0900000000ull;
    void *p = mmap((void *)next, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    next += (size + 0xffff) & ~(uintptr_t)0xffff;
    return p == MAP_FAILED ? NULL : p;
}

static uintptr_t doubler;
static ABI uint64_t host_sum(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f + 1000000 * g + 10000000 * h;
}
static ABI uint64_t host_nested(uint64_t x) {
    uint64_t arg = x + 1;
    return guest_cpu_call(doubler, 1, &arg) + 1;
}
static ABI double host_scale(double x, int64_t k) { return x * (double)k; }

int main(void) {
    guest_cpu_init(map_low);
    unsigned char *code = map_low(65536, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!code) { puts("FAIL: no guest code memory"); return 1; }
    guest_cpu_code((uintptr_t)code, 65536);
    unsigned char *p = code;
    /* doubler(x) = 2x */
    doubler = (uintptr_t)p;
    static const unsigned char dbl[] = {0x48, 0x8d, 0x04, 0x3f, 0xc3};
    memcpy(p, dbl, sizeof(dbl)); p += 16;
    /* calls_sum(x) = host_sum(x, 2, 3, 4, 5, 6, 7, 8) + 1 */
    uintptr_t calls_sum = (uintptr_t)p;
    uintptr_t sum = guest_cpu_host_function((void *)host_sum);
    static const unsigned char cs1[] = {0x48, 0x83, 0xec, 0x08, 0x6a, 0x08, 0x6a, 0x07,
        0xbe, 2, 0, 0, 0, 0xba, 3, 0, 0, 0, 0xb9, 4, 0, 0, 0, 0x41, 0xb8, 5, 0, 0, 0, 0x41, 0xb9, 6, 0, 0, 0, 0x48, 0xb8};
    static const unsigned char cs2[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x18, 0x48, 0x83, 0xc0, 0x01, 0xc3};
    memcpy(p, cs1, sizeof(cs1)); p += sizeof(cs1);
    memcpy(p, &sum, 8); p += 8;
    memcpy(p, cs2, sizeof(cs2)); p += sizeof(cs2);
    p = code + 128;
    /* calls_nested(x) = host_nested(x) (jmp: tail call) */
    uintptr_t calls_nested = (uintptr_t)p;
    uintptr_t nested = guest_cpu_host_function((void *)host_nested);
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &nested, 8); p += 8; *p++ = 0xff; *p++ = 0xe0;
    p = code + 192;
    /* read_gs() = gs:[0] */
    uintptr_t read_gs = (uintptr_t)p;
    static const unsigned char rg[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
    memcpy(p, rg, sizeof(rg));
    p = code + 256;
    /* scaled(x) = (uint64_t)host_scale((double)x, 3): cvtsi2sd xmm0, rdi; mov edi, 3; call; cvttsd2si rax, xmm0 */
    uintptr_t scaled = (uintptr_t)p;
    uintptr_t scale = guest_cpu_host_function((void *)host_scale);
    static const unsigned char sc1[] = {0x48, 0x83, 0xec, 0x08, 0xf2, 0x48, 0x0f, 0x2a, 0xc7, 0xbf, 3, 0, 0, 0, 0x48, 0xb8};
    static const unsigned char sc2[] = {0xff, 0xd0, 0xf2, 0x48, 0x0f, 0x2c, 0xc0, 0x48, 0x83, 0xc4, 0x08, 0xc3};
    memcpy(p, sc1, sizeof(sc1)); p += sizeof(sc1);
    memcpy(p, &scale, 8); p += 8;
    memcpy(p, sc2, sizeof(sc2));
    p = code + 320;
    /* tsc() = rdtsc as edx:eax */
    uintptr_t tsc = (uintptr_t)p;
    static const unsigned char ts[] = {0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0xc3};
    memcpy(p, ts, sizeof(ts));

    uint64_t arg = 21;
    CHECK(guest_cpu_call(doubler, 1, &arg) == 42);
    arg = 1;
    CHECK(guest_cpu_call(calls_sum, 1, &arg) == 87654322);
    uint64_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(guest_cpu_call(sum, 8, eight) == 87654321);
    arg = 4;
    CHECK(guest_cpu_call(calls_nested, 1, &arg) == 11);
    static uint64_t tcb[4];
    tcb[0] = 0x1234;
    guest_cpu_set_gs(tcb);
    CHECK(guest_cpu_call(read_gs, 0, NULL) == 0x1234);
    arg = 7;
    CHECK(guest_cpu_call(scaled, 1, &arg) == 21);
    uint64_t t0 = guest_cpu_tsc(), guest = guest_cpu_call(tsc, 0, NULL), t1 = guest_cpu_tsc();
    CHECK(t0 <= guest && guest <= t1);
    CHECK(guest_cpu_tsc_frequency() >= 1000000000ull);
    printf("guest CPU test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
