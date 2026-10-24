// FEX front end driver. The IR for one function is produced exactly as FEXCore's JIT path does
// (ContextImpl::GenerateIR, FEXCore/Source/Interface/Core/Core.cpp:521): Decoder::DecodeLoop ->
// OpDispatchBuilder per instruction -> Finalize -> IR passes. It is repeated here because
// GenerateIR runs the full PassManager, whose last pass is register allocation: LLVM wants the SSA
// IR before RA, so only the two passes that matter are run (x87 stack optimisation and dead flag
// calculation elimination = FEX's "RedundantFlagCalculationElimination.cpp").
#include <algorithm>
#include <array>
#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "Interface/Core/Frontend.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/OpcodeDispatcher.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"
#include "Interface/IR/Passes.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>

#include "lifter.h"
#include "jumptable.h"

using namespace FEXCore;

// The decoder keeps the bounds of the multiblock it may grow into (SectionMin/MaxAddress) private and
// only sets them from a code cache; a function-bounded unit needs them set per function. Explicit
// template instantiation is exempt from access checks, so FEX stays unpatched.
namespace {
using SectionPtr = uint64_t Frontend::Decoder::*;
struct MinTag { friend SectionPtr sectionMin(MinTag); };
struct MaxTag { friend SectionPtr sectionMax(MaxTag); };
template <SectionPtr M> struct RobMin { friend SectionPtr sectionMin(MinTag) { return M; } };
template <SectionPtr M> struct RobMax { friend SectionPtr sectionMax(MaxTag) { return M; } };
template struct RobMin<&Frontend::Decoder::SectionMinAddress>;
template struct RobMax<&Frontend::Decoder::SectionMaxAddress>;
void setSection(Frontend::Decoder& d, uint64_t lo, uint64_t hi) { d.*sectionMin(MinTag{}) = lo; d.*sectionMax(MaxTag{}) = hi; }

// The decoder's worklist of block entries: switch case targets are added to it before DecodeLoop, so they
// become blocks of the same multiblock unit (nothing else in the code branches to them).
using TodoPtr = fextl::set<uint64_t> Frontend::Decoder::*;
struct TodoTag { friend TodoPtr blocksToDecode(TodoTag); };
template <TodoPtr M> struct RobTodo { friend TodoPtr blocksToDecode(TodoTag) { return M; } };
template struct RobTodo<&Frontend::Decoder::BlocksToDecode>;
void addEntry(Frontend::Decoder& d, uint64_t pc) { (d.*blocksToDecode(TodoTag{})).insert(pc); }
} // namespace

const char* statusName(LiftStatus s) {
  switch (s) {
  case LiftStatus::Clean: return "clean";
  case LiftStatus::Partial: return "partial";
  case LiftStatus::Failed: return "failed";
  case LiftStatus::Crashed: return "crashed";
  }
  return "?";
}

namespace {
// ---------------------------------------------------------------- crash containment ----------
// FEX asserts through LogMan::Throw (which then traps) and may fault on input it does not expect.
// Both are turned into a longjmp back to Lifter::lift, which marks the function Crashed and rebuilds
// the unit (the old one is leaked: it may be mid-update).
thread_local sigjmp_buf* tl_jmp = nullptr;
thread_local std::string* tl_log = nullptr;
thread_local std::string tl_crash;
constexpr size_t LogCap = 4096;

[[noreturn]] void bail(int code) { siglongjmp(*tl_jmp, code); }
void onSignal(int sig) { if (tl_jmp) { tl_crash = std::string("signal ") + strsignal(sig); bail(sig); } _exit(128 + sig); }
void onAssert(const char* msg) { tl_crash = std::string("assert: ") + msg; if (tl_jmp) bail(100); fprintf(stderr, "%s\n", msg); abort(); }
thread_local std::string tl_valErr; // first IR validation error text
void onMsg(LogMan::DebugLevels, const char* msg) {
  if (const char* e = strstr(msg, "Errors:\n")) { if (tl_valErr.empty()) tl_valErr = std::string(e + 8).substr(0, 160); return; }
  if (tl_log && tl_log->size() < LogCap) { tl_log->append(msg); tl_log->push_back('\n'); }
}

void installCrashHandlers() {
#ifndef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction sa {}; sa.sa_handler = onSignal; sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) sigaction(s, &sa, nullptr);
    LogMan::Throw::InstallHandler(onAssert);
    LogMan::Msg::InstallHandler(onMsg);
  });
  // Every worker needs its own alternate stack: a stack overflow in FEX is also a crash to survive.
  thread_local std::unique_ptr<char[]> altStack;
  if (!altStack) {
    altStack.reset(new char[1 << 16]);
    stack_t ss {}; ss.ss_sp = altStack.get(); ss.ss_size = 1 << 16; sigaltstack(&ss, nullptr);
  }
#else
  LogMan::Throw::InstallHandler(onAssert);
  LogMan::Msg::InstallHandler(onMsg);
#endif
}

// ---------------------------------------------------------------- host + config ---------------
HostFeatures makeHost(const std::string& profile) {
  HostFeatures f {};
  f.DCacheLineLog2 = 4;
  f.SupportsAES = f.SupportsCRC = f.SupportsAtomics = f.SupportsSHA = f.SupportsPMULL_128Bit = 1;
  f.SupportsAVX = 1; // AVX1 through FEX's AVX128 path on NEON (no SVE256): the game is AVX1/Jaguar
  f.SupportsAES256 = 1;
  f.SupportsFCMA = 1; // Armv8.3
  f.SupportsRCPC = 1; // Armv8.3 LRCPC
  f.HostType = HostFeatures::HostTypeEnum::Linux;
  f.Supports3DNow = 1;
  f.SupportsSSE4a = 1; // Jaguar
  f.CPUMIDRs.assign(8, 0x410fd4e0); // Cortex-X3
  if (profile == "armv8.2") return f;
  f.SupportsFlagM = 1; f.SupportsTSOImm9 = 1; f.SupportsRCPC = 1; // Armv8.4
  if (profile == "armv8.4") return f;
  f.SupportsFlagM2 = 1; f.SupportsFRINTTS = 1; // Armv8.5 / Armv9.0 (8 Gen 2: X3, A715, A510)
  if (profile == "armv9") return f;
  fprintf(stderr, "unknown host profile '%s' (armv8.2, armv8.4, armv9)\n", profile.c_str());
  exit(2);
}

void setConfig(const LiftOptions& o) {
  static std::once_flag once;
  std::call_once(once, [&] {
    Config::Initialize();
    Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
    Config::Set(Config::CONFIG_MULTIBLOCK, o.multiblock ? "1" : "0");
    Config::Set(Config::CONFIG_TSOENABLED, o.tso ? "1" : "0");
    Config::Set(Config::CONFIG_VECTORTSOENABLED, o.tso ? "1" : "0");
    Config::Set(Config::CONFIG_MEMCPYSETTSOENABLED, o.tso ? "1" : "0");
    Config::Set(Config::CONFIG_HALFBARRIERTSOENABLED, "0");
    Config::Set(Config::CONFIG_X87REDUCEDPRECISION, "1");
    Config::Set(Config::CONFIG_DISABLETELEMETRY, "1");
    Config::Set(Config::CONFIG_MAXINST, std::to_string(o.maxInst));
  });
}

// ---------------------------------------------------------------- syscall handler stub --------
// The decoder asks which guest range is executable; the answer is the function being lifted, so a
// multiblock unit cannot wander into the next function (a branch past the end ends the unit there).
struct Range { uint64_t lo = 0, hi = 0; };
thread_local Range tl_range;
class Handler final : public HLE::SyscallHandler {
 public:
  void HandleSyscall(Core::CpuStateFrame*) override {}
  HLE::ExecutableRangeInfo QueryGuestExecutableRange(Core::InternalThreadState*, uint64_t a) override {
    if (a < tl_range.lo || a >= tl_range.hi) return {0, 0, false};
    return {tl_range.lo, tl_range.hi - tl_range.lo, false};
  }
  std::optional<ExecutableFileSectionInfo> LookupExecutableFileSection(Core::InternalThreadState*, uint64_t) override { return std::nullopt; }
};
Handler g_handler;

std::string hexBytes(uint64_t addr, size_t n, uint64_t hi) {
  std::string s; char b[4];
  for (size_t i = 0; i < n && addr + i < hi; ++i) { snprintf(b, sizeof b, "%02x", *(const uint8_t*)(addr + i)); s += b; if (i + 1 < n) s += ' '; }
  return s;
}

// A run of 4-byte entries that look like switch table offsets (small signed values): PS4 code keeps its
// jump tables in .text, inside the function's FDE, right after the code. Returns the run length in bytes
// (0 when pc does not start such a run; at least two entries are needed).
uint64_t dataRun(uint64_t pc, uint64_t end, uint64_t hi) {
  uint64_t n = 0;
  while (pc + n + 4 <= std::min(end, hi)) {
    int32_t v; memcpy(&v, (const void*)(pc + n), 4);
    if (v < -0x400000 || v > 0x400000) break;
    n += 4;
  }
  return n >= 8 ? n : 0;
}

// Padding between functions and after noreturn calls: int3, nop forms (66/2e prefixes), ud2.
bool isPadding(const uint8_t* p, size_t n) {
  size_t i = 0;
  while (i < n && (p[i] == 0x66 || p[i] == 0x2e || p[i] == 0x3e || p[i] == 0xf3 || p[i] == 0xf2)) ++i;
  if (i >= n) return false;
  if (p[i] == 0xcc || p[i] == 0x90) return true;
  return p[i] == 0x0f && i + 1 < n && (p[i + 1] == 0x1f || p[i + 1] == 0x0b);
}


// ---------------------------------------------------------------- jump table analysis input ----
jt::Op jtOperand(const X86Tables::DecodedOperand& o) {
  using T = X86Tables::DecodedOperand::OpType;
  jt::Op r;
  switch (o.Type) {
  case T::GPR: r.k = jt::Op::Reg; r.reg = o.Data.GPR.GPR & 15; break;
  case T::GPRDirect: r.k = jt::Op::Mem; r.base = o.Data.GPR.GPR & 15; break;
  case T::GPRIndirect: r.k = jt::Op::Mem; r.base = o.Data.GPRIndirect.GPR & 15; r.imm = o.Data.GPRIndirect.Displacement; break;
  case T::SIB:
    r.k = jt::Op::Mem; r.imm = o.Data.SIB.Offset; r.scale = o.Data.SIB.Scale;
    r.base = o.Data.SIB.Base < 16 ? o.Data.SIB.Base : -1; r.index = o.Data.SIB.Index < 16 ? o.Data.SIB.Index : -1; break;
  case T::RIPRelative: r.k = jt::Op::Mem; r.rip = true; r.imm = o.Data.RIPLiteral.Value; break;
  case T::Literal: {
    r.k = jt::Op::Imm; const uint64_t v = o.Data.Literal.Value;
    switch (o.Data.Literal.Size) { case 1: r.imm = (int8_t)v; break; case 2: r.imm = (int16_t)v; break; case 4: r.imm = (int32_t)v; break; default: r.imm = (int64_t)v; }
    break;
  }
  default: break;
  }
  return r;
}

jt::Ins jtInst(const X86Tables::DecodedInst& d) {
  jt::Ins r;
  r.pc = d.PC; r.size = d.InstSize;
  const char* n = d.TableInfo && d.TableInfo->Name ? d.TableInfo->Name : "";
  auto is = [&](const char* s) { return !strcmp(n, s); };
  auto starts = [&](const char* s) { return !strncmp(n, s, strlen(s)); };
  // Group 5 (jmp/call r/m): FEX fills Dest/Src[0] with the opcode extension; the operand is the ModRM r/m
  auto rmOperand = [&] {
    if ((d.ModRM >> 6) == 3) { r.src.k = jt::Op::Reg; r.src.reg = (d.ModRM & 7) | ((d.Flags & X86Tables::DecodeFlags::FLAG_REX_XGPR_B) ? 8 : 0); }
    else r.src = jtOperand(!d.Src[0].IsGPR() ? d.Src[0] : d.Dest);
  };
  r.dst = jtOperand(d.Dest); r.src = jtOperand(d.Src[0]);
  if (d.Src[1].Type == X86Tables::DecodedOperand::OpType::Literal) r.src = jtOperand(d.Src[1]); // group 1 (83 /7 ib): Src[0] is the opcode extension
  if (d.TableInfo && (d.TableInfo->Flags & X86Tables::InstFlags::FLAGS_XMM_FLAGS)) {
    // SSE/AVX/x87: register operands are XMM (or x87) registers unless the table says the destination is a GPR
    if (!(d.TableInfo->Flags & X86Tables::InstFlags::FLAGS_SF_DST_GPR) && r.dst.k == jt::Op::Reg) r.dst = {};
    if (!(d.TableInfo->Flags & X86Tables::InstFlags::FLAGS_SF_SRC_GPR) && r.src.k == jt::Op::Reg) r.src = {};
  }
  if (is("CMP")) r.kind = jt::K::Cmp;
  else if (is("TEST") || is("PUSH") || is("NOP") || starts("PREFETCH")) r.kind = jt::K::NoWrite;
  else if (is("LEA")) r.kind = jt::K::Lea;
  else if (is("MOV")) r.kind = jt::K::Mov;
  else if (is("MOVSXD")) r.kind = jt::K::Movsxd;
  else if (is("MOVZX")) r.kind = jt::K::Movzx;
  else if (is("ADD")) r.kind = jt::K::Add;
  else if (is("AND")) r.kind = jt::K::And;
  else if (is("RET")) r.kind = jt::K::Ret;
  else if (is("CALL")) {
    r.kind = d.OP == 0xE8 ? jt::K::Call : jt::K::CallInd;
    if (r.kind == jt::K::CallInd) rmOperand();
  }
  else if (is("JMP")) {
    if (d.OP == 0xE9 || d.OP == 0xEB) r.kind = jt::K::Jmp;
    else {
      // Group 5 /4: the operand is the ModRM r/m (FEX fills Dest/Src[0] with the opcode extension)
      r.kind = jt::K::JmpInd;
      rmOperand();
    }
  }
  else if (n[0] == 'J' && (d.TableInfo->Flags & X86Tables::InstFlags::FLAGS_SETS_RIP)) {
    r.kind = jt::K::Jcc;
    r.cc = (d.OP >= 0x70 && d.OP <= 0x8f) ? d.OP & 15 : 0xff; // 70-7f: rel8, 0f 80-8f: rel32 (FEX keeps the low byte)
  }
  else if (is("MUL") || is("DIV") || is("IDIV") || is("CDQ") || is("CQO") || is("CBW") || is("CWD") || is("CWDE") || is("CDQE") || is("XCHG") ||
           is("XADD") || starts("CMPXCHG") || is("CPUID") || starts("RDTSC") || is("SYSCALL") || starts("MOVS") || starts("STOS") ||
           starts("LODS") || starts("SCAS") || starts("CMPS") || is("LEAVE") || is("ENTER") || is("SAHF") || is("LAHF") || is("XLAT"))
    r.kind = (is("MOVSXD") || starts("MOVSX") || is("MOVSS") || is("MOVSD") || is("MOVSLDUP") || is("MOVSHDUP")) ? jt::K::Other : jt::K::Clobber;
  return r;
}

constexpr size_t NumOps = IR::OP_LAST;
constexpr size_t SizeBuckets = 9; // 0 1 2 4 8 16 32 10(f80) other
size_t sizeBucket(IR::OpSize s) {
  switch (static_cast<uint8_t>(s)) { case 0: return 0; case 1: return 1; case 2: return 2; case 4: return 3; case 8: return 4;
                                      case 16: return 5; case 32: return 6; case 10: return 7; default: return 8; }
}
constexpr unsigned bucketBits[SizeBuckets] = {0, 8, 16, 32, 64, 128, 256, 80, 999};
} // namespace

size_t Lifter::numOps() { return NumOps; }
const char* Lifter::opName(size_t op) { return IR::GetName(static_cast<IR::IROps>(op)).data(); }
bool Lifter::isPseudoOp(size_t op) { return op == IR::OP_DUMMY || op == IR::OP_IRHEADER; }
bool Lifter::isStructuralOp(size_t op) {
  const std::string_view n = IR::GetName(static_cast<IR::IROps>(op));
  return n.starts_with("Inline") || n == "BeginBlock" || n == "EndBlock" || n == "CodeBlock" || n == "GuestOpcode" || n == "Dummy" || n == "IRHeader";
}
size_t Lifter::sizeHistSlots() { return NumOps * SizeBuckets * SizeBuckets; }
void Lifter::sizeSlotDecode(size_t slot, size_t& op, unsigned& sb, unsigned& eb) {
  eb = bucketBits[slot % SizeBuckets]; slot /= SizeBuckets;
  sb = bucketBits[slot % SizeBuckets]; op = slot / SizeBuckets;
}

// ---------------------------------------------------------------- the unit -------------------
enum class StepKind { Pad, Inst, Data, Invalid };
struct Lifter::Impl {
  const ElfImage& img;
  LiftOptions opt;
  Context::ContextImpl* ctx = nullptr;
  Core::InternalThreadState* th = nullptr;
  std::vector<fextl::unique_ptr<IR::Pass>> passes;

  Impl(const ElfImage& i, const LiftOptions& o) : img(i), opt(o) { build(); }

  void build() {
    // FEXCore's config is a process-wide store whose reads convert the stored string in place (Config::MetaLayer::GetConv<bool> emplaces
    // the variant): two threads creating a context at once free the same string (glibc "free(): invalid pointer", found with ASan:
    // bad-free in GetConv from the ContextImpl constructor). Every thread of the tool builds its own Lifter, so building is serialised.
    static std::mutex buildLock;
    std::lock_guard<std::mutex> guard(buildLock);
    setConfig(opt);
    ctx = static_cast<Context::ContextImpl*>(Context::Context::CreateNewContext(makeHost(opt.host)).release());
    ctx->SetSyscallHandler(&g_handler);
    // What ContextImpl::CreateThread does, minus the JIT backend: a thread state with the decoder and
    // the dispatcher builder. Nothing here can execute guest code.
    th = new Core::InternalThreadState {.CTX = ctx};
    auto& s = th->CurrentFrame->State;
    s.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = s.private_gdt; // flat 64-bit code segment
    s.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = s.private_gdt;
    s.cs_idx = Core::CPUState::DEFAULT_USER_CS << 3;
    Core::CPUState::GetSegmentFromIndex(s, s.cs_idx)->L = 1;
    th->CurrentFrame->Thread = th;
    th->OpDispatcher = fextl::make_unique<IR::OpDispatchBuilder>(ctx, th);
    th->OpDispatcher->SetMultiblock(ctx->Config.Multiblock);
    th->FrontendDecoder = fextl::make_unique<Frontend::Decoder>(th);
    passes.clear();
    if (opt.passes) {
      passes.push_back(IR::CreateX87StackOptimizationPass(ctx->HostFeatures, IR::OpSize::i64Bit));
      passes.push_back(IR::CreateDeadFlagCalculationEliminination());
    }
    if (opt.validate) passes.push_back(IR::Validation::CreateIRValidation());
  }

  // Stand-in for ContextImpl::GenerateIR; returns false when no instruction could be lifted.
  struct Gen {
    uint32_t insts = 0; std::string reason; bool dispatchFail = false; std::vector<std::pair<uint64_t, uint64_t>> cover; bool pastEnd = false;
    std::vector<jt::Jump> jumps;    // the indirect jumps of the unit and what became of them
    std::vector<uint64_t> retSites; // return addresses (image vaddrs) of calls to returns-twice targets (setjmp)
    std::vector<uint64_t> leaTargets; // rip-relative lea targets (collectTargets)
    std::vector<uint64_t> outTargets; // direct call/jmp/jcc targets outside the unit's function range (image vaddrs; collectTargets)
  };
  bool generate(uint64_t rip, uint64_t lo, uint64_t hi, Gen& g);
  void decode(uint64_t rip, uint64_t lo, uint64_t hi, const std::set<uint64_t>& seeds);
  std::vector<jt::Jump> analyseJumps(uint64_t lo, uint64_t hi, Gen* g);
  std::vector<std::pair<uint64_t, uint64_t>> tables; // jump table bytes found so far in this function (host addresses)
  uint64_t imgLo = 0, imgHi = 0;                     // mapped image range (host addresses)
  bool onlyBounded = false;                          // gap units: only trust tables whose bound was found (the unit may be junk)
  // Jump tables resolved by earlier units of the current function (pc -> the jump): a secondary unit entered inside a loop
  // often holds the table jump without the lea that loads the table base (it sits before the loop, in the entry unit), so it
  // reuses the entry unit's result (lift() clears this per function).
  std::map<uint64_t, jt::Jump> fnJumps;
  fextl::set<uint64_t> external;                     // branch targets the decoder left out of the multiblock (more than 4 pages ahead, or behind the entry)
  using Step = StepKind;
  Step step(uint64_t pc, uint64_t lo, uint64_t hi, uint64_t end, uint64_t& len, std::string* why);
  uint64_t findEntry(Gen& g, uint64_t lo, uint64_t hi, uint64_t from);
  void gaps(uint64_t lo, uint64_t hi, Gen& g, FuncResult& r);
};

void Lifter::Impl::decode(uint64_t rip, uint64_t lo, uint64_t hi, const std::set<uint64_t>& seeds) {
  auto& Dec = *th->FrontendDecoder;
  Dec.ResetExecutableRangeCache();
  Dec.SetupDecodeInstructionsAtEntry(th, rip, opt.maxInst);
  setSection(Dec, lo, hi); // multiblock membership: [entry, function end)
  for (uint64_t s : seeds) addEntry(Dec, s);
  external.clear();
  Dec.SetExternalBranches(&external);
  Dec.DecodeLoop(reinterpret_cast<const uint8_t*>(rip));
  Dec.SetExternalBranches(nullptr);
}

// Recognises switch tables among the indirect jumps of the current decode (lo..hi: the unit's range).
std::vector<jt::Jump> Lifter::Impl::analyseJumps(uint64_t lo, uint64_t hi, Gen* g) {
  std::vector<jt::Ins> ins;
  for (auto& b : th->FrontendDecoder->GetDecodedBlockInfo()->Blocks)
    for (uint32_t i = 0; i < b.NumInstructions; ++i) ins.push_back(jtInst(b.DecodedInstructions[i]));
  std::sort(ins.begin(), ins.end(), [](const jt::Ins& a, const jt::Ins& b) { return a.pc < b.pc; });
  ins.erase(std::unique(ins.begin(), ins.end(), [](const jt::Ins& a, const jt::Ins& b) { return a.pc == b.pc; }), ins.end());
  if (g && opt.collectTargets)
    for (auto& i : ins) {
      if (i.kind == jt::K::Lea && i.src.k == jt::Op::Mem && i.src.rip) {
        const uint64_t t = i.pc + i.size + i.src.imm;
        if (t >= imgLo && t < imgHi) g->leaTargets.push_back(t - img.base);
      }
    }
  if (g && opt.collectTargets)
    for (auto& i : ins)
      if ((i.kind == jt::K::Call || i.kind == jt::K::Jmp || i.kind == jt::K::Jcc) && i.src.k == jt::Op::Imm) {
        const uint64_t t = i.pc + i.size + i.src.imm;
        if ((t < lo || t >= hi) && t >= imgLo && t < imgHi) g->outTargets.push_back(t - img.base);
      }
  if (g && opt.collectTargets) {
    // Exits of this decode to addresses inside the function that it did not decode as blocks: a block that runs into the unit's
    // bound (a case body falling through into the loop head of the entry unit), or a jmp/jcc there. The lowering makes them
    // constant tail jumps to an address that is no unit entry, which the runtime can only run on the fallback CPU to the end of
    // the guest stack (the thread's whole loop when it is near its root). Collected like out-of-function targets, they become
    // entry units of the function (discovery: "call/jmp targets inside functions that no unit started at").
    std::set<uint64_t> have;
    for (auto& b : th->FrontendDecoder->GetDecodedBlockInfo()->Blocks) have.insert(b.Entry);
    auto exitTo = [&](uint64_t t) { if (t >= lo && t < hi && !have.count(t)) g->outTargets.push_back(t - img.base); };
    for (auto& b : th->FrontendDecoder->GetDecodedBlockInfo()->Blocks) {
      if (!b.NumInstructions) continue;
      const jt::Ins last = jtInst(b.DecodedInstructions[b.NumInstructions - 1]);
      if (last.kind != jt::K::Jmp && last.kind != jt::K::JmpInd && last.kind != jt::K::Ret) exitTo(last.pc + last.size);  // falls through
      if ((last.kind == jt::K::Jmp || last.kind == jt::K::Jcc) && last.src.k == jt::Op::Imm) exitTo(last.pc + last.size + last.src.imm);
    }
  }
  if (g && opt.retTwiceTargets)
    for (auto& i : ins) {
      uint64_t site = 0;
      if (i.kind == jt::K::Call && i.src.k == jt::Op::Imm && opt.retTwiceTargets->count(i.pc + i.size + i.src.imm - img.base)) site = i.pc + i.size;
      else if (i.kind == jt::K::CallInd && i.src.k == jt::Op::Mem && i.src.rip && opt.retTwiceSlots && opt.retTwiceSlots->count(i.pc + i.size + i.src.imm - img.base)) site = i.pc + i.size;
      if (site) g->retSites.push_back(site - img.base);
    }
  jt::Env env;
  env.lo = lo; env.hi = hi;
  env.readS32 = [this](uint64_t a, int32_t& v) { if (a < imgLo || a + 4 > imgHi) return false; memcpy(&v, (const void*)a, 4); return true; };
  auto r = jt::analyze(ins, env);
  for (auto& j : r) {
    if (j.why == jt::Why::Resolved && !j.targets.empty()) { fnJumps[j.pc] = j; continue; }
    auto c = fnJumps.find(j.pc);
    if (c == fnJumps.end()) continue;
    jt::Jump k = c->second;
    k.targets.erase(std::remove_if(k.targets.begin(), k.targets.end(), [&](uint64_t t) { return t < lo || t >= hi; }), k.targets.end());
    if (!k.targets.empty()) j = k;
  }
  if (getenv("BBR_DEBUG_JUMPS")) {
    // Debugging aid: the instructions before every indirect jump, as the analysis sees them.
    std::map<uint64_t, const X86Tables::DecodedInst*> raw;
    for (auto& b : th->FrontendDecoder->GetDecodedBlockInfo()->Blocks) for (uint32_t i = 0; i < b.NumInstructions; ++i) raw[b.DecodedInstructions[i].PC] = &b.DecodedInstructions[i];
    auto opText = [](const jt::Op& o) { char b[96]; switch (o.k) { case jt::Op::Reg: snprintf(b, sizeof b, "r%u", o.reg); break; case jt::Op::Imm: snprintf(b, sizeof b, "#%ld", (long)o.imm); break;
      case jt::Op::Mem: snprintf(b, sizeof b, "[%s b%d i%d*%u %+ld]", o.rip ? "rip" : "", o.base, o.index, o.scale, (long)o.imm); break; default: snprintf(b, sizeof b, "-"); } return std::string(b); };
    for (auto& j : r) {
      fprintf(stderr, "jump @%#lx: %s targets=%zu\n", (unsigned long)(j.pc - img.base), jt::whyName(j.why), j.targets.size());
      auto it = std::lower_bound(ins.begin(), ins.end(), j.pc, [](const jt::Ins& a, uint64_t pc) { return a.pc < pc; });
      for (size_t k = (size_t)(it - ins.begin()) >= 10 ? (it - ins.begin()) - 10 : 0; k <= (size_t)(it - ins.begin()); ++k) {
        auto* d = raw[ins[k].pc];
        fprintf(stderr, "  %#lx %-8s op=%#x modrm=%#x kind=%d dst=%s src=%s\n", (unsigned long)(ins[k].pc - img.base), d && d->TableInfo ? d->TableInfo->Name : "?", d ? d->OP : 0, d ? d->ModRM : 0,
                (int)ins[k].kind, opText(ins[k].dst).c_str(), opText(ins[k].src).c_str());
      }
    }
  }
  for (auto& j : r) if (!j.targets.empty()) tables.push_back({j.table, j.table + 4ull * j.entries});
  return r;
}

bool Lifter::Impl::generate(uint64_t GuestRIP, uint64_t lo, uint64_t hi, Gen& g) {
  auto& OD = *th->OpDispatcher;
  auto& Dec = *th->FrontendDecoder;
  if (opt.injectCrash >= 0 && lo == img.base + (uint64_t)opt.injectCrash) *(volatile int*)8 = 1;
  if (opt.injectAssert >= 0 && lo == img.base + (uint64_t)opt.injectAssert) LogMan::Throw::AFmt(false, "injected assert");
  OD.ResetWorkingList();
  // Decode; every round that finds switch tables adds their case targets as block entries and decodes again.
  {
    std::set<uint64_t> seeds;
    for (int round = 0;; ++round) {
      decode(GuestRIP, lo, hi, seeds);
      if (!opt.switchTables && !opt.retTwiceTargets && !opt.collectTargets) break;
      g.retSites.clear();
      g.outTargets.clear(); g.leaTargets.clear();
      g.jumps = analyseJumps(lo, hi, &g);
      if (!opt.switchTables) { g.jumps.clear(); break; }
      std::set<uint64_t> have;
      for (auto& b : Dec.GetDecodedBlockInfo()->Blocks) have.insert(b.Entry);
      bool more = false;
      // Branches the decoder does not follow (more than 4 pages ahead, ...) but that land inside this function.
      for (uint64_t t : external)
        if (t >= lo && t < hi && !have.count(t) && seeds.insert(t).second) more = true;
      for (auto& j : g.jumps)
        for (uint64_t t : j.targets)
          if (!onlyBounded || j.why == jt::Why::Resolved)
          if (!have.count(t) && seeds.insert(t).second) more = true;
      if (!more || round >= 7) break;
      Dec.DelayedDisownBuffer();
    }
  }

  const auto* BlockInfo = Dec.GetDecodedBlockInfo();
  const auto& CodeBlocks = BlockInfo->Blocks;
  for (auto& b : CodeBlocks) {
    g.cover.push_back({b.Entry, b.Entry + b.Size});
    if (b.Entry + b.Size > hi) g.pastEnd = true;
  }
  if (CodeBlocks.empty() || CodeBlocks.front().Entry != GuestRIP) {
    if (getenv("BBR_DEBUG_JUMPS")) { fprintf(stderr, "entry block missing: %zu blocks, first at %#lx, entry %#lx\n", CodeBlocks.size(), CodeBlocks.empty() ? 0ul : (unsigned long)CodeBlocks.front().Entry, (unsigned long)GuestRIP); }
    g.reason = "entry block missing after decoding with switch case entries";
    Dec.DelayedDisownBuffer();
    return false;
  }
  OD.BeginFunction(GuestRIP, &CodeBlocks, BlockInfo->TotalInstructionCount, BlockInfo->Is64BitMode, false);
  const auto GPRSize = OD.GetGPROpSize();

  auto fail = [&](const char* what, const X86Tables::X86InstInfo* ti, uint64_t addr) {
    if (!g.reason.empty()) return;
    char b[64]; snprintf(b, sizeof b, " @%#lx", (unsigned long)(addr - (uint64_t)img.base));
    g.reason = std::string(what) + ": " + (ti && ti->Name ? ti->Name : "UND") + " [" + hexBytes(addr, 6, hi) + "]" + b;
  };

  for (size_t j = 0; j < CodeBlocks.size(); ++j) {
    const auto& Block = CodeBlocks[j];
    bool HadDispatchError = false, HadInvalidInst = false;
    OD.SetNewBlockIfChanged(Block.Entry);
    OD.StartNewBlock();
    const uint64_t InstsInBlock = Block.NumInstructions;
    if (InstsInBlock == 0) OD.ExitFunction(OD._InlineEntrypointOffset(GPRSize, Block.Entry - GuestRIP));

    uint64_t BlockInstructionsLength {};
    for (size_t i = 0; i < InstsInBlock; ++i) {
      const uint64_t InstAddress = Block.Entry + BlockInstructionsLength;
      const X86Tables::X86InstInfo* TableInfo = Block.DecodedInstructions[i].TableInfo;
      const X86Tables::DecodedInst* DecodedInfo = &Block.DecodedInstructions[i];
      const bool IsLocked = DecodedInfo->Flags & X86Tables::DecodeFlags::FLAG_LOCK;

      OD.FlushRegisterCache(true);
      if (OD.CanHaveSideEffects(TableInfo, DecodedInfo) || (opt.forceGuestOpcode && opt.forceGuestOpcode->count(InstAddress - img.base))) OD._GuestOpcode(InstAddress - GuestRIP);

      if (TableInfo && TableInfo->OpcodeDispatcher.OpDispatch) {
        auto Fn = TableInfo->OpcodeDispatcher.OpDispatch;
        OD.ResetHandledLock();
        OD.ResetDecodeFailure();
        OD.SetForceTSO((DecodedInfo->Flags & X86Tables::DecodeFlags::FLAG_FORCE_TSO) ? IR::ForceTSOMode::ForceEnabled : IR::ForceTSOMode::NoOverride);
        std::invoke(Fn, OD, DecodedInfo);
        if (OD.HadDecodeFailure()) {
          HadDispatchError = true;
          fail("dispatch failed", TableInfo, InstAddress);
        } else {
          if (OD.HasHandledLock() != IsLocked) { HadDispatchError = true; fail("lock not handled", TableInfo, InstAddress); }
          BlockInstructionsLength += DecodedInfo->InstSize;
          ++g.insts;
        }
      } else {
        // Invalid instruction. The decoder drops such blocks silently unless they are the entry block.
        if (!BlockInstructionsLength) {
          if (Block.BlockStatus == Frontend::Decoder::DecodedBlockStatus::INVALID_INST || Block.BlockStatus == Frontend::Decoder::DecodedBlockStatus::BAD_RELOCATION) {
            OD.InvalidOp(DecodedInfo); fail("invalid instruction", TableInfo, InstAddress);
          } else if (Block.BlockStatus == Frontend::Decoder::DecodedBlockStatus::UNIMPLEMENTED_INST) {
            OD.UnimplementedOp(DecodedInfo); fail("unimplemented instruction", TableInfo, InstAddress);
          } else { OD.NoExecOp(DecodedInfo); fail("not executable", TableInfo, InstAddress); }
        }
        HadInvalidInst = true;
      }

      const bool NeedsBlockEnd = (HadDispatchError && g.insts > 0) || (OD.NeedsBlockEnder() && i + 1 == InstsInBlock) || HadInvalidInst;
      if (HadDispatchError && g.insts == 0) { OD.DelayedDisownBuffer(); return false; }
      if (NeedsBlockEnd) { OD.ExitFunction(OD._InlineEntrypointOffset(GPRSize, Block.Entry + BlockInstructionsLength - GuestRIP)); break; }
      if (OD.FinishOp(DecodedInfo->PC + DecodedInfo->InstSize, i + 1 == InstsInBlock)) break;
    }
    g.dispatchFail |= HadDispatchError;
    (void)HadInvalidInst;
  }
  OD.Finalize();
  Dec.DelayedDisownBuffer();
  for (auto& p : passes) p->Run(&OD);
  return true;
}

// ---- gaps: bytes of [lo, hi) no decoded block covers -------------------------------------------
// Two things live there: padding and switch table data (PS4 code keeps rel32 tables in .text), and
// code that nothing branches to directly (switch case bodies, reached through the table). The latter
// is lifted as extra units, one per run, so that it is part of the census and of the IR.
// Classifies what starts at pc (hi bounds the executable range, end the gap) and its length.
Lifter::Impl::Step Lifter::Impl::step(uint64_t pc, uint64_t lo, uint64_t hi, uint64_t end, uint64_t& len, std::string* why) {
  auto& Dec = *th->FrontendDecoder;
  for (auto [a, b] : tables) if (pc >= a && pc < b) { len = std::min(b, end) - pc; if (len) return Step::Data; }
  if (uint64_t n = dataRun(pc, end, hi)) { len = n; return Step::Data; }
  Dec.ResetExecutableRangeCache();
  Dec.SetupDecodeInstructionsAtEntry(th, pc, 1);
  setSection(Dec, lo, hi);
  Dec.DecodeLoop(reinterpret_cast<const uint8_t*>(pc));
  const auto& Blocks = Dec.GetDecodedBlockInfo()->Blocks;
  const X86Tables::DecodedInst* in = Blocks.empty() || !Blocks[0].NumInstructions ? nullptr : &Blocks[0].DecodedInstructions[0];
  Step st;
  if (in && Blocks[0].BlockStatus == Frontend::Decoder::DecodedBlockStatus::SUCCESS && in->InstSize) {
    len = in->InstSize;
    st = isPadding(reinterpret_cast<const uint8_t*>(pc), len) ? Step::Pad : Step::Inst;
  } else {
    static const char* names[] = {"success", "invalid instruction", "not executable", "partial decode", "bad relocation", "unimplemented instruction"};
    int code = Blocks.empty() ? 2 : static_cast<int>(Blocks[0].BlockStatus);
    if (why) {
      char b[64]; snprintf(b, sizeof b, " @%#lx", (unsigned long)(pc - (uint64_t)img.base));
      *why = std::string(names[code]) + ": " + (in && in->TableInfo && in->TableInfo->Name ? in->TableInfo->Name : "UND") + " [" + hexBytes(pc, 6, hi) + "]" + b;
    }
    // Table entries that start at an offset the run detector did not take (a negative rel32 is ff ff .. ..).
    const uint8_t* p = reinterpret_cast<const uint8_t*>(pc);
    const bool frag = p[0] >= 0xfe && (pc + 1 >= end || p[1] == 0xff);
    len = 1; st = frag ? Step::Data : Step::Invalid;
  }
  Dec.DelayedDisownBuffer();
  return st;
}

// Gaps of the sorted cover list inside [lo, hi), skipping what lies before `from`.
static std::vector<std::pair<uint64_t, uint64_t>> gapList(std::vector<std::pair<uint64_t, uint64_t>>& cover, uint64_t lo, uint64_t hi, uint64_t from) {
  std::sort(cover.begin(), cover.end());
  std::vector<std::pair<uint64_t, uint64_t>> out;
  uint64_t pos = lo;
  for (auto [a, b] : cover) {
    a = std::max(a, lo); b = std::min(b, hi);
    if (b <= pos) continue;
    a = std::max(a, pos);
    if (a > pos) out.push_back({pos, a});
    pos = b;
  }
  if (pos < hi) out.push_back({pos, hi});
  std::vector<std::pair<uint64_t, uint64_t>> res;
  for (auto [a, b] : out) { a = std::max(a, from); if (a < b) res.push_back({a, b}); }
  return res;
}

// First decodable, non-padding instruction in a gap at or after `from`; 0 when there is none.
uint64_t Lifter::Impl::findEntry(Gen& g, uint64_t lo, uint64_t hi, uint64_t from) {
  for (auto [a, b] : gapList(g.cover, lo, hi, from)) {
    for (uint64_t pc = a, len; pc < b; pc += len)
      if (step(pc, lo, hi, b, len, nullptr) == Step::Inst) return pc;
  }
  return 0;
}

// Final classification of what is left (after the extra units) for the coverage statistics.
void Lifter::Impl::gaps(uint64_t lo, uint64_t hi, Gen& g, FuncResult& r) {
  uint64_t gapTotal = 0;
  for (auto [a, b] : gapList(g.cover, lo, hi, lo)) {
    gapTotal += b - a;
    for (uint64_t pc = a, len; pc < b; pc += len) {
      std::string why;
      switch (step(pc, lo, hi, b, len, &why)) {
      case Step::Pad: r.gapPad += len; break;
      case Step::Inst: r.gapInsts += len; break;
      case Step::Data: r.gapData += len; break;
      case Step::Invalid: r.gapInvalid += 1; if (r.gapReasons.size() < 4) r.gapReasons.push_back(why); break;
      }
    }
  }
  r.gapBytes = gapTotal;
  r.coveredBytes = (hi - lo) - gapTotal;
}

Lifter::Lifter(const ElfImage& img, const LiftOptions& opt) {
  installCrashHandlers();
  impl_ = std::make_unique<Impl>(img, opt);
  uint64_t top = 0;
  for (auto& sg : img.loads) top = std::max(top, sg.vaddr + sg.memsz);
  impl_->imgLo = img.base; impl_->imgHi = img.base + top;
}
Lifter::~Lifter() = default;

void Lifter::lift(const Func& f, FuncResult& r, std::vector<uint32_t>* opCounts, std::vector<uint64_t>* sizeHist, std::string* irText, UnitSink* sink) {
  r = FuncResult {};
  log_.clear();
  std::vector<uint32_t> local;
  std::vector<uint32_t>& cnt = opCounts ? *opCounts : local;
  cnt.assign(NumOps, 0);
  const uint64_t lo = impl_->img.base + f.start, hi = impl_->img.base + f.end;
  tl_range = {lo, hi};
  tl_log = &log_;
  impl_->fnJumps.clear();
  Impl::Gen g;
  std::vector<uint64_t> extraEntries, retSites, outTargets, leaTargets; // gap unit entries (host addresses), setjmp return sites and out-of-function branch targets (image vaddrs)
  impl_->tables.clear();
  std::map<uint64_t, jt::Jump> jmps; // distinct indirect jumps of the function (image vaddr -> best outcome over its units)
  auto noteJumps = [&](const Impl::Gen& gu) {
    for (auto& j : gu.jumps) {
      jt::Jump jv = j; jv.pc -= impl_->img.base; jv.table = jv.table ? jv.table - impl_->img.base : 0;
      for (auto& t : jv.targets) t -= impl_->img.base;
      auto [it, fresh] = jmps.emplace(jv.pc, jv);
      if (!fresh && it->second.targets.empty() && !jv.targets.empty()) it->second = jv;
    }
  };
  tl_valErr.clear();
  sigjmp_buf jb;
  tl_jmp = &jb;
  if (sigsetjmp(jb, 1) == 0) {
    // Adds the IR of the last generate() to the totals.
    auto account = [&](uint64_t entry, bool extra, const Impl::Gen& gu) {
      auto IR = impl_->th->OpDispatcher->ViewIR();
      auto count = [&](const IR::IROp_Header* op) {
        ++cnt[op->Op]; ++r.irOps;
        if (sizeHist) ++(*sizeHist)[((size_t)op->Op * SizeBuckets + sizeBucket(op->Size)) * SizeBuckets + sizeBucket(op->ElementSize)];
      };
      for (auto [BlockNode, BlockHeader] : IR.GetBlocks()) {
        ++r.blocks; count(BlockHeader);
        for (auto [CodeNode, IROp] : IR.GetCode(BlockNode)) count(IROp);
      }
      if (sink) {
        UnitMeta meta;
        for (auto& c : gu.cover) meta.end = std::max(meta.end, c.second - impl_->img.base);
        for (auto& j : gu.jumps) {
          if (j.targets.empty()) continue;
          SwitchInfo sw {j.pc - impl_->img.base, {}};
          for (uint64_t t : j.targets) sw.targets.push_back(t - impl_->img.base);
          meta.switches.push_back(std::move(sw));
        }
        sink->unit(entry - impl_->img.base, extra, &IR, meta);
      }
      if (irText) {
        fextl::ostringstream out; IR::Dump(&out, &IR);
        char h[96]; snprintf(h, sizeof h, "; ---- %s unit at vaddr %#lx ----\n", extra ? "extra" : "entry", (unsigned long)(entry - impl_->img.base));
        *irText += h; *irText += out.str().c_str();
      }
    };
    const bool ok = impl_->generate(lo, lo, hi, g);
    r.insts = g.insts;
    r.spansPastEnd = g.pastEnd;
    if (!ok) {
      r.status = LiftStatus::Failed;
      r.reason = g.reason.empty() ? "no instruction lifted" : g.reason;
    } else {
      r.status = g.reason.empty() ? LiftStatus::Clean : LiftStatus::Partial;
      r.reason = g.reason;
      account(lo, false, g);
      noteJumps(g);
      retSites.insert(retSites.end(), g.retSites.begin(), g.retSites.end());
      outTargets.insert(outTargets.end(), g.outTargets.begin(), g.outTargets.end());
      leaTargets.insert(leaTargets.end(), g.leaTargets.begin(), g.leaTargets.end());
      // Code no block of the entry unit reaches (switch case bodies): lift each run as a unit of its own.
      if (impl_->opt.gapUnits && f.kind == 0) { // (pseudo-functions have no FDE bounds: a sweep could decode the data around them)
        uint64_t from = lo;
        for (int n = 0; n < 4096; ++n) {
          const uint64_t pc = impl_->findEntry(g, lo, hi, from);
          if (!pc) break;
          Impl::Gen g2;
          impl_->onlyBounded = true;
          const bool ok2 = impl_->generate(pc, pc, hi, g2);
          impl_->onlyBounded = false;
          ++r.extraUnits;
          g.cover.insert(g.cover.end(), g2.cover.begin(), g2.cover.end());
          if (!ok2 || g2.cover.empty()) {
            ++r.extraFailed; g.cover.push_back({pc, pc + 1}); // do not come back to this byte
            if (r.extraReason.empty()) r.extraReason = (g2.reason.empty() ? "no instruction lifted" : g2.reason);
          } else {
            r.extraInsts += g2.insts;
            if (!g2.reason.empty()) { ++r.extraFailed; if (r.extraReason.empty()) r.extraReason = g2.reason; }
            account(pc, true, g2);
            noteJumps(g2);
            extraEntries.push_back(pc);
            retSites.insert(retSites.end(), g2.retSites.begin(), g2.retSites.end());
            outTargets.insert(outTargets.end(), g2.outTargets.begin(), g2.outTargets.end());
            leaTargets.insert(leaTargets.end(), g2.leaTargets.begin(), g2.leaTargets.end());
          }
          from = pc;
        }
      }
    }
    // Secondary entries: landing pads of the function and the return sites of its setjmp calls become units of their
    // own (the unwinder and longjmp resume there, which no unit entry covers).
    if (impl_->opt.landingPads || impl_->opt.retTwiceTargets || impl_->opt.entryUnits) {
      std::set<uint64_t> entries;           // host addresses that already are a unit entry
      entries.insert(lo);
      for (auto& e : extraEntries) entries.insert(e);
      std::vector<std::pair<uint64_t, int>> want; // (image vaddr, kind: 0 landing pad, 1 setjmp site, 2 call/jmp target inside the function)
      if (impl_->opt.landingPads) {
        auto it = impl_->opt.landingPads->find(f.start);
        if (it != impl_->opt.landingPads->end()) for (uint64_t a : it->second) { want.push_back({a, 0}); ++r.padsKnown; }
      }
      for (uint64_t a : retSites) want.push_back({a, 1});
      if (impl_->opt.entryUnits) {
        auto it = impl_->opt.entryUnits->find(f.start);
        if (it != impl_->opt.entryUnits->end()) for (uint64_t a : it->second) want.push_back({a, 2});
      }
      std::sort(want.begin(), want.end());
      want.erase(std::unique(want.begin(), want.end()), want.end());
      for (auto [a, kind] : want) {
        const uint64_t pc = impl_->img.base + a;
        if (pc < lo || pc >= hi) continue;
        if (entries.count(pc)) { if (kind == 0) ++r.padsInUnit; continue; }
        Impl::Gen g3;
        impl_->onlyBounded = true;
        const bool ok3 = impl_->generate(pc, pc, hi, g3);
        impl_->onlyBounded = false;
        entries.insert(pc);
        (kind == 1 ? r.setjmpUnits : kind == 2 ? r.entryUnitsLifted : r.padUnits) += 1;
        g.cover.insert(g.cover.end(), g3.cover.begin(), g3.cover.end());
        if (!ok3 || g3.cover.empty()) {
          ++r.secondaryFailed;
          if (r.extraReason.empty()) r.extraReason = "secondary entry: " + (g3.reason.empty() ? std::string("no instruction lifted") : g3.reason);
        } else {
          r.secondaryInsts += g3.insts;
          if (!g3.reason.empty()) { ++r.secondaryFailed; if (r.extraReason.empty()) r.extraReason = "secondary entry: " + g3.reason; }
          account(pc, true, g3);
          noteJumps(g3);
        }
      }
    }
    if (!tl_valErr.empty()) {
      std::string e = tl_valErr; for (auto& c : e) if (c == '\n') c = ' ';
      if (r.status == LiftStatus::Clean) r.status = LiftStatus::Partial;
      if (r.reason.empty()) r.reason = "ir validation: " + e;
    }
    std::sort(outTargets.begin(), outTargets.end());
    outTargets.erase(std::unique(outTargets.begin(), outTargets.end()), outTargets.end());
    r.outTargets = std::move(outTargets);
    std::sort(leaTargets.begin(), leaTargets.end());
    leaTargets.erase(std::unique(leaTargets.begin(), leaTargets.end()), leaTargets.end());
    r.leaTargets = std::move(leaTargets);
    impl_->gaps(lo, hi, g, r);
    {
      std::set<uint64_t> seenTables;
      for (auto& [pc, j] : jmps) {
        ++r.indJmps;
        const bool res = !j.targets.empty();
        if (j.memForm) { ++r.jmpMem; r.jmpMemRip += j.ripMem; r.jmpMemIdx += j.idxMem; }
        else if (res && j.why == jt::Why::Resolved) ++r.jmpResolved;
        else if (res) ++r.jmpScanned;
        else if (j.why == jt::Why::NotTable) ++r.jmpReg;
        else ++r.jmpFailed;
        if (res && seenTables.insert(j.table).second) { r.tableEntries += j.entries; r.tableBytes += 4ull * j.entries; }
        if (!res && !j.memForm && r.jmpExamples.size() < 4) {
          char b[160]; snprintf(b, sizeof b, "%#lx %s", (unsigned long)pc, jt::whyName(j.why));
          r.jmpExamples.push_back(b);
        }
      }
    }
    tl_jmp = nullptr;
  } else {
    tl_jmp = nullptr;
    r = FuncResult {};
    r.status = LiftStatus::Crashed;
    r.reason = tl_crash;
    cnt.assign(NumOps, 0);
    impl_->build(); // the old unit is leaked, it may be mid-update
  }
  tl_log = nullptr;
}
