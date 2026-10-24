// In-process host runner: units lowered for x86-64, compiled with LLVM ORC, called with a FEX CPUState.
// Shared by bb-recomp-asmtest (FEX's unit tests) and bb-recomp-difftest (game functions against native execution).
// The bbr_* helpers the lowered code calls are implemented here for the host (the real runtime maps them to
// guest_cpu.h): bbr_trap ends the run with siglongjmp(g_halt).
#pragma once
#include "codegen.h"
#include "lifter.h"
#include "lower_llvm.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>

#include <FEXCore/Core/CoreState.h>

#include <csetjmp>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

using FEXCore::Core::CPUState;

extern sigjmp_buf g_halt;
extern volatile int g_haltKind;   // 1 hlt, 2 other Break, 3 unsupported op reached, 4 signal, 5 bad dispatch
extern volatile uint64_t g_trapArg;
void jitOnSignal(int sig);        // signal handler: records the signal and leaves through g_halt

class Jit {
 public:
  Jit(const ElfImage& img, const LiftOptions& lo, const TargetSpec& ts);
  std::string err;

  // Lifts [from, hi) as units and adds them to the JIT. False: nothing lifted or an op is unsupported.
  bool compile(uint64_t from, uint64_t hi);
  using Fn = void (*)(CPUState*);
  Fn find(uint64_t addr);             // the compiled unit entered at addr, or nullptr
  static void dispatch(CPUState* s, uint64_t target);
  static void dispatchTail(CPUState* s);
  static void unwound(CPUState* s);

  // Where a dispatch to `target` may be compiled from: the end of the code range that holds it (0: not compilable).
  std::function<uint64_t(uint64_t)> rangeEnd;
  uint64_t liftBase = 0, imageSize = 0; // lifter base of the guest addresses; image span (constants inside are addresses)
  uint32_t callExits = 0;               // call exits (ExitFunction/Call) in the units of the last compile
  bool resumeEntries = false;           // lower with resume entries (call-return blocks re-enterable with the resume flag)
  bool calleeSaved = false;             // false: reload everything after calls (exact); true: the game's setting

  Lifter lifter;
  TargetSpec spec;
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::unique_ptr<llvm::TargetMachine> tm;
  std::set<uint64_t> known;
  std::map<uint64_t, Fn> fns;
  std::set<std::string> unsupported;
  std::string liftFailure;
};
extern Jit* g_jit;
extern bool g_allowSyscalls;       // false: bbr_syscall ends the run instead of calling the kernel
