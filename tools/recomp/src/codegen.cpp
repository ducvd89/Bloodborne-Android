#include "codegen.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdio>
#include <mutex>

using namespace llvm;

void initLlvmTargets() {
  static std::once_flag once;
  std::call_once(once, [] {
    LLVMInitializeAArch64TargetInfo(); LLVMInitializeAArch64Target(); LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeX86TargetInfo(); LLVMInitializeX86Target(); LLVMInitializeX86TargetMC(); LLVMInitializeX86AsmPrinter();
    // LLVM 23 x86-fixup-bw-insts turns `movb 552(%rcx),%cl` into `movzbl` although the upper bits of rcx are live
    // (the byte-merge `(x & -256) | byte` loses x's upper bits). Only the x86 oracle path is affected.
    static const char* argv[] = {"bb-recomp", "-fixup-byte-word-insts=0"};
    cl::ParseCommandLineOptions(2, argv);
  });
}

static Triple tripleOf(const TargetSpec& t) { return Triple(t.aarch64 ? "aarch64-unknown-linux-android30" : "x86_64-unknown-linux-gnu"); }

std::unique_ptr<TargetMachine> makeTargetMachine(const TargetSpec& t, bool jit) {
  initLlvmTargets();
  Triple tr = tripleOf(t);
  std::string err;
  const Target* tg = TargetRegistry::lookupTarget(tr, err);
  if (!tg) { fprintf(stderr, "bb-recomp: no LLVM target for %s: %s\n", tr.str().c_str(), err.c_str()); return nullptr; }
  std::string cpu = t.cpu, feat = t.features;
  if (t.aarch64) {
    if (cpu.empty()) cpu = "generic";
    if (feat.empty()) feat = "+v8.2a,+lse,+crc,+fp-armv8,+neon";
  } else {
    if (cpu.empty()) cpu = sys::getHostCPUName().str();
    if (feat.empty()) {
      SubtargetFeatures f;
      for (auto& e : sys::getHostCPUFeatures()) f.AddFeature(e.first(), e.second);
      feat = f.getString();
    }
  }
  TargetOptions opts;
  opts.FunctionSections = false;
  CodeGenOptLevel ol = t.opt <= 0 ? CodeGenOptLevel::None : t.opt == 1 ? CodeGenOptLevel::Less : CodeGenOptLevel::Default;
  auto reloc = jit ? Reloc::Static : Reloc::PIC_;
  return std::unique_ptr<TargetMachine>(tg->createTargetMachine(tr, cpu, feat, opts, reloc, std::nullopt, ol, jit));
}

void setModuleTarget(Module& m, TargetMachine& tm) {
  m.setTargetTriple(tm.getTargetTriple());
  m.setDataLayout(tm.createDataLayout());
}

void optimizeModule(Module& m, TargetMachine& tm, int level) {
  if (level <= 0) return;
  PassBuilder pb(&tm);
  LoopAnalysisManager lam; FunctionAnalysisManager fam; CGSCCAnalysisManager cgam; ModuleAnalysisManager mam;
  pb.registerModuleAnalyses(mam); pb.registerCGSCCAnalyses(cgam); pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam); pb.crossRegisterProxies(lam, fam, cgam, mam);
  // O1: promote registers/flags to SSA, fold, and clean up. O2 adds GVN, DSE and a second round.
  const char* o1 = "function(sroa<modify-cfg>,early-cse<memssa>,simplifycfg,instcombine<no-verify-fixpoint>,dse,adce,simplifycfg)";
  const char* o2 = "function(sroa<modify-cfg>,early-cse<memssa>,simplifycfg,instcombine<no-verify-fixpoint>,correlated-propagation,reassociate,gvn,"
                   "dse,instcombine<no-verify-fixpoint>,simplifycfg,adce,instcombine<no-verify-fixpoint>)";
  ModulePassManager mpm;
  if (auto e = pb.parsePassPipeline(mpm, level == 1 ? o1 : o2)) {
    fprintf(stderr, "bb-recomp: bad pass pipeline: %s\n", toString(std::move(e)).c_str());
    return;
  }
  mpm.run(m, mam);
}

bool emitObject(Module& m, TargetMachine& tm, std::string& out) {
  out.clear();
  SmallVector<char, 0> buf;
  raw_svector_ostream os(buf);
  legacy::PassManager pm;
  if (tm.addPassesToEmitFile(pm, os, nullptr, CodeGenFileType::ObjectFile)) {
    fprintf(stderr, "bb-recomp: target cannot emit object files\n");
    return false;
  }
  pm.run(m);
  out.assign(buf.data(), buf.size());
  return true;
}
