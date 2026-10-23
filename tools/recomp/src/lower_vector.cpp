// Vector and floating-point ops (step 2b). Step 2a lowers only the three that scalar code uses.
#include "lower_impl.h"

using namespace llvm;

namespace bbr {

bool Lower::emitVectorOp(Ref node, IR::IROp_Header* op) {
  using namespace IR;
  const unsigned size = sz(op);
  switch (op->Op) {
  case OP_VEXTRACTTOGPR: {
    // umov: element `Index` of width Size
    auto* o = op->C<IROp_VExtractToGPR>();
    Value* v = A(o->Vector);
    const unsigned eb = size * 8;
    Value* e = B.CreateExtractElement(B.CreateBitCast(v, FixedVectorType::get(iN(eb), 128 / eb)), (uint64_t)o->Index);
    def(node, zx(e));
    return true;
  }
  case OP_FLOAT_TOGPR_ZS: {
    // fcvtzs: saturating, NaN -> 0 (the frontend adds the x86 fix-ups)
    auto* o = op->C<IROp_Float_ToGPR_ZS>();
    Value* v = A(o->Scalar);
    Value* e = B.CreateExtractElement(v, (uint64_t)0);
    Value* f = o->SrcElementSize == OpSize::i64Bit ? B.CreateBitCast(e, Type::getDoubleTy(C)) : B.CreateBitCast(B.CreateTrunc(e, I32), Type::getFloatTy(C));
    const unsigned rb = size == 8 ? 64 : 32;
    Value* r = B.CreateIntrinsic(Intrinsic::fptosi_sat, {iN(rb), f->getType()}, {f});
    def(node, zx(r));
    return true;
  }
  case OP_FCMP: {
    auto* o = op->C<IROp_FCmp>();
    auto fl = [&](Value* v) -> Value* {
      Value* e = B.CreateExtractElement(v, (uint64_t)0);
      return o->ElementSize == OpSize::i64Bit ? B.CreateBitCast(e, Type::getDoubleTy(C)) : B.CreateBitCast(B.CreateTrunc(e, I32), Type::getFloatTy(C));
    };
    setFromFCmp(fl(A(o->Scalar1)), fl(A(o->Scalar2)));
    return true;
  }
  default: return false;
  }
}

} // namespace bbr
