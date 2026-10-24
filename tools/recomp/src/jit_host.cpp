#include "jit_host.h"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <cpuid.h>
#include <xmmintrin.h>
#include <sys/syscall.h>
#include <csignal>
#include <sys/time.h>
#include <unistd.h>

using namespace llvm;

sigjmp_buf g_halt;
volatile int g_haltKind = 0;
volatile uint64_t g_trapArg = 0;
Jit* g_jit = nullptr;
bool g_allowSyscalls = true;

void jitOnSignal(int sig) { g_haltKind = 4; g_trapArg = sig; siglongjmp(g_halt, 1); }

extern "C" {
void bbr_trap(CPUState* s, uint64_t kind) {
  g_trapArg = kind;
  if (kind & BBR_TRAP_UNSUPPORTED) g_haltKind = 3;
  else g_haltKind = *(const uint8_t*)s->rip == 0xF4 ? 1 : 2;
  siglongjmp(g_halt, 1);
}
struct CpuidRes { uint64_t lo, hi; };
CpuidRes bbr_cpuid(CPUState*, uint64_t fn, uint64_t leaf) {
  unsigned a, b, c, d;
  __cpuid_count((unsigned)fn, (unsigned)leaf, a, b, c, d); // the host's values: fine for the tests, the runtime returns Jaguar's
  return {(uint64_t)a | ((uint64_t)b << 32), (uint64_t)c | ((uint64_t)d << 32)};
}
uint64_t bbr_rdtsc() { return __builtin_ia32_rdtsc(); }
uint64_t bbr_processor_id() { return 0; }
// x86 control-state helpers on the host MXCSR (the real runtime maps these to FPCR)
void bbr_set_rounding(uint64_t mode, uint64_t mxcsr, uint64_t setDaz) {
  unsigned csr = _mm_getcsr();
  csr = (csr & ~0x6000u) | ((unsigned)(mode & 3) << 13);               // rounding control
  csr = (csr & ~0x8000u) | ((unsigned)((mode >> 2) & 1) << 15);        // flush to zero
  if (setDaz) csr = (csr & ~0x40u) | ((unsigned)(mxcsr & 0x40));       // denormals are zero
  _mm_setcsr(csr);
}
uint64_t bbr_xgetbv(uint64_t) { return 7; }
void bbr_syscall(CPUState* s) {
  if (!g_allowSyscalls) { g_haltKind = 2; siglongjmp(g_halt, 1); }
  // straight to the host kernel, like FEX's syscall handler does for a 64-bit guest
  auto& g = s->gregs; // FEX order: RAX 0, RCX 1, RDX 2, RBX 3, RSP 4, RBP 5, RSI 6, RDI 7, R8..R15 8..15
  g[0] = (uint64_t)syscall(g[0], g[7], g[6], g[2], g[10], g[8], g[9]);
  s->rip += 2; // the handler skips the 2-byte instruction (FEX Syscalls.cpp)
}
uint64_t bbr_get_rounding() { return (_mm_getcsr() >> 13) & 3; }
uint64_t bbr_push_rounding(uint64_t mode) { const unsigned old = _mm_getcsr(); _mm_setcsr((old & ~0x6000u) | ((unsigned)(mode & 3) << 13)); return old; }
void bbr_pop_rounding(uint64_t saved) { _mm_setcsr((unsigned)saved); }
}

Jit::Jit(const ElfImage& img, const LiftOptions& lo, const TargetSpec& ts) : lifter(img, lo), spec(ts) {
  initLlvmTargets();
  InitializeNativeTarget();
  auto jtmb = orc::JITTargetMachineBuilder::detectHost();
  if (!jtmb) { err = "detectHost"; consumeError(jtmb.takeError()); return; }
  jtmb->setCodeGenOptLevel(ts.opt <= 1 ? CodeGenOptLevel::Less : CodeGenOptLevel::Default);
  jtmb->setRelocationModel(Reloc::Static);
  jtmb->setCodeModel(CodeModel::Large);
  auto j = orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
  if (!j) { err = toString(j.takeError()); return; }
  jit = std::move(*j);
  auto& jd = jit->getMainJITDylib();
  orc::SymbolMap sm;
  auto sym = [&](const char* n, void* p) {
    sm[jit->mangleAndIntern(n)] = {orc::ExecutorAddr::fromPtr(p), JITSymbolFlags::Exported | JITSymbolFlags::Callable};
  };
  sym("bbr_trap", (void*)&bbr_trap); sym("bbr_dispatch", (void*)&Jit::dispatch); sym("bbr_dispatch_tail", (void*)&Jit::dispatchTail);
  sym("bbr_cpuid", (void*)&bbr_cpuid); sym("bbr_rdtsc", (void*)&bbr_rdtsc); sym("bbr_processor_id", (void*)&bbr_processor_id);
  sym("bbr_set_rounding", (void*)&bbr_set_rounding); sym("bbr_xgetbv", (void*)&bbr_xgetbv); sym("bbr_syscall", (void*)&bbr_syscall);
  sym("bbr_get_rounding", (void*)&bbr_get_rounding); sym("bbr_push_rounding", (void*)&bbr_push_rounding); sym("bbr_pop_rounding", (void*)&bbr_pop_rounding);
  cantFail(jd.define(orc::absoluteSymbols(std::move(sm))));
  auto gen = orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix());
  if (gen) jd.addGenerator(std::move(*gen)); else consumeError(gen.takeError());
  tm = makeTargetMachine(spec, true);
}

bool Jit::compile(uint64_t from, uint64_t hi) {
  callExits = 0;
  struct Sink : UnitSink {
    Jit& j; std::unique_ptr<LLVMContext> ctx; std::unique_ptr<Module> mod; std::unique_ptr<UnitLowerer> ul;
    std::vector<std::string> unsup; std::vector<uint64_t> entries;
    explicit Sink(Jit& jj) : j(jj), ctx(new LLVMContext()), mod(new Module("t", *ctx)) {
      setModuleTarget(*mod, *j.tm);
      LowerOptions lo;
      lo.liftBase = j.liftBase; lo.imageSize = j.imageSize; lo.fixedImageBase = true; lo.imageBase = j.liftBase;
      lo.aarch64 = false; lo.hiddenUnits = false; lo.calleeSaved = j.calleeSaved; lo.resumeEntries = j.resumeEntries; lo.storeFlagsAtCalls = true;
      lo.isKnownUnit = [](uint64_t) { return false; };
      ul.reset(new UnitLowerer(*ctx, *mod, lo));
    }
    void unit(uint64_t entry, bool, const void* ir, const UnitMeta& meta) override {
      LowerResult lr; ul->lower(entry, ir, &lr, &meta);
      j.callExits += lr.callExits;
      for (auto& u : lr.unsupported) unsup.push_back(u);
      entries.push_back(entry + j.liftBase);
    }
  } sink(*this);
  FuncResult fr;
  std::string irText;
  lifter.lift(Func{from - liftBase, hi - liftBase}, fr, nullptr, nullptr, getenv("BBR_DUMP_IR") ? &irText : nullptr, &sink);
  if (getenv("BBR_DUMP_IR")) fputs(irText.c_str(), stderr);
  if (fr.status == LiftStatus::Failed || fr.status == LiftStatus::Crashed || sink.entries.empty()) { liftFailure = fr.reason.empty() ? "no unit" : fr.reason; return false; }
  if (!sink.unsup.empty()) { for (auto& u : sink.unsup) unsupported.insert(u); return false; }
  if (getenv("BBR_DUMP_LL")) sink.mod->print(errs(), nullptr); // debugging: the lowered IR of every unit
  optimizeModule(*sink.mod, *tm, spec.opt);
  if (auto e = jit->addIRModule(orc::ThreadSafeModule(std::move(sink.mod), std::move(sink.ctx)))) { err = toString(std::move(e)); return false; }
  for (uint64_t en : sink.entries) known.insert(en);
  return true;
}

Jit::Fn Jit::find(uint64_t addr) {
  auto it = fns.find(addr);
  if (it != fns.end()) return it->second;
  if (!known.count(addr)) return nullptr;
  auto r = jit->lookup(unitName(addr - liftBase));
  if (!r) { err = toString(r.takeError()); return nullptr; }
  Fn f = r->toPtr<Fn>();
  fns[addr] = f;
  return f;
}

void Jit::dispatch(CPUState* s, uint64_t target) {
  Jit& j = *g_jit;
  // A run timer (difftest) escapes with siglongjmp: it must not fire while we allocate or compile. SIGALRM stays
  // pending while blocked and fires when the mask is restored, just before the unit runs.
  sigset_t blk, old;
  sigemptyset(&blk); sigaddset(&blk, SIGALRM);
  sigprocmask(SIG_BLOCK, &blk, &old);
  Fn f = j.find(target);
  if (!f && j.rangeEnd) {
    if (const uint64_t hi = j.rangeEnd(target)) {
      const bool ok = j.compile(target, hi);
      if (ok) f = j.find(target);
      else if (!j.unsupported.empty()) { g_haltKind = 3; siglongjmp(g_halt, 1); }
    }
  }
  if (!f) { g_haltKind = 5; g_trapArg = target; siglongjmp(g_halt, 1); }
  sigprocmask(SIG_SETMASK, &old, nullptr);
  f(s);
}
void Jit::dispatchTail(CPUState* s) { dispatch(s, s->rip); }
