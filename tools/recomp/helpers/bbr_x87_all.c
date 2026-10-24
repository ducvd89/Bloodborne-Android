// One translation unit for the whole helper set (bbr_x87.c and the SoftFloat subset it needs), so that bb-recomp compiles and
// links a single object. Build: clang -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off -c bbr_x87_all.c
// SoftFloat's configuration, as FEX builds it (External/SoftFloat-3e/CMakeLists.txt)
#define FEXCORE_PRESERVE_ALL_ATTR
#define SOFTFLOAT_BUILTIN_CLZ 1
#define INLINE static inline
#define INLINE_LEVEL 4
#define SOFTFLOAT_FAST_INT64 1
#define SOFTFLOAT_FAST_DIV32TO16 1
#define SOFTFLOAT_FAST_DIV64TO32 1
#include "softfloat/bbr_rename.h"
#include "bbr_x87.c"
#include "softfloat/extF80_roundToInt.c"
#include "softfloat/extF80_to_f32.c"
#include "softfloat/extF80_to_f64.c"
#include "softfloat/extF80_to_i32.c"
#include "softfloat/extF80_to_i64.c"
#include "softfloat/f32_to_extF80.c"
#include "softfloat/f64_to_extF80.c"
#include "softfloat/i32_to_extF80.c"
#include "softfloat/s_commonNaNToExtF80UI.c"
#include "softfloat/s_commonNaNToF32UI.c"
#include "softfloat/s_commonNaNToF64UI.c"
#include "softfloat/s_countLeadingZeros8.c"
#include "softfloat/s_extF80UIToCommonNaN.c"
#include "softfloat/s_f32UIToCommonNaN.c"
#include "softfloat/s_f64UIToCommonNaN.c"
#include "softfloat/s_normSubnormalExtF80Sig.c"
#include "softfloat/s_normSubnormalF32Sig.c"
#include "softfloat/s_normSubnormalF64Sig.c"
#include "softfloat/softfloat_raiseFlags.c"
#include "softfloat/s_propagateNaNExtF80UI.c"
#include "softfloat/s_roundPackToF32.c"
#include "softfloat/s_roundPackToF64.c"
#include "softfloat/s_roundToI32.c"
#include "softfloat/s_roundToI64.c"
#include "softfloat/ui64_to_extF80.c"
