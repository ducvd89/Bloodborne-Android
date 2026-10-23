// LLVM side of bb-recomp: target machines, the pass pipeline (no interprocedural passes: they make big
// modules superlinear, see docs/STATIC_RECOMPILE_PLAN.md) and object emission.
#pragma once
#include <memory>
#include <string>

namespace llvm { class Module; class TargetMachine; class raw_pwrite_stream; }

struct TargetSpec {
  bool aarch64 = true;           // false: x86-64 host (ASM test runner, PC-side checks)
  int opt = 1;                   // 0, 1 or 2
  std::string cpu, features;     // empty: defaults (aarch64: generic + v8.2a + LSE; x86-64: the host's)
};

void initLlvmTargets();
std::unique_ptr<llvm::TargetMachine> makeTargetMachine(const TargetSpec& t, bool jit = false);
void setModuleTarget(llvm::Module& m, llvm::TargetMachine& tm);
// Per-function passes only (SROA/mem2reg, EarlyCSE, InstCombine, SimplifyCFG, GVN, DSE ...).
void optimizeModule(llvm::Module& m, llvm::TargetMachine& tm, int level);
// Machine code for the module as an ELF object.
bool emitObject(llvm::Module& m, llvm::TargetMachine& tm, std::string& out);
