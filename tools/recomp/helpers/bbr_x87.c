// x87 helpers for the lowered code, see bbr_x87.h. Adapted from FEX 2609.1 (MIT, Copyright (c) the FEX-Emu authors):
// FEXCore/Source/Interface/Core/Interpreter/Fallbacks/F80Fallbacks.h and FEXCore/Source/Common/SoftFloat.h, rewritten as C
// over the SoftFloat 3e that FEX bundles (BSD-3, John R. Hauser; subset in softfloat/).
#include "bbr_x87.h"

#include <math.h>
#include <string.h>

#include "softfloat/platform.h"
#include "softfloat/softfloat.h"

#define FCW_OF(s) (*(const uint16_t*)((const uint8_t*)(s) + BBR_X87_FCW_OFFSET))
#define IE_OF(s) (*((uint8_t*)(s) + BBR_X87_IE_OFFSET))

// FEXCore::CPU::SoftFloatStateFromFCW
static void stateFromFCW(struct softfloat_state* st, uint16_t fcw) {
  memset(st, 0, sizeof *st);
  st->detectTininess = softfloat_tininess_afterRounding;
  st->exceptionFlags = 0;
  st->roundingPrecision = 80;
  switch ((fcw >> 8) & 3) {
  case 0: st->roundingPrecision = 32; break;
  case 2: st->roundingPrecision = 64; break;
  case 3: st->roundingPrecision = 80; break;
  default: break; // 1 is invalid (FEX asserts in debug builds, keeps 80 otherwise)
  }
  switch ((fcw >> 10) & 3) {
  case 0: st->roundingMode = softfloat_round_near_even; break;
  case 1: st->roundingMode = softfloat_round_min; break;
  case 2: st->roundingMode = softfloat_round_max; break;
  default: st->roundingMode = softfloat_round_minMag; break;
  }
}

// ScopedSoftFloatState's destructor (HandleX87Exception): the invalid-operation flag becomes the guest's IE flag
static void finish(const struct softfloat_state* st, void* s) {
  if (st->exceptionFlags & softfloat_flag_invalid) IE_OF(s) = 1;
}

static extFloat80_t mk(uint64_t sig, uint64_t top) {
  extFloat80_t v;
  v.signif = sig;
  v.signExp = (uint16_t)top;
  return v;
}

static bbr_f80 ret(extFloat80_t v) {
  bbr_f80 r = {v.signif, v.signExp};
  return r;
}

double bbr_x87_f80_to_f64(void* s, uint64_t sig, uint64_t top) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  const float64_t r = extF80_to_f64(&st, mk(sig, top));
  finish(&st, s);
  double d;
  memcpy(&d, &r, sizeof d);
  return d;
}

float bbr_x87_f80_to_f32(void* s, uint64_t sig, uint64_t top) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  const float32_t r = extF80_to_f32(&st, mk(sig, top));
  finish(&st, s);
  float f;
  memcpy(&f, &r, sizeof f);
  return f;
}

bbr_f80 bbr_x87_f64_to_f80(void* s, double v) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  float64_t in;
  memcpy(&in, &v, sizeof in);
  const bbr_f80 r = ret(f64_to_extF80(&st, in));
  finish(&st, s);
  return r;
}

bbr_f80 bbr_x87_f32_to_f80(void* s, float v) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  float32_t in;
  memcpy(&in, &v, sizeof in);
  const bbr_f80 r = ret(f32_to_extF80(&st, in));
  finish(&st, s);
  return r;
}

// OpHandlers<IR::OP_F80CVTINT>: handle2/4/8 round with the FCW mode (ToI16 goes through the 32-bit conversion and gives the
// 16-bit indefinite value on overflow), handle2t/4t/8t (Truncate) round toward zero.
int64_t bbr_x87_f80_to_int(void* s, uint64_t sig, uint64_t top, uint64_t size, uint64_t truncate) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  const extFloat80_t v = mk(sig, top);
  const uint_fast8_t mode = truncate ? softfloat_round_minMag : st.roundingMode;
  int64_t r;
  if (size == 8) {
    r = extF80_to_i64(&st, v, mode, false);
  } else {
    const int32_t i = (int32_t)extF80_to_i32(&st, v, mode, false);
    r = size == 2 ? ((i > INT16_MAX || i < INT16_MIN) ? INT16_MIN : i) : i;
  }
  finish(&st, s);
  return r;
}

// OpHandlers<IR::OP_F80CVTTOINT> (no state: the conversion is exact)
bbr_f80 bbr_x87_int_to_f80(void* s, int64_t v, uint64_t size) {
  (void)s;
  return ret(i32_to_extF80(size == 2 ? (int32_t)(int16_t)v : (int32_t)v));
}

bbr_f80 bbr_x87_bcd_load(void* s, uint64_t sig, uint64_t top) {
  (void)s;
  uint8_t src[16];
  memset(src, 0, sizeof src);
  memcpy(src, &sig, 8);
  memcpy(src + 8, &top, 2);
  uint64_t bcd = 0;
  // 9 bytes of two digits, most significant byte first; A-F nibbles give undefined data like on FEX
  for (int i = 0; i < 9; ++i) {
    const uint8_t digit = src[8 - i];
    bcd *= 100;
    bcd += (digit >> 4) * 10;
    bcd += digit & 0xF;
  }
  const int negative = (src[9] & 0x80) != 0;
  extFloat80_t t = ui64_to_extF80(bcd);
  t.signExp = (uint16_t)((t.signExp & 0x7FFF) | (negative ? 0x8000 : 0));
  return ret(t);
}

bbr_f80 bbr_x87_bcd_store(void* s, uint64_t sig, uint64_t top) {
  struct softfloat_state st;
  stateFromFCW(&st, FCW_OF(s));
  const int negative = (top >> 15) & 1;
  extFloat80_t v = extF80_roundToInt(&st, mk(sig, top), st.roundingMode, false);
  v.signExp &= 0x7FFF; // clear the sign
  uint64_t tmp = (uint64_t)extF80_to_i64(&st, v, st.roundingMode, false);
  uint8_t bcd[16];
  memset(bcd, 0, sizeof bcd);
  for (size_t i = 0; i < 9; ++i) {
    if (tmp == 0) break;
    const uint8_t digit = (uint8_t)(tmp % 100);
    tmp /= 100;
    bcd[i] = (uint8_t)(((digit / 10) << 4) | (digit % 10));
  }
  bcd[9] = negative ? 0x80 : 0;
  finish(&st, s);
  bbr_f80 r;
  memcpy(&r.sig, bcd, 8);
  uint16_t t;
  memcpy(&t, bcd + 8, 2);
  r.top = t;
  return r;
}

double bbr_x87_f64_fprem(double a, double b) { return fmod(a, b); }
double bbr_x87_f64_fprem1(double a, double b) { return remainder(a, b); }
double bbr_x87_f64_scale(double a, double b) {
  if (a == 0.0) return a; // keeps the sign of zero
  return a * exp2(trunc(b));
}
double bbr_x87_f64_f2xm1(double a) { return exp2(a) - 1.0; }
double bbr_x87_f64_fyl2x(double x, double y) { return y * log2(x); }
double bbr_x87_f64_fyl2xp1(double x, double y) { return y * log2(1.0 + x); }
double bbr_x87_f64_atan(double y, double x) { return atan2(y, x); }
double bbr_x87_f64_tan(double a) { return tan(a); }
double bbr_x87_f64_sin(double a) { return sin(a); }
double bbr_x87_f64_cos(double a) { return cos(a); }
