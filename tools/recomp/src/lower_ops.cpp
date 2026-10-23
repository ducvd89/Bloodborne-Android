// Scalar, flag, memory and atomic ops. Each case follows the Arm64 JIT's emission of the same op
// (FEXCore/Source/Interface/Core/JIT/{ALUOps,MemoryOps,AtomicOps,MiscOps}.cpp), including the x86 fix-ups.
// Conventions: every GPR value is an i64 whose upper bits are what the JIT leaves there (a 32-bit op
// zero-extends; 8/16-bit ops run as 32-bit ops); FPR values are <2 x i64>.
#include "lower_impl.h"

using namespace llvm;

namespace bbr {

// ------------------------------------------------------------------------------------ helpers -------
Value* Lower::sdivSafe(Value* a, Value* b, bool rem) {
  // ARM sdiv: x/0 = 0, INT_MIN/-1 = INT_MIN (wraps). LLVM sdiv would be undefined for both.
  auto* ty = a->getType();
  Value* zero = ConstantInt::get(ty, 0);
  Value* isZero = B.CreateICmpEQ(b, zero);
  Value* isM1 = B.CreateICmpEQ(b, Constant::getAllOnesValue(ty));
  Value* bs = B.CreateSelect(B.CreateOr(isZero, isM1), ConstantInt::get(ty, 1), b);
  Value* q = B.CreateSDiv(a, bs);
  q = B.CreateSelect(isM1, B.CreateNeg(a), q);
  q = B.CreateSelect(isZero, zero, q);
  return rem ? B.CreateSub(a, B.CreateMul(q, b)) : q;
}
Value* Lower::udivSafe(Value* a, Value* b, bool rem) {
  auto* ty = a->getType();
  Value* zero = ConstantInt::get(ty, 0);
  Value* isZero = B.CreateICmpEQ(b, zero);
  Value* q = B.CreateUDiv(a, B.CreateSelect(isZero, ConstantInt::get(ty, 1), b));
  q = B.CreateSelect(isZero, zero, q);
  return rem ? B.CreateSub(a, B.CreateMul(q, b)) : q;
}

static Value* shiftBy(IRBuilder<>& B, IR::ShiftType t, Value* v, unsigned amt) {
  if (!amt) return v;
  Value* a = ConstantInt::get(v->getType(), amt);
  switch (t) {
  case IR::ShiftType::LSL: return B.CreateShl(v, a);
  case IR::ShiftType::LSR: return B.CreateLShr(v, a);
  case IR::ShiftType::ASR: return B.CreateAShr(v, a);
  case IR::ShiftType::ROR: return B.CreateIntrinsic(Intrinsic::fshr, {v->getType()}, {v, v, a});
  }
  return v;
}

Value* Lower::loadVal(Value* addr, unsigned bytes, bool fpr) {
  Value* p = B.CreateIntToPtr(addr, Ptr);
  if (!fpr) return zx(B.CreateAlignedLoad(iN(bytes * 8), p, Align(1)));
  if (bytes == 16) return B.CreateAlignedLoad(V128, p, Align(1));
  return toV(B.CreateZExt(B.CreateAlignedLoad(iN(bytes * 8), p, Align(1)), I128));
}
void Lower::storeVal(Value* addr, Value* v, unsigned bytes, bool fpr) {
  Value* p = B.CreateIntToPtr(addr, Ptr);
  if (!fpr) B.CreateAlignedStore(tr(v, bytes * 8), p, Align(1));
  else if (bytes == 16) B.CreateAlignedStore(v, p, Align(1));
  else B.CreateAlignedStore(B.CreateTrunc(fromV(v), iN(bytes * 8)), p, Align(1));
}
Value* Lower::ctxLoad(uint64_t off, unsigned bytes, bool fpr, Value* base) {
  Value* p = base ? B.CreateGEP(I8, base, {c64(off)}) : gep(off);
  if (!fpr) return zx(B.CreateAlignedLoad(iN(bytes * 8), p, Align(1)));
  if (bytes == 16) return B.CreateAlignedLoad(V128, p, Align(1));
  return toV(B.CreateZExt(B.CreateAlignedLoad(iN(bytes * 8), p, Align(1)), I128));
}
void Lower::ctxStore(uint64_t off, Value* v, unsigned bytes, bool fpr, Value* base) {
  Value* p = base ? B.CreateGEP(I8, base, {c64(off)}) : gep(off);
  if (!fpr) B.CreateAlignedStore(tr(v, bytes * 8), p, Align(1));
  else if (bytes == 16) B.CreateAlignedStore(v, p, Align(1));
  else B.CreateAlignedStore(B.CreateTrunc(fromV(v), iN(bytes * 8)), p, Align(1));
}

// LoadMem/StoreMem address: Addr + (Offset: none | inline constant, added as is | register, extended and scaled).
Value* Lower::memAddr(W addr, W off, IR::MemOffsetType t, uint8_t scale) {
  Value* a = A(addr);
  if (off.IsInvalid()) return a;
  uint64_t k;
  if (K(off, k)) return k ? B.CreateAdd(a, c64(k)) : a;
  Value* o = A(off);
  switch (t) {
  case IR::MemOffsetType::SXTX: break;
  case IR::MemOffsetType::UXTW: o = B.CreateZExt(B.CreateTrunc(o, I32), I64); break;
  case IR::MemOffsetType::SXTW: o = B.CreateSExt(B.CreateTrunc(o, I32), I64); break;
  }
  if (scale > 1) o = B.CreateShl(o, c64(Log2_32(scale)));
  return B.CreateAdd(a, o);
}

// ------------------------------------------------------------------------------------ scalar ops -----
bool Lower::emitScalarOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned size = sz(op);
  const unsigned bits = size == 8 ? 64 : 32; // ConvertSize: everything below 64 bits runs as a 32-bit op
  auto a32 = [&](W w) { return tr(A(w), bits); };
  auto fin = [&](Value* r) { def(node, zx(r)); };
  // 32-bit shifts mask the amount (lslv/lsrv/asrv/rorv), LLVM shifts do not.
  auto amount = [&](W w, unsigned b) -> Value* {
    uint64_t k;
    if (K(w, k)) return cN(b, k & (b - 1));
    return B.CreateAnd(tr(A(w), b), cN(b, b - 1));
  };
  auto fmask = [&](unsigned width) -> uint64_t { return width >= 64 ? ~0ull : ((1ull << width) - 1); };

  switch (op->Op) {
  // ---- constants
  case OP_CONSTANT: {
    const uint64_t k = op->C<IROp_Constant>()->Constant;
    if (m.O.imageSize && k >= m.O.liftBase && k - m.O.liftBase < m.O.imageSize) { ++res->relocConstants; def(node, reloc(k)); }
    else def(node, c64(k));
    return true;
  }
  case OP_ENTRYPOINTOFFSET: {
    const uint64_t a = entryRip + op->C<IROp_EntrypointOffset>()->Offset;
    if (op->Size == OpSize::i32Bit) def(node, c64(a & 0xFFFFFFFFull)); else { ++res->relocConstants; def(node, reloc(a)); }
    return true;
  }
  case OP_PATCHABLEGUESTDATA: def(node, c64(op->C<IROp_PatchableGuestData>()->Value)); return true;
  case OP_CYCLECOUNTER: def(node, B.CreateCall(m.helper("bbr_rdtsc", I64, {}), {})); return true;
  case OP_YIELD:
    if (m.O.aarch64) B.CreateIntrinsic(Intrinsic::aarch64_hint, {}, {c32(1)});
    return true;

  // ---- plain ALU (no flags)
  case OP_ADD: fin(B.CreateAdd(a32(op->C<IROp_Add>()->Src1), a32(op->C<IROp_Add>()->Src2))); return true;
  case OP_SUB: fin(B.CreateSub(a32(op->C<IROp_Sub>()->Src1), a32(op->C<IROp_Sub>()->Src2))); return true;
  case OP_AND: fin(B.CreateAnd(a32(op->C<IROp_And>()->Src1), a32(op->C<IROp_And>()->Src2))); return true;
  case OP_OR: fin(B.CreateOr(a32(op->C<IROp_Or>()->Src1), a32(op->C<IROp_Or>()->Src2))); return true;
  case OP_XOR: fin(B.CreateXor(a32(op->C<IROp_Xor>()->Src1), a32(op->C<IROp_Xor>()->Src2))); return true;
  case OP_ANDN: fin(B.CreateAnd(a32(op->C<IROp_Andn>()->Src1), B.CreateNot(a32(op->C<IROp_Andn>()->Src2)))); return true;
  case OP_NOT: fin(B.CreateNot(a32(op->C<IROp_Not>()->Src))); return true;
  case OP_NEG: {
    auto* o = op->C<IROp_Neg>();
    Value* x = a32(o->Src);
    Value* n = B.CreateNeg(x);
    fin(o->Cond == CondClass::AL ? n : B.CreateSelect(cond(o->Cond), n, x));
    return true;
  }
  case OP_ADDSHIFT: { auto* o = op->C<IROp_AddShift>(); fin(B.CreateAdd(a32(o->Src1), shiftBy(B, o->Shift, a32(o->Src2), o->ShiftAmount))); return true; }
  case OP_SUBSHIFT: { auto* o = op->C<IROp_SubShift>(); fin(B.CreateSub(a32(o->Src1), shiftBy(B, o->Shift, a32(o->Src2), o->ShiftAmount))); return true; }
  case OP_ORLSHL: { auto* o = op->C<IROp_Orlshl>(); fin(B.CreateOr(a32(o->Src1), B.CreateShl(a32(o->Src2), cN(bits, o->BitShift)))); return true; }
  case OP_ORLSHR: { auto* o = op->C<IROp_Orlshr>(); fin(B.CreateOr(a32(o->Src1), B.CreateLShr(a32(o->Src2), cN(bits, o->BitShift)))); return true; }
  case OP_ORNROR: {
    auto* o = op->C<IROp_Ornror>();
    Value* r = shiftBy(B, ShiftType::ROR, a32(o->Src2), o->BitShift);
    fin(B.CreateOr(a32(o->Src1), B.CreateNot(r)));
    return true;
  }
  case OP_LSHL: { auto* o = op->C<IROp_Lshl>(); fin(B.CreateShl(a32(o->Src1), amount(o->Src2, bits))); return true; }
  case OP_LSHR: { auto* o = op->C<IROp_Lshr>(); fin(B.CreateLShr(a32(o->Src1), amount(o->Src2, bits))); return true; }
  case OP_ROR: {
    auto* o = op->C<IROp_Ror>();
    Value* x = a32(o->Src1);
    fin(B.CreateIntrinsic(Intrinsic::fshr, {x->getType()}, {x, x, amount(o->Src2, bits)}));
    return true;
  }
  case OP_ASHR: {
    auto* o = op->C<IROp_Ashr>();
    Value* x = a32(o->Src1);
    if (size >= 4) { fin(B.CreateAShr(x, amount(o->Src2, bits))); return true; }
    const unsigned sb = size * 8; // sub-32-bit: sign-extend the field, shift, mask back
    Value* s = B.CreateSExt(B.CreateTrunc(x, iN(sb)), I32);
    Value* r = B.CreateAShr(s, amount(o->Src2, 32));
    fin(B.CreateTrunc(r, iN(sb)));
    return true;
  }
  case OP_MUL: fin(B.CreateMul(a32(op->C<IROp_Mul>()->Src1), a32(op->C<IROp_Mul>()->Src2))); return true;
  case OP_UMUL: fin(B.CreateMul(a32(op->C<IROp_UMul>()->Src1), a32(op->C<IROp_UMul>()->Src2))); return true;
  case OP_SMULL: {
    auto* o = op->C<IROp_SMull>();
    def(node, B.CreateMul(B.CreateSExt(tr(A(o->Src1), 32), I64), B.CreateSExt(tr(A(o->Src2), 32), I64)));
    return true;
  }
  case OP_UMULL: {
    auto* o = op->C<IROp_UMull>();
    def(node, B.CreateMul(B.CreateZExt(tr(A(o->Src1), 32), I64), B.CreateZExt(tr(A(o->Src2), 32), I64)));
    return true;
  }
  case OP_MULH: {
    auto* o = op->C<IROp_MulH>();
    if (size == 4) {
      Value* p = B.CreateMul(B.CreateSExt(tr(A(o->Src1), 32), I64), B.CreateSExt(tr(A(o->Src2), 32), I64));
      def(node, B.CreateLShr(p, c64(32)));
    } else {
      Value* p = B.CreateMul(B.CreateSExt(A(o->Src1), I128), B.CreateSExt(A(o->Src2), I128));
      def(node, B.CreateTrunc(B.CreateLShr(p, cN(128, 64)), I64));
    }
    return true;
  }
  case OP_UMULH: {
    auto* o = op->C<IROp_UMulH>();
    if (size == 4) {
      Value* p = B.CreateMul(B.CreateZExt(tr(A(o->Src1), 32), I64), B.CreateZExt(tr(A(o->Src2), 32), I64));
      def(node, B.CreateLShr(p, c64(32)));
    } else {
      Value* p = B.CreateMul(B.CreateZExt(A(o->Src1), I128), B.CreateZExt(A(o->Src2), I128));
      def(node, B.CreateTrunc(B.CreateLShr(p, cN(128, 64)), I64));
    }
    return true;
  }

  // ---- bit manipulation
  case OP_POPCOUNT: {
    Value* x = a32(op->C<IROp_Popcount>()->Src);
    if (size < 4) x = B.CreateAnd(x, cN(32, fmask(size * 8)));
    fin(B.CreateIntrinsic(Intrinsic::ctpop, {x->getType()}, {x}));
    return true;
  }
  case OP_FINDLSB: { Value* x = a32(op->C<IROp_FindLSB>()->Src); fin(B.CreateIntrinsic(Intrinsic::cttz, {x->getType()}, {x, B.getFalse()})); return true; }
  case OP_FINDMSB: {
    Value* x = a32(op->C<IROp_FindMSB>()->Src);
    if (size == 2) x = B.CreateShl(x, cN(32, 16));
    Value* lz = zx(B.CreateIntrinsic(Intrinsic::ctlz, {x->getType()}, {x, B.getFalse()}));
    def(node, B.CreateSub(c64(size * 8 - 1), lz));
    return true;
  }
  case OP_FINDTRAILINGZEROES: {
    Value* x = a32(op->C<IROp_FindTrailingZeroes>()->Src);
    if (size == 2) x = B.CreateOr(x, cN(32, 0x10000));
    fin(B.CreateIntrinsic(Intrinsic::cttz, {x->getType()}, {x, B.getFalse()}));
    return true;
  }
  case OP_COUNTLEADINGZEROES: {
    Value* x = a32(op->C<IROp_CountLeadingZeroes>()->Src);
    if (size == 2) x = B.CreateOr(B.CreateShl(x, cN(32, 16)), cN(32, 0x8000));
    fin(B.CreateIntrinsic(Intrinsic::ctlz, {x->getType()}, {x, B.getFalse()}));
    return true;
  }
  case OP_REV: {
    Value* x = a32(op->C<IROp_Rev>()->Src);
    Value* r = B.CreateIntrinsic(Intrinsic::bswap, {x->getType()}, {x});
    if (size == 2) r = B.CreateLShr(r, cN(32, 16));
    fin(r);
    return true;
  }
  case OP_RBIT: { Value* x = a32(op->C<IROp_Rbit>()->Src); fin(B.CreateIntrinsic(Intrinsic::bitreverse, {x->getType()}, {x})); return true; }
  case OP_BFI: {
    auto* o = op->C<IROp_Bfi>();
    Value* d = a32(o->Dest);
    Value* s = a32(o->Src);
    const uint64_t mk = fmask(o->Width);
    Value* ins = B.CreateShl(B.CreateAnd(s, cN(bits, mk)), cN(bits, o->lsb));
    Value* r = B.CreateOr(B.CreateAnd(d, cN(bits, ~(mk << o->lsb))), ins);
    if (size < 4) r = B.CreateAnd(r, cN(bits, fmask(size * 8)));
    fin(r);
    return true;
  }
  case OP_BFE: {
    auto* o = op->C<IROp_Bfe>();
    Value* s = a32(o->Src);
    if (o->lsb == 0 && o->Width == 32) { def(node, zx(tr(A(o->Src), 32))); return true; }
    Value* r = o->lsb ? B.CreateLShr(s, cN(bits, o->lsb)) : s;
    if (o->Width < bits) r = B.CreateAnd(r, cN(bits, fmask(o->Width)));
    fin(r);
    return true;
  }
  case OP_SBFE: {
    auto* o = op->C<IROp_Sbfe>();
    Value* s = a32(o->Src);
    Value* r = B.CreateShl(s, cN(bits, bits - o->lsb - o->Width));
    fin(B.CreateAShr(r, cN(bits, bits - o->Width)));
    return true;
  }
  case OP_EXTR: {
    auto* o = op->C<IROp_Extr>();
    Value *u = a32(o->Upper), *l = a32(o->Lower);
    fin(B.CreateIntrinsic(Intrinsic::fshr, {u->getType()}, {u, l, cN(bits, o->LSB)}));
    return true;
  }
  case OP_PARITY: {
    auto* o = op->C<IROp_Parity>();
    Value* raw = tr(A(o->Raw), 32);
    Value* t = B.CreateXor(raw, B.CreateLShr(raw, c32(4)));
    t = B.CreateXor(t, B.CreateLShr(t, c32(2)));
    Value* sh = B.CreateLShr(t, c32(1));
    Value* r = B.CreateXor(t, o->Invert ? B.CreateNot(sh) : sh);
    if (o->Mask) r = B.CreateAnd(r, c32(1));
    fin(r);
    return true;
  }

  // ---- flag-setting ALU
  case OP_ADDWITHFLAGS: {
    auto* o = op->C<IROp_AddWithFlags>();
    Value *a = a32(o->Src1), *b = a32(o->Src2), *r = B.CreateAdd(a, b);
    setAdd(a, b, r); fin(r);
    return true;
  }
  case OP_SUBWITHFLAGS: {
    auto* o = op->C<IROp_SubWithFlags>();
    Value *a = a32(o->Src1), *b = a32(o->Src2), *r = B.CreateSub(a, b);
    setSub(a, b, r); fin(r);
    return true;
  }
  case OP_ANDWITHFLAGS: {
    auto* o = op->C<IROp_AndWithFlags>();
    Value* r = B.CreateAnd(a32(o->Src1), a32(o->Src2));
    setLogic(size < 4 ? B.CreateShl(r, cN(32, 32 - size * 8)) : r);
    fin(r);
    return true;
  }
  case OP_ADDNZCV: {
    auto* o = op->C<IROp_AddNZCV>();
    Value *a = a32(o->Src1), *b = a32(o->Src2);
    if (size < 4) { a = B.CreateShl(a, cN(32, 32 - size * 8)); b = B.CreateShl(b, cN(32, 32 - size * 8)); }
    setAdd(a, b);
    return true;
  }
  case OP_SUBNZCV: {
    auto* o = op->C<IROp_SubNZCV>();
    Value *a = a32(o->Src1), *b = a32(o->Src2);
    if (size < 4) { a = B.CreateShl(a, cN(32, 32 - size * 8)); b = B.CreateShl(b, cN(32, 32 - size * 8)); }
    setSub(a, b);
    return true;
  }
  case OP_TESTNZ: {
    auto* o = op->C<IROp_TestNZ>();
    Value* r = B.CreateAnd(a32(o->Src1), a32(o->Src2));
    setLogic(size < 4 ? B.CreateShl(r, cN(32, 32 - size * 8)) : r);
    return true;
  }
  case OP_TESTZ: {
    auto* o = op->C<IROp_TestZ>();
    Value* r = B.CreateAnd(B.CreateAnd(tr(A(o->Src1), 32), tr(A(o->Src2), 32)), cN(32, size >= 4 ? 0xFFFFFFFFull : fmask(size * 8)));
    setLogic(r);
    return true;
  }
  case OP_CARRYINVERT: {
    if (fd.k == FlagDesc::Gen) { fd.c = B.CreateNot(fd.c); fd.fa = fd.fb = nullptr; fd.dirty = true; }
    else if (fd.k == FlagDesc::Mem) { flagsToGen(); fd.c = B.CreateNot(fd.c); fd.dirty = true; }
    else { fd.cinv = !fd.cinv; fd.dirty = true; }
    flagsWritten = true;
    return true;
  }
  case OP_RMIFNZCV: {
    auto* o = op->C<IROp_RmifNZCV>();
    Value* s = A(o->Src);
    Value* r = o->Rotate ? B.CreateIntrinsic(Intrinsic::fshr, {I64}, {s, s, c64(o->Rotate)}) : s;
    auto take = [&](unsigned bit) { return B.CreateTrunc(B.CreateLShr(r, c64(bit)), I1); };
    flagsToGen();
    Value *n = fd.n, *z = fd.z, *c = fd.c, *v = fd.v;
    if (o->Mask & 8) n = take(3);
    if (o->Mask & 4) z = take(2);
    if (o->Mask & 2) c = take(1);
    if (o->Mask & 1) v = take(0);
    setGen(n, z, c, v);
    return true;
  }
  case OP_SETSMALLNZV: {
    auto* o = op->C<IROp_SetSmallNZV>();
    Value* x = tr(A(o->Src), 32);
    const unsigned top = size * 8 - 1; // setf8: bit 7, setf16: bit 15
    auto bit = [&](unsigned b) { return B.CreateTrunc(B.CreateLShr(x, c32(b)), I1); };
    Value* n = bit(top);
    Value* z = B.CreateICmpEQ(B.CreateAnd(x, c32((1u << (top + 1)) - 1)), c32(0));
    Value* v = B.CreateXor(bit(top + 1), n);
    flagsToGen();
    setGen(n, z, fd.c, v);
    return true;
  }
  case OP_AXFLAG: {
    flagsToGen();
    Value *z = B.CreateOr(fd.z, fd.v), *c = B.CreateAnd(fd.c, B.CreateNot(fd.v));
    setGen(ConstantInt::getFalse(C), z, c, ConstantInt::getFalse(C));
    return true;
  }
  case OP_CONDSUBNZCV: {
    auto* o = op->C<IROp_CondSubNZCV>();
    Value *a = a32(o->Src1), *b = a32(o->Src2);
    Value* take = cond(o->Cond);
    FlagDesc sd; sd.k = FlagDesc::Sub; sd.a = a; sd.b = b;
    FlagDesc saved = fd;
    fd = sd;
    Value *n = flagN(), *z = flagZ(), *c = flagC(), *v = flagV();
    fd = saved;
    auto lit = [&](unsigned bit) { return ConstantInt::get(I1, (o->FalseNZCV >> bit) & 1); };
    setGen(B.CreateSelect(take, n, lit(3)), B.CreateSelect(take, z, lit(2)), B.CreateSelect(take, c, lit(1)), B.CreateSelect(take, v, lit(0)));
    return true;
  }
  case OP_ADC: case OP_SBB: case OP_ADCWITHFLAGS: case OP_SBBWITHFLAGS: {
    // adc: a + b + C; sbc: a + ~b + C (C is the ARM carry). The *WithFlags forms set flags like adcs/sbcs.
    const bool sub = op->Op == OP_SBB || op->Op == OP_SBBWITHFLAGS;
    const bool flags = op->Op == OP_SBBWITHFLAGS || op->Op == OP_ADCWITHFLAGS;
    W s1 = sub ? (op->Op == OP_SBB ? op->C<IROp_Sbb>()->Src1 : op->C<IROp_SbbWithFlags>()->Src1) : (op->Op == OP_ADC ? op->C<IROp_Adc>()->Src1 : op->C<IROp_AdcWithFlags>()->Src1);
    W s2 = sub ? (op->Op == OP_SBB ? op->C<IROp_Sbb>()->Src2 : op->C<IROp_SbbWithFlags>()->Src2) : (op->Op == OP_ADC ? op->C<IROp_Adc>()->Src2 : op->C<IROp_AdcWithFlags>()->Src2);
    Value *a = a32(s1), *b = a32(s2);
    if (sub) b = B.CreateNot(b);
    Value* cin = B.CreateZExt(flagC(), a->getType());
    Value* r = B.CreateAdd(B.CreateAdd(a, b), cin);
    if (flags) {
      auto* t = a->getType();
      Value* zero = ConstantInt::get(t, 0);
      Value* c1 = B.CreateExtractValue(B.CreateIntrinsic(Intrinsic::uadd_with_overflow, {t}, {a, b}), 1);
      Value* s1v = B.CreateAdd(a, b);
      Value* c2 = B.CreateICmpULT(r, s1v);
      Value* carry = B.CreateOr(c1, c2);
      Value* ov = B.CreateICmpSLT(B.CreateAnd(B.CreateXor(a, r), B.CreateXor(b, r)), zero);
      setGen(B.CreateICmpSLT(r, zero), B.CreateICmpEQ(r, zero), carry, ov);
    }
    fin(r);
    return true;
  }
  case OP_ADCZERO: case OP_ADCZEROWITHFLAGS: {
    // cinc with condition CC: Src1 + 1 when C is clear (x86 CF is the inverted ARM carry here)
    Value* a = a32(op->Op == OP_ADCZERO ? op->C<IROp_AdcZero>()->Src1 : op->C<IROp_AdcZeroWithFlags>()->Src1);
    Value* inc = B.CreateZExt(B.CreateNot(flagC()), a->getType());
    Value* r = B.CreateAdd(a, inc);
    if (op->Op == OP_ADCZEROWITHFLAGS) setAdd(a, inc, r);
    fin(r);
    return true;
  }
  case OP_SHIFTFLAGS: {
    auto* o = op->C<IROp_ShiftFlags>();
    const unsigned osz = static_cast<unsigned>(o->Size);
    const unsigned eb = osz == 8 ? 64 : 32;
    Value* resFull = A(o->Result);
    Value* dst = tr(resFull, eb);
    Value* src1 = tr(A(o->Src1), eb);
    Value* src2 = tr(A(o->Src2), eb);
    Value* pfIn = A(o->PFInput);
    Value* masked = B.CreateAnd(tr(A(o->Src2), 32), c32(osz == 8 ? 0x3f : 0x1f));
    Value* nz = B.CreateICmpNE(masked, c32(0));
    Value *n, *z, *pfNew;
    if (osz >= 4) {
      n = B.CreateICmpSLT(dst, cN(eb, 0)); z = B.CreateICmpEQ(dst, cN(eb, 0)); pfNew = zx(dst);
    } else {
      Value* sh = B.CreateShl(tr(resFull, 32), c32(32 - osz * 8));
      n = B.CreateICmpSLT(sh, c32(0)); z = B.CreateICmpEQ(sh, c32(0)); pfNew = resFull;
    }
    Value* cf;
    if (o->Shift == ShiftType::LSL) {
      if (osz >= 4) cf = B.CreateTrunc(B.CreateLShr(src1, B.CreateAnd(B.CreateNeg(src2), cN(eb, eb - 1))), I1);
      else cf = B.CreateTrunc(B.CreateLShr(resFull, c64(osz * 8)), I1);
    } else {
      Value* amt = B.CreateAnd(B.CreateTrunc(B.CreateSub(zx(src2), c64(1)), iN(eb)), cN(eb, eb - 1));
      cf = B.CreateTrunc(B.CreateLShr(src1, amt), I1);
    }
    if (o->InvertCF) cf = B.CreateNot(cf);
    Value* of = ConstantInt::getFalse(C);
    if (o->Shift != ShiftType::ASR) of = B.CreateTrunc(B.CreateLShr(B.CreateXor(src1, dst), cN(eb, osz * 8 - 1)), I1);
    flagsToGen();
    setGen(B.CreateSelect(nz, n, fd.n), B.CreateSelect(nz, z, fd.z), B.CreateSelect(nz, cf, fd.c), B.CreateSelect(nz, of, fd.v));
    def(node, B.CreateSelect(nz, pfNew, pfIn));
    return true;
  }
  case OP_LOADNZCV: def(node, zx(packNZCV())); return true;
  case OP_STORENZCV: unpackNZCV(tr(A(op->C<IROp_StoreNZCV>()->Value), 32)); return true;
  case OP_LOADDF: {
    Value* b = B.CreateAlignedLoad(I8, gep(offsetof(Core::CPUState, flags) + X86State::RFLAG_DF_RAW_LOC), Align(1));
    def(node, B.CreateSExt(b, I64));
    return true;
  }
  case OP_LOADPF: def(node, B.CreateLoad(I64, gpr[16])); return true;
  case OP_LOADAF: def(node, B.CreateLoad(I64, gpr[17])); return true;
  case OP_STOREPF: B.CreateStore(A(op->C<IROp_StorePF>()->Value), gpr[16]); dirty |= 1ull << 16; return true;
  case OP_STOREAF: B.CreateStore(A(op->C<IROp_StoreAF>()->Value), gpr[17]); dirty |= 1ull << 17; return true;
  case OP_LOADREGISTER: {
    auto* o = op->C<IROp_LoadRegister>();
    if (o->Class == RegClass::GPR) def(node, B.CreateLoad(I64, gpr[o->Reg]));
    else def(node, B.CreateLoad(V128, xmm[o->Reg]));
    return true;
  }
  case OP_STOREREGISTER: {
    auto* o = op->C<IROp_StoreRegister>();
    PhysicalRegister pr(node);
    Value* v = A(o->Value);
    if (pr.AsRegClass() == RegClass::GPRFixed) B.CreateStore(v, gpr[pr.Reg]); else B.CreateStore(v, xmm[pr.Reg]);
    dirty |= 1ull << (pr.AsRegClass() == RegClass::GPRFixed ? pr.Reg : NumStaticGPR + pr.Reg);
    return true;
  }

  // ---- selects
  case OP_NZCVSELECT: {
    auto* o = op->C<IROp_NZCVSelect>();
    Value* c = cond(o->Cond);
    Value *t = a32(o->TrueVal), *f = a32(o->FalseVal);
    fin(B.CreateSelect(c, t, f));
    return true;
  }
  case OP_NZCVSELECTINCREMENT: {
    auto* o = op->C<IROp_NZCVSelectIncrement>();
    Value* c = cond(o->Cond);
    Value *t = a32(o->TrueVal), *f = a32(o->FalseVal);
    fin(B.CreateSelect(c, t, B.CreateAdd(f, ConstantInt::get(f->getType(), 1))));
    return true;
  }
  case OP_SELECT: {
    auto* o = op->C<IROp_Select>();
    Value* x = A(o->Cmp1);
    Value* y = A(o->Cmp2);
    const unsigned cb = o->CompareSize == OpSize::i64Bit ? 64 : 32;
    Value* c;
    if (x->getType() == V128) {
      auto fl = [&](Value* v) -> Value* {
        Value* e = B.CreateExtractElement(v, (uint64_t)0);
        return cb == 64 ? B.CreateBitCast(e, Type::getDoubleTy(C)) : B.CreateBitCast(B.CreateTrunc(e, I32), Type::getFloatTy(C));
      };
      FlagDesc t; t.k = FlagDesc::Gen; t.fa = fl(x); t.fb = fl(y);
      c = condOn(t, o->Cond);
    } else {
      FlagDesc t; t.k = FlagDesc::Sub; t.a = tr(x, cb); t.b = tr(y, cb);
      c = condOn(t, o->Cond);
    }
    fin(B.CreateSelect(c, a32(o->TrueVal), a32(o->FalseVal)));
    return true;
  }

  // ---- division
  case OP_DIV: case OP_UDIV: {
    const bool sg = op->Op == OP_DIV;
    W lo = sg ? op->C<IROp_Div>()->Lower : op->C<IROp_UDiv>()->Lower;
    W up = sg ? op->C<IROp_Div>()->Upper : op->C<IROp_UDiv>()->Upper;
    W dv = sg ? op->C<IROp_Div>()->Divisor : op->C<IROp_UDiv>()->Divisor;
    W oq = sg ? op->C<IROp_Div>()->OutQuotient : op->C<IROp_UDiv>()->OutQuotient;
    W orm = sg ? op->C<IROp_Div>()->OutRemainder : op->C<IROp_UDiv>()->OutRemainder;
    auto both = [&](Value* q, Value* r) { setVar(oq, zx(q)); setVar(orm, zx(r)); };
    auto divide = [&](Value* a, Value* b) {
      Value* q = sg ? sdivSafe(a, b, false) : udivSafe(a, b, false);
      both(q, B.CreateSub(a, B.CreateMul(q, b)));
    };
    if (up.IsInvalid()) {
      Value *a = a32(lo), *b = a32(dv);
      if (sg && size == 1) { a = B.CreateSExt(B.CreateTrunc(a, I8), I32); b = B.CreateSExt(B.CreateTrunc(b, I8), I32); }
      if (sg && size == 2) { a = B.CreateSExt(B.CreateTrunc(a, I16), I32); b = B.CreateSExt(B.CreateTrunc(b, I16), I32); }
      divide(a, b);
      return true;
    }
    if (size == 2) {
      Value* t1 = B.CreateOr(B.CreateAnd(tr(A(lo), 32), c32(0xFFFF)), B.CreateShl(B.CreateAnd(tr(A(up), 32), c32(0xFFFF)), c32(16)));
      Value* t2 = sg ? B.CreateSExt(B.CreateTrunc(A(dv), I16), I32) : tr(A(dv), 32);
      if (!sg) t2 = B.CreateAnd(t2, c32(0xFFFF)); // (x86 div r/m16: the frontend passes the divisor zero-extended)
      divide(t1, t2);
    } else if (size == 4) {
      Value* t1 = B.CreateOr(B.CreateZExt(tr(A(lo), 32), I64), B.CreateShl(B.CreateZExt(tr(A(up), 32), I64), c64(32)));
      Value* t2 = sg ? B.CreateSExt(tr(A(dv), 32), I64) : B.CreateZExt(tr(A(dv), 32), I64);
      divide(t1, t2);
    } else {
      Value *l = A(lo), *u = A(up), *d = A(dv);
      // Upper is just the sign (signed) / zero (unsigned) extension of Lower: a 64-bit divide. Else 128/64.
      Value* fits = sg ? B.CreateICmpEQ(B.CreateXor(B.CreateAShr(l, c64(63)), u), c64(0)) : B.CreateICmpEQ(u, c64(0));
      auto* b64 = newBB("div64");
      auto* b128 = newBB("div128");
      auto* done = newBB("divdone");
      B.CreateCondBr(fits, b64, b128);
      B.SetInsertPoint(b64);
      divide(l, d);
      B.CreateBr(done);
      B.SetInsertPoint(b128);
      Value* n = B.CreateOr(B.CreateShl(sg ? B.CreateSExt(u, I128) : B.CreateZExt(u, I128), cN(128, 64)), B.CreateZExt(l, I128));
      Value* dd = sg ? B.CreateSExt(d, I128) : B.CreateZExt(d, I128);
      Value* isZero = B.CreateICmpEQ(dd, cN(128, 0));
      Value* ds = B.CreateSelect(isZero, cN(128, 1), dd);
      Value* q = sg ? B.CreateSDiv(n, ds) : B.CreateUDiv(n, ds);
      Value* r = sg ? B.CreateSRem(n, ds) : B.CreateURem(n, ds);
      setVar(oq, B.CreateSelect(isZero, c64(0), B.CreateTrunc(q, I64)));
      setVar(orm, B.CreateSelect(isZero, c64(0), B.CreateTrunc(r, I64)));
      B.CreateBr(done);
      B.SetInsertPoint(done);
    }
    return true;
  }

  // ---- misc
  case OP_CPUID: {
    auto* o = op->C<IROp_CPUID>();
    auto* sty = StructType::get(C, {I64, I64});
    Value* r = B.CreateCall(m.helper("bbr_cpuid", sty, {Ptr, I64, I64}), {S, A(o->Function), A(o->Leaf)});
    Value* lo = B.CreateExtractValue(r, 0);
    Value* hi = B.CreateExtractValue(r, 1);
    setVar(o->OutEAX, B.CreateAnd(lo, c64(0xFFFFFFFF)));
    setVar(o->OutEBX, B.CreateLShr(lo, c64(32)));
    setVar(o->OutECX, B.CreateAnd(hi, c64(0xFFFFFFFF)));
    setVar(o->OutEDX, B.CreateLShr(hi, c64(32)));
    return true;
  }
  case OP_PROCESSORID: def(node, B.CreateCall(m.helper("bbr_processor_id", I64, {}), {})); return true;
  case OP_SETROUNDINGMODE: {
    auto* o = op->C<IROp_SetRoundingMode>();
    B.CreateCall(m.helper("bbr_set_rounding", m.Void, {I64, I64, I64}), {A(o->RoundMode), A(o->MXCSR), c64(o->SetDAZ)});
    return true;
  }
  case OP_FENCE: {
    switch (op->C<IROp_Fence>()->Fence) {
    case FenceType::Load: B.CreateFence(AtomicOrdering::Acquire); break;
    case FenceType::Store: B.CreateFence(AtomicOrdering::Release); break;
    case FenceType::LoadStore: B.CreateFence(AtomicOrdering::SequentiallyConsistent); break;
    case FenceType::Inst: break; // isb: nothing to do for ahead-of-time code
    }
    return true;
  }
  default: return emitExtraOp(node, op);
  }
}

// ---------------------------------------------------------------------------- rarely used scalar ops ---
// Used by FEX's unit tests, the unused-in-game list of the census, or only after register allocation.
bool Lower::emitExtraOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned size = sz(op);
  const unsigned bits = size == 8 ? 64 : 32;
  auto a32 = [&](W w) { return tr(A(w), bits); };
  auto fin = [&](Value* r) { def(node, zx(r)); };
  switch (op->Op) {
  case OP_ANDSHIFT: { auto* o = op->C<IROp_AndShift>(); fin(B.CreateAnd(a32(o->Src1), shiftBy(B, o->Shift, a32(o->Src2), o->ShiftAmount))); return true; }
  case OP_XORSHIFT: { auto* o = op->C<IROp_XorShift>(); fin(B.CreateXor(a32(o->Src1), shiftBy(B, o->Shift, a32(o->Src2), o->ShiftAmount))); return true; }
  case OP_XORNSHIFT: { auto* o = op->C<IROp_XornShift>(); fin(B.CreateXor(a32(o->Src1), B.CreateNot(shiftBy(B, o->Shift, a32(o->Src2), o->ShiftAmount)))); return true; }
  case OP_BFXIL: {
    // Dest with Src[lsb+width-1:lsb] inserted at bit 0
    auto* o = op->C<IROp_Bfxil>();
    const uint64_t mk = o->Width >= 64 ? ~0ull : ((1ull << o->Width) - 1);
    Value* f = B.CreateAnd(o->lsb ? B.CreateLShr(a32(o->Src), cN(bits, o->lsb)) : a32(o->Src), cN(bits, mk));
    fin(B.CreateOr(B.CreateAnd(a32(o->Dest), cN(bits, ~mk)), f));
    return true;
  }
  case OP_MASKGENERATEFROMBITWIDTH: {
    // csinv: all ones when w == 0, else ~(~0 << w), the low w bits (lslv takes w mod 64)
    Value* w = A(op->C<IROp_MaskGenerateFromBitWidth>()->BitWidth);
    Value* sh = B.CreateShl(c64(~0ull), B.CreateAnd(w, c64(63)));
    def(node, B.CreateSelect(B.CreateICmpEQ(w, c64(0)), c64(~0ull), B.CreateNot(sh)));
    return true;
  }
  case OP_CMPPAIRZ: {
    // Z = (Src1 == Src2) as a pair; N, C, V keep their values
    auto* o = op->C<IROp_CmpPairZ>();
    Value* eq = B.CreateAnd(B.CreateICmpEQ(a32(o->Src1Lo), a32(o->Src2Lo)), B.CreateICmpEQ(a32(o->Src1Hi), a32(o->Src2Hi)));
    flagsToGen();
    setGen(fd.n, eq, fd.c, fd.v);
    return true;
  }
  case OP_ADCNZCV: case OP_SBBNZCV: {
    const bool sub = op->Op == OP_SBBNZCV;
    Value *a = a32(sub ? op->C<IROp_SbbNZCV>()->Src1 : op->C<IROp_AdcNZCV>()->Src1), *b = a32(sub ? op->C<IROp_SbbNZCV>()->Src2 : op->C<IROp_AdcNZCV>()->Src2);
    if (sub) b = B.CreateNot(b);
    Value* cin = B.CreateZExt(flagC(), a->getType());
    Value* r = B.CreateAdd(B.CreateAdd(a, b), cin);
    auto* t = a->getType();
    Value* zero = ConstantInt::get(t, 0);
    Value* c1 = B.CreateExtractValue(B.CreateIntrinsic(Intrinsic::uadd_with_overflow, {t}, {a, b}), 1);
    Value* c2 = B.CreateICmpULT(r, B.CreateAdd(a, b));
    Value* ov = B.CreateICmpSLT(B.CreateAnd(B.CreateXor(a, r), B.CreateXor(b, r)), zero);
    setGen(B.CreateICmpSLT(r, zero), B.CreateICmpEQ(r, zero), B.CreateOr(c1, c2), ov);
    return true;
  }
  case OP_CONDADDNZCV: {
    auto* o = op->C<IROp_CondAddNZCV>();
    Value *a = a32(o->Src1), *b = a32(o->Src2);
    Value* take = cond(o->Cond);
    FlagDesc ad; ad.k = FlagDesc::Add; ad.a = a; ad.b = b;
    FlagDesc saved = fd;
    fd = ad;
    Value *n = flagN(), *z = flagZ(), *c = flagC(), *v = flagV();
    fd = saved;
    auto lit = [&](unsigned bit) { return ConstantInt::get(I1, (o->FalseNZCV >> bit) & 1); };
    setGen(B.CreateSelect(take, n, lit(3)), B.CreateSelect(take, z, lit(2)), B.CreateSelect(take, c, lit(1)), B.CreateSelect(take, v, lit(0)));
    return true;
  }
  case OP_ROTATEFLAGS: {
    // rol/ror flags: only when the (masked) count is non-zero; the JIT tests the full Shift register against zero
    auto* o = op->C<IROp_RotateFlags>();
    const unsigned osz = static_cast<unsigned>(o->Size);
    const unsigned eb = osz == 8 ? 64 : 32;
    const unsigned bs = osz * 8;
    Value* resFull = A(o->Result);
    Value* res = tr(resFull, 64);
    Value* nz = B.CreateICmpNE(tr(A(o->Shift), eb), cN(eb, 0));
    Value* cf = B.CreateTrunc(B.CreateLShr(res, c64(o->Left ? 0 : bs - 1)), I1);
    Value* t = B.CreateXor(res, B.CreateLShr(res, c64(o->Left ? bs - 1 : 1)));
    Value* of = B.CreateTrunc(B.CreateLShr(t, c64(o->Left ? 0 : bs - 2)), I1);
    flagsToGen();
    // the inverted carry is what the JIT inserts: C = !CF
    setGen(fd.n, fd.z, B.CreateSelect(nz, B.CreateNot(cf), fd.c), B.CreateSelect(nz, of, fd.v));
    return true;
  }
  case OP_PDEP: case OP_PEXT: {
    const bool dep = op->Op == OP_PDEP;
    Value* in = a32(dep ? op->C<IROp_PDep>()->Input : op->C<IROp_PExt>()->Input);
    Value* mk = a32(dep ? op->C<IROp_PDep>()->Mask : op->C<IROp_PExt>()->Mask);
    auto* t = in->getType();
    auto* pre = curBB();
    auto* loop = newBB("pbloop");
    auto* done = newBB("pbdone");
    B.CreateBr(loop);
    B.SetInsertPoint(loop);
    PHINode *m = B.CreatePHI(t, 2), *i = B.CreatePHI(t, 2), *r = B.CreatePHI(t, 2), *k = B.CreatePHI(t, 2);
    m->addIncoming(mk, pre); i->addIncoming(in, pre); r->addIncoming(ConstantInt::get(t, 0), pre); k->addIncoming(ConstantInt::get(t, 0), pre);
    Value* low = B.CreateAnd(m, B.CreateNeg(m));                  // lowest set bit of the mask
    Value *nr, *ni, *nk;
    if (dep) { // deposit: input bit k goes to this mask bit
      Value* bit = B.CreateAnd(i, ConstantInt::get(t, 1));
      nr = B.CreateOr(r, B.CreateSelect(B.CreateICmpNE(bit, ConstantInt::get(t, 0)), low, ConstantInt::get(t, 0)));
      ni = B.CreateLShr(i, ConstantInt::get(t, 1)); nk = k;
    } else {   // extract: the input bit at this mask bit goes to result bit k
      Value* has = B.CreateICmpNE(B.CreateAnd(i, low), ConstantInt::get(t, 0));
      nr = B.CreateOr(r, B.CreateSelect(has, B.CreateShl(ConstantInt::get(t, 1), k), ConstantInt::get(t, 0)));
      ni = i; nk = B.CreateAdd(k, ConstantInt::get(t, 1));
    }
    Value* nm = B.CreateXor(m, low);
    m->addIncoming(nm, loop); i->addIncoming(ni, loop); r->addIncoming(nr, loop); k->addIncoming(nk, loop);
    B.CreateCondBr(B.CreateICmpNE(nm, ConstantInt::get(t, 0)), loop, done);
    B.SetInsertPoint(done);
    // zero mask: the loop body runs once with low = 0 and leaves r unchanged = 0
    fin(nr);
    return true;
  }
  case OP_CRC32: {
    auto* o = op->C<IROp_CRC32>();
    const unsigned sb = static_cast<unsigned>(o->SrcSize) * 8;
    Value* acc = tr(A(o->Src1), sb == 64 ? 64 : 32);
    Value* d = tr(A(o->Src2), sb);
    Value* r;
    if (m.O.aarch64) {
      static const Intrinsic::ID ids[] = {Intrinsic::aarch64_crc32cb, Intrinsic::aarch64_crc32ch, Intrinsic::aarch64_crc32cw, Intrinsic::aarch64_crc32cx};
      const unsigned ix = sb == 8 ? 0 : sb == 16 ? 1 : sb == 32 ? 2 : 3;
      r = B.CreateIntrinsic(ids[ix], {}, {acc, ix == 3 ? d : B.CreateZExt(d, I32)});
    } else {
      switch (sb) {
      case 8: r = B.CreateIntrinsic(Intrinsic::x86_sse42_crc32_32_8, {}, {acc, d}); break;
      case 16: r = B.CreateIntrinsic(Intrinsic::x86_sse42_crc32_32_16, {}, {acc, d}); break;
      case 32: r = B.CreateIntrinsic(Intrinsic::x86_sse42_crc32_32_32, {}, {acc, d}); break;
      default: r = B.CreateIntrinsic(Intrinsic::x86_sse42_crc32_64_64, {}, {acc, d}); break;
      }
    }
    fin(r);
    return true;
  }
  case OP_MEMSET: {
    auto* o = op->C<IROp_MemSet>();
    const unsigned es = static_cast<unsigned>(o->Size);
    Value* base = A(o->Addr);
    if (!o->Prefix.IsInvalid()) base = B.CreateAdd(A(o->Prefix), base);
    Value* val = tr(A(o->Value), es * 8);
    Value* n = A(o->Length);
    uint64_t dk;
    Value* back;
    if (K(o->Direction, dk)) back = ConstantInt::get(I1, (int64_t)dk == -1);
    else back = B.CreateICmpNE(B.CreateAnd(A(o->Direction), c64(2)), c64(0));
    Value* bytes = B.CreateMul(n, c64(es));
    auto* pre = curBB();
    auto* loop = newBB("msloop");
    auto* done = newBB("msdone");
    B.CreateCondBr(B.CreateICmpEQ(n, c64(0)), done, loop);
    B.SetInsertPoint(loop);
    PHINode* i = B.CreatePHI(I64, 2);
    i->addIncoming(c64(0), pre);
    Value* off = B.CreateMul(i, c64(es));
    Value* p = B.CreateSelect(back, B.CreateSub(base, off), B.CreateAdd(base, off));
    B.CreateAlignedStore(val, B.CreateIntToPtr(p, Ptr), Align(1));
    Value* i1 = B.CreateAdd(i, c64(1));
    i->addIncoming(i1, loop);
    B.CreateCondBr(B.CreateICmpNE(i1, n), loop, done);
    B.SetInsertPoint(done);
    def(node, B.CreateSelect(back, B.CreateSub(base, bytes), B.CreateAdd(base, bytes)));
    return true;
  }
  case OP_STORECONTEXTINDEXED: {
    auto* o = op->C<IROp_StoreContextIndexed>();
    Value* base = B.CreateGEP(I8, S, {B.CreateMul(A(o->Index), c64(o->Stride))});
    ctxStore(o->BaseOffset, A(o->Value), size, o->Class == RegClass::FPR, base);
    return true;
  }
  case OP_LOADCONTEXTPAIR: {
    auto* o = op->C<IROp_LoadContextPair>();
    const bool f = o->Class == RegClass::FPR;
    setVar(o->OutValue1, ctxLoad(o->Offset, size, f));
    setVar(o->OutValue2, ctxLoad(o->Offset + size, size, f));
    return true;
  }
  case OP_CACHELINECLEAR: case OP_CACHELINECLEAN: case OP_TELEMETRYSETVALUE: return true;
  case OP_CACHELINEZERO: {
    Value* a = B.CreateAnd(A(op->C<IROp_CacheLineZero>()->Addr), c64(~63ull));
    B.CreateMemSet(B.CreateIntToPtr(a, Ptr), B.getInt8(0), 64, Align(1));
    return true;
  }
  case OP_XGETBV: {
    auto* o = op->C<IROp_XGetBV>();
    Value* r = B.CreateCall(m.helper("bbr_xgetbv", I64, {I64}), {A(o->Function)});
    setVar(o->OutEAX, B.CreateAnd(r, c64(0xFFFFFFFF)));
    setVar(o->OutEDX, B.CreateLShr(r, c64(32)));
    return true;
  }
  case OP_SYSCALL: {
    spill(true);
    B.CreateCall(m.helper("bbr_syscall", m.Void, {Ptr}), {S});
    fill();
    return true;
  }
  case OP_GETROUNDINGMODE: def(node, B.CreateCall(m.helper("bbr_get_rounding", I64, {}), {})); return true;
  case OP_PUSHROUNDINGMODE: def(node, B.CreateCall(m.helper("bbr_push_rounding", I64, {I64}), {c64(op->C<IROp_PushRoundingMode>()->RoundMode)})); return true;
  case OP_POPROUNDINGMODE: B.CreateCall(m.helper("bbr_pop_rounding", m.Void, {I64}), {A(op->C<IROp_PopRoundingMode>()->FPCR)}); return true;
  default: return false;
  }
}

// ------------------------------------------------------------------------------------ memory ops -----
bool Lower::emitMemOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned size = sz(op);
  switch (op->Op) {
  case OP_LOADMEM: {
    auto* o = op->C<IROp_LoadMem>();
    def(node, loadVal(memAddr(o->Addr, o->Offset, o->OffsetType, o->OffsetScale), size, o->Class == RegClass::FPR));
    return true;
  }
  case OP_STOREMEM: {
    auto* o = op->C<IROp_StoreMem>();
    storeVal(memAddr(o->Addr, o->Offset, o->OffsetType, o->OffsetScale), A(o->Value), size, o->Class == RegClass::FPR);
    return true;
  }
  case OP_LOADMEMPAIR: {
    auto* o = op->C<IROp_LoadMemPair>();
    const bool f = o->Class == RegClass::FPR;
    Value* a = B.CreateAdd(A(o->Addr), c64((int64_t)(int32_t)o->Offset));
    setVar(o->OutValue1, loadVal(a, size, f));
    setVar(o->OutValue2, loadVal(B.CreateAdd(a, c64(size)), size, f));
    return true;
  }
  case OP_STOREMEMPAIR: {
    auto* o = op->C<IROp_StoreMemPair>();
    const bool f = o->Class == RegClass::FPR;
    Value* a = B.CreateAdd(A(o->Addr), c64((int64_t)(int32_t)o->Offset));
    storeVal(a, A(o->Value1), size, f);
    storeVal(B.CreateAdd(a, c64(size)), A(o->Value2), size, f);
    return true;
  }
  case OP_LOADCONTEXT: {
    auto* o = op->C<IROp_LoadContext>();
    def(node, ctxLoad(o->Offset, size, o->Class == RegClass::FPR));
    return true;
  }
  case OP_STORECONTEXT: {
    auto* o = op->C<IROp_StoreContext>();
    ctxStore(o->Offset, A(o->Value), size, o->Class == RegClass::FPR);
    return true;
  }
  case OP_STORECONTEXTPAIR: {
    auto* o = op->C<IROp_StoreContextPair>();
    const bool f = o->Class == RegClass::FPR;
    ctxStore(o->Offset, A(o->Value1), size, f);
    ctxStore(o->Offset + size, A(o->Value2), size, f);
    return true;
  }
  case OP_LOADCONTEXTINDEXED: {
    auto* o = op->C<IROp_LoadContextIndexed>();
    Value* base = B.CreateGEP(I8, S, {B.CreateMul(A(o->Index), c64(o->Stride))});
    def(node, ctxLoad(o->BaseOffset, size, o->Class == RegClass::FPR, base));
    return true;
  }
  case OP_FORMCONTEXTADDRESS: {
    auto* o = op->C<IROp_FormContextAddress>();
    def(node, B.CreateAdd(B.CreatePtrToInt(S, I64), B.CreateMul(A(o->Index), c64(o->Stride))));
    return true;
  }
  case OP_CONTEXTCLEAR: {
    auto* o = op->C<IROp_ContextClear>();
    B.CreateMemSet(gep(o->Offset), B.getInt8(0), o->Size, Align(1));
    return true;
  }
  case OP_PUSH: {
    auto* o = op->C<IROp_Push>();
    const unsigned vs = static_cast<unsigned>(o->ValueSize);
    Value* na = B.CreateSub(A(o->Addr), c64(vs));
    if (size == 4) na = zx(tr(na, 32));
    storeVal(na, A(o->Value), vs, false);
    def(node, na);
    return true;
  }
  case OP_POP: {
    auto* o = op->C<IROp_Pop>();
    const unsigned ps = static_cast<unsigned>(o->Size);
    Value* addr = A(o->InoutAddr);
    setVar(o->OutValue, loadVal(addr, ps, false));
    Value* na = B.CreateAdd(addr, c64(ps));
    setVar(o->InoutAddr, size == 4 ? zx(tr(na, 32)) : na);
    return true;
  }
  case OP_MEMCPY: {
    auto* o = op->C<IROp_MemCpy>();
    const unsigned es = static_cast<unsigned>(o->Size);
    Value *dst = A(o->Dest), *src = A(o->Src), *n = A(o->Length);
    uint64_t dk;
    Value* back;
    if (K(o->Direction, dk)) back = ConstantInt::get(I1, (int64_t)dk == -1);
    else back = B.CreateICmpNE(B.CreateAnd(A(o->Direction), c64(2)), c64(0));
    Value* bytes = B.CreateMul(n, c64(es));
    auto* done = newBB("mcdone");
    auto* chk = newBB("mcchk");
    B.CreateCondBr(B.CreateICmpEQ(n, c64(0)), done, chk);
    B.SetInsertPoint(chk);
    // Backward copies start at the highest element; the covered range is [p - (n-1)*es, p + es).
    Value* span = B.CreateSub(bytes, c64(es));
    Value* dlo = B.CreateSelect(back, B.CreateSub(dst, span), dst);
    Value* slo = B.CreateSelect(back, B.CreateSub(src, span), src);
    Value* dist = B.CreateSub(dlo, slo);
    Value* adist = B.CreateSelect(B.CreateICmpSLT(dist, c64(0)), B.CreateNeg(dist), dist);
    auto* fast = newBB("mcfast");
    auto* slow = newBB("mcslow");
    B.CreateCondBr(B.CreateICmpUGE(adist, bytes), fast, slow);
    B.SetInsertPoint(fast); // disjoint ranges: sequential copy == memcpy
    B.CreateMemCpy(B.CreateIntToPtr(dlo, Ptr), Align(1), B.CreateIntToPtr(slo, Ptr), Align(1), bytes);
    B.CreateBr(done);
    B.SetInsertPoint(slow); // overlapping: element by element in the guest's order
    auto* loop = newBB("mcloop");
    B.CreateBr(loop);
    B.SetInsertPoint(loop);
    PHINode* i = B.CreatePHI(I64, 2);
    i->addIncoming(c64(0), slow);
    Value* off = B.CreateMul(i, c64(es));
    Value* sp = B.CreateSelect(back, B.CreateSub(src, off), B.CreateAdd(src, off));
    Value* dp = B.CreateSelect(back, B.CreateSub(dst, off), B.CreateAdd(dst, off));
    Value* e = B.CreateAlignedLoad(iN(es * 8), B.CreateIntToPtr(sp, Ptr), Align(1));
    B.CreateAlignedStore(e, B.CreateIntToPtr(dp, Ptr), Align(1));
    Value* i1 = B.CreateAdd(i, c64(1));
    i->addIncoming(i1, loop);
    B.CreateCondBr(B.CreateICmpNE(i1, n), loop, done);
    B.SetInsertPoint(done);
    setVar(o->OutDstAddress, B.CreateSelect(back, B.CreateSub(dst, bytes), B.CreateAdd(dst, bytes)));
    setVar(o->OutSrcAddress, B.CreateSelect(back, B.CreateSub(src, bytes), B.CreateAdd(src, bytes)));
    return true;
  }
  case OP_PREFETCH: return true;
  default: return false;
  }
}

// ------------------------------------------------------------------------------------ atomics -------
bool Lower::emitAtomicOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned size = sz(op);
  const auto SC = AtomicOrdering::SequentiallyConsistent;
  auto ptr = [&](W w) { return B.CreateIntToPtr(A(w), Ptr); };
  auto rmw = [&](AtomicRMWInst::BinOp bop, W val, W addr, bool negate = false, bool inv = false) {
    Value* v = tr(A(val), size * 8);
    if (negate) v = B.CreateNeg(v);
    if (inv) v = B.CreateNot(v);
    Value* old = B.CreateAtomicRMW(bop, ptr(addr), v, Align(size), SC);
    def(node, zx(old));
  };
  switch (op->Op) {
  case OP_CAS: {
    auto* o = op->C<IROp_CAS>();
    auto* r = B.CreateAtomicCmpXchg(ptr(o->Addr), tr(A(o->Expected), size * 8), tr(A(o->Desired), size * 8), Align(size), SC, SC);
    def(node, zx(B.CreateExtractValue(r, 0)));
    return true;
  }
  case OP_CASPAIR: {
    auto* o = op->C<IROp_CASPair>();
    const unsigned eb = static_cast<unsigned>(op->ElementSize) * 8;
    auto pack = [&](W lo, W hi) {
      Value* l = B.CreateZExt(tr(A(lo), eb), iN(eb * 2));
      Value* h = B.CreateShl(B.CreateZExt(tr(A(hi), eb), iN(eb * 2)), cN(eb * 2, eb));
      return B.CreateOr(l, h);
    };
    auto* r = B.CreateAtomicCmpXchg(ptr(o->Addr), pack(o->ExpectedLo, o->ExpectedHi), pack(o->DesiredLo, o->DesiredHi), Align(eb / 4), SC, SC);
    Value* old = B.CreateExtractValue(r, 0);
    setVar(o->OutLo, zx(B.CreateTrunc(old, iN(eb))));
    setVar(o->OutHi, zx(B.CreateTrunc(B.CreateLShr(old, cN(eb * 2, eb)), iN(eb))));
    return true;
  }
  case OP_ATOMICSWAP: { auto* o = op->C<IROp_AtomicSwap>(); rmw(AtomicRMWInst::Xchg, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHADD: { auto* o = op->C<IROp_AtomicFetchAdd>(); rmw(AtomicRMWInst::Add, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHSUB: { auto* o = op->C<IROp_AtomicFetchSub>(); rmw(AtomicRMWInst::Sub, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHAND: { auto* o = op->C<IROp_AtomicFetchAnd>(); rmw(AtomicRMWInst::And, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHCLR: { auto* o = op->C<IROp_AtomicFetchCLR>(); rmw(AtomicRMWInst::And, o->Value, o->Addr, false, true); return true; }
  case OP_ATOMICFETCHOR: { auto* o = op->C<IROp_AtomicFetchOr>(); rmw(AtomicRMWInst::Or, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHXOR: { auto* o = op->C<IROp_AtomicFetchXor>(); rmw(AtomicRMWInst::Xor, o->Value, o->Addr); return true; }
  case OP_ATOMICFETCHNEG: {
    // old = *p; *p = -old, atomically: a compare-exchange loop.
    auto* o = op->C<IROp_AtomicFetchNeg>();
    Value* p = ptr(o->Addr);
    auto* ty = iN(size * 8);
    LoadInst* l0 = B.CreateAlignedLoad(ty, p, Align(size));
    l0->setAtomic(AtomicOrdering::Monotonic);
    auto* pre = curBB();
    auto* loop = newBB("negloop");
    auto* done = newBB("negdone");
    B.CreateBr(loop);
    B.SetInsertPoint(loop);
    PHINode* cur = B.CreatePHI(ty, 2);
    cur->addIncoming(l0, pre);
    auto* r = B.CreateAtomicCmpXchg(p, cur, B.CreateNeg(cur), Align(size), SC, SC);
    Value* old = B.CreateExtractValue(r, 0);
    cur->addIncoming(old, loop);
    B.CreateCondBr(B.CreateExtractValue(r, 1), done, loop);
    B.SetInsertPoint(done);
    def(node, zx(old));
    return true;
  }
  default: return false;
  }
}

} // namespace bbr
