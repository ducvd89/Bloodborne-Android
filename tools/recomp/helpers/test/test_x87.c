// Checks the bbr_x87_* conversion helpers against the x87 unit of the CPU this runs on (x86-64 only): random and edge-case
// 80-bit, f64 and f32 values, the four rounding modes. A difference is not necessarily a bug (FEX's SoftFloat is the specification,
// the hardware is the oracle for everything FEX models): the NaN payload and the "integer indefinite" cases are listed apart.
//   clang -O1 -I.. test_x87.c ../bbr_x87_all.c -lm -o /tmp/test_x87 && /tmp/test_x87
#include "bbr_x87.h"

#include <inttypes.h>
#include <stdalign.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { alignas(16) uint8_t b[10]; } f80;
static uint8_t state[2048]; // stands in for the guest's CPUState: FCW and the IE flag at the offsets the helpers use

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void set_fcw(unsigned rc) {
  const uint16_t fcw = (uint16_t)(0x37f | (rc << 10));
  memcpy(state + BBR_X87_FCW_OFFSET, &fcw, 2);
  __asm__ volatile("fldcw %0" : : "m"(fcw));
}
static f80 mk(uint64_t sig, uint16_t se) { f80 v; memcpy(v.b, &sig, 8); memcpy(v.b + 8, &se, 2); return v; }

static uint64_t hw_f80_to_f64(f80 v, uint16_t* sw) { uint64_t r; __asm__ volatile("fnclex; fldt %2; fstpl %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(v) : "st"); return r; }
static uint32_t hw_f80_to_f32(f80 v, uint16_t* sw) { uint32_t r; __asm__ volatile("fnclex; fldt %2; fstps %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(v) : "st"); return r; }
static f80 hw_f64_to_f80(uint64_t d, uint16_t* sw) { f80 r; __asm__ volatile("fnclex; fldl %2; fstpt %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(d) : "st"); return r; }
static f80 hw_f32_to_f80(uint32_t d, uint16_t* sw) { f80 r; __asm__ volatile("fnclex; flds %2; fstpt %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(d) : "st"); return r; }
static int64_t hw_f80_to_i64(f80 v, uint16_t* sw) { int64_t r; __asm__ volatile("fnclex; fldt %2; fistpll %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(v) : "st"); return r; }
static int32_t hw_f80_to_i32(f80 v, uint16_t* sw) { int32_t r; __asm__ volatile("fnclex; fldt %2; fistpl %0; fnstsw %1" : "=m"(r), "=a"(*sw) : "m"(v) : "st"); return r; }

static f80 rand_f80(void) {
  const uint64_t k = rnd() % 16;
  uint16_t exp;
  switch (k) {
  case 0: exp = 0; break;                                     // zero / denormal
  case 1: exp = 0x7fff; break;                                // inf / nan
  case 2: exp = (uint16_t)(0x3fff + (int)(rnd() % 80) - 40); break;
  case 3: exp = (uint16_t)(0x3c00 + rnd() % 0x20); break;     // f64 denormal range edge
  case 4: exp = (uint16_t)(0x407e + rnd() % 4); break;        // f64 overflow edge
  case 5: exp = (uint16_t)(0x3f80 + rnd() % 0x20); break;     // f32 denormal edge
  case 6: exp = (uint16_t)(0x407e - 2 + rnd() % 4); break;
  default: exp = (uint16_t)(0x3fff + (int)(rnd() % 130) - 65); break;
  }
  uint64_t sig = rnd();
  if (exp != 0 && k != 1 && (rnd() & 7)) sig |= 1ull << 63; // normalised mostly (pseudo-denormals and unnormals stay rare)
  if (k == 1 && (rnd() & 1)) sig = 0x8000000000000000ull | (rnd() & 0xff);
  return mk(sig, (uint16_t)(exp | ((rnd() & 1) << 15)));
}

static unsigned fails[16], runs[16], nanfail[16];
static const char* names[] = {"f80->f64", "f80->f32", "f64->f80", "f32->f80", "f80->i64", "f80->i32", "f80->i64 trunc"};
static void note(int id, int ok, int isnan, const char* what, uint64_t a, uint64_t b, uint16_t se) {
  ++runs[id];
  if (ok) return;
  if (isnan) { ++nanfail[id]; return; }
  if (++fails[id] <= 3) printf("MISMATCH %s %s: input %04x, helper %016" PRIx64 ", x87 %016" PRIx64 "\n", names[id], what, se, a, b);
}

int main(void) {
  for (unsigned rc = 0; rc < 4; ++rc) {
    set_fcw(rc);
    for (int i = 0; i < 400000; ++i) {
      uint16_t sw;
      f80 v = rand_f80();
      uint64_t sig; uint16_t se;
      memcpy(&sig, v.b, 8); memcpy(&se, v.b + 8, 2);
      const int nan = (se & 0x7fff) == 0x7fff && (sig << 1) != 0, snan = nan && !(sig & (1ull << 62)), unnormal = (se & 0x7fff) != 0 && !(sig >> 63);
      char what[32]; snprintf(what, sizeof what, "rc=%u", rc);
      state[BBR_X87_IE_OFFSET] = 0;
      double d = bbr_x87_f80_to_f64(state, sig, se); uint64_t dh; memcpy(&dh, &d, 8);
      uint64_t dx = hw_f80_to_f64(v, &sw);
      note(0, dh == dx && ((sw & 1) == state[BBR_X87_IE_OFFSET] || unnormal), nan || unnormal, what, dh, dx, se);
      state[BBR_X87_IE_OFFSET] = 0;
      float f = bbr_x87_f80_to_f32(state, sig, se); uint32_t fh; memcpy(&fh, &f, 4);
      uint32_t fx = hw_f80_to_f32(v, &sw);
      note(1, fh == fx, nan || unnormal, what, fh, fx, se);
      // f64 -> f80 (exact), f32 -> f80
      uint64_t din = rnd(); if (rnd() & 1) din = (din & 0x800fffffffffffffull) | ((uint64_t)(rnd() % 2047) << 52);
      state[BBR_X87_IE_OFFSET] = 0;
      bbr_f80 r = bbr_x87_f64_to_f80(state, *(double*)&din);
      f80 x = hw_f64_to_f80(din, &sw); uint64_t xs; uint16_t xe; memcpy(&xs, x.b, 8); memcpy(&xe, x.b + 8, 2);
      note(2, r.sig == xs && r.top == xe, (din >> 52 & 0x7ff) == 0x7ff && (din << 12) != 0, what, r.sig, xs, (uint16_t)(din >> 48));
      uint32_t fin = (uint32_t)rnd(); if (rnd() & 1) fin = (fin & 0x807fffffu) | ((uint32_t)(rnd() % 255) << 23);
      float ff; memcpy(&ff, &fin, 4);
      r = bbr_x87_f32_to_f80(state, ff);
      x = hw_f32_to_f80(fin, &sw); memcpy(&xs, x.b, 8); memcpy(&xe, x.b + 8, 2);
      note(3, r.sig == xs && r.top == xe, (fin >> 23 & 0xff) == 0xff && (fin << 9) != 0, what, r.sig, xs, (uint16_t)(fin >> 16));
      // f80 -> integers with the FCW rounding (fist), truncation is not tested against fisttp (SSE3) here
      if (!unnormal && !nan) {
        int64_t h = bbr_x87_f80_to_int(state, sig, se, 8, 0), x64 = hw_f80_to_i64(v, &sw);
        note(4, h == x64, 0, what, (uint64_t)h, (uint64_t)x64, se);
        int64_t h32 = bbr_x87_f80_to_int(state, sig, se, 4, 0); int32_t x32 = hw_f80_to_i32(v, &sw);
        note(5, (int32_t)h32 == x32, 0, what, (uint64_t)h32, (uint64_t)(int64_t)x32, se);
      }
    }
  }
  int bad = 0;
  for (int i = 0; i < 6; ++i) {
    printf("%-16s %8u runs, %u mismatches, %u NaN/unnormal differences (not counted)\n", names[i], runs[i], fails[i], nanfail[i]);
    bad += fails[i] != 0;
  }
  return bad;
}
