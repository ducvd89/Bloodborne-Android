// FEX IR -> LLVM IR: unit setup, blocks, flags, exits (docs/RECOMP_ABI.md). Scalar/memory/atomic ops are in
// lower_ops.cpp, vector and FP ops in lower_vector.cpp.
#include "lower_impl.h"

#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string_view>
#include <cstdio>

using namespace llvm;

std::string unitNameWith(const std::string& prefix, uint64_t vaddr) {
  char b[24];
  snprintf(b, sizeof b, "%llx", (unsigned long long)vaddr);
  return prefix + b;
}
std::string unitName(uint64_t vaddr) { return unitNameWith("bbr_", vaddr); }

namespace bbr {

// ------------------------------------------------------------------------------- module context -----
ModuleCtx::ModuleCtx(LLVMContext& c, Module& m, const LowerOptions& o) : C(c), M(m), O(o) {
  I1 = Type::getInt1Ty(C); I8 = Type::getInt8Ty(C); I16 = Type::getInt16Ty(C); I32 = Type::getInt32Ty(C);
  I64 = Type::getInt64Ty(C); I128 = Type::getInt128Ty(C); Ptr = PointerType::getUnqual(C);
  V128 = FixedVectorType::get(I64, 2); Void = Type::getVoidTy(C);
}

FunctionCallee ModuleCtx::helper(const char* name, Type* ret, ArrayRef<Type*> args) {
  return M.getOrInsertFunction(name, FunctionType::get(ret, args, false));
}

static FunctionCallee unitFnNamed(ModuleCtx& m, const std::string& n) {
  if (auto* f = m.M.getFunction(n)) return f;
  auto* f = Function::Create(FunctionType::get(m.Void, {m.Ptr}, false), GlobalValue::ExternalLinkage, n, &m.M);
  if (m.O.hiddenUnits) f->setVisibility(GlobalValue::HiddenVisibility);
  f->addFnAttr(Attribute::NoUnwind);
  f->addParamAttr(0, Attribute::NoAlias);
  return f;
}
FunctionCallee ModuleCtx::unitFn(uint64_t vaddr) { return unitFnNamed(*this, O.callName ? O.callName(vaddr) : unitName(vaddr)); }
FunctionCallee ModuleCtx::defFn(uint64_t vaddr) { return unitFnNamed(*this, O.defPrefix.empty() ? unitName(vaddr) : unitNameWith(O.defPrefix, vaddr)); }

// ------------------------------------------------------------------------------------ helpers -------
Lower::Lower(ModuleCtx& mc, const IR::IRListView& ir, LowerResult* r, uint64_t entry, const UnitMeta* um)
  : m(mc), C(mc.C), M(mc.M), B(mc.C), IR(ir), res(r), entryVaddr(entry), meta(um) {
  I1 = m.I1; I8 = m.I8; I16 = m.I16; I32 = m.I32; I64 = m.I64; I128 = m.I128; Ptr = m.Ptr; V128 = m.V128;
  entryRip = IR.GetHeader()->OriginalRIP;
  val.assign(IR.GetSSACount() + 2, nullptr);
  var.assign(IR.GetSSACount() + 2, nullptr);
  regionOf.assign(IR.GetSSACount() + 2, 0);
  slotOf.assign(IR.GetSSACount() + 2, nullptr);
  granularUnit = um && um->granular;
}

AllocaInst* Lower::newAlloca(Type* t, const char* name) {
  IRBuilder<> ab(allocaBB->getTerminator());
  return ab.CreateAlloca(t, nullptr, name);
}

Value* Lower::gep(uint64_t off) { return off ? B.CreateConstInBoundsGEP1_64(I8, S, off) : S; }

Value* Lower::tr(Value* v, unsigned bits) {
  const unsigned w = v->getType()->getIntegerBitWidth();
  if (w == bits) return v;
  return w > bits ? B.CreateTrunc(v, iN(bits)) : B.CreateZExt(v, iN(bits));
}
Value* Lower::zx(Value* v) { return v->getType() == I64 ? v : B.CreateZExt(v, I64); }
Value* Lower::sxN(Value* v, unsigned bits) {
  v = tr(v, bits);
  return bits == 64 ? v : B.CreateSExt(v, I64);
}
Value* Lower::toV(Value* i128) { return B.CreateBitCast(i128, V128); }
Value* Lower::fromV(Value* v) { return B.CreateBitCast(v, I128); }

// Image-relative address: the IR holds liftBase + vaddr; the .so adds the runtime image base.
Value* Lower::reloc(uint64_t a) {
  const uint64_t rel = a - m.O.liftBase;
  if (m.O.fixedImageBase) return c64(m.O.imageBase + rel);
  return B.CreateAdd(ImgBase, c64(rel), "", true, true);
}

bool Lower::K(W w, uint64_t& k) {
  if (w.IsInvalid() || w.IsImmediate()) return false;
  auto* o = opOf(w);
  if (o->Op != IR::OP_INLINECONSTANT) return false;
  k = o->C<IR::IROp_InlineConstant>()->Constant;
  return true;
}

// A constant guest address (call/jump target, return address): InlineEntrypointOffset, EntrypointOffset, or a
// Constant inside the image. Returns the lift-space address.
bool Lower::addrConst(W w, uint64_t& a) {
  if (w.IsInvalid() || w.IsImmediate()) return false;
  auto* o = opOf(w);
  switch (o->Op) {
  case IR::OP_INLINEENTRYPOINTOFFSET: {
    if (o->Size == IR::OpSize::i32Bit) return false;
    a = entryRip + o->C<IR::IROp_InlineEntrypointOffset>()->Offset; return true;
  }
  case IR::OP_ENTRYPOINTOFFSET: {
    if (o->Size == IR::OpSize::i32Bit) return false;
    a = entryRip + o->C<IR::IROp_EntrypointOffset>()->Offset; return true;
  }
  case IR::OP_CONSTANT: case IR::OP_INLINECONSTANT: {
    const uint64_t k = o->Op == IR::OP_CONSTANT ? o->C<IR::IROp_Constant>()->Constant : o->C<IR::IROp_InlineConstant>()->Constant;
    if (m.O.imageSize && k >= m.O.liftBase && k - m.O.liftBase < m.O.imageSize) { a = k; return true; }
    return false;
  }
  default: return false;
  }
}

Value* Lower::A(W w) {
  if (w.IsInvalid()) return nullptr;
  const uint32_t id = w.ID().Value;
  if (auto* a = var[id]) return B.CreateLoad(a->getAllocatedType(), a);
  if (val[id]) {
    if (granularUnit && regionOf[id] != curRegion) {
      if (auto* inst = dyn_cast<Instruction>(val[id])) {
        if (!slotOf[id]) {
          slotOf[id] = newAlloca(inst->getType(), "xr");
          IRBuilder<> sb(inst->getParent());
          if (inst->getNextNode()) sb.SetInsertPoint(inst->getNextNode());
          sb.CreateStore(inst, slotOf[id]);
        }
        return B.CreateLoad(inst->getType(), slotOf[id]);
      }
    }
    return val[id];
  }
  auto* o = opOf(w);
  switch (o->Op) {
  case IR::OP_INLINECONSTANT: return c64(o->C<IR::IROp_InlineConstant>()->Constant);
  case IR::OP_INLINEENTRYPOINTOFFSET: {
    uint64_t a;
    if (addrConst(w, a)) { ++res->relocConstants; return reloc(a); }
    return c64((entryRip + o->C<IR::IROp_InlineEntrypointOffset>()->Offset) & 0xFFFFFFFFull);
  }
  default: return UndefValue::get(I64); // use before def: cannot happen in valid IR
  }
}

void Lower::def(Ref node, Value* v) { const uint32_t id = IR.GetID(node).Value; val[id] = v; regionOf[id] = curRegion; slotOf[id] = nullptr; }

AllocaInst* Lower::varFor(W w) { return var[w.ID().Value]; }
void Lower::setVar(W out, Value* v) {
  auto* a = var[out.ID().Value];
  if (!a) { // an output node that was not an Allocate/RMW handle: make it one
    a = newAlloca(v->getType(), "out");
    var[out.ID().Value] = a;
  }
  B.CreateStore(v, a);
}

// ------------------------------------------------------------------------------------ flags ---------
static IntegerType* tyOf(Value* v) { return cast<IntegerType>(v->getType()); }

Value* Lower::flagN() {
  switch (fd.k) {
  case FlagDesc::Mem: return B.CreateLoad(I1, fN);
  case FlagDesc::Sub: return B.CreateICmpSLT(fd.res ? fd.res : B.CreateSub(fd.a, fd.b), ConstantInt::get(tyOf(fd.a), 0));
  case FlagDesc::Add: return B.CreateICmpSLT(fd.res ? fd.res : B.CreateAdd(fd.a, fd.b), ConstantInt::get(tyOf(fd.a), 0));
  case FlagDesc::Logic: return B.CreateICmpSLT(fd.res, ConstantInt::get(tyOf(fd.res), 0));
  case FlagDesc::Gen: return fd.n;
  }
  return nullptr;
}
Value* Lower::flagZ() {
  switch (fd.k) {
  case FlagDesc::Mem: return B.CreateLoad(I1, fZ);
  case FlagDesc::Sub: return B.CreateICmpEQ(fd.a, fd.b);
  case FlagDesc::Add: return B.CreateICmpEQ(fd.res ? fd.res : B.CreateAdd(fd.a, fd.b), ConstantInt::get(tyOf(fd.a), 0));
  case FlagDesc::Logic: return B.CreateICmpEQ(fd.res, ConstantInt::get(tyOf(fd.res), 0));
  case FlagDesc::Gen: return fd.z;
  }
  return nullptr;
}
Value* Lower::flagC() {
  Value* c = nullptr;
  switch (fd.k) {
  case FlagDesc::Mem: return B.CreateLoad(I1, fC);
  case FlagDesc::Sub: c = B.CreateICmpUGE(fd.a, fd.b); break;
  case FlagDesc::Add: c = B.CreateICmpULT(fd.res ? fd.res : B.CreateAdd(fd.a, fd.b), fd.a); break;
  case FlagDesc::Logic: c = ConstantInt::getFalse(C); break;
  case FlagDesc::Gen: return fd.c;
  }
  return fd.cinv ? B.CreateNot(c) : c;
}
Value* Lower::flagV() {
  auto ov = [&](Intrinsic::ID id) { return B.CreateExtractValue(B.CreateIntrinsic(id, {fd.a->getType()}, {fd.a, fd.b}), 1); };
  switch (fd.k) {
  case FlagDesc::Mem: return B.CreateLoad(I1, fV);
  case FlagDesc::Sub: return ov(Intrinsic::ssub_with_overflow);
  case FlagDesc::Add: return ov(Intrinsic::sadd_with_overflow);
  case FlagDesc::Logic: return ConstantInt::getFalse(C);
  case FlagDesc::Gen: return fd.v;
  }
  return nullptr;
}

void Lower::flagsToGen() {
  if (fd.k == FlagDesc::Gen) { fd.fa = fd.fb = nullptr; return; }
  Value *n = flagN(), *z = flagZ(), *c = flagC(), *v = flagV();
  const bool dirty = fd.k != FlagDesc::Mem ? fd.dirty : false;
  fd = FlagDesc{};
  fd.k = FlagDesc::Gen; fd.n = n; fd.z = z; fd.c = c; fd.v = v; fd.dirty = dirty;
}
void Lower::setSub(Value* a, Value* b, Value* r) { fd = FlagDesc{}; fd.k = FlagDesc::Sub; fd.a = a; fd.b = b; fd.res = r; fd.dirty = true; flagsWritten = true; }
void Lower::setAdd(Value* a, Value* b, Value* r) { fd = FlagDesc{}; fd.k = FlagDesc::Add; fd.a = a; fd.b = b; fd.res = r; fd.dirty = true; flagsWritten = true; }
void Lower::setLogic(Value* r) { fd = FlagDesc{}; fd.k = FlagDesc::Logic; fd.res = r; fd.dirty = true; flagsWritten = true; }
void Lower::setGen(Value* n, Value* z, Value* c, Value* v) {
  fd = FlagDesc{}; fd.k = FlagDesc::Gen; fd.n = n; fd.z = z; fd.c = c; fd.v = v; fd.dirty = true; flagsWritten = true;
}
void Lower::materialize() {
  if (fd.k == FlagDesc::Mem || !fd.dirty) return;
  Value *n = flagN(), *z = flagZ(), *c = flagC(), *v = flagV();
  B.CreateStore(n, fN); B.CreateStore(z, fZ); B.CreateStore(c, fC); B.CreateStore(v, fV);
  fd.dirty = false;
}
Value* Lower::packNZCV() {
  auto bit = [&](Value* f, unsigned sh) { return B.CreateShl(B.CreateZExt(f, I32), sh); };
  Value *n = flagN(), *z = flagZ(), *c = flagC(), *v = flagV();
  return B.CreateOr(B.CreateOr(bit(n, 31), bit(z, 30)), B.CreateOr(bit(c, 29), bit(v, 28)));
}
void Lower::unpackNZCV(Value* w) {
  auto bit = [&](unsigned sh) { return B.CreateTrunc(B.CreateLShr(w, sh), I1); };
  setGen(bit(31), bit(30), bit(29), bit(28));
}

// Float compare, as the JIT sees it after fcmp: lt N=1; eq Z=1,C=1; gt C=1; unordered C=1,V=1.
void Lower::setFromFCmp(Value* a, Value* b) {
  setGen(B.CreateFCmpOLT(a, b), B.CreateFCmpOEQ(a, b), B.CreateFCmpUGE(a, b), B.CreateFCmpUNO(a, b));
  fd.fa = a; fd.fb = b;
}

namespace {
enum Arm { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, BAD };
Arm armCC(IR::CondClass c) {
  using CC_ = IR::CondClass;
  switch (c) {
  case CC_::EQ: return EQ; case CC_::NEQ: return NE; case CC_::UGE: return CS; case CC_::ULT: return CC;
  case CC_::MI: return MI; case CC_::PL: return PL; case CC_::VS: case CC_::FU: return VS;
  case CC_::VC: case CC_::FNU: return VC; case CC_::UGT: return HI; case CC_::ULE: return LS;
  case CC_::SGE: case CC_::FGE: return GE; case CC_::SLT: case CC_::FLU: return LT;
  case CC_::SGT: case CC_::FGT: return GT; case CC_::SLE: case CC_::FLEU: return LE;
  default: return BAD;
  }
}
} // namespace

Value* Lower::cond(IR::CondClass cc) {
  const Arm a = armCC(cc);
  if (a == BAD) return ConstantInt::getTrue(C); // AL
  // Directly on the operands where we know them.
  if (fd.k == FlagDesc::Gen && fd.fa) {
    Value *x = fd.fa, *y = fd.fb;
    switch (a) {
    case EQ: return B.CreateFCmpOEQ(x, y);   case NE: return B.CreateFCmpUNE(x, y);
    case CS: return B.CreateFCmpUGE(x, y);   case CC: return B.CreateFCmpOLT(x, y);
    case MI: return B.CreateFCmpOLT(x, y);   case PL: return B.CreateFCmpUGE(x, y);
    case VS: return B.CreateFCmpUNO(x, y);   case VC: return B.CreateFCmpORD(x, y);
    case HI: return B.CreateFCmpUGT(x, y);   case LS: return B.CreateFCmpOLE(x, y);
    case GE: return B.CreateFCmpOGE(x, y);   case LT: return B.CreateFCmpULT(x, y);
    case GT: return B.CreateFCmpOGT(x, y);   case LE: return B.CreateFCmpULE(x, y);
    default: break;
    }
  }
  if (fd.k == FlagDesc::Sub) {
    Value *x = fd.a, *y = fd.b;
    const bool inv = fd.cinv;
    switch (a) {
    case EQ: return B.CreateICmpEQ(x, y);
    case NE: return B.CreateICmpNE(x, y);
    case CS: return inv ? B.CreateICmpULT(x, y) : B.CreateICmpUGE(x, y);
    case CC: return inv ? B.CreateICmpUGE(x, y) : B.CreateICmpULT(x, y);
    case HI: return inv ? B.CreateICmpULT(x, y) : B.CreateICmpUGT(x, y);
    case LS: return inv ? B.CreateICmpUGE(x, y) : B.CreateICmpULE(x, y);
    case GE: return B.CreateICmpSGE(x, y);
    case LT: return B.CreateICmpSLT(x, y);
    case GT: return B.CreateICmpSGT(x, y);
    case LE: return B.CreateICmpSLE(x, y);
    default: break;
    }
  }
  if (fd.k == FlagDesc::Add || fd.k == FlagDesc::Logic) {
    switch (a) {
    case EQ: return flagZ();
    case NE: return B.CreateNot(flagZ());
    case MI: return flagN();
    case PL: return B.CreateNot(flagN());
    default: break;
    }
  }
  switch (a) {
  case EQ: return flagZ();
  case NE: return B.CreateNot(flagZ());
  case CS: return flagC();
  case CC: return B.CreateNot(flagC());
  case MI: return flagN();
  case PL: return B.CreateNot(flagN());
  case VS: return flagV();
  case VC: return B.CreateNot(flagV());
  case HI: return B.CreateAnd(flagC(), B.CreateNot(flagZ()));
  case LS: return B.CreateOr(B.CreateNot(flagC()), flagZ());
  case GE: return B.CreateICmpEQ(flagN(), flagV());
  case LT: return B.CreateICmpNE(flagN(), flagV());
  case GT: return B.CreateAnd(B.CreateNot(flagZ()), B.CreateICmpEQ(flagN(), flagV()));
  case LE: return B.CreateOr(flagZ(), B.CreateICmpNE(flagN(), flagV()));
  default: return ConstantInt::getTrue(C);
  }
}

Value* Lower::condOn(const FlagDesc& d, IR::CondClass cc) {
  FlagDesc saved = fd;
  fd = d;
  Value* r = cond(cc);
  fd = saved;
  return r;
}

// ------------------------------------------------------------------------------ state load/store ----
void Lower::scan() {
  auto useGpr = [&](unsigned r, bool w) { if (r < NumStaticGPR) { gprUsed[r] = true; gprWritten[r] |= w; } };
  auto useFpr = [&](unsigned r, bool w) { if (r < NumStaticFPR) { xmmUsed[r] = true; xmmWritten[r] |= w; } };
  for (auto [BlockNode, BlockHeader] : IR.GetBlocks()) {
    (void)BlockHeader;
    for (auto [Node, op] : IR.GetCode(BlockNode)) {
      ++res->irOps;
      switch (op->Op) {
      case IR::OP_LOADREGISTER: {
        auto* o = op->C<IR::IROp_LoadRegister>();
        if (o->Class == IR::RegClass::GPR) useGpr(o->Reg, false); else useFpr(o->Reg, false);
        break;
      }
      case IR::OP_STOREREGISTER: {
        IR::PhysicalRegister pr(Node);
        if (pr.AsRegClass() == IR::RegClass::GPRFixed) useGpr(pr.Reg, true); else useFpr(pr.Reg, true);
        break;
      }
      case IR::OP_LOADPF: useGpr(16, false); break;
      case IR::OP_LOADAF: useGpr(17, false); break;
      case IR::OP_STOREPF: useGpr(16, true); break;
      case IR::OP_STOREAF: useGpr(17, true); break;
      case IR::OP_LOADNZCV: case IR::OP_ADCWITHFLAGS: case IR::OP_ADC: case IR::OP_SBB: case IR::OP_SBBWITHFLAGS:
      case IR::OP_ADCZERO: case IR::OP_ADCZEROWITHFLAGS: case IR::OP_NZCVSELECT: case IR::OP_NZCVSELECTINCREMENT:
      case IR::OP_NZCVSELECTV: case IR::OP_CARRYINVERT: case IR::OP_RMIFNZCV: case IR::OP_AXFLAG: case IR::OP_SETSMALLNZV:
      case IR::OP_CONDSUBNZCV: case IR::OP_CONDADDNZCV: case IR::OP_SHIFTFLAGS: case IR::OP_ROTATEFLAGS: case IR::OP_NEG:
        flagsRead = true; break;
      case IR::OP_CONDJUMP:
        if (op->C<IR::IROp_CondJump>()->FromNZCV) flagsRead = true;
        break;
      case IR::OP_SUBNZCV: case IR::OP_ADDNZCV: case IR::OP_TESTNZ: case IR::OP_TESTZ: case IR::OP_ADDWITHFLAGS:
      case IR::OP_SUBWITHFLAGS: case IR::OP_ANDWITHFLAGS: case IR::OP_STORENZCV: case IR::OP_FCMP:
        flagsWritten = true; break;
      default: break;
      }
    }
  }
  if (flagsRead || flagsWritten) flagsRead = true; // a unit that writes only part of the flags keeps the rest
}

void Lower::prologue() {
  if (!m.O.fixedImageBase) {
    auto* li = B.CreateLoad(I64, m.imageBaseVar, "imgbase");
    li->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(C, {}));
    ImgBase = li;
  }
  for (unsigned i = 0; i < NumStaticGPR; ++i) {
    if (!gprUsed[i]) continue;
    gpr[i] = newAlloca(I64, "r");
    if (i < 16) {
      B.CreateStore(B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, gregs) + 8 * i), Align(8)), gpr[i]);
    } else {
      const uint64_t off = i == 16 ? offsetof(Core::CPUState, pf_raw) : offsetof(Core::CPUState, af_raw);
      B.CreateStore(B.CreateZExt(B.CreateAlignedLoad(I32, gep(off), Align(4)), I64), gpr[i]);
    }
  }
  for (unsigned i = 0; i < NumStaticFPR; ++i) {
    if (!xmmUsed[i]) continue;
    xmm[i] = newAlloca(V128, "x");
    B.CreateStore(B.CreateAlignedLoad(V128, gep(offsetof(Core::CPUState, xmm.sse.data) + 16 * i), Align(16)), xmm[i]);
  }
  fN = newAlloca(I1, "N"); fZ = newAlloca(I1, "Z"); fC = newAlloca(I1, "C"); fV = newAlloca(I1, "V");
}

// The flags at unit entry: from the context (when the unit reads them) or the x86 reset values. Not done on the resume path: the
// flags are dead after a call, so a call-return block does not care what they were.
void Lower::flagsInit() {
  if (flagsRead) {
    Value* w = B.CreateAlignedLoad(I32, gep(offsetof(Core::CPUState, flags) + X86State::RFLAG_NZCV_LOC), Align(1));
    auto bit = [&](unsigned sh) { return B.CreateTrunc(B.CreateLShr(w, sh), I1); };
    B.CreateStore(bit(31), fN); B.CreateStore(bit(30), fZ); B.CreateStore(bit(29), fC); B.CreateStore(bit(28), fV);
  } else {
    B.CreateStore(ConstantInt::getFalse(C), fN); B.CreateStore(ConstantInt::getFalse(C), fZ);
    B.CreateStore(ConstantInt::getTrue(C), fC); B.CreateStore(ConstantInt::getFalse(C), fV);
  }
}

// Registers and flags back to the context: before every exit and call.
void Lower::spill(bool flags) {
  // PF and AF are flags: dead across calls and returns, so they are only stored where the flags are (they stay dirty).
  const uint64_t keep = flags ? 0 : (3ull << 16) & dirty;
  for (unsigned i = 0; i < NumStaticGPR; ++i) {
    if (!(dirty >> i & 1) || (i >= 16 && !flags)) continue;
    Value* v = B.CreateLoad(I64, gpr[i]);
    if (i < 16) B.CreateAlignedStore(v, gep(offsetof(Core::CPUState, gregs) + 8 * i), Align(8));
    else B.CreateAlignedStore(B.CreateTrunc(v, I32), gep(i == 16 ? offsetof(Core::CPUState, pf_raw) : offsetof(Core::CPUState, af_raw)), Align(4));
  }
  for (unsigned i = 0; i < NumStaticFPR; ++i)
    if (dirty >> (NumStaticGPR + i) & 1) B.CreateAlignedStore(B.CreateLoad(V128, xmm[i]), gep(offsetof(Core::CPUState, xmm.sse.data) + 16 * i), Align(16));
  dirty = keep;
  if (flags && flagsWritten) B.CreateAlignedStore(packNZCV(), gep(offsetof(Core::CPUState, flags) + X86State::RFLAG_NZCV_LOC), Align(1));
}

// Registers reloaded after a call (the callee may have changed any of them).
void Lower::fill() {
  for (unsigned i = 0; i < NumStaticGPR; ++i) {
    if (!gprUsed[i]) continue;
    // rbx 3, rbp 5, r12-r15: callee-saved, still what the spill stored
    if (m.O.calleeSaved && (i == 3 || i == 5 || (i >= 12 && i <= 15))) continue;
    if (i >= 16 && !m.O.storeFlagsAtCalls) continue; // PF/AF are dead after a call
    if (i < 16) B.CreateStore(B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, gregs) + 8 * i), Align(8)), gpr[i]);
    else B.CreateStore(B.CreateZExt(B.CreateAlignedLoad(I32, gep(i == 16 ? offsetof(Core::CPUState, pf_raw) : offsetof(Core::CPUState, af_raw)), Align(4)), I64), gpr[i]);
  }
  for (unsigned i = 0; i < NumStaticFPR; ++i)
    if (xmmUsed[i]) B.CreateStore(B.CreateAlignedLoad(V128, gep(offsetof(Core::CPUState, xmm.sse.data) + 16 * i), Align(16)), xmm[i]);
}

// Every used register from the context (after a hook callback, which may have changed any of them, callee-saved ones included).
void Lower::fillAll() {
  for (unsigned i = 0; i < NumStaticGPR; ++i) {
    if (!gprUsed[i]) continue;
    if (i < 16) B.CreateStore(B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, gregs) + 8 * i), Align(8)), gpr[i]);
    else B.CreateStore(B.CreateZExt(B.CreateAlignedLoad(I32, gep(i == 16 ? offsetof(Core::CPUState, pf_raw) : offsetof(Core::CPUState, af_raw)), Align(4)), I64), gpr[i]);
  }
  for (unsigned i = 0; i < NumStaticFPR; ++i)
    if (xmmUsed[i]) B.CreateStore(B.CreateAlignedLoad(V128, gep(offsetof(Core::CPUState, xmm.sse.data) + 16 * i), Align(16)), xmm[i]);
}

// A native hook site (docs/RECOMP_ABI.md, "Hook slots"), at the start of the guest instruction at hs.vaddr:
//   if (bbr_hook_slot[k]) { spill; rip = site; act = slot(s, k); reload;
//                           act 0: run this instruction;  act 2: return (rip and rsp set by the slot);
//                           else: continue at s->rip: a case of hs.resume (a block of this unit), else the dispatcher }
// Flags stay as they were (the callback cannot change them): they are put into the allocas before the test, so the jump path
// continues from there, while the fall-through path goes on with the lazy flag descriptor.
void Lower::emitHookSite(const HookSite& hs) {
  materialize();
  auto* slotAddr = B.CreateConstInBoundsGEP1_64(Ptr, m.hookSlotVar, hs.index);
  auto* slot = B.CreateAlignedLoad(Ptr, slotAddr, Align(8), "hslot");
  slot->setAtomic(AtomicOrdering::Monotonic);
  auto* hookBB = newBB("hook");
  auto* contBB = newBB("hcont");
  auto* br = B.CreateCondBr(B.CreateICmpNE(slot, ConstantPointerNull::get(cast<PointerType>(Ptr))), hookBB, contBB);
  br->setMetadata(LLVMContext::MD_prof, MDBuilder(C).createBranchWeights(1, 4000));
  B.SetInsertPoint(hookBB);
  const uint64_t savedDirty = dirty;
  spill(true);
  B.CreateAlignedStore(reloc(m.O.liftBase + hs.vaddr), gep(offsetof(Core::CPUState, rip)), Align(8));
  Value* act = B.CreateCall(FunctionType::get(I64, {Ptr, I64}, false), slot, {S, c64(hs.index)});
  fillAll();
  dirty = 0;
  auto* retBB = newBB("hret");
  auto* jmpBB = newBB("hjump");
  auto* sw = B.CreateSwitch(act, jmpBB, 2);
  sw->addCase(cast<ConstantInt>(ConstantInt::get(I64, 0)), contBB);
  sw->addCase(cast<ConstantInt>(ConstantInt::get(I64, 2)), retBB);
  B.SetInsertPoint(retBB);
  B.CreateRetVoid();
  B.SetInsertPoint(jmpBB);
  Value* rip = B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, rip)), Align(8));
  std::vector<std::pair<uint64_t, BasicBlock*>> cases;
  for (uint64_t r : hs.resume) {
    auto sp = hookSplit.find(r);
    if (sp != hookSplit.end()) { cases.push_back({r, sp->second}); continue; }
    auto bi = hookBlockAt.find(r);
    if (bi != hookBlockAt.end() && hookReachable.count(bi->second)) cases.push_back({r, bbOf[bi->second]});
  }
  auto* miss = newBB("hmiss");
  if (cases.empty()) {
    B.CreateBr(miss);
  } else {
    Value* off = B.CreateSub(rip, m.O.fixedImageBase ? (Value*)c64(m.O.imageBase) : ImgBase);
    auto* si = B.CreateSwitch(off, miss, cases.size());
    for (auto [vaddr, bb] : cases) si->addCase(cast<ConstantInt>(ConstantInt::get(I64, vaddr)), bb);
  }
  B.SetInsertPoint(miss);
  auto* c = B.CreateCall(m.helper("bbr_dispatch_tail", m.Void, {Ptr}), {S});
  c->setTailCallKind(CallInst::TCK_MustTail);
  B.CreateRetVoid();
  B.SetInsertPoint(contBB);
  dirty = savedDirty;
  ++res->hookSites;
  res->hookIdx.push_back(hs.index);
}

// ----------------------------------------------------------------------------------- control flow ----
void Lower::emitBreak(const IR::IROp_Break* op) {
  const uint64_t kind = (uint64_t)op->Reason.Signal | ((uint64_t)op->Reason.TrapNumber << 8) | ((uint64_t)op->Reason.si_code << 16) |
                        ((uint64_t)op->Reason.ErrorRegister << 32);
  spill(true);
  // (the IR stores the faulting RIP into the context itself, before the Break)
  B.CreateCall(m.helper("bbr_trap", m.Void, {Ptr, I64}), {S, c64(kind)});
  B.CreateRetVoid();
}

void Lower::emitExit(Ref node, const IR::IROp_ExitFunction* op) {
  const auto hint = op->Hint;
  uint64_t a = 0;
  const bool isConst = addrConst(op->NewRIP, a);
  Value* target = A(op->NewRIP);
  const uint64_t vaddr = a - m.O.liftBase;
  const bool direct = isConst && m.O.isKnownUnit && m.O.isKnownUnit(vaddr);
  const uint64_t ripOff = offsetof(Core::CPUState, rip);

  if (hint == IR::BranchHint::Return) {
    spill(m.O.storeFlagsAtCalls);
    B.CreateAlignedStore(target, gep(ripOff), Align(8));
    B.CreateRetVoid();
    return;
  }
  if (hint == IR::BranchHint::Call) {
    Value* retAddr = A(op->CallReturnAddress);
    ++res->callExits;
    spill(m.O.storeFlagsAtCalls);
    if (direct) {
      ++res->directCalls;
      B.CreateCall(m.unitFn(vaddr), {S});
    } else {
      ++res->dispatchCalls;
      B.CreateAlignedStore(target, gep(ripOff), Align(8));
      B.CreateCall(m.helper("bbr_dispatch", m.Void, {Ptr, I64}), {S, target});
    }
    // The callee returns to the pushed address (rip == retaddr) or somewhere else (longjmp, exception). Somewhere else: the
    // state is all in `s`; bbr_unwound runs the unit at rip if there is one (a setjmp return site, a landing pad) from this
    // host frame, so the code keeps running as units and its eventual return meets a caller's rip check; else it returns
    // and the callers unwind (docs/RECOMP_ABI.md).
    auto unwound = [&] {
      auto* c = B.CreateCall(m.helper("bbr_unwound", m.Void, {Ptr}), {S});
      c->setTailCallKind(CallInst::TCK_MustTail);
      B.CreateRetVoid();
    };
    Value* rip = B.CreateAlignedLoad(I64, gep(ripOff), Align(8));
    if (!op->CallReturnBlock.IsInvalid()) {
      auto* ok = newBB("retok");
      auto* other = newBB("retother");
      B.CreateCondBr(B.CreateICmpEQ(rip, retAddr), ok, other);
      B.SetInsertPoint(other);
      unwound();
      B.SetInsertPoint(ok);
      fill();
      fd = FlagDesc{};
      B.CreateBr(bbOf[op->CallReturnBlock.ID().Value]);
    } else {
      unwound();
    }
    return;
  }
  // An indirect jump whose jump table the lifter resolved: a switch on the image offset over the unit's blocks.
  // What is not a case (a stale table, a computed value) goes the ordinary way below.
  if (!isConst) {
    auto sw = swCases.find(IR.GetID(node).Value);
    if (sw != swCases.end() && !sw->second.empty()) {
      materialize();
      Value* off = B.CreateSub(target, m.O.fixedImageBase ? (Value*)c64(m.O.imageBase) : ImgBase);
      auto* def = newBB("swdef");
      auto* si = B.CreateSwitch(off, def, sw->second.size());
      for (auto [vaddr, id] : sw->second) si->addCase(cast<ConstantInt>(ConstantInt::get(I64, vaddr)), bbOf[id]);
      ++res->switches; res->switchCases += sw->second.size();
      B.SetInsertPoint(def);
    }
  }
  // Tail call, jmp out of the unit, indirect jump.
  spill(true);
  if (direct) {
    ++res->directCalls;
    auto* c = B.CreateCall(m.unitFn(vaddr), {S});
    c->setTailCallKind(CallInst::TCK_MustTail);
  } else {
    ++res->dispatchCalls;
    B.CreateAlignedStore(target, gep(ripOff), Align(8));
    auto* c = B.CreateCall(m.helper("bbr_dispatch_tail", m.Void, {Ptr}), {S});
    c->setTailCallKind(CallInst::TCK_MustTail);
  }
  B.CreateRetVoid();
}

void Lower::trapUnsupported(Ref node, IR::IROp_Header* op) {
  unsup.insert(std::string(IR::GetName(op->Op)));
  ++res->trapSites;
  B.CreateCall(m.helper("bbr_trap", m.Void, {Ptr, I64}), {S, c64(BBR_TRAP_UNSUPPORTED | op->Op)});
  if (IR::GetHasDest(op->Op) && node) {
    const auto cls = IR::GetRegClass(op->Op);
    def(node, cls == IR::RegClass::FPR ? (Value*)ConstantAggregateZero::get(V128) : (Value*)c64(0));
  }
}

bool Lower::emitOp(Ref node, IR::IROp_Header* op) {
  switch (op->Op) {
  case IR::OP_BEGINBLOCK: case IR::OP_ENDBLOCK: case IR::OP_CODEBLOCK: case IR::OP_IRHEADER: case IR::OP_DUMMY:
  case IR::OP_INVALIDATEFLAGS: case IR::OP_INLINECONSTANT: case IR::OP_INLINEENTRYPOINTOFFSET:
    return true;
  case IR::OP_GUESTOPCODE: {
    curGuest = op->C<IR::IROp_GuestOpcode>()->GuestEntryOffset;
    if (m.O.hookSites || !hookSplit.empty()) {
      const uint64_t vaddr = entryRip - m.O.liftBase + curGuest;
      // A point a hook callback may continue at, in the middle of a block: the block starts here (the fall-through path comes in
      // with its flags in the allocas), the callback's jump path joins it.
      auto sp = hookSplit.find(vaddr);
      if (sp != hookSplit.end() && !terminated()) {
        materialize();
        fd = FlagDesc{};
        B.CreateBr(sp->second);
        B.SetInsertPoint(sp->second);
        ++curRegion;
      }
      if (m.O.hookSites) {
        auto hs = m.O.hookSites->find(vaddr);
        if (hs != m.O.hookSites->end()) emitHookSite(hs->second);
      }
    }
    return true;
  }
  case IR::OP_JUMP: {
    materialize();
    B.CreateBr(bbOf[op->C<IR::IROp_Jump>()->TargetBlock.ID().Value]);
    return true;
  }
  case IR::OP_CONDJUMP: {
    auto* o = op->C<IR::IROp_CondJump>();
    Value* c;
    if (o->FromNZCV) {
      c = cond(o->Cond);
    } else {
      uint64_t k = 0;
      const bool isK = this->K(o->Cmp2, k);
      Value* x = A(o->Cmp1);
      const unsigned bits = o->CompareSize == IR::OpSize::i32Bit ? 32 : 64;
      switch (o->Cond) {
      case IR::CondClass::EQ: c = B.CreateICmpEQ(tr(x, bits), cN(bits, 0)); break;
      case IR::CondClass::NEQ: c = B.CreateICmpNE(tr(x, bits), cN(bits, 0)); break;
      case IR::CondClass::TSTZ: c = B.CreateICmpEQ(B.CreateAnd(x, c64(1ull << (isK ? k & 63 : 0))), c64(0)); break;
      case IR::CondClass::TSTNZ: c = B.CreateICmpNE(B.CreateAnd(x, c64(1ull << (isK ? k & 63 : 0))), c64(0)); break;
      default: c = ConstantInt::getTrue(C); break;
      }
    }
    materialize();
    B.CreateCondBr(c, bbOf[o->TrueBlock.ID().Value], bbOf[o->FalseBlock.ID().Value]);
    return true;
  }
  case IR::OP_EXITFUNCTION: emitExit(node, op->C<IR::IROp_ExitFunction>()); return true;
  case IR::OP_BREAK: emitBreak(op->C<IR::IROp_Break>()); return true;
  default: break;
  }
  if (emitScalarOp(node, op)) return true;
  if (emitMemOp(node, op)) return true;
  if (emitAtomicOp(node, op)) return true;
  if (emitVectorOp(node, op)) return true;
  return false;
}

void Lower::run() {
  scan();
  const bool granular = meta && meta->granular;
  const std::string name = unitName(entryVaddr);
  F = cast<Function>(m.defFn(entryVaddr).getCallee());
  F->setDoesNotThrow();
  if (!F->empty()) { fprintf(stderr, "bb-recomp: unit %s lowered twice\n", name.c_str()); return; }
  S = F->getArg(0);
  S->setName("s");
  allocaBB = BasicBlock::Create(C, "entry", F);
  auto* proBB = BasicBlock::Create(C, "pro", F);
  if (m.O.dirtyCheck && m.O.unitIndex) {
    // The runtime sets bbr_dirty[i] when the code of unit i was modified (hooks, patches): the unit then does not run, the
    // dispatcher's slow path (the fallback CPU) does. First thing in the unit; one byte load and a predictable branch.
    const int64_t idx = m.O.unitIndex(entryVaddr);
    if (idx < 0) fprintf(stderr, "bb-recomp: unit %s has no index in bbr_units\n", name.c_str());
    auto* chk = BasicBlock::Create(C, "chk", F);
    auto* dirtyBB = BasicBlock::Create(C, "dirty", F);
    IRBuilder<>(allocaBB).CreateBr(chk);
    B.SetInsertPoint(chk);
    Value* p = B.CreateConstInBoundsGEP1_64(I8, m.dirtyVar, idx < 0 ? 0 : (uint64_t)idx);
    Value* d = B.CreateLoad(I8, p);
    auto* br = B.CreateCondBr(B.CreateICmpNE(d, ConstantInt::get(I8, 0)), dirtyBB, proBB);
    br->setMetadata(LLVMContext::MD_prof, MDBuilder(C).createBranchWeights(1, 2000));
    B.SetInsertPoint(dirtyBB);
    if (m.O.resumeEntries || granular) {
      // a resume request (flag set, rip = the return address wanted) at a dirty unit: not here, the dispatcher tries the next unit
      auto* go = BasicBlock::Create(C, "dirty_go", F);
      auto* rs = BasicBlock::Create(C, "dirty_resume", F);
      B.CreateCondBr(B.CreateICmpNE(B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, _pad1)), Align(8)), c64(0)), rs, go);
      B.SetInsertPoint(rs); B.CreateRetVoid();
      B.SetInsertPoint(go);
    }
    // the unit's image-relative vaddr: the runtime adds the image base and sets rip itself (a two-instruction tail call here)
    auto* c = B.CreateCall(m.helper("bbr_dirty_entry", m.Void, {Ptr, I64}), {S, c64(entryVaddr)});
    c->setTailCall(true);
    B.CreateRetVoid();
  } else {
    IRBuilder<>(allocaBB).CreateBr(proBB);
  }
  B.SetInsertPoint(proBB);
  prologue();

  // Blocks of the unit, in IR order; blocks are created up front so branches can name them.
  struct BlockInfo { Ref node; IR::IROp_Header* hdr; std::vector<uint32_t> succ; };
  std::vector<uint32_t> order;
  std::unordered_map<uint32_t, BlockInfo> blocks;
  // Guest address (image vaddr) of every block, for the switch cases.
  std::unordered_map<uint64_t, uint32_t> blockAt;
  if (meta && !meta->switches.empty())
    for (auto [BlockNode, BlockHeader] : IR.GetBlocks())
      blockAt[entryRip - m.O.liftBase + BlockHeader->C<IR::IROp_CodeBlock>()->GuestEntryOffset] = IR.GetID(BlockNode).Value;
  std::set<uint64_t> guestAt; // image vaddr of every GuestOpcode of the unit (hook sites)
  for (auto [BlockNode, BlockHeader] : IR.GetBlocks()) {
    const uint32_t id = IR.GetID(BlockNode).Value;
    BlockInfo bi{BlockNode, BlockHeader, {}};
    int64_t lastGuest = -1;
    if (m.O.hookSites || granular) hookBlockAt[entryRip - m.O.liftBase + BlockHeader->C<IR::IROp_CodeBlock>()->GuestEntryOffset] = id;
    for (auto [Node, op] : IR.GetCode(BlockNode)) {
      if (op->Op == IR::OP_GUESTOPCODE) {
        lastGuest = op->C<IR::IROp_GuestOpcode>()->GuestEntryOffset;
        if (m.O.hookSites || granular) guestAt.insert(entryRip - m.O.liftBase + lastGuest);
      }
      if (op->Op == IR::OP_EXITFUNCTION && m.O.resumeEntries) {
        auto* o = op->C<IR::IROp_ExitFunction>();
        uint64_t ra;
        if (o->Hint == IR::BranchHint::Call && !o->CallReturnBlock.IsInvalid() && addrConst(o->CallReturnAddress, ra))
          retBlocks.push_back({ra - m.O.liftBase, o->CallReturnBlock.ID().Value});
      }
      if (op->Op == IR::OP_EXITFUNCTION && lastGuest >= 0 && meta && !meta->switches.empty()) {
        auto* o = op->C<IR::IROp_ExitFunction>();
        uint64_t k;
        if (o->Hint != IR::BranchHint::Call && o->Hint != IR::BranchHint::Return && !addrConst(o->NewRIP, k)) {
          const uint64_t pc = entryRip - m.O.liftBase + lastGuest;
          for (auto& sw : meta->switches) {
            if (sw.jmpVaddr != pc) continue;
            auto& cases = swCases[IR.GetID(Node).Value];
            for (uint64_t t : sw.targets) {
              auto it = blockAt.find(t);
              if (it == blockAt.end()) continue;
              cases.push_back({t, it->second});
              bi.succ.push_back(it->second);
            }
            break;
          }
        }
      }
      if (op->Op == IR::OP_JUMP) bi.succ.push_back(op->C<IR::IROp_Jump>()->TargetBlock.ID().Value);
      else if (op->Op == IR::OP_CONDJUMP) {
        auto* o = op->C<IR::IROp_CondJump>();
        bi.succ.push_back(o->TrueBlock.ID().Value); bi.succ.push_back(o->FalseBlock.ID().Value);
      } else if (op->Op == IR::OP_EXITFUNCTION) {
        auto* o = op->C<IR::IROp_ExitFunction>();
        if (o->Hint == IR::BranchHint::Call && !o->CallReturnBlock.IsInvalid()) bi.succ.push_back(o->CallReturnBlock.ID().Value);
      }
    }
    order.push_back(id);
    bbOf[id] = BasicBlock::Create(C, "b" + std::to_string(id), F);
    blocks.emplace(id, std::move(bi));
  }
  if (order.empty()) { B.CreateUnreachable(); return; }
  auto* proEnd = B.GetInsertBlock(); // the prologue block: its terminator is emitted after the blocks (resume switch)
  if (granular) {
    // Granular entries: every instruction boundary of the unit is a point the unit can be entered at. An instruction inside a block (not a
    // block entry) gets a block of its own, like a hook resume point: the fall-through path joins it with its flags in the allocas.
    for (uint64_t v : guestAt)
      if (!hookBlockAt.count(v) && !hookSplit.count(v) && std::binary_search(meta->insts.begin(), meta->insts.end(), v)) hookSplit[v] = newBB("gsplit");
  }
  if (m.O.hookSites) {
    // Resume points of the hook sites that are instructions inside a block (no block entry): the block gets a split there.
    for (auto& [vaddr, hs] : *m.O.hookSites) {
      if (!guestAt.count(vaddr)) continue;
      for (uint64_t r : hs.resume)
        if (!hookBlockAt.count(r) && guestAt.count(r) && !hookSplit.count(r)) hookSplit[r] = newBB("hsplit");
    }
  }

  // Reverse post-order from the entry: a block's dominators are emitted first, so every operand exists
  // when it is used (the IR list order does not guarantee that).
  std::vector<uint32_t> post;
  {
    std::unordered_map<uint32_t, int> state;
    std::vector<std::pair<uint32_t, size_t>> st;
    st.push_back({order[0], 0}); state[order[0]] = 1;
    while (!st.empty()) {
      auto& [id, i] = st.back();
      auto& s = blocks[id].succ;
      if (i < s.size()) {
        uint32_t n = s[i++];
        if (!state[n] && blocks.count(n)) { state[n] = 1; st.push_back({n, 0}); }
      } else { post.push_back(id); st.pop_back(); }
    }
  }
  std::reverse(post.begin(), post.end());
  hookReachable.insert(post.begin(), post.end());

  // Which static registers may differ from the context at each block start: a register is dirty from its
  // StoreRegister until the next spill (exit, call, syscall); a call's return block starts clean.
  for (uint32_t id : order) dirtyIn[id] = 0;
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t id : post) {
      uint64_t d = dirtyIn[id];
      for (auto [Node, op] : IR.GetCode(blocks[id].node)) {
        switch (op->Op) {
        case IR::OP_STOREREGISTER: {
          IR::PhysicalRegister pr(Node);
          d |= 1ull << (pr.AsRegClass() == IR::RegClass::GPRFixed ? pr.Reg : NumStaticGPR + pr.Reg);
          break;
        }
        case IR::OP_STOREPF: d |= 1ull << 16; break;
        case IR::OP_STOREAF: d |= 1ull << 17; break;
        case IR::OP_EXITFUNCTION: if (swCases.count(IR.GetID(Node).Value) && !swCases[IR.GetID(Node).Value].empty()) break; d = 0; break; // a switch keeps the registers dirty
        case IR::OP_BREAK: case IR::OP_SYSCALL: d = 0; break;
        default: break;
        }
      }
      for (uint32_t sc : blocks[id].succ)
        if (blocks.count(sc) && (dirtyIn[sc] | d) != dirtyIn[sc]) { dirtyIn[sc] |= d; changed = true; }
    }
  }

  // Loops (strongly connected blocks) with no guest store, atomic, fence or exit: their guest loads are volatile (loadVal).
  quietLoop.clear();
  {
    auto sideEffect = [&](uint32_t id) {
      for (auto [Node, op] : IR.GetCode(blocks[id].node)) {
        const std::string_view n = IR::GetName(op->Op);
        if (n.find("StoreMem") != n.npos || n.find("VStore") != n.npos || n.find("CAS") != n.npos || n.find("Atomic") != n.npos ||
            n == "Fence" || n == "ExitFunction" || n == "Break" || n == "Syscall" || n == "Thunk" || n == "CallbackReturn" ||
            n.find("Push") != n.npos || n.find("CacheLine") != n.npos || n == "MemSet" || n == "MemCpy" || n == "StoreStackMem")
          return true;
      }
      return false;
    };
    std::unordered_map<uint32_t, int> index, low;
    std::vector<uint32_t> stack;
    std::set<uint32_t> onStack;
    int next = 0;
    std::function<void(uint32_t)> strong = [&](uint32_t v) {
      index[v] = low[v] = next++;
      stack.push_back(v); onStack.insert(v);
      for (uint32_t w : blocks[v].succ) {
        if (!blocks.count(w)) continue;
        if (!index.count(w)) { strong(w); low[v] = std::min(low[v], low[w]); }
        else if (onStack.count(w)) low[v] = std::min(low[v], index[w]);
      }
      if (low[v] != index[v]) return;
      std::vector<uint32_t> scc;
      uint32_t w;
      do { w = stack.back(); stack.pop_back(); onStack.erase(w); scc.push_back(w); } while (w != v);
      const auto& sv = blocks[v].succ;
      const bool loop = scc.size() > 1 || std::find(sv.begin(), sv.end(), v) != sv.end();
      if (!loop) return;
      for (uint32_t b : scc) if (sideEffect(b)) return;
      quietLoop.insert(scc.begin(), scc.end());
    };
    for (uint32_t id : order) if (!index.count(id)) strong(id);
  }

  std::set<uint32_t> emitted;
  for (uint32_t id : post) {
    emitted.insert(id);
    curBlock = id;
    B.SetInsertPoint(bbOf[id]);
    fd = FlagDesc{};
    ++curRegion;
    dirty = dirtyIn[id];
    for (auto [Node, op] : IR.GetCode(blocks[id].node)) {
      if (terminated()) break;
      // Multi-output ops write their handle nodes through allocas.
      if (op->Op == IR::OP_RMWHANDLE || op->Op == IR::OP_ALLOCATEGPR || op->Op == IR::OP_ALLOCATEFPR || op->Op == IR::OP_ALLOCATEGPRAFTER) {
        const uint32_t nid = IR.GetID(Node).Value;
        var[nid] = newAlloca(op->Op == IR::OP_ALLOCATEFPR ? V128 : I64, "h");
        if (op->Op == IR::OP_RMWHANDLE) B.CreateStore(A(op->C<IR::IROp_RMWHandle>()->Value), var[nid]);
        continue;
      }
      if (!emitOp(Node, op)) trapUnsupported(Node, op);
    }
    if (!terminated()) {
      materialize();
      B.CreateUnreachable();
    }
  }
  // Prologue exit: into the unit's entry block, or (resume entries, granular entries) into the switch that re-enters at a call-return block or at
  // any instruction boundary of the unit when the dispatcher asks for it with the resume flag in the guest state (docs/RECOMP_ABI.md).
  B.SetInsertPoint(proEnd);
  std::vector<std::pair<uint64_t, uint32_t>> cases;
  std::vector<std::pair<uint64_t, BasicBlock*>> bbCases;   // granular: the targets as blocks (instruction boundaries inside blocks are split blocks)
  if (m.O.resumeEntries) for (auto& rb : retBlocks) if (emitted.count(rb.second)) cases.push_back(rb);
  if (granular) {
    for (auto& [v, id] : hookBlockAt) if (emitted.count(id) && std::binary_search(meta->insts.begin(), meta->insts.end(), v)) cases.push_back({v, id});
    for (auto& [v, bb] : hookSplit) if (!bb->empty()) bbCases.push_back({v, bb});   // (a split of a block that was never emitted stays empty)
  }
  std::sort(cases.begin(), cases.end());
  cases.erase(std::unique(cases.begin(), cases.end(), [](auto& a, auto& b) { return a.first == b.first; }), cases.end());
  std::sort(bbCases.begin(), bbCases.end(), [](auto& a, auto& b) { return a.first < b.first; });
  if ((m.O.resumeEntries || granular) && (!cases.empty() || !bbCases.empty())) {
    auto* rsw = newBB("resume");
    auto* fi = newBB("flags");
    auto* flag = B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, _pad1)), Align(8));
    B.CreateCondBr(B.CreateICmpNE(flag, c64(0)), rsw, fi);
    B.SetInsertPoint(fi);
    flagsInit();
    B.CreateBr(bbOf[order[0]]);
    B.SetInsertPoint(rsw);
    B.CreateAlignedStore(c64(0), gep(offsetof(Core::CPUState, _pad1)), Align(8));
    // Granular entries are entered by jumps: the flags the jumping unit stored are the ones the code at the target sees. (After a call the
    // flags are dead, so the call-return resume entries leave them alone.)
    if (granular) flagsInit();
    Value* rip = B.CreateAlignedLoad(I64, gep(offsetof(Core::CPUState, rip)), Align(8));
    Value* off = B.CreateSub(rip, m.O.fixedImageBase ? (Value*)c64(m.O.imageBase) : ImgBase);
    auto* miss = newBB("resume_miss");
    auto* si = B.CreateSwitch(off, miss, cases.size() + bbCases.size());
    for (auto [vaddr, id] : cases) si->addCase(cast<ConstantInt>(ConstantInt::get(I64, vaddr)), bbOf[id]);
    for (auto [vaddr, bb] : bbCases) si->addCase(cast<ConstantInt>(ConstantInt::get(I64, vaddr)), bb);
    B.SetInsertPoint(miss);
    B.CreateAlignedStore(c64(1), gep(offsetof(Core::CPUState, _pad1)), Align(8)); // not here: the dispatcher tries the next unit
    B.CreateRetVoid();
    res->resumeCases = cases.size() + bbCases.size();
    if (granular) { res->granularUnits = 1; res->granularCases = cases.size() + bbCases.size(); }
  } else {
    flagsInit();
    B.CreateBr(bbOf[order[0]]);
  }
  for (uint32_t id : order)
    if (!emitted.count(id)) bbOf[id]->eraseFromParent();
  for (auto& [v, bb] : hookSplit)
    if (bb->empty()) { IRBuilder<> ub(bb); ub.CreateUnreachable(); } // a resume point whose instruction was never reached: nothing jumps here

  for (auto& s : unsup) res->unsupported.push_back(s);
}

} // namespace bbr

// ------------------------------------------------------------------------------------- public API ----
using namespace bbr;

struct UnitLowerer::Impl {
  ModuleCtx m;
  Impl(LLVMContext& c, Module& mod, const LowerOptions& o) : m(c, mod, o) {
    if (!o.fixedImageBase) {
      m.imageBaseVar = new GlobalVariable(mod, m.I64, false, GlobalValue::ExternalLinkage, nullptr, "bbr_image_base");
      m.imageBaseVar->setDSOLocal(true);
    }
    if (o.dirtyCheck) {
      m.dirtyVar = new GlobalVariable(mod, m.I8, false, GlobalValue::ExternalLinkage, nullptr, "bbr_dirty");
      m.dirtyVar->setDSOLocal(true);
    }
    if (o.hookSites) {
      m.hookSlotVar = new GlobalVariable(mod, m.Ptr, false, GlobalValue::ExternalLinkage, nullptr, "bbr_hook_slot");
      m.hookSlotVar->setDSOLocal(true);
    }
  }
};

UnitLowerer::UnitLowerer(LLVMContext& ctx, Module& mod, const LowerOptions& opt) : impl_(new Impl(ctx, mod, opt)) {}
UnitLowerer::~UnitLowerer() = default;

void UnitLowerer::lower(uint64_t entry, const void* irView, LowerResult* res, const UnitMeta* meta) {
  const auto& ir = *static_cast<const IR::IRListView*>(irView);
  Lower l(impl_->m, ir, res, entry, meta);
  l.run();
}

void UnitLowerer::reconfigure(const std::string& defPrefix, std::function<std::string(uint64_t)> callName) {
  auto& o = impl_->m.O;
  o.defPrefix = defPrefix;
  o.callName = callName;
  o.isKnownUnit = [callName](uint64_t v) { return !callName(v).empty(); };
}

void UnitLowerer::stub(uint64_t entry) {
  auto& m = impl_->m;
  auto* f = cast<Function>(m.defFn(entry).getCallee());
  auto* bb = BasicBlock::Create(m.C, "entry", f);
  IRBuilder<> b(bb);
  b.CreateCall(m.helper("bbr_trap", m.Void, {m.Ptr, m.I64}), {f->getArg(0), ConstantInt::get(m.I64, BBR_TRAP_UNLIFTABLE | entry)});
  b.CreateRetVoid();
}
