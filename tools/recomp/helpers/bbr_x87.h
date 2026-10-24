// x87 helpers that the lowered code calls (docs/RECOMP_ABI.md, "x87 helpers"). Plain C, compiled for the target by bb-recomp
// (NDK clang for aarch64, system clang for x86-64) and linked into the generated .so, so the runtime provides nothing for them.
//
// They are FEX's fallback handlers (FEXCore/Source/Interface/Core/Interpreter/Fallbacks/F80Fallbacks.h, MIT) with the
// softfloat_state built from the guest's FCW exactly as FEX does: `s` is the GuestState (FEX's CPUState layout); the helpers read
// the x87 control word from it and, when SoftFloat raises the invalid-operation flag, set the IE flag the way FEX's
// ScopedSoftFloatState does. An 80-bit value is passed as two integers: the 64-bit significand and the sign/exponent word
// (the register layout FEX uses, low 10 bytes of the 128-bit FPR; the bytes above are zero). F80 results are returned in a
// struct of two uint64_t, which both the x86-64 SysV and the AArch64 ABI return in two integer registers.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CPUState layout (static_asserts in the lowering: lower_ops.cpp)
#define BBR_X87_FCW_OFFSET 1200 // offsetof(CPUState, FCW), uint16_t
#define BBR_X87_IE_OFFSET 1040  // offsetof(CPUState, flags) + X87FLAG_IE_LOC, uint8_t

typedef struct { uint64_t sig; uint64_t top; } bbr_f80; // significand; sign (bit 15) and exponent (bits 0-14), upper bits zero

// IR F80CVT (reduced precision: fld/fstp tbyte, 80-bit memory value to the register's f64) and F80CVTTo
double bbr_x87_f80_to_f64(void* s, uint64_t sig, uint64_t top);
float bbr_x87_f80_to_f32(void* s, uint64_t sig, uint64_t top);
bbr_f80 bbr_x87_f64_to_f80(void* s, double v);
bbr_f80 bbr_x87_f32_to_f80(void* s, float v);
// IR F80CVTInt (size 2, 4 or 8 bytes; truncate: round toward zero instead of the FCW mode) and F80CVTToInt (size 2 or 4)
int64_t bbr_x87_f80_to_int(void* s, uint64_t sig, uint64_t top, uint64_t size, uint64_t truncate);
bbr_f80 bbr_x87_int_to_f80(void* s, int64_t v, uint64_t size);
// IR F80BCDLoad / F80BCDStore (the 80-bit operand is the 10 packed-BCD bytes)
bbr_f80 bbr_x87_bcd_load(void* s, uint64_t sig, uint64_t top);
bbr_f80 bbr_x87_bcd_store(void* s, uint64_t sig, uint64_t top);
// IR F64* (reduced precision): libm, as FEX's OpHandlers<IR::OP_F64*>
double bbr_x87_f64_fprem(double a, double b);   // fmod
double bbr_x87_f64_fprem1(double a, double b);  // remainder (IEEE)
double bbr_x87_f64_scale(double a, double b);   // a == 0 ? a : a * exp2(trunc(b))
double bbr_x87_f64_f2xm1(double a);             // exp2(a) - 1
double bbr_x87_f64_fyl2x(double x, double y);   // y * log2(x)
double bbr_x87_f64_fyl2xp1(double x, double y); // y * log2(1 + x)
double bbr_x87_f64_atan(double y, double x);    // atan2
double bbr_x87_f64_tan(double a);
double bbr_x87_f64_sin(double a);
double bbr_x87_f64_cos(double a);

// The names, for the loaders that must resolve them by name (the in-process JIT of the test tools).
#define BBR_X87_HELPERS(X) \
  X(bbr_x87_f80_to_f64) X(bbr_x87_f80_to_f32) X(bbr_x87_f64_to_f80) X(bbr_x87_f32_to_f80) X(bbr_x87_f80_to_int) X(bbr_x87_int_to_f80) \
  X(bbr_x87_bcd_load) X(bbr_x87_bcd_store) X(bbr_x87_f64_fprem) X(bbr_x87_f64_fprem1) X(bbr_x87_f64_scale) X(bbr_x87_f64_f2xm1) \
  X(bbr_x87_f64_fyl2x) X(bbr_x87_f64_fyl2xp1) X(bbr_x87_f64_atan) X(bbr_x87_f64_tan) X(bbr_x87_f64_sin) X(bbr_x87_f64_cos)

#ifdef __cplusplus
}
#endif
