// Vector and floating-point ops. Each op does what the Arm64 JIT's NEON sequence does
// (FEXCore/Source/Interface/Core/JIT/{VectorOps,ConversionOps}.cpp), written as generic LLVM vector IR; the few
// ops with no portable form (tbl/tbx with run-time indices) use the aarch64 intrinsic or a pshufb sequence.
//
// Conventions: a vector value is <2 x i64> (128 bits). An op of IR size S < 16 works on the low S bytes and zeroes the
// rest (what NEON's D/S/H forms do); S = 16 uses all lanes. `n` = S / ElementSize lanes.
#include "lower_impl.h"
#include "bbr_x87.h"

using namespace llvm;

namespace bbr {

// the helpers read the guest's x87 state at these offsets (bbr_x87.h)
static_assert(offsetof(Core::CPUState, FCW) == BBR_X87_FCW_OFFSET);
static_assert(offsetof(Core::CPUState, flags) + X86State::X87FLAG_IE_LOC == BBR_X87_IE_OFFSET);

namespace {
// ---- the named vector constants (FEXCore/Source/Interface/Core/CPUBackend.cpp, copied) ----
constexpr uint64_t NamedVectorConstants[IR::NamedVectorConstant::NAMED_VECTOR_CONST_POOL_MAX][2] = {
  {0x0003'0002'0001'0000ULL, 0x0007'0006'0005'0004ULL}, {0x000B'000A'0009'0008ULL, 0x000F'000E'000D'000CULL},
  {0x0000'0000'8000'0000ULL, 0x0000'0000'8000'0000ULL}, {0x0000'0000'8000'0000ULL, 0x0000'0000'8000'0000ULL},
  {0x8000'0000'0000'0000ULL, 0x0000'0000'0000'0000ULL}, {0x8000'0000'0000'0000ULL, 0x0000'0000'0000'0000ULL},
  {0x8000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL}, {0x8000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL},
  {0x0000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL}, {0x0000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL},
  {0x0000'0001'0000'0000ULL, 0x0000'0003'0000'0002ULL}, {0x040B'0E01'0B0E'0104ULL, 0x0C03'0609'0306'090CULL},
  {0x0706'0504'FFFF'FFFFULL, 0xFFFF'FFFF'0B0A'0908ULL}, {0x0706'0504'0302'0100ULL, 0xFFFF'FFFF'0B0A'0908ULL},
  {0xFFFF'FFFF'0302'0100ULL, 0x0F0E'0D0C'FFFF'FFFFULL}, {0x0706'0504'0302'0100ULL, 0x0F0E'0D0C'FFFF'FFFFULL},
  {0xFFFF'FFFF'0302'0100ULL, 0x0F0E'0D0C'0B0A'0908ULL}, {0x0706'0504'FFFF'FFFFULL, 0x0F0E'0D0C'0B0A'0908ULL},
  {0x8040'2010'0804'0201ULL, 0x8040'2010'0804'0201ULL}, {0x8040'2010'0804'0201ULL, 0x8040'2010'0804'0201ULL},
  {0x0706'0504'0302'0100ULL, 0x1716'1514'1312'1110ULL}, {0x0F0E'0D0C'0B0A'0908ULL, 0x1F1E'1D1C'1B1A'1918ULL},
  {0x8000'0000'0000'0000ULL, 0x0000'0000'0000'3FFFULL}, {0xD49A'784B'CD1B'8AFEULL, 0x0000'0000'0000'4000ULL},
  {0xB8AA'3B29'5C17'F0BCULL, 0x0000'0000'0000'3FFFULL}, {0xC90F'DAA2'2168'C235ULL, 0x0000'0000'0000'4000ULL},
  {0x9A20'9A84'FBCF'F799ULL, 0x0000'0000'0000'3FFDULL}, {0xB172'17F7'D1CF'79ACULL, 0x0000'0000'0000'3FFEULL},
  {0x4F00'0000'4F00'0000ULL, 0x4F00'0000'4F00'0000ULL}, {0x4F00'0000'4F00'0000ULL, 0x4F00'0000'4F00'0000ULL},
  {0x5F00'0000'5F00'0000ULL, 0x5F00'0000'5F00'0000ULL}, {0x41E0'0000'0000'0000ULL, 0x41E0'0000'0000'0000ULL},
  {0x41E0'0000'0000'0000ULL, 0x41E0'0000'0000'0000ULL}, {0x43E0'0000'0000'0000ULL, 0x43E0'0000'0000'0000ULL},
  {0x8000'0000'8000'0000ULL, 0x8000'0000'8000'0000ULL}, {0x8000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL},
  {0x0000'0000'0000'0000ULL, 0x0000'0000'0000'8000ULL}, {0x5A82'7999'5A82'7999ULL, 0x5A82'7999'5A82'7999ULL},
  {0x6ED9'EBA1'6ED9'EBA1ULL, 0x6ED9'EBA1'6ED9'EBA1ULL}, {0x8F1B'BCDC'8F1B'BCDCULL, 0x8F1B'BCDC'8F1B'BCDCULL},
  {0xCA62'C1D6'CA62'C1D6ULL, 0xCA62'C1D6'CA62'C1D6ULL},
};

// The indexed tables (same CPUBackend.cpp), as bytes. Entry size 16.
std::vector<uint8_t> indexedTable(IR::IndexNamedVectorConstant c) {
  std::vector<uint8_t> t;
  auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) t.push_back(v >> (8 * i)); };
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) t.push_back(v >> (8 * i)); };
  auto put16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) t.push_back(v >> (8 * i)); };
  const uint64_t w16[4] = {0x01'00, 0x03'02, 0x05'04, 0x07'06}, h16[4] = {0x09'08, 0x0b'0a, 0x0d'0c, 0x0f'0e};
  const uint64_t w32[4] = {0x03'02'01'00, 0x07'06'05'04, 0x0b'0a'09'08, 0x0f'0e'0d'0c};
  switch (c) {
  case IR::INDEXED_NAMED_VECTOR_PSHUFLW:
    for (unsigned i = 0; i < 256; ++i) {
      put64((w16[i & 3] << 0) | (w16[i >> 2 & 3] << 16) | (w16[i >> 4 & 3] << 32) | (w16[i >> 6 & 3] << 48));
      put64(0x0f'0e'0d'0c'0b'0a'09'08);
    }
    break;
  case IR::INDEXED_NAMED_VECTOR_PSHUFHW:
    for (unsigned i = 0; i < 256; ++i) {
      put64(0x07'06'05'04'03'02'01'00);
      put64((h16[i & 3] << 0) | (h16[i >> 2 & 3] << 16) | (h16[i >> 4 & 3] << 32) | (h16[i >> 6 & 3] << 48));
    }
    break;
  case IR::INDEXED_NAMED_VECTOR_PSHUFD:
    for (unsigned i = 0; i < 256; ++i) { put64((w32[i & 3] << 0) | (w32[i >> 2 & 3] << 32)); put64((w32[i >> 4 & 3] << 0) | (w32[i >> 6 & 3] << 32)); }
    break;
  case IR::INDEXED_NAMED_VECTOR_SHUFPS:
    for (unsigned i = 0; i < 256; ++i) {
      put64((w32[i & 3] << 0) | (w32[i >> 2 & 3] << 32));
      put64(((w32[i >> 4 & 3] + 0x10101010) << 0) | ((w32[i >> 6 & 3] + 0x10101010) << 32));
    }
    break;
  case IR::INDEXED_NAMED_VECTOR_DPPS_MASK:
    for (unsigned i = 0; i < 16; ++i) for (unsigned j = 0; j < 4; ++j) put32(i & (1u << j) ? 0xFFFFFFFFu : 0u);
    break;
  case IR::INDEXED_NAMED_VECTOR_DPPD_MASK:
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 2; ++j) put64(i & (1u << j) ? ~0ull : 0ull);
    break;
  case IR::INDEXED_NAMED_VECTOR_PBLENDW:
    for (unsigned i = 0; i < 256; ++i) for (unsigned j = 0; j < 8; ++j) put16((i >> j & 1) ? (uint16_t)(((2 * j + 1) << 8) | (2 * j)) : 0xFFFF);
    break;
  default: break;
  }
  return t;
}
} // namespace

bool Lower::emitVectorOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned isz = sz(op);        // the IR register size in bytes
  const unsigned size = isz;          // lanes used: the IR size unless a case overrides it (the JIT picks Q or D forms itself)
  const unsigned e = esz(op);         // element size in bytes (0 when the op has none)
  auto I = [&](unsigned bytes) { return iN(bytes * 8); };
  auto FT = [&](unsigned bytes) -> Type* { return bytes == 2 ? Type::getHalfTy(C) : bytes == 4 ? Type::getFloatTy(C) : Type::getDoubleTy(C); };
  auto VT = [&](Type* el, unsigned cnt) { return FixedVectorType::get(el, cnt); };
  // V128 -> its low `bytes` as an integer
  auto lowBits = [&](Value* v, unsigned bytes) -> Value* { Value* i = fromV(v); return bytes >= 16 ? i : B.CreateTrunc(i, I(bytes)); };
  auto lanesI = [&](Value* v, unsigned bytes, unsigned eb) { return B.CreateBitCast(lowBits(v, bytes), VT(I(eb), bytes / eb)); };
  auto lanesF = [&](Value* v, unsigned bytes, unsigned eb) { return B.CreateBitCast(lowBits(v, bytes), VT(FT(eb), bytes / eb)); };
  // a vector (or scalar) of at most 128 bits -> V128, zero-extended
  auto wide = [&](Value* small) -> Value* {
    const unsigned bits = small->getType()->getPrimitiveSizeInBits().getFixedValue();
    Value* i = B.CreateBitCast(small, iN(bits));
    return toV(bits >= 128 ? i : B.CreateZExt(i, I128));
  };
  auto zeroV = [&]() -> Value* { return ConstantAggregateZero::get(V128); };
  auto ret = [&](Value* small) { def(node, wide(small)); return true; };
  const unsigned n = e ? size / e : 1; // lanes

  auto cmpMask = [&](Value* i1v, unsigned ebits, unsigned lanes) { return B.CreateSExt(i1v, VT(iN(ebits), lanes)); };

  // scalar insert: V1 with lane 0 replaced (element size in bytes)
  auto insertLane0 = [&](Value* v1, Value* scalarBits, unsigned eb) -> Value* {
    return B.CreateBitCast(B.CreateInsertElement(B.CreateBitCast(v1, VT(I(eb), 16 / eb)), scalarBits, (uint64_t)0), V128);
  };
  auto lane0 = [&](Value* v, unsigned eb) -> Value* { return B.CreateExtractElement(B.CreateBitCast(v, VT(I(eb), 16 / eb)), (uint64_t)0); };
  auto toFp = [&](Value* bitsV, unsigned eb) { return B.CreateBitCast(bitsV, FT(eb)); };
  auto fromFp = [&](Value* f, unsigned eb) { return B.CreateBitCast(f, I(eb)); };
  auto roundId = [&](RoundMode r) {
    switch (r) {
    case RoundMode::Nearest: return Intrinsic::roundeven;
    case RoundMode::NegInfinity: return Intrinsic::floor;
    case RoundMode::PosInfinity: return Intrinsic::ceil;
    case RoundMode::TowardsZero: return Intrinsic::trunc;
    default: return Intrinsic::nearbyint;
    }
  };

  switch (op->Op) {
  case OP_VEXTRACTTOGPR: {
    auto* o = op->C<IROp_VExtractToGPR>();
    def(node, zx(B.CreateExtractElement(lanesI(A(o->Vector), 16, size), (uint64_t)o->Index)));
    return true;
  }
  case OP_FLOAT_TOGPR_ZS: case OP_FLOAT_TOGPR_S: {
    // fcvtzs (saturating, NaN -> 0); the _S form rounds with the current mode first
    const bool zs = op->Op == OP_FLOAT_TOGPR_ZS;
    Value* v = A(zs ? op->C<IROp_Float_ToGPR_ZS>()->Scalar : op->C<IROp_Float_ToGPR_S>()->Scalar);
    const unsigned se = static_cast<unsigned>(zs ? op->C<IROp_Float_ToGPR_ZS>()->SrcElementSize : op->C<IROp_Float_ToGPR_S>()->SrcElementSize);
    Value* f = toFp(lane0(v, se), se);
    if (!zs) f = B.CreateUnaryIntrinsic(Intrinsic::nearbyint, f);
    def(node, zx(B.CreateIntrinsic(Intrinsic::fptosi_sat, {iN(size == 8 ? 64 : 32), f->getType()}, {f})));
    return true;
  }
  case OP_FCMP: {
    auto* o = op->C<IROp_FCmp>();
    const unsigned fe = static_cast<unsigned>(o->ElementSize);
    setFromFCmp(toFp(lane0(A(o->Scalar1), fe), fe), toFp(lane0(A(o->Scalar2), fe), fe));
    return true;
  }

  // ---- constants and moves
  case OP_LOADNAMEDVECTORCONSTANT: {
    const auto c = op->C<IROp_LoadNamedVectorConstant>()->Constant;
    if (c == NamedVectorConstant::NAMED_VECTOR_ZERO) { def(node, zeroV()); return true; }
    const uint64_t* d = NamedVectorConstants[c];
    const uint64_t words[2] = {d[0], d[1]};
    APInt val(128, words);
    if (size < 16) val = val.zextOrTrunc(size * 8).zext(128);
    def(node, toV(ConstantInt::get(C, val)));
    return true;
  }
  case OP_LOADNAMEDVECTORINDEXEDCONSTANT: {
    auto* o = op->C<IROp_LoadNamedVectorIndexedConstant>();
    auto t = indexedTable(o->Constant);
    if (o->Index + size > t.size()) return false;
    APInt v(128, 0);
    for (unsigned i = 0; i < size; ++i) v.insertBits(APInt(8, t[o->Index + i]), i * 8);
    def(node, toV(ConstantInt::get(C, v)));
    return true;
  }
  case OP_VECTORIMM: {
    auto* o = op->C<IROp_VectorImm>();
    const uint64_t lane = e == 1 ? o->Immediate : ((uint64_t)o->Immediate << o->ShiftAmount);
    Constant* el = ConstantInt::get(I(e), lane);
    def(node, B.CreateBitCast(ConstantVector::getSplat(ElementCount::getFixed(16 / e), el), V128));
    return true;
  }
  case OP_VMOV: {
    Value* v = A(op->C<IROp_VMov>()->Source);
    def(node, size >= 16 ? v : wide(B.CreateTrunc(fromV(v), I(size))));
    return true;
  }
  case OP_VCASTFROMGPR: {
    Value* g = A(op->C<IROp_VCastFromGPR>()->Src);
    def(node, toV(B.CreateZExt(tr(g, e * 8), I128)));
    return true;
  }
  case OP_VLOADTWOGPRS: {
    auto* o = op->C<IROp_VLoadTwoGPRs>();
    Value* v = B.CreateInsertElement(B.CreateInsertElement(zeroV(), A(o->Lower), (uint64_t)0), A(o->Upper), (uint64_t)1);
    def(node, v);
    return true;
  }
  case OP_VDUPFROMGPR: {
    Value* g = tr(A(op->C<IROp_VDupFromGPR>()->Src), e * 8);
    return ret(B.CreateVectorSplat(n, g));
  }
  case OP_VINSGPR: {
    auto* o = op->C<IROp_VInsGPR>();
    Value* v = B.CreateInsertElement(lanesI(A(o->DestVector), 16, e), tr(A(o->Src), e * 8), (uint64_t)o->DestIdx);
    def(node, B.CreateBitCast(v, V128));
    return true;
  }
  case OP_VINSELEMENT: {
    auto* o = op->C<IROp_VInsElement>();
    Value* x = B.CreateExtractElement(lanesI(A(o->SrcVector), 16, e), (uint64_t)o->SrcIdx);
    def(node, B.CreateBitCast(B.CreateInsertElement(lanesI(A(o->DestVector), 16, e), x, (uint64_t)o->DestIdx), V128));
    return true;
  }
  case OP_VDUPELEMENT: {
    auto* o = op->C<IROp_VDupElement>();
    Value* x = B.CreateExtractElement(lanesI(A(o->Vector), 16, e), (uint64_t)o->Index);
    return ret(B.CreateVectorSplat(n, x));
  }
  case OP_VEXTR: {
    // ext Vd, Vn = VectorUpper, Vm = VectorLower: bytes [Index*e, +size) of (Vn:Vm)
    auto* o = op->C<IROp_VExtr>();
    Value *lo = A(o->VectorUpper), *hi = A(o->VectorLower);
    unsigned off = o->Index * e;
    if (off >= size) { lo = hi; hi = zeroV(); off -= size; }
    auto* bt = VT(I8, 16);
    Value* a = B.CreateBitCast(lo, bt);
    Value* b = B.CreateBitCast(hi, bt);
    std::vector<int> mask;
    for (unsigned i = 0; i < size; ++i) { const unsigned j = off + i; mask.push_back(j < size ? (int)j : (int)(16 + (j - size))); }
    return ret(B.CreateShuffleVector(a, b, mask));
  }
  case OP_VBROADCASTFROMMEM: {
    Value* addr = A(op->C<IROp_VBroadcastFromMem>()->Address);
    if (e == 16) { def(node, loadVal(addr, 16, true)); return true; }
    return ret(B.CreateVectorSplat(n, tr(loadVal(addr, e, false), e * 8)));
  }
  case OP_VLOADVECTORELEMENT: {
    auto* o = op->C<IROp_VLoadVectorElement>();
    if (e == 16) { def(node, loadVal(A(o->Addr), 16, true)); return true; }
    Value* x = tr(loadVal(A(o->Addr), e, false), e * 8);
    def(node, B.CreateBitCast(B.CreateInsertElement(lanesI(A(o->DstSrc), 16, e), x, (uint64_t)o->Index), V128));
    return true;
  }
  case OP_VSTOREVECTORELEMENT: {
    auto* o = op->C<IROp_VStoreVectorElement>();
    if (e == 16) { storeVal(A(o->Addr), A(o->Value), 16, true); return true; }
    Value* x = B.CreateExtractElement(lanesI(A(o->Value), 16, e), (uint64_t)o->Index);
    storeVal(A(o->Addr), zx(x), e, false);
    return true;
  }
  case OP_VLOADNONTEMPORAL: {
    auto* o = op->C<IROp_VLoadNonTemporal>();
    def(node, loadVal(B.CreateAdd(A(o->Addr), c64((int64_t)o->Offset)), size, true));
    return true;
  }
  case OP_VSTORENONTEMPORAL: {
    auto* o = op->C<IROp_VStoreNonTemporal>();
    storeVal(B.CreateAdd(A(o->Addr), c64((int64_t)o->Offset)), A(o->Value), size, true);
    return true;
  }

  // ---- integer lanes
  case OP_VADD: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VAdd>(); return ret(B.CreateAdd(lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VSUB: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VSub>(); return ret(B.CreateSub(lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VMUL: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VMul>(); return ret(B.CreateMul(lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VAND: { const unsigned size = (isz == 16 ? 16u : 8u), n = size / e; auto* o = op->C<IROp_VAnd>(); return ret(B.CreateAnd(lanesI(A(o->Vector1), size, 1), lanesI(A(o->Vector2), size, 1))); }
  case OP_VOR: { const unsigned size = (isz == 16 ? 16u : 8u), n = size / e; auto* o = op->C<IROp_VOr>(); return ret(B.CreateOr(lanesI(A(o->Vector1), size, 1), lanesI(A(o->Vector2), size, 1))); }
  case OP_VXOR: { const unsigned size = (isz == 16 ? 16u : 8u), n = size / e; auto* o = op->C<IROp_VXor>(); return ret(B.CreateXor(lanesI(A(o->Vector1), size, 1), lanesI(A(o->Vector2), size, 1))); }
  case OP_VANDN: { const unsigned size = (isz == 16 ? 16u : 8u), n = size / e; auto* o = op->C<IROp_VAndn>(); return ret(B.CreateAnd(lanesI(A(o->Vector1), size, 1), B.CreateNot(lanesI(A(o->Vector2), size, 1)))); }
  case OP_VORN: { const unsigned size = (isz == 16 ? 16u : 8u), n = size / e; auto* o = op->C<IROp_VOrn>(); return ret(B.CreateOr(lanesI(A(o->Vector1), size, 1), B.CreateNot(lanesI(A(o->Vector2), size, 1)))); }
  case OP_VNOT: return ret(B.CreateNot(lanesI(A(op->C<IROp_VNot>()->Vector), size, 1)));
  case OP_VNEG: return ret(B.CreateNeg(lanesI(A(op->C<IROp_VNeg>()->Vector), size, e)));
  case OP_VABS: { const unsigned size = (e == isz ? 8u : 16u), n = size / e; Value* x = lanesI(A(op->C<IROp_VAbs>()->Vector), size, e); return ret(B.CreateBinaryIntrinsic(Intrinsic::abs, x, B.getFalse())); }
  case OP_VPOPCOUNT: { const unsigned size = (e == isz ? 8u : 16u), n = size / e; Value* x = lanesI(A(op->C<IROp_VPopcount>()->Vector), size, 1); return ret(B.CreateUnaryIntrinsic(Intrinsic::ctpop, x)); }
  case OP_VUQADD: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VUQAdd>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::uadd_sat, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VUQSUB: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VUQSub>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::usub_sat, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VSQADD: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VSQAdd>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::sadd_sat, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VSQSUB: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VSQSub>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::ssub_sat, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VUMIN: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VUMin>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::umin, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VUMAX: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VUMax>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::umax, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VSMIN: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VSMin>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::smin, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VSMAX: { const unsigned size = 16, n = size / e; auto* o = op->C<IROp_VSMax>(); return ret(B.CreateBinaryIntrinsic(Intrinsic::smax, lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e))); }
  case OP_VURAVG: { const unsigned size = 16, n = size / e;
    auto* o = op->C<IROp_VURAvg>();
    Value *a = lanesI(A(o->Vector1), size, e), *b = lanesI(A(o->Vector2), size, e);
    Value* one = ConstantInt::get(a->getType(), 1);
    return ret(B.CreateSub(B.CreateOr(a, b), B.CreateLShr(B.CreateXor(a, b), one))); // (a + b + 1) >> 1 without overflow
  }
  case OP_VADDV: { const unsigned size = 16, n = size / e; // addv: sum of all lanes into lane 0
    Value* x = lanesI(A(op->C<IROp_VAddV>()->Vector), size, e);
    return ret(B.CreateIntrinsic(Intrinsic::vector_reduce_add, {x->getType()}, {x}));
  }
  case OP_VUMINV: { const unsigned size = 16, n = size / e; Value* x = lanesI(A(op->C<IROp_VUMinV>()->Vector), size, e); return ret(B.CreateIntrinsic(Intrinsic::vector_reduce_umin, {x->getType()}, {x})); }
  case OP_VUMAXV: { const unsigned size = 16, n = size / e; Value* x = lanesI(A(op->C<IROp_VUMaxV>()->Vector), size, e); return ret(B.CreateIntrinsic(Intrinsic::vector_reduce_umax, {x->getType()}, {x})); }
  case OP_VCMPEQ: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VCMPEQ>(); return ret(cmpMask(B.CreateICmpEQ(lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e)), e * 8, n)); }
  case OP_VCMPGT: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VCMPGT>(); return ret(cmpMask(B.CreateICmpSGT(lanesI(A(o->Vector1), size, e), lanesI(A(o->Vector2), size, e)), e * 8, n)); }
  case OP_VCMPEQZ: { const unsigned size = (e == isz ? e : 16u), n = size / e; Value* x = lanesI(A(op->C<IROp_VCMPEQZ>()->Vector), size, e); return ret(cmpMask(B.CreateICmpEQ(x, Constant::getNullValue(x->getType())), e * 8, n)); }
  case OP_VCMPGTZ: { const unsigned size = (e == isz ? e : 16u), n = size / e; Value* x = lanesI(A(op->C<IROp_VCMPGTZ>()->Vector), size, e); return ret(cmpMask(B.CreateICmpSGT(x, Constant::getNullValue(x->getType())), e * 8, n)); }
  case OP_VCMPLTZ: { const unsigned size = (e == isz ? e : 16u), n = size / e; Value* x = lanesI(A(op->C<IROp_VCMPLTZ>()->Vector), size, e); return ret(cmpMask(B.CreateICmpSLT(x, Constant::getNullValue(x->getType())), e * 8, n)); }
  case OP_VBSL: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    auto* o = op->C<IROp_VBSL>();
    Value *m = lanesI(A(o->VectorMask), size, 1), *t = lanesI(A(o->VectorTrue), size, 1), *f = lanesI(A(o->VectorFalse), size, 1);
    return ret(B.CreateOr(B.CreateAnd(m, t), B.CreateAnd(B.CreateNot(m), f)));
  }

  // ---- shifts
  case OP_VSHLI: case OP_VUSHRI: case OP_VSSHRI: { const unsigned size = 16, n = size / e;
    const unsigned bs = op->Op == OP_VSHLI ? op->C<IROp_VShlI>()->BitShift : op->Op == OP_VUSHRI ? op->C<IROp_VUShrI>()->BitShift : op->C<IROp_VSShrI>()->BitShift;
    Value* x = lanesI(A(op->Op == OP_VSHLI ? op->C<IROp_VShlI>()->Vector : op->Op == OP_VUSHRI ? op->C<IROp_VUShrI>()->Vector : op->C<IROp_VSShrI>()->Vector), size, e);
    auto* t = x->getType();
    if (op->Op == OP_VSSHRI) return ret(B.CreateAShr(x, ConstantInt::get(t, std::min(bs, e * 8 - 1))));
    if (bs >= e * 8) return ret(Constant::getNullValue(t));
    Value* a = ConstantInt::get(t, bs);
    return ret(op->Op == OP_VSHLI ? B.CreateShl(x, a) : B.CreateLShr(x, a));
  }
  case OP_VUSHL: case OP_VUSHR: case OP_VSSHR: { const unsigned size = 16, n = size / e;
    // ushl/sshl take a per-lane amount (RangeCheck clamps it first); right shifts negate it
    const bool left = op->Op == OP_VUSHL, arith = op->Op == OP_VSSHR;
    Value* vec = A(left ? op->C<IROp_VUShl>()->Vector : op->Op == OP_VUSHR ? op->C<IROp_VUShr>()->Vector : op->C<IROp_VSShr>()->Vector);
    Value* shv = A(left ? op->C<IROp_VUShl>()->ShiftVector : op->Op == OP_VUSHR ? op->C<IROp_VUShr>()->ShiftVector : op->C<IROp_VSShr>()->ShiftVector);
    const bool rc = left ? op->C<IROp_VUShl>()->RangeCheck : op->Op == OP_VUSHR ? op->C<IROp_VUShr>()->RangeCheck : op->C<IROp_VSShr>()->RangeCheck;
    Value *x = lanesI(vec, size, e), *s = lanesI(shv, size, e);
    auto* t = x->getType();
    const unsigned Wd = e * 8;
    if (rc) s = B.CreateBinaryIntrinsic(Intrinsic::umin, s, ConstantInt::get(t, arith ? Wd - 1 : Wd));
    Value* inRange = B.CreateICmpULT(s, ConstantInt::get(t, Wd));
    Value* amt = B.CreateAnd(s, ConstantInt::get(t, Wd - 1));
    Value* r;
    if (left) r = B.CreateSelect(inRange, B.CreateShl(x, amt), Constant::getNullValue(t));
    else if (arith) r = B.CreateAShr(x, B.CreateSelect(inRange, amt, ConstantInt::get(t, Wd - 1)));
    else r = B.CreateSelect(inRange, B.CreateLShr(x, amt), Constant::getNullValue(t));
    return ret(r);
  }
  case OP_VUSHLS: case OP_VUSHRS: case OP_VSSHRS: { const unsigned size = 16, n = size / e;
    // one count (the low element of ShiftScalar) for every lane
    const bool left = op->Op == OP_VUSHLS, arith = op->Op == OP_VSSHRS;
    Value* vec = A(left ? op->C<IROp_VUShlS>()->Vector : op->Op == OP_VUSHRS ? op->C<IROp_VUShrS>()->Vector : op->C<IROp_VSShrS>()->Vector);
    Value* shs = A(left ? op->C<IROp_VUShlS>()->ShiftScalar : op->Op == OP_VUSHRS ? op->C<IROp_VUShrS>()->ShiftScalar : op->C<IROp_VSShrS>()->ShiftScalar);
    Value* x = lanesI(vec, size, e);
    auto* t = x->getType();
    const unsigned Wd = e * 8;
    Value* cnt = B.CreateVectorSplat(n, B.CreateExtractElement(lanesI(shs, 16, e), (uint64_t)0));
    Value* inRange = B.CreateICmpULT(cnt, ConstantInt::get(t, Wd));
    Value* amt = B.CreateAnd(cnt, ConstantInt::get(t, Wd - 1));
    Value* r;
    if (left) r = B.CreateSelect(inRange, B.CreateShl(x, amt), Constant::getNullValue(t));
    else if (arith) r = B.CreateAShr(x, B.CreateSelect(inRange, amt, ConstantInt::get(t, Wd - 1)));
    else r = B.CreateSelect(inRange, B.CreateLShr(x, amt), Constant::getNullValue(t));
    return ret(r);
  }
  case OP_VUSHRSWIDE: case OP_VSSHRSWIDE: case OP_VUSHLSWIDE: { const unsigned size = 16, n = size / e;
    // 64-bit count in the low lane of ShiftScalar for all lanes; counts >= the lane width saturate
    const bool left = op->Op == OP_VUSHLSWIDE, arith = op->Op == OP_VSSHRSWIDE;
    Value* vec = A(left ? op->C<IROp_VUShlSWide>()->Vector : op->Op == OP_VUSHRSWIDE ? op->C<IROp_VUShrSWide>()->Vector : op->C<IROp_VSShrSWide>()->Vector);
    Value* shs = A(left ? op->C<IROp_VUShlSWide>()->ShiftScalar : op->Op == OP_VUSHRSWIDE ? op->C<IROp_VUShrSWide>()->ShiftScalar : op->C<IROp_VSShrSWide>()->ShiftScalar);
    Value* x = lanesI(vec, size, e);
    auto* t = x->getType();
    const unsigned Wd = e * 8;
    Value* cnt = B.CreateExtractElement(B.CreateBitCast(shs, VT(I64, 2)), (uint64_t)0);
    Value* big = B.CreateICmpUGE(cnt, c64(Wd));
    Value* amtN = B.CreateTrunc(B.CreateAnd(cnt, c64(Wd - 1)), I(e));
    Value* amt = B.CreateVectorSplat(n, amtN);
    Value* isBig = B.CreateVectorSplat(n, big);
    Value* r;
    if (left) r = B.CreateSelect(isBig, Constant::getNullValue(t), B.CreateShl(x, amt));
    else if (arith) r = B.CreateAShr(x, B.CreateSelect(isBig, ConstantInt::get(t, Wd - 1), amt));
    else r = B.CreateSelect(isBig, Constant::getNullValue(t), B.CreateLShr(x, amt));
    return ret(r);
  }

  // ---- permutes
  case OP_VZIP: case OP_VZIP2: case OP_VUNZIP: case OP_VUNZIP2: case OP_VTRN: case OP_VTRN2: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    Value *lo, *hi;
    switch (op->Op) {
    case OP_VZIP: lo = A(op->C<IROp_VZip>()->VectorLower); hi = A(op->C<IROp_VZip>()->VectorUpper); break;
    case OP_VZIP2: lo = A(op->C<IROp_VZip2>()->VectorLower); hi = A(op->C<IROp_VZip2>()->VectorUpper); break;
    case OP_VUNZIP: lo = A(op->C<IROp_VUnZip>()->VectorLower); hi = A(op->C<IROp_VUnZip>()->VectorUpper); break;
    case OP_VUNZIP2: lo = A(op->C<IROp_VUnZip2>()->VectorLower); hi = A(op->C<IROp_VUnZip2>()->VectorUpper); break;
    case OP_VTRN: lo = A(op->C<IROp_VTrn>()->VectorLower); hi = A(op->C<IROp_VTrn>()->VectorUpper); break;
    default: lo = A(op->C<IROp_VTrn2>()->VectorLower); hi = A(op->C<IROp_VTrn2>()->VectorUpper); break;
    }
    Value *a = lanesI(lo, size, e), *b = lanesI(hi, size, e);
    std::vector<int> m;
    const int N = n;
    for (int i = 0; i < N; ++i) {
      switch (op->Op) {
      case OP_VZIP: m.push_back(i & 1 ? N + i / 2 : i / 2); break;
      case OP_VZIP2: m.push_back(i & 1 ? N + N / 2 + i / 2 : N / 2 + i / 2); break;
      case OP_VUNZIP: m.push_back(i < N / 2 ? 2 * i : N + 2 * (i - N / 2)); break;
      case OP_VUNZIP2: m.push_back(i < N / 2 ? 2 * i + 1 : N + 2 * (i - N / 2) + 1); break;
      case OP_VTRN: m.push_back(i & 1 ? N + (i - 1) : i); break;
      default: m.push_back(i & 1 ? N + i : i + 1); break;
      }
    }
    return ret(B.CreateShuffleVector(a, b, m));
  }
  case OP_VREV32: case OP_VREV64: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    const unsigned grp = op->Op == OP_VREV32 ? 4 : 8;
    Value* x = lanesI(A(op->Op == OP_VREV32 ? op->C<IROp_VRev32>()->Vector : op->C<IROp_VRev64>()->Vector), size, e);
    std::vector<int> m;
    const unsigned per = grp / e;
    for (unsigned i = 0; i < n; ++i) m.push_back((i / per) * per + (per - 1 - i % per));
    return ret(B.CreateShuffleVector(x, m));
  }
  case OP_VADDP: case OP_VFADDP: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    const bool fp = op->Op == OP_VFADDP;
    W wl = fp ? op->C<IROp_VFAddP>()->VectorLower : op->C<IROp_VAddP>()->VectorLower;
    W wu = fp ? op->C<IROp_VFAddP>()->VectorUpper : op->C<IROp_VAddP>()->VectorUpper;
    Value *a = fp ? lanesF(A(wl), size, e) : lanesI(A(wl), size, e), *b = fp ? lanesF(A(wu), size, e) : lanesI(A(wu), size, e);
    std::vector<int> ev, od;
    const int N = n;
    for (int i = 0; i < N; ++i) { // result lanes: pairs of the lower vector, then pairs of the upper vector
      const int src = i < N / 2 ? 0 : N, k = i < N / 2 ? i : i - N / 2;
      ev.push_back(src + 2 * k); od.push_back(src + 2 * k + 1);
    }
    Value* l = B.CreateShuffleVector(a, b, ev);
    Value* r = B.CreateShuffleVector(a, b, od);
    return ret(fp ? B.CreateFAdd(l, r) : B.CreateAdd(l, r));
  }
  case OP_VTBL1: case OP_VTBL2: case OP_VTBX1: {
    Value *t1, *t2 = nullptr, *idx, *dst = nullptr;
    if (op->Op == OP_VTBL1) { auto* o = op->C<IROp_VTBL1>(); t1 = A(o->VectorTable); idx = A(o->VectorIndices); }
    else if (op->Op == OP_VTBL2) { auto* o = op->C<IROp_VTBL2>(); t1 = A(o->VectorTable1); t2 = A(o->VectorTable2); idx = A(o->VectorIndices); }
    else { auto* o = op->C<IROp_VTBX1>(); t1 = A(o->VectorTable); idx = A(o->VectorIndices); dst = A(o->VectorSrcDst); }
    auto* v16 = VT(I8, 16);
    Value* tb1 = B.CreateBitCast(t1, v16);
    Value* ix = B.CreateBitCast(idx, v16);
    Value* r;
    if (m.O.aarch64) {
      if (op->Op == OP_VTBL1) r = B.CreateIntrinsic(Intrinsic::aarch64_neon_tbl1, {v16}, {tb1, ix});
      else if (op->Op == OP_VTBL2) r = B.CreateIntrinsic(Intrinsic::aarch64_neon_tbl2, {v16}, {tb1, B.CreateBitCast(t2, v16), ix});
      else r = B.CreateIntrinsic(Intrinsic::aarch64_neon_tbx1, {v16}, {B.CreateBitCast(dst, v16), tb1, ix});
    } else {
      // pshufb picks table[idx & 15] and zeroes lanes whose idx has bit 7 set: force that for out-of-range idx
      auto pshufb = [&](Value* tbl, Value* ixv, Value* oob) {
        Value* k = B.CreateOr(ixv, B.CreateSExt(oob, v16));
        return B.CreateIntrinsic(Intrinsic::x86_ssse3_pshuf_b_128, {}, {tbl, k});
      };
      Value* oob1 = B.CreateICmpUGT(ix, ConstantInt::get(v16, 15));
      if (op->Op == OP_VTBL1) r = pshufb(tb1, ix, oob1);
      else if (op->Op == OP_VTBX1) r = B.CreateSelect(oob1, B.CreateBitCast(dst, v16), pshufb(tb1, ix, oob1));
      else {
        Value* lo = pshufb(tb1, ix, oob1);
        Value* ix2 = B.CreateSub(ix, ConstantInt::get(v16, 16));
        Value* oob2 = B.CreateOr(B.CreateICmpULT(ix, ConstantInt::get(v16, 16)), B.CreateICmpUGT(ix, ConstantInt::get(v16, 31)));
        Value* hi = pshufb(B.CreateBitCast(t2, v16), ix2, oob2);
        r = B.CreateOr(lo, hi);
      }
    }
    Value* rv = B.CreateBitCast(r, V128);
    def(node, size < 16 ? wide(B.CreateTrunc(fromV(rv), I(size))) : rv);
    return true;
  }

  // ---- widening, narrowing
  case OP_VSXTL: case OP_VUXTL: case OP_VSXTL2: case OP_VUXTL2: {
    const bool sg = op->Op == OP_VSXTL || op->Op == OP_VSXTL2, hi = op->Op == OP_VSXTL2 || op->Op == OP_VUXTL2;
    Value* v = A(op->Op == OP_VSXTL ? op->C<IROp_VSXTL>()->Vector : op->Op == OP_VUXTL ? op->C<IROp_VUXTL>()->Vector : op->Op == OP_VSXTL2 ? op->C<IROp_VSXTL2>()->Vector : op->C<IROp_VUXTL2>()->Vector);
    const unsigned se = e / 2, ln = 8 / se; // source lanes: one 64-bit half
    Value* h = B.CreateTrunc(hi ? B.CreateLShr(fromV(v), cN(128, 64)) : fromV(v), I(8));
    Value* x = B.CreateBitCast(h, VT(I(se), ln));
    return ret(sg ? B.CreateSExt(x, VT(I(e), ln)) : B.CreateZExt(x, VT(I(e), ln)));
  }
  case OP_VSSHLL: case OP_VSSHLL2: {
    const bool hi = op->Op == OP_VSSHLL2;
    Value* v = A(hi ? op->C<IROp_VSSHLL2>()->Vector : op->C<IROp_VSSHLL>()->Vector);
    const unsigned bs = hi ? op->C<IROp_VSSHLL2>()->BitShift : op->C<IROp_VSSHLL>()->BitShift;
    const unsigned se = e / 2, ln = 8 / se;
    Value* h = B.CreateTrunc(hi ? B.CreateLShr(fromV(v), cN(128, 64)) : fromV(v), I(8));
    Value* x = B.CreateSExt(B.CreateBitCast(h, VT(I(se), ln)), VT(I(e), ln));
    return ret(B.CreateShl(x, ConstantInt::get(x->getType(), bs)));
  }
  case OP_VUMULL: case OP_VSMULL: case OP_VUMULL2: case OP_VSMULL2: {
    const bool sg = op->Op == OP_VSMULL || op->Op == OP_VSMULL2, hi = op->Op == OP_VUMULL2 || op->Op == OP_VSMULL2;
    W w1 = op->Op == OP_VUMULL ? op->C<IROp_VUMull>()->Vector1 : op->Op == OP_VSMULL ? op->C<IROp_VSMull>()->Vector1 : op->Op == OP_VUMULL2 ? op->C<IROp_VUMull2>()->Vector1 : op->C<IROp_VSMull2>()->Vector1;
    W w2 = op->Op == OP_VUMULL ? op->C<IROp_VUMull>()->Vector2 : op->Op == OP_VSMULL ? op->C<IROp_VSMull>()->Vector2 : op->Op == OP_VUMULL2 ? op->C<IROp_VUMull2>()->Vector2 : op->C<IROp_VSMull2>()->Vector2;
    const unsigned se = e / 2, ln = 8 / se;
    auto half = [&](Value* v) {
      Value* h = B.CreateTrunc(hi ? B.CreateLShr(fromV(v), cN(128, 64)) : fromV(v), I(8));
      Value* x = B.CreateBitCast(h, VT(I(se), ln));
      return sg ? B.CreateSExt(x, VT(I(e), ln)) : B.CreateZExt(x, VT(I(e), ln));
    };
    return ret(B.CreateMul(half(A(w1)), half(A(w2))));
  }
  case OP_VUMULH: case OP_VSMULH: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    const bool sg = op->Op == OP_VSMULH;
    W w1 = sg ? op->C<IROp_VSMulH>()->Vector1 : op->C<IROp_VUMulH>()->Vector1, w2 = sg ? op->C<IROp_VSMulH>()->Vector2 : op->C<IROp_VUMulH>()->Vector2;
    Value *a = lanesI(A(w1), size, e), *b = lanesI(A(w2), size, e);
    auto* wt = VT(I(e * 2), n);
    Value* p = B.CreateMul(sg ? B.CreateSExt(a, wt) : B.CreateZExt(a, wt), sg ? B.CreateSExt(b, wt) : B.CreateZExt(b, wt));
    return ret(B.CreateTrunc(B.CreateLShr(p, ConstantInt::get(wt, e * 8)), a->getType()));
  }
  case OP_VUABDL: case OP_VUABDL2: {
    const bool hi = op->Op == OP_VUABDL2;
    W w1 = hi ? op->C<IROp_VUABDL2>()->Vector1 : op->C<IROp_VUABDL>()->Vector1, w2 = hi ? op->C<IROp_VUABDL2>()->Vector2 : op->C<IROp_VUABDL>()->Vector2;
    const unsigned se = e / 2, ln = 8 / se;
    auto half = [&](Value* v) {
      Value* h = B.CreateTrunc(hi ? B.CreateLShr(fromV(v), cN(128, 64)) : fromV(v), I(8));
      return B.CreateZExt(B.CreateBitCast(h, VT(I(se), ln)), VT(I(e), ln));
    };
    Value *a = half(A(w1)), *b = half(A(w2));
    return ret(B.CreateSub(B.CreateBinaryIntrinsic(Intrinsic::umax, a, b), B.CreateBinaryIntrinsic(Intrinsic::umin, a, b)));
  }
  case OP_VUSHRNI: case OP_VUSHRNI2: case OP_VRSHRN: case OP_VRSHRNPAIR: {
    // narrow with a right shift (the result element is `e` wide, the source 2e); the 2-operand forms add a second half
    const bool rnd = op->Op == OP_VRSHRN || op->Op == OP_VRSHRNPAIR;
    Value *lo, *hi = nullptr;
    unsigned bs;
    switch (op->Op) {
    case OP_VUSHRNI: lo = A(op->C<IROp_VUShrNI>()->Vector); bs = op->C<IROp_VUShrNI>()->BitShift; break;
    case OP_VUSHRNI2: lo = A(op->C<IROp_VUShrNI2>()->VectorLower); hi = A(op->C<IROp_VUShrNI2>()->VectorUpper); bs = op->C<IROp_VUShrNI2>()->BitShift; break;
    case OP_VRSHRN: lo = A(op->C<IROp_VRSHRN>()->Vector); bs = op->C<IROp_VRSHRN>()->BitShift; break;
    default: lo = A(op->C<IROp_VRSHRNPair>()->VectorLower); hi = A(op->C<IROp_VRSHRNPair>()->VectorUpper); bs = op->C<IROp_VRSHRNPair>()->BitShift; break;
    }
    const unsigned se = e * 2, ln = 16 / se;
    auto nar = [&](Value* v) -> Value* { // 128 bits -> 64 bits
      Value* x = lanesI(v, 16, se);
      auto* t = x->getType();
      if (rnd && bs) { // the rounding add must not wrap
        auto* wt2 = VT(I(se * 2), ln);
        x = B.CreateTrunc(B.CreateLShr(B.CreateAdd(B.CreateZExt(x, wt2), ConstantInt::get(wt2, (uint64_t)1 << (bs - 1))), ConstantInt::get(wt2, bs)), t);
      } else if (bs) x = B.CreateLShr(x, ConstantInt::get(t, bs));
      return B.CreateBitCast(B.CreateTrunc(x, VT(I(e), ln)), I64);
    };
    if (!hi) return ret(nar(lo));
    Value* first = op->Op == OP_VUSHRNI2 ? B.CreateTrunc(fromV(lo), I64) : nar(lo); // NI2 keeps Lower's low half as it is
    def(node, B.CreateInsertElement(B.CreateInsertElement(zeroV(), first, (uint64_t)0), nar(hi), (uint64_t)1));
    return true;
  }
  case OP_VSQXTN: case OP_VSQXTUN: case OP_VSQXTN2: case OP_VSQXTUN2: case OP_VSQXTNPAIR: case OP_VSQXTUNPAIR: {
    const bool uns = op->Op == OP_VSQXTUN || op->Op == OP_VSQXTUN2 || op->Op == OP_VSQXTUNPAIR;
    const bool two = op->Op == OP_VSQXTN2 || op->Op == OP_VSQXTUN2, pair = op->Op == OP_VSQXTNPAIR || op->Op == OP_VSQXTUNPAIR;
    Value *lo, *hi = nullptr;
    switch (op->Op) {
    case OP_VSQXTN: lo = A(op->C<IROp_VSQXTN>()->Vector); break;
    case OP_VSQXTUN: lo = A(op->C<IROp_VSQXTUN>()->Vector); break;
    case OP_VSQXTN2: lo = A(op->C<IROp_VSQXTN2>()->VectorLower); hi = A(op->C<IROp_VSQXTN2>()->VectorUpper); break;
    case OP_VSQXTUN2: lo = A(op->C<IROp_VSQXTUN2>()->VectorLower); hi = A(op->C<IROp_VSQXTUN2>()->VectorUpper); break;
    case OP_VSQXTNPAIR: lo = A(op->C<IROp_VSQXTNPair>()->VectorLower); hi = A(op->C<IROp_VSQXTNPair>()->VectorUpper); break;
    default: lo = A(op->C<IROp_VSQXTUNPair>()->VectorLower); hi = A(op->C<IROp_VSQXTUNPair>()->VectorUpper); break;
    }
    const unsigned se = e * 2, ln = 16 / se;
    auto sat = [&](Value* v128) -> Value* { // se-wide lanes -> 64 bits of e-wide lanes, saturated
      Value* x = lanesI(v128, 16, se);
      auto* t = x->getType();
      const int64_t mx = uns ? (int64_t)((1ull << (e * 8)) - 1) : (int64_t)((1ull << (e * 8 - 1)) - 1), mn = uns ? 0 : -(int64_t)(1ull << (e * 8 - 1));
      x = B.CreateBinaryIntrinsic(Intrinsic::smax, x, ConstantInt::get(t, (uint64_t)mn));
      x = B.CreateBinaryIntrinsic(Intrinsic::smin, x, ConstantInt::get(t, (uint64_t)mx));
      return B.CreateBitCast(B.CreateTrunc(x, VT(I(e), ln)), I64);
    };
    if (!two && !pair) return ret(sat(lo));
    if (pair && size == 8) { // zip1 of the 64-bit halves, then narrow: the result is 64 bits
      Value* z = B.CreateInsertElement(B.CreateInsertElement(zeroV(), B.CreateTrunc(fromV(lo), I64), (uint64_t)0), B.CreateTrunc(fromV(hi), I64), (uint64_t)1);
      return ret(sat(z));
    }
    Value* first = two ? B.CreateTrunc(fromV(lo), I64) : sat(lo);
    def(node, B.CreateInsertElement(B.CreateInsertElement(zeroV(), first, (uint64_t)0), sat(hi), (uint64_t)1));
    return true;
  }
  case OP_VSRSHR: {
    auto* o = op->C<IROp_VSRSHR>();
    Value* x = lanesI(A(o->Vector), size, e);
    if (!o->BitShift) return ret(x);
    auto* wt = VT(I(e * 2), n); // the rounding add must not wrap
    Value* r = B.CreateAdd(B.CreateSExt(x, wt), ConstantInt::get(wt, (uint64_t)1 << (o->BitShift - 1)));
    return ret(B.CreateTrunc(B.CreateAShr(r, ConstantInt::get(wt, o->BitShift)), x->getType()));
  }
  case OP_VSQSHL: {
    // signed saturating shift left by an immediate: shift in a wider lane and clamp
    auto* o = op->C<IROp_VSQSHL>();
    Value* x = lanesI(A(o->Vector), size, e);
    auto* wt = VT(I(e * 2), n);
    Value* w = B.CreateShl(B.CreateSExt(x, wt), ConstantInt::get(wt, o->BitShift));
    const int64_t mx = (int64_t)((1ull << (e * 8 - 1)) - 1);
    w = B.CreateBinaryIntrinsic(Intrinsic::smax, w, ConstantInt::get(wt, (uint64_t)(-mx - 1)));
    w = B.CreateBinaryIntrinsic(Intrinsic::smin, w, ConstantInt::get(wt, (uint64_t)mx));
    return ret(B.CreateTrunc(w, x->getType()));
  }

  // ---- floating point lanes
  case OP_VFADD: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VFAdd>(); return ret(B.CreateFAdd(lanesF(A(o->Vector1), size, e), lanesF(A(o->Vector2), size, e))); }
  case OP_VFSUB: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VFSub>(); return ret(B.CreateFSub(lanesF(A(o->Vector1), size, e), lanesF(A(o->Vector2), size, e))); }
  case OP_VFMUL: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VFMul>(); return ret(B.CreateFMul(lanesF(A(o->Vector1), size, e), lanesF(A(o->Vector2), size, e))); }
  case OP_VFDIV: { const unsigned size = (e == isz ? e : 16u), n = size / e; auto* o = op->C<IROp_VFDiv>(); return ret(B.CreateFDiv(lanesF(A(o->Vector1), size, e), lanesF(A(o->Vector2), size, e))); }
  case OP_VFSQRT: { const unsigned size = (e == isz ? e : 16u), n = size / e; Value* x = lanesF(A(op->C<IROp_VFSqrt>()->Vector), size, e); return ret(B.CreateUnaryIntrinsic(Intrinsic::sqrt, x)); }
  case OP_VFABS: { const unsigned size = (e == isz ? e : 16u), n = size / e; Value* x = lanesF(A(op->C<IROp_VFAbs>()->Vector), size, e); return ret(B.CreateUnaryIntrinsic(Intrinsic::fabs, x)); }
  case OP_VFNEG: { const unsigned size = 16, n = size / e; Value* x = lanesF(A(op->C<IROp_VFNeg>()->Vector), size, e); return ret(B.CreateFNeg(x)); }
  case OP_VFRECP: case OP_VFRECPPRECISION: { const unsigned size = (e == isz ? e : 16u), n = size / e;
    Value* x = lanesF(A(op->Op == OP_VFRECP ? op->C<IROp_VFRecp>()->Vector : op->C<IROp_VFRecpPrecision>()->Vector), size, e);
    return ret(B.CreateFDiv(ConstantFP::get(x->getType(), 1.0), x));
  }
  case OP_VFRSQRT: case OP_VFRSQRTPRECISION: { const unsigned size = (e == isz ? e : 16u), n = size / e;
    Value* x = lanesF(A(op->Op == OP_VFRSQRT ? op->C<IROp_VFRSqrt>()->Vector : op->C<IROp_VFRSqrtPrecision>()->Vector), size, e);
    return ret(B.CreateFDiv(ConstantFP::get(x->getType(), 1.0), B.CreateUnaryIntrinsic(Intrinsic::sqrt, x)));
  }
  case OP_VFMIN: case OP_VFMAX: { const unsigned size = 16, n = size / e;
    // x86 min/max: take the first operand only when strictly less (greater); otherwise the second
    const bool mn = op->Op == OP_VFMIN;
    Value *a = lanesF(A(mn ? op->C<IROp_VFMin>()->Vector1 : op->C<IROp_VFMax>()->Vector1), size, e), *b = lanesF(A(mn ? op->C<IROp_VFMin>()->Vector2 : op->C<IROp_VFMax>()->Vector2), size, e);
    return ret(B.CreateSelect(mn ? B.CreateFCmpOGT(b, a) : B.CreateFCmpOGT(a, b), a, b));
  }
  case OP_VFADDV: { const unsigned size = 16, n = size / e; // faddp, faddp: ((l0 + l1) + (l2 + l3))
    Value* x = lanesF(A(op->C<IROp_VFAddV>()->Vector), size, e);
    if (n == 4) {
      Value* ev = B.CreateShuffleVector(x, std::vector<int>{0, 2});
      Value* od = B.CreateShuffleVector(x, std::vector<int>{1, 3});
      Value* p = B.CreateFAdd(ev, od);
      return ret(B.CreateFAdd(B.CreateExtractElement(p, (uint64_t)0), B.CreateExtractElement(p, (uint64_t)1)));
    }
    return ret(B.CreateFAdd(B.CreateExtractElement(x, (uint64_t)0), B.CreateExtractElement(x, (uint64_t)1)));
  }
  case OP_VFCMPEQ: case OP_VFCMPNEQ: case OP_VFCMPLT: case OP_VFCMPGT: case OP_VFCMPLE: case OP_VFCMPORD: case OP_VFCMPUNO: { const unsigned size = (e == isz ? e : 16u), n = size / e;
    W w1, w2;
    switch (op->Op) {
    case OP_VFCMPEQ: w1 = op->C<IROp_VFCMPEQ>()->Vector1; w2 = op->C<IROp_VFCMPEQ>()->Vector2; break;
    case OP_VFCMPNEQ: w1 = op->C<IROp_VFCMPNEQ>()->Vector1; w2 = op->C<IROp_VFCMPNEQ>()->Vector2; break;
    case OP_VFCMPLT: w1 = op->C<IROp_VFCMPLT>()->Vector1; w2 = op->C<IROp_VFCMPLT>()->Vector2; break;
    case OP_VFCMPGT: w1 = op->C<IROp_VFCMPGT>()->Vector1; w2 = op->C<IROp_VFCMPGT>()->Vector2; break;
    case OP_VFCMPLE: w1 = op->C<IROp_VFCMPLE>()->Vector1; w2 = op->C<IROp_VFCMPLE>()->Vector2; break;
    case OP_VFCMPORD: w1 = op->C<IROp_VFCMPORD>()->Vector1; w2 = op->C<IROp_VFCMPORD>()->Vector2; break;
    default: w1 = op->C<IROp_VFCMPUNO>()->Vector1; w2 = op->C<IROp_VFCMPUNO>()->Vector2; break;
    }
    Value *a = lanesF(A(w1), size, e), *b = lanesF(A(w2), size, e);
    Value* c;
    switch (op->Op) {
    case OP_VFCMPEQ: c = B.CreateFCmpOEQ(a, b); break;
    case OP_VFCMPNEQ: c = B.CreateFCmpUNE(a, b); break;
    case OP_VFCMPLT: c = B.CreateFCmpOLT(a, b); break;
    case OP_VFCMPGT: c = B.CreateFCmpOGT(a, b); break;
    case OP_VFCMPLE: c = B.CreateFCmpOLE(a, b); break;
    case OP_VFCMPORD: c = B.CreateFCmpORD(a, b); break;
    default: c = B.CreateFCmpUNO(a, b); break;
    }
    return ret(cmpMask(c, e * 8, n));
  }
  case OP_VFCADD: {
    auto* o = op->C<IROp_VFCADD>();
    Value *a = lanesF(A(o->Vector1), size, e), *b = lanesF(A(o->Vector2), size, e);
    std::vector<int> sw;
    for (unsigned i = 0; i < n; i += 2) { sw.push_back(i + 1); sw.push_back(i); }
    Value* bs = B.CreateShuffleVector(b, sw); // (imag, real) pairs
    // rot 90: re = a.re - b.im, im = a.im + b.re; rot 270: re = a.re + b.im, im = a.im - b.re
    std::vector<Constant*> sg;
    for (unsigned i = 0; i < n; ++i) sg.push_back(ConstantFP::get(a->getType()->getScalarType(), (i & 1) == (o->Rotate == 90 ? 1u : 0u) ? 1.0 : -1.0));
    Value* t = B.CreateFMul(bs, ConstantVector::get(sg));
    return ret(B.CreateFAdd(a, t));
  }

  // ---- scalar-insert forms: the result is Vector1 with lane 0 replaced
  case OP_VFADDSCALARINSERT: case OP_VFSUBSCALARINSERT: case OP_VFMULSCALARINSERT: case OP_VFDIVSCALARINSERT:
  case OP_VFMINSCALARINSERT: case OP_VFMAXSCALARINSERT: {
    W w1, w2;
    switch (op->Op) {
    case OP_VFADDSCALARINSERT: w1 = op->C<IROp_VFAddScalarInsert>()->Vector1; w2 = op->C<IROp_VFAddScalarInsert>()->Vector2; break;
    case OP_VFSUBSCALARINSERT: w1 = op->C<IROp_VFSubScalarInsert>()->Vector1; w2 = op->C<IROp_VFSubScalarInsert>()->Vector2; break;
    case OP_VFMULSCALARINSERT: w1 = op->C<IROp_VFMulScalarInsert>()->Vector1; w2 = op->C<IROp_VFMulScalarInsert>()->Vector2; break;
    case OP_VFDIVSCALARINSERT: w1 = op->C<IROp_VFDivScalarInsert>()->Vector1; w2 = op->C<IROp_VFDivScalarInsert>()->Vector2; break;
    case OP_VFMINSCALARINSERT: w1 = op->C<IROp_VFMinScalarInsert>()->Vector1; w2 = op->C<IROp_VFMinScalarInsert>()->Vector2; break;
    default: w1 = op->C<IROp_VFMaxScalarInsert>()->Vector1; w2 = op->C<IROp_VFMaxScalarInsert>()->Vector2; break;
    }
    Value* v1 = A(w1);
    Value *a = toFp(lane0(v1, e), e), *b = toFp(lane0(A(w2), e), e);
    Value* r;
    switch (op->Op) {
    case OP_VFADDSCALARINSERT: r = B.CreateFAdd(a, b); break;
    case OP_VFSUBSCALARINSERT: r = B.CreateFSub(a, b); break;
    case OP_VFMULSCALARINSERT: r = B.CreateFMul(a, b); break;
    case OP_VFDIVSCALARINSERT: r = B.CreateFDiv(a, b); break;
    case OP_VFMINSCALARINSERT: r = B.CreateSelect(B.CreateFCmpOGT(b, a), a, b); break;
    default: r = B.CreateSelect(B.CreateFCmpOGT(a, b), a, b); break;
    }
    def(node, insertLane0(v1, fromFp(r, e), e));
    return true;
  }
  case OP_VFSQRTSCALARINSERT: case OP_VFRSQRTSCALARINSERT: case OP_VFRECPSCALARINSERT: {
    W w1, w2;
    if (op->Op == OP_VFSQRTSCALARINSERT) { w1 = op->C<IROp_VFSqrtScalarInsert>()->Vector1; w2 = op->C<IROp_VFSqrtScalarInsert>()->Vector2; }
    else if (op->Op == OP_VFRSQRTSCALARINSERT) { w1 = op->C<IROp_VFRSqrtScalarInsert>()->Vector1; w2 = op->C<IROp_VFRSqrtScalarInsert>()->Vector2; }
    else { w1 = op->C<IROp_VFRecpScalarInsert>()->Vector1; w2 = op->C<IROp_VFRecpScalarInsert>()->Vector2; }
    Value* v1 = A(w1);
    Value* x = toFp(lane0(A(w2), e), e);
    Value* one = ConstantFP::get(x->getType(), 1.0);
    Value* r = op->Op == OP_VFSQRTSCALARINSERT ? B.CreateUnaryIntrinsic(Intrinsic::sqrt, x)
             : op->Op == OP_VFRSQRTSCALARINSERT ? B.CreateFDiv(one, B.CreateUnaryIntrinsic(Intrinsic::sqrt, x)) : B.CreateFDiv(one, x);
    def(node, insertLane0(v1, fromFp(r, e), e));
    return true;
  }
  case OP_VFCMPSCALARINSERT: {
    auto* o = op->C<IROp_VFCMPScalarInsert>();
    Value* v1 = A(o->Vector1);
    Value *a = toFp(lane0(v1, e), e), *b = toFp(lane0(A(o->Vector2), e), e);
    Value* c;
    switch (o->Op) {
    case FloatCompareOp::EQ: c = B.CreateFCmpOEQ(a, b); break;
    case FloatCompareOp::LT: c = B.CreateFCmpOLT(a, b); break;
    case FloatCompareOp::LE: c = B.CreateFCmpOLE(a, b); break;
    case FloatCompareOp::UNO: c = B.CreateFCmpUNO(a, b); break;
    case FloatCompareOp::NEQ: c = B.CreateFCmpUNE(a, b); break;
    default: c = B.CreateFCmpORD(a, b); break;
    }
    def(node, insertLane0(v1, B.CreateSExt(c, I(e)), e));
    return true;
  }
  case OP_VFTOFSCALARINSERT: { // fcvt between float formats on lane 0
    auto* o = op->C<IROp_VFToFScalarInsert>();
    const unsigned se = static_cast<unsigned>(o->SrcElementSize);
    Value* v1 = A(o->Vector1);
    Value* x = toFp(lane0(A(o->Vector2), se), se);
    Value* r = e > se ? B.CreateFPExt(x, FT(e)) : B.CreateFPTrunc(x, FT(e));
    def(node, insertLane0(v1, fromFp(r, e), e));
    return true;
  }
  case OP_VSTOFVECTORINSERT: { // scvtf of the low int lane(s)
    auto* o = op->C<IROp_VSToFVectorInsert>();
    Value* v1 = A(o->Vector1);
    Value* v2 = A(o->Vector2);
    if (o->HasTwoElements) {
      Value* x = B.CreateSIToFP(lanesI(v2, 8, 4), VT(Type::getFloatTy(C), 2));
      def(node, B.CreateInsertElement(v1, B.CreateBitCast(x, I64), (uint64_t)0));
    } else {
      Value* x = B.CreateSIToFP(lane0(v2, e), FT(e));
      def(node, insertLane0(v1, fromFp(x, e), e));
    }
    return true;
  }
  case OP_VSTOFGPRINSERT: {
    auto* o = op->C<IROp_VSToFGPRInsert>();
    const unsigned se = static_cast<unsigned>(o->SrcElementSize);
    Value* v = A(o->Vector);
    Value* x = B.CreateSIToFP(tr(A(o->Src), se * 8), FT(e));
    def(node, insertLane0(v, fromFp(x, e), e));
    return true;
  }
  case OP_VFTOISCALARINSERT: { // frint* on lane 0
    auto* o = op->C<IROp_VFToIScalarInsert>();
    Value* v1 = A(o->Vector1);
    Value* x = toFp(lane0(A(o->Vector2), e), e);
    def(node, insertLane0(v1, fromFp(B.CreateUnaryIntrinsic(roundId(o->Round), x), e), e));
    return true;
  }

  // ---- conversions
  case OP_VECTOR_STOF: { const unsigned size = (e == isz ? e : (isz == 8 ? 8u : 16u)), n = size / e; Value* x = lanesI(A(op->C<IROp_Vector_SToF>()->Vector), size, e); return ret(B.CreateSIToFP(x, VT(FT(e), n))); }
  case OP_VECTOR_FTOZS: { const unsigned size = (e == isz ? e : (isz == 8 ? 8u : 16u)), n = size / e;
    Value* x = lanesF(A(op->C<IROp_Vector_FToZS>()->Vector), size, e);
    return ret(B.CreateIntrinsic(Intrinsic::fptosi_sat, {VT(I(e), n), x->getType()}, {x}));
  }
  case OP_VECTOR_FTOS: { const unsigned size = (isz == 8 ? 8u : 16u), n = size / e;
    Value* x = lanesF(A(op->C<IROp_Vector_FToS>()->Vector), size, e);
    x = B.CreateUnaryIntrinsic(Intrinsic::nearbyint, x);
    return ret(B.CreateIntrinsic(Intrinsic::fptosi_sat, {VT(I(e), n), x->getType()}, {x}));
  }
  case OP_VECTOR_FTOI: { const unsigned size = (e == isz ? e : 16u), n = size / e;
    auto* o = op->C<IROp_Vector_FToI>();
    Value* x = lanesF(A(o->Vector), size, e);
    return ret(B.CreateUnaryIntrinsic(roundId(o->Round), x));
  }
  case OP_VECTOR_FTOF: { // fcvtl: widen the low 64 bits (e > src); fcvtn: narrow all lanes into the low 64 bits
    auto* o = op->C<IROp_Vector_FToF>();
    const unsigned se = static_cast<unsigned>(o->SrcElementSize);
    Value* v = A(o->Vector);
    if (e > se) return ret(B.CreateFPExt(lanesF(v, 8, se), VT(FT(e), 8 / se)));
    return ret(B.CreateFPTrunc(lanesF(v, 16, se), VT(FT(e), 16 / se)));
  }
  case OP_VFCVTL2: { // the high half widened
    auto* o = op->C<IROp_VFCVTL2>();
    const unsigned se = e / 2, ln = 8 / se;
    Value* h = B.CreateTrunc(B.CreateLShr(fromV(A(o->Vector)), cN(128, 64)), I(8));
    return ret(B.CreateFPExt(B.CreateBitCast(h, VT(FT(se), ln)), VT(FT(e), ln)));
  }
  case OP_VFCVTN2: { // Lower's low half, then the narrowed Upper
    auto* o = op->C<IROp_VFCVTN2>();
    const unsigned se = e * 2, ln = 16 / se;
    Value* x = B.CreateFPTrunc(lanesF(A(o->VectorUpper), 16, se), VT(FT(e), ln));
    Value* l64 = B.CreateTrunc(fromV(A(o->VectorLower)), I64);
    def(node, B.CreateInsertElement(B.CreateInsertElement(zeroV(), l64, (uint64_t)0), B.CreateBitCast(x, I64), (uint64_t)1));
    return true;
  }
  case OP_VECTOR_F64TOI32: {
    auto* o = op->C<IROp_Vector_F64ToI32>();
    Value* x = B.CreateUnaryIntrinsic(roundId(o->Round), lanesF(A(o->Vector), 16, 8));
    // fcvtzs to i64, then the saturating narrow to i32 (sqxtn)
    Value* l = B.CreateIntrinsic(Intrinsic::fptosi_sat, {VT(I64, 2), x->getType()}, {x});
    l = B.CreateBinaryIntrinsic(Intrinsic::smax, l, ConstantInt::get(l->getType(), (uint64_t)(int64_t)INT32_MIN));
    l = B.CreateBinaryIntrinsic(Intrinsic::smin, l, ConstantInt::get(l->getType(), (uint64_t)INT32_MAX));
    return ret(B.CreateTrunc(l, VT(I32, 2)));
  }
  case OP_FLOAT_FROMGPR_S: {
    auto* o = op->C<IROp_Float_FromGPR_S>();
    const unsigned se = static_cast<unsigned>(o->SrcElementSize);
    Value* x = B.CreateSIToFP(tr(A(o->Src), se * 8), FT(e));
    def(node, toV(B.CreateZExt(fromFp(x, e), I128)));
    return true;
  }
  case OP_FLOAT_FTOF: {
    auto* o = op->C<IROp_Float_FToF>();
    const unsigned se = static_cast<unsigned>(o->SrcElementSize);
    Value* x = toFp(lane0(A(o->Scalar), se), se);
    Value* r = e > se ? B.CreateFPExt(x, FT(e)) : B.CreateFPTrunc(x, FT(e));
    def(node, toV(B.CreateZExt(fromFp(r, e), I128)));
    return true;
  }
  case OP_NZCVSELECTV: {
    // fcsel: the selected scalar (S or D), the rest of the register zeroed
    auto* o = op->C<IROp_NZCVSelectV>();
    const unsigned eb = isz == 4 ? 4 : 8;
    Value* c = cond(o->Cond);
    Value *t = lane0(A(o->TrueVal), eb), *f = lane0(A(o->FalseVal), eb);
    def(node, toV(B.CreateZExt(B.CreateSelect(c, t, f), I128)));
    return true;
  }

  // ---- x87 (X87ReducedPrecision=1: F64 ops, 80-bit values only in memory conversions). The JIT calls FEX's fallback handlers for
  // these (F80Fallbacks.h, InterpreterFallbacks.cpp); the calls go to the bbr_x87_* helpers (tools/recomp/helpers/bbr_x87.c: the
  // same handlers over the same SoftFloat), linked into the generated .so. An 80-bit value is a V128 whose low 10 bytes are the
  // register: significand in lane 0, sign/exponent in the low 16 bits of lane 1.
  case OP_F80CVT: case OP_F80CVTTO: case OP_F80CVTINT: case OP_F80CVTTOINT: case OP_F80BCDLOAD: case OP_F80BCDSTORE: {
    Type *Dbl = Type::getDoubleTy(C), *Flt = Type::getFloatTy(C), *F80 = StructType::get(C, {I64, I64});
    auto sigOf = [&](Value* v) -> Value* { return B.CreateExtractElement(v, (uint64_t)0); };
    auto topOf = [&](Value* v) -> Value* { return B.CreateAnd(B.CreateExtractElement(v, (uint64_t)1), c64(0xffff)); };
    auto f80Out = [&](Value* agg) {
      Value* v = B.CreateInsertElement(zeroV(), B.CreateExtractValue(agg, 0), (uint64_t)0);
      def(node, B.CreateInsertElement(v, B.CreateExtractValue(agg, 1), (uint64_t)1));
      return true;
    };
    switch (op->Op) {
    case OP_F80CVT: {
      Value* x = A(op->C<IROp_F80CVT>()->X80Src);
      const bool d = isz == 8;
      return ret(B.CreateCall(m.helper(d ? "bbr_x87_f80_to_f64" : "bbr_x87_f80_to_f32", d ? Dbl : Flt, {Ptr, I64, I64}), {S, sigOf(x), topOf(x)}));
    }
    case OP_F80CVTTO: {
      auto* o = op->C<IROp_F80CVTTo>();
      Value* x = A(o->X80Src);
      const bool d = o->SrcSize == OpSize::i64Bit;
      Value* in = toFp(lane0(x, d ? 8 : 4), d ? 8 : 4);
      return f80Out(B.CreateCall(m.helper(d ? "bbr_x87_f64_to_f80" : "bbr_x87_f32_to_f80", F80, {Ptr, d ? Dbl : Flt}), {S, in}));
    }
    case OP_F80CVTINT: {
      auto* o = op->C<IROp_F80CVTInt>();
      Value* x = A(o->X80Src);
      Value* r = B.CreateCall(m.helper("bbr_x87_f80_to_int", I64, {Ptr, I64, I64, I64, I64}), {S, sigOf(x), topOf(x), c64(isz), c64(o->Truncate ? 1 : 0)});
      def(node, zx(tr(r, isz * 8)));
      return true;
    }
    case OP_F80CVTTOINT: {
      auto* o = op->C<IROp_F80CVTToInt>();
      return f80Out(B.CreateCall(m.helper("bbr_x87_int_to_f80", F80, {Ptr, I64, I64}), {S, A(o->Src), c64(static_cast<unsigned>(o->SrcSize))}));
    }
    case OP_F80BCDLOAD: {
      Value* x = A(op->C<IROp_F80BCDLoad>()->X80Src);
      return f80Out(B.CreateCall(m.helper("bbr_x87_bcd_load", F80, {Ptr, I64, I64}), {S, sigOf(x), topOf(x)}));
    }
    default: {
      Value* x = A(op->C<IROp_F80BCDStore>()->X80Src);
      return f80Out(B.CreateCall(m.helper("bbr_x87_bcd_store", F80, {Ptr, I64, I64}), {S, sigOf(x), topOf(x)}));
    }
    }
  }
  case OP_F64FPREM: case OP_F64FPREM1: case OP_F64SCALE: case OP_F64ATAN: case OP_F64FYL2X: case OP_F64FYL2XP1: {
    Type* Dbl = Type::getDoubleTy(C);
    W w1, w2; const char* fn;
    switch (op->Op) {
    case OP_F64FPREM: w1 = op->C<IROp_F64FPREM>()->Src1; w2 = op->C<IROp_F64FPREM>()->Src2; fn = "bbr_x87_f64_fprem"; break;
    case OP_F64FPREM1: w1 = op->C<IROp_F64FPREM1>()->Src1; w2 = op->C<IROp_F64FPREM1>()->Src2; fn = "bbr_x87_f64_fprem1"; break;
    case OP_F64SCALE: w1 = op->C<IROp_F64SCALE>()->Src1; w2 = op->C<IROp_F64SCALE>()->Src2; fn = "bbr_x87_f64_scale"; break;
    case OP_F64ATAN: w1 = op->C<IROp_F64ATAN>()->Src1; w2 = op->C<IROp_F64ATAN>()->Src2; fn = "bbr_x87_f64_atan"; break;
    case OP_F64FYL2X: w1 = op->C<IROp_F64FYL2X>()->Src; w2 = op->C<IROp_F64FYL2X>()->Src2; fn = "bbr_x87_f64_fyl2x"; break;
    default: w1 = op->C<IROp_F64FYL2XP1>()->Src; w2 = op->C<IROp_F64FYL2XP1>()->Src2; fn = "bbr_x87_f64_fyl2xp1"; break;
    }
    return ret(B.CreateCall(m.helper(fn, Dbl, {Dbl, Dbl}), {toFp(lane0(A(w1), 8), 8), toFp(lane0(A(w2), 8), 8)}));
  }
  case OP_F64F2XM1: case OP_F64TAN: case OP_F64SIN: case OP_F64COS: {
    Type* Dbl = Type::getDoubleTy(C);
    W w; const char* fn;
    switch (op->Op) {
    case OP_F64F2XM1: w = op->C<IROp_F64F2XM1>()->Src; fn = "bbr_x87_f64_f2xm1"; break;
    case OP_F64TAN: w = op->C<IROp_F64TAN>()->Src; fn = "bbr_x87_f64_tan"; break;
    case OP_F64SIN: w = op->C<IROp_F64SIN>()->Src; fn = "bbr_x87_f64_sin"; break;
    default: w = op->C<IROp_F64COS>()->Src; fn = "bbr_x87_f64_cos"; break;
    }
    return ret(B.CreateCall(m.helper(fn, Dbl, {Dbl}), {toFp(lane0(A(w), 8), 8)}));
  }
  default: return false;
  }
}

} // namespace bbr
