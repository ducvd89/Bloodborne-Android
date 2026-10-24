// Internal to lower_llvm.cpp / lower_vector.cpp: the per-unit lowering state.
// Semantics of every op follow FEXCore/Source/Interface/Core/JIT/*.cpp (the Arm64 JIT is the specification).
#pragma once
#include "lower_llvm.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAArch64.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "Interface/IR/IntrusiveIRList.h"
#include "Interface/IR/RegisterAllocationData.h"
#include <FEXCore/Core/CoreState.h>

#include <map>
#include <set>
#include <unordered_map>

namespace bbr {
using namespace FEXCore;
using llvm::Value;
using W = IR::OrderedNodeWrapper;
using Ref = IR::Ref;

constexpr unsigned NumStaticGPR = 18; // 16 x86 registers + PF + AF (FEX's PF_AS_GREG / AF_AS_GREG)
constexpr unsigned NumStaticFPR = 16;

// Flags are ARM NZCV as FEX defines them. After a flag-setting op the four flags are not computed: the
// descriptor remembers the operands, so a following condition becomes one icmp on the original values
// (which LLVM folds into cmp + b.cond). The descriptor is written back to the i1 allocas at block ends.
struct FlagDesc {
  enum Kind { Mem, Sub, Add, Logic, Gen } k = Mem; // Mem: the four allocas hold the flags
  Value *a = nullptr, *b = nullptr, *res = nullptr; // Sub: a-b; Add: a+b (res optional); Logic: res
  Value *n = nullptr, *z = nullptr, *c = nullptr, *v = nullptr; // Gen
  Value *fa = nullptr, *fb = nullptr; // Gen from an fcmp: its operands, so conditions become fcmp predicates
  bool cinv = false;  // Sub/Add/Logic: the stored C is the inverse of the natural one (CarryInvert)
  bool dirty = false; // not yet stored to the allocas
};

// Shared module-level state (one per UnitLowerer).
struct ModuleCtx {
  llvm::LLVMContext& C;
  llvm::Module& M;
  LowerOptions O;
  llvm::Type *I1, *I8, *I16, *I32, *I64, *I128, *Ptr, *V128, *Void;
  llvm::GlobalVariable* imageBaseVar = nullptr;
  llvm::GlobalVariable* dirtyVar = nullptr; // bbr_dirty[]
  ModuleCtx(llvm::LLVMContext& c, llvm::Module& m, const LowerOptions& o);
  llvm::FunctionCallee helper(const char* name, llvm::Type* ret, llvm::ArrayRef<llvm::Type*> args);
  llvm::FunctionCallee unitFn(uint64_t vaddr); // declaration of bbr_<vaddr>
};

struct Lower {
  ModuleCtx& m;
  llvm::LLVMContext& C;
  llvm::Module& M;
  llvm::IRBuilder<> B;
  const IR::IRListView& IR;
  LowerResult* res;
  uint64_t entryVaddr; // image-relative
  uint64_t entryRip;   // IRHeader::OriginalRIP (lift address)
  llvm::Type *I1, *I8, *I16, *I32, *I64, *I128, *Ptr, *V128;

  llvm::Function* F = nullptr;
  Value* S = nullptr;       // GuestState*
  Value* ImgBase = nullptr; // i64
  llvm::BasicBlock* allocaBB = nullptr;
  llvm::Instruction* allocaPt = nullptr; // allocas are inserted before this

  std::vector<Value*> val;               // node id -> value (i64 for GPR nodes, <2 x i64> for FPR nodes)
  std::vector<llvm::AllocaInst*> var;    // node id -> alloca, for RMWHandle/Allocate* nodes that ops update in place
  std::unordered_map<uint32_t, llvm::BasicBlock*> bbOf; // CodeBlock node id -> block
  llvm::AllocaInst *gpr[NumStaticGPR] = {}, *xmm[NumStaticFPR] = {};
  bool gprUsed[NumStaticGPR] = {}, gprWritten[NumStaticGPR] = {}, xmmUsed[NumStaticFPR] = {}, xmmWritten[NumStaticFPR] = {};
  bool flagsRead = false, flagsWritten = false;
  llvm::AllocaInst *fN = nullptr, *fZ = nullptr, *fC = nullptr, *fV = nullptr;
  FlagDesc fd;
  uint64_t dirty = 0;                       // static registers (bit g: GPR g, bit 18+x: XMM x) that differ from the context
  std::unordered_map<uint32_t, uint64_t> dirtyIn; // per block, from the dataflow in run()
  uint32_t curGuest = 0;       // offset of the current guest instruction from the entry (GuestOpcode)
  std::set<std::string> unsup; // unsupported op names of this unit

  const UnitMeta* meta = nullptr;
  std::vector<std::pair<uint64_t, uint32_t>> retBlocks; // (image vaddr of the return address, IR block id) of every call with a return block
  // ExitFunction node id -> (image vaddr, IR block id) of the cases of the switch the jump became
  std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> swCases;

  Lower(ModuleCtx& mc, const IR::IRListView& ir, LowerResult* r, uint64_t entry, const UnitMeta* meta = nullptr);
  void run();

  // ---- helpers
  static unsigned sz(const IR::IROp_Header* op) { return static_cast<unsigned>(op->Size); }
  static unsigned esz(const IR::IROp_Header* op) { return static_cast<unsigned>(op->ElementSize); }
  llvm::Constant* c64(uint64_t v) { return llvm::ConstantInt::get(I64, v); }
  llvm::Constant* c32(uint32_t v) { return llvm::ConstantInt::get(I32, v); }
  llvm::Constant* cN(unsigned bits, uint64_t v) { return llvm::ConstantInt::get(llvm::IntegerType::get(C, bits), v); }
  llvm::Type* iN(unsigned bits) { return llvm::IntegerType::get(C, bits); }
  llvm::AllocaInst* newAlloca(llvm::Type* t, const char* name);
  Value* gep(uint64_t off); // S + off
  Value* tr(Value* v, unsigned bits);    // i64 -> iN (no-op for 64)
  Value* zx(Value* v);                   // iN -> i64
  Value* sxN(Value* v, unsigned bits);   // sign-extend the low `bits` of i64 (or of a narrower value) to i64
  Value* toV(Value* i128);               // i128 -> <2 x i64>
  Value* fromV(Value* v);                // <2 x i64> -> i128
  Value* reloc(uint64_t liftAddr);       // image-relative addressing: ImgBase + (addr - liftBase)
  Value* A(W w);                         // operand value
  bool K(W w, uint64_t& k);              // operand is an InlineConstant
  bool addrConst(W w, uint64_t& liftAddr); // operand is a constant address (lift space)
  void def(Ref node, Value* v);
  IR::IROp_Header* opOf(W w) { return IR.GetOp<IR::IROp_Header>(w); }

  // ---- flags
  Value* flagN(); Value* flagZ(); Value* flagC(); Value* flagV();
  void flagsToGen();                      // descriptor -> four explicit i1 values
  void setSub(Value* a, Value* b, Value* r = nullptr);
  void setAdd(Value* a, Value* b, Value* r = nullptr);
  void setLogic(Value* r);
  void setGen(Value* n, Value* z, Value* c, Value* v);
  void materialize();                     // store dirty flags to the allocas
  Value* packNZCV();                      // i32: N<<31 | Z<<30 | C<<29 | V<<28
  void unpackNZCV(Value* w32);            // set Gen from a packed word
  Value* cond(IR::CondClass cc);          // i1
  Value* condOn(const FlagDesc& d, IR::CondClass cc);
  Value* fcmpIcmp(IR::CondClass cc, Value* a, Value* b); // float compare, the condition the JIT gets after fcmp
  void setFromFCmp(Value* a, Value* b);

  // ---- state
  void scan();
  void prologue();
  void flagsInit();
  void spill(bool flags);                 // store the dirty registers (and the flags when asked)
  void fill();
  Value* loadRegValue(unsigned cls, unsigned reg);

  // ---- control flow
  void emitBlockTerminatorsFix();
  void emitExit(Ref node, const IR::IROp_ExitFunction* op);
  void emitBreak(const IR::IROp_Break* op);
  llvm::BasicBlock* curBB() { return B.GetInsertBlock(); }
  llvm::BasicBlock* newBB(const char* n) { return llvm::BasicBlock::Create(C, n, F); }
  bool terminated() { return curBB()->hasTerminator(); }

  // ---- ops
  bool emitOp(Ref node, IR::IROp_Header* op);        // false: not lowered
  bool emitScalarOp(Ref node, IR::IROp_Header* op);
  bool emitExtraOp(Ref node, IR::IROp_Header* op);
  bool emitMemOp(Ref node, IR::IROp_Header* op);
  bool emitAtomicOp(Ref node, IR::IROp_Header* op);
  bool emitVectorOp(Ref node, IR::IROp_Header* op);  // lower_vector.cpp
  void trapUnsupported(Ref node, IR::IROp_Header* op);
  Value* memAddr(W addr, W off, IR::MemOffsetType t, uint8_t scale);
  Value* loadVal(Value* addrI64, unsigned bytes, bool fpr);
  void storeVal(Value* addrI64, Value* v, unsigned bytes, bool fpr);
  Value* ctxLoad(uint64_t off, unsigned bytes, bool fpr, Value* base = nullptr);
  void ctxStore(uint64_t off, Value* v, unsigned bytes, bool fpr, Value* base = nullptr);
  void setVar(W outNode, Value* v);                  // update an RMWHandle/Allocate node in place
  Value* sdivSafe(Value* a, Value* b, bool rem);
  Value* udivSafe(Value* a, Value* b, bool rem);
  llvm::AllocaInst* varFor(W w);
};

} // namespace bbr
