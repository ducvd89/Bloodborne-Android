// bb-recomp-asmtest: runs FEX's unittests/ASM/**/*.asm through the lifter + the LLVM lowering, compiled for the
// x86-64 host with LLVM ORC, and compares the registers the test expects (RegData).
//
// Conventions copied from FEX's TestHarnessRunner / HarnessHelpers.h:
//   - nasm gets "BITS 64" prepended and a trailing "ret" (unittests/ASM/CMakeLists.txt); -i unittests/ASM/Includes
//   - the code is mapped at 0x10000 (RWX); scratch memory at 0xe0000000 (10 pages) and 0xe8000000-page (2 pages)
//   - the JSON in %ifdef CONFIG gives MemoryRegions (mapped RWX), MemoryData (hex tokens, each a number stored
//     little-endian, tokens in ascending address order) and RegData (expected registers; list = 64-bit lanes)
//   - the initial state is a default CPUState (all registers 0, flags as FEX resets them); the test ends at hlt
// Each test runs in a forked child: fixed address mappings, signals and crashes stay isolated.
#include "jit_host.h"
#include "variant_compile.h"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>

#include <algorithm>
#include <atomic>
#include <cpuid.h>
#include <xmmintrin.h>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace llvm;
using FEXCore::Core::CPUState;

namespace {

constexpr uint64_t CodeBase = 0x10000;
constexpr size_t PageSz = 4096;

struct RegExp { std::string name; std::vector<uint64_t> vals; };
struct MemData { uint64_t addr; std::vector<uint8_t> bytes; };
struct Config {
  bool is32 = false;
  std::vector<std::pair<uint64_t, uint64_t>> regions;
  std::vector<MemData> mem;
  std::vector<RegExp> regs;
  std::vector<std::string> features;
  bool x87Reduced = false; // "Env": {"FEX_X87REDUCEDPRECISION": "1"}: the test expects the reduced-precision (F64) x87
};
struct Test {
  std::string rel, category, text, bin; // rel: path under unittests/ASM; bin: assembled flat binary
  bool x87Full = false;                 // uses x87 and does not ask for reduced precision: written for FEX's default 80-bit x87
  Config cfg;
  bool configOk = true;
};

uint64_t parseInt(const std::string& s) {
  if (!s.empty() && s[0] == '-') return (uint64_t)strtoll(s.c_str(), nullptr, 0);
  return strtoull(s.c_str(), nullptr, 0);
}

// Scripts/json_config_parse.py parse_hexstring
std::vector<uint8_t> hexBytes(const std::string& s) {
  std::vector<uint8_t> out;
  const bool pre = s.rfind("0x", 0) == 0;
  size_t pos = 0;
  while (true) {
    const size_t sp = s.find(' ', pos);
    std::string tok = s.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
    if (pre && tok.size() >= 2) tok = tok.substr(2);
    while (!tok.empty()) {
      const std::string b = tok.size() >= 2 ? tok.substr(tok.size() - 2) : tok;
      out.push_back((uint8_t)strtoul(b.c_str(), nullptr, 16));
      tok.resize(tok.size() >= 2 ? tok.size() - 2 : 0);
    }
    if (sp == std::string::npos) break;
    pos = sp + 1;
  }
  return out;
}

bool parseConfig(const std::string& text, Config& c) {
  const size_t a = text.find("%ifdef CONFIG");
  if (a == std::string::npos) return true;
  const size_t b = text.find("%endif", a);
  if (b == std::string::npos) return true;
  std::string js = text.substr(a + 13, b - a - 13);
  auto v = json::parse(js);
  if (!v) { consumeError(v.takeError()); return false; }
  auto* o = v->getAsObject();
  if (!o) return false;
  for (auto& [k, val] : *o) {
    std::string key = k.str();
    for (auto& ch : key) ch = toupper(ch);
    if (key == "MODE") { if (auto s = val.getAsString()) { std::string m = s->str(); for (auto& ch : m) ch = toupper(ch); c.is32 = m == "32BIT"; } }
    else if (key == "MEMORYREGIONS") { if (auto* r = val.getAsObject()) for (auto& [ak, av] : *r) c.regions.push_back({parseInt(ak.str()), parseInt(av.getAsString().value_or("0").str())}); }
    else if (key == "MEMORYDATA") { if (auto* r = val.getAsObject()) for (auto& [ak, av] : *r) c.mem.push_back({parseInt(ak.str()), hexBytes(av.getAsString().value_or("").str())}); }
    else if (key == "REGDATA") {
      if (auto* r = val.getAsObject())
        for (auto& [ak, av] : *r) {
          RegExp e; e.name = ak.str();
          for (auto& ch : e.name) ch = toupper(ch);
          if (auto* arr = av.getAsArray()) { for (auto& x : *arr) e.vals.push_back(parseInt(x.getAsString().value_or("0").str())); }
          else e.vals.push_back(parseInt(av.getAsString().value_or("0").str()));
          c.regs.push_back(std::move(e));
        }
    } else if (key == "ENV") {
      if (auto* r = val.getAsObject())
        for (auto& [ak, av] : *r) if (ak.str() == "FEX_X87REDUCEDPRECISION" && av.getAsString().value_or("") == "1") c.x87Reduced = true;
    } else if (key == "HOSTFEATURES") {
      if (auto* arr = val.getAsArray()) for (auto& x : *arr) { std::string f = x.getAsString().value_or("").str(); for (auto& ch : f) ch = toupper(ch); c.features.push_back(f); }
    }
  }
  return true;
}


void mapFixed(uint64_t addr, size_t size) {
  void* p = mmap((void*)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE, -1, 0);
  if (p != (void*)addr) { fprintf(stderr, "mmap %#lx+%zx failed\n", (unsigned long)addr, size); _exit(90); }
}

std::string hex(uint64_t v) { char b[32]; snprintf(b, sizeof b, "0x%016lx", (unsigned long)v); return b; }

// Returns the one-line verdict: PASS | FAIL <why> | UNSUP <ops> | LIFT <why> | CRASH <why>
std::string runTest(const Test& t, int opt) {
  const Config& c = t.cfg;
  // memory layout (HarnessCodeLoader::MapMemory)
  mapFixed(0xe0000000, PageSz * 10);
  mapFixed(0xe8000000 - PageSz, PageSz * 2);
  const size_t codeLen = (t.bin.size() + PageSz - 1) & ~(PageSz - 1);
  mapFixed(CodeBase, codeLen);
  memcpy((void*)CodeBase, t.bin.data(), t.bin.size());
  for (auto& [a, s] : c.regions) mapFixed(a, s);
  for (auto& m : c.mem) memcpy((void*)m.addr, m.bytes.data(), m.bytes.size());
  const uint64_t codeHi = CodeBase + t.bin.size();

  static ElfImage img; // base 0: IR addresses are the guest addresses
  img.base = 0;
  LiftOptions lo; lo.gapUnits = false; lo.multiblock = true;
  TargetSpec ts; ts.aarch64 = false; ts.opt = opt;
  Jit jit(img, lo, ts);
  g_jit = &jit;
  jit.rangeEnd = [codeHi](uint64_t t) -> uint64_t { return t >= CodeBase && t < codeHi ? codeHi : 0; };
  if (!jit.err.empty()) return "FAIL jit init: " + jit.err;

  alignas(64) static CPUState st;
  new (&st) CPUState();
  // The thread state FEX sets up (ThreadManager.cpp): a GDT with a 64-bit code segment
  static CPUState::gdt_segment gdt[32];
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
  st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  auto* cs = CPUState::GetSegmentFromIndex(st, st.cs_idx);
  CPUState::SetGDTBase(cs, 0);
  CPUState::SetGDTLimit(cs, 0xFFFFFu);
  cs->L = 1; cs->D = 0;
  st.rip = CodeBase;
  if (!jit.compile(CodeBase, codeHi)) {
    if (!jit.unsupported.empty()) { std::string s = "UNSUP"; for (auto& u : jit.unsupported) s += " " + u; return s; }
    if (!jit.liftFailure.empty()) return "LIFT " + jit.liftFailure;
    return "FAIL " + jit.err;
  }
  Jit::Fn entry = jit.find(CodeBase);
  if (!entry) return "FAIL no entry: " + jit.err;

  struct sigaction sa {}; sa.sa_handler = jitOnSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER | SA_ONSTACK;
  static char altstack[1 << 16];
  stack_t ss {}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(s, &sa, nullptr);
  alarm(20);
  if (sigsetjmp(g_halt, 1) == 0) {
    entry(&st);
    // The outermost dispatcher loop (RECOMP_ABI.md): a unit that returned somewhere else (ret/jmp out of the code)
    for (;;) Jit::dispatch(&st, st.rip);
  }
  alarm(0);

  if (g_haltKind == 3) { std::string s = "UNSUP"; for (auto& u : jit.unsupported) s += " " + u; if (jit.unsupported.empty()) s += " (op reached at run time)"; return s; }
  if (g_haltKind == 4) return "CRASH signal " + std::to_string(g_trapArg) + " rip " + hex(st.rip);
  if (g_haltKind == 5) return "FAIL dispatch to " + hex(g_trapArg);
  if (g_haltKind == 2) return "FAIL Break kind " + hex(g_trapArg) + " (not hlt)";
  if (g_haltKind != 1) return "FAIL test did not reach hlt";

  static const char* gprNames[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
  auto check = [&](const std::string& what, uint64_t got, uint64_t want) -> std::string {
    return got == want ? "" : "FAIL " + what + " got " + hex(got) + " want " + hex(want);
  };
  for (auto& r : c.regs) {
    bool done = false;
    for (unsigned i = 0; i < 16 && !done; ++i)
      if (r.name == gprNames[i]) { auto e = check(r.name, st.gregs[i], r.vals[0]); if (!e.empty()) return e; done = true; }
    if (done) continue;
    if (r.name == "RIP") { auto e = check("RIP", st.rip, r.vals[0]); if (!e.empty()) return e; continue; }
    if (r.name == "FS") { auto e = check("FS", st.fs_cached, r.vals[0]); if (!e.empty()) return e; continue; }
    if (r.name == "GS") { auto e = check("GS", st.gs_cached, r.vals[0]); if (!e.empty()) return e; continue; }
    if (r.name.rfind("XMM", 0) == 0) {
      const unsigned n = atoi(r.name.c_str() + 3);
      for (size_t j = 0; j < r.vals.size(); ++j) {
        const uint64_t got = j < 2 ? st.xmm.sse.data[n][j] : st.avx_high[n][j - 2];
        auto e = check(r.name + "[" + std::to_string(j) + "]", got, r.vals[j]);
        if (!e.empty()) return e;
      }
      continue;
    }
    if (r.name.rfind("MM", 0) == 0) {
      const unsigned n = atoi(r.name.c_str() + 2);
      for (size_t j = 0; j < r.vals.size(); ++j) { auto e = check(r.name + "[" + std::to_string(j) + "]", st.mm[n][j], r.vals[j]); if (!e.empty()) return e; }
      continue;
    }
    return "FAIL unknown register " + r.name;
  }
  return "PASS";
}

// Resume entries (docs/RECOMP_ABI.md): a unit entered with the resume flag set and rip at a call-return address continues there
// with the registers in the guest state; at an address that is not a call-return block it returns at once with the flag still set.
//     mov rbx, 5 ; call f ; R: mov rax, rbx ; add rax, 3 ; hlt ; f: ret
std::string runResumeTest(int opt) {
  static const uint8_t code[] = {0x48, 0xc7, 0xc3, 5, 0, 0, 0,  0xe8, 8, 0, 0, 0,  0x48, 0x89, 0xd8,  0x48, 0x83, 0xc0, 3,  0xf4,  0xc3};
  constexpr uint64_t R = CodeBase + 12;
  mapFixed(0xe8000000 - PageSz, PageSz * 2);
  mapFixed(CodeBase, PageSz);
  memcpy((void*)CodeBase, code, sizeof code);
  static ElfImage img; img.base = 0;
  LiftOptions lo; lo.gapUnits = false; lo.multiblock = true;
  TargetSpec ts; ts.aarch64 = false; ts.opt = opt;
  Jit jit(img, lo, ts);
  jit.resumeEntries = true;
  g_jit = &jit;
  jit.rangeEnd = [](uint64_t t) -> uint64_t { return t >= CodeBase && t < CodeBase + sizeof code ? CodeBase + sizeof code : 0; };
  if (!jit.err.empty()) return "FAIL jit init: " + jit.err;
  alignas(64) static CPUState st;
  static CPUState::gdt_segment gdt[32];
  auto reset = [&] {
    new (&st) CPUState();
    st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
    st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
    st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
    auto* cs = CPUState::GetSegmentFromIndex(st, st.cs_idx);
    CPUState::SetGDTBase(cs, 0); CPUState::SetGDTLimit(cs, 0xFFFFFu); cs->L = 1; cs->D = 0;
    st.gregs[4] = 0xe8000000;
  };
  if (!jit.compile(CodeBase, CodeBase + sizeof code)) return "FAIL compile: " + jit.err + jit.liftFailure;
  Jit::Fn entry = jit.find(CodeBase);
  if (!entry) return "FAIL no entry";
  struct sigaction sa {}; sa.sa_handler = jitOnSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER | SA_ONSTACK;
  static char altstack[1 << 16];
  stack_t ss {}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
  for (int sg : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(sg, &sa, nullptr);
  alarm(20);
  auto run = [&](uint64_t rip, uint64_t flag, uint64_t rbx) -> int { // 0 reached hlt, 1 returned without hlt, 2 failed
    reset(); st.rip = rip; st.gregs[3] = rbx; st._pad1 = flag;
    g_haltKind = 0;
    if (sigsetjmp(g_halt, 1) == 0) { entry(&st); return 1; }
    return g_haltKind == 1 ? 0 : 2;
  };
  if (run(CodeBase, 0, 0) != 0 || st.gregs[0] != 8) return "FAIL ordinary run: rax " + std::to_string(st.gregs[0]);
  if (run(R, 1, 5) != 0 || st.gregs[0] != 8 || st._pad1 != 0) return "FAIL resume at the call-return address: rax " + std::to_string(st.gregs[0]) + " flag " + std::to_string(st._pad1);
  if (run(R, 1, 40) != 0 || st.gregs[0] != 43) return "FAIL resume keeps the registers: rax " + std::to_string(st.gregs[0]);
  if (run(R + 1, 1, 5) != 1 || st._pad1 != 1) return "FAIL a non-call-return address must return with the flag set: flag " + std::to_string(st._pad1);
  alarm(0);
  return "PASS";
}


// Hook slots (docs/RECOMP_ABI.md, "Hook slots"): at a hook site the unit tests bbr_hook_slot[k]; when set it spills the state, calls the slot and
// acts on the result: 0 = run the instruction, 1 = rip set by the callback (a resume point of the site, or the dispatcher), 2 = the callback
// returned from the function. The slot may be set and cleared while the code runs. Each case compiles its own unit set.
struct HookRun {
  std::vector<uint8_t> code;
  HookSites sites;
  std::function<void(CPUState*)> prepare;   // before the run
  uint64_t start = CodeBase;
};
uint64_t g_hookCalls = 0;
std::function<uint64_t(CPUState*, uint64_t)> g_hookFn;
extern "C" uint64_t bbrTestHook(CPUState* s, uint64_t k) { ++g_hookCalls; return g_hookFn(s, k); }

// Runs the case: returns the guest state after `hlt` in `out`; "" or a failure text.
std::string runHookCase(HookRun& hr, int opt, CPUState& out, bool slotSet) {
  mapFixed(0xe8000000 - PageSz, PageSz * 2);
  mapFixed(CodeBase, (hr.code.size() + PageSz - 1) & ~(PageSz - 1));
  memcpy((void*)CodeBase, hr.code.data(), hr.code.size());
  static ElfImage img; img.base = 0;
  static std::set<uint64_t> force;
  force.clear();
  for (auto& [v, h] : hr.sites) { force.insert(v); for (uint64_t r : h.resume) force.insert(r); }
  LiftOptions lo; lo.gapUnits = false; lo.multiblock = true; lo.forceGuestOpcode = &force;
  TargetSpec ts; ts.aarch64 = false; ts.opt = opt;
  Jit jit(img, lo, ts);
  jit.hookSites = &hr.sites;
  g_jit = &jit;
  const uint64_t hi = CodeBase + hr.code.size();
  jit.rangeEnd = [hi](uint64_t t) -> uint64_t { return t >= CodeBase && t < hi ? hi : 0; };
  if (!jit.err.empty()) return "jit init: " + jit.err;
  for (auto& s : g_hookSlot) s = nullptr;
  if (slotSet) for (auto& [v, h] : hr.sites) g_hookSlot[h.index] = (void*)&bbrTestHook;
  alignas(64) static CPUState st;
  static CPUState::gdt_segment gdt[32];
  new (&st) CPUState();
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
  st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  auto* cs = CPUState::GetSegmentFromIndex(st, st.cs_idx);
  CPUState::SetGDTBase(cs, 0); CPUState::SetGDTLimit(cs, 0xFFFFFu); cs->L = 1; cs->D = 0;
  st.gregs[4] = 0xe8000000;
  st.rip = hr.start;
  if (hr.prepare) hr.prepare(&st);
  if (!jit.compile(CodeBase, hi)) return "compile: " + jit.err + jit.liftFailure;
  Jit::Fn entry = jit.find(hr.start);
  if (!entry) return "no entry unit";
  struct sigaction sa {}; sa.sa_handler = jitOnSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER | SA_ONSTACK;
  static char altstack[1 << 16];
  stack_t ss {}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
  for (int sg : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(sg, &sa, nullptr);
  alarm(20);
  g_haltKind = 0;
  if (sigsetjmp(g_halt, 1) == 0) {
    entry(&st);
    for (;;) Jit::dispatch(&st, st.rip);
  }
  alarm(0);
  if (g_haltKind != 1) return "the code did not reach hlt (kind " + std::to_string(g_haltKind) + ", arg " + hex(g_trapArg) + ")";
  memcpy((void*)&out, (const void*)&st, sizeof out);
  return "";
}

std::string runHookTest(int opt) {
  CPUState st;
  std::string e;
  auto expect = [&](const char* what, uint64_t got, uint64_t want) -> std::string { return got == want ? "" : std::string("FAIL ") + what + ": got " + std::to_string(got) + " want " + std::to_string(want); };
  auto site = [](uint64_t at, std::vector<uint64_t> resume = {}) { HookSites h; HookSite s; s.vaddr = at; s.index = 0; s.resume = std::move(resume); h[at] = s; return h; };

  // 1. An observer edits a register and the instruction then runs. The slot is set after the code was compiled: the test is in the unit.
  //    mov rbx, 5 ; H: add rbx, 1 ; mov rax, rbx ; hlt
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc3, 5, 0, 0, 0,  0x48, 0x83, 0xc3, 1,  0x48, 0x89, 0xd8,  0xf4};
    hr.sites = site(CodeBase + 7);
    g_hookFn = [](CPUState* s, uint64_t k) -> uint64_t { s->gregs[3] += 100; return k == 0 ? 0 : 9; };
    g_hookCalls = 0;
    if (!(e = runHookCase(hr, opt, st, false)).empty()) return "FAIL 1a: " + e;
    if (!(e = expect("1a no slot: rax", st.gregs[0], 6)).empty() || g_hookCalls != 0) return e + " (hook calls " + std::to_string(g_hookCalls) + ")";
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 1b: " + e;
    if (!(e = expect("1b slot: rax (rbx edited by the hook, then add 1)", st.gregs[0], 106)).empty() || g_hookCalls != 1) return e + " (hook calls " + std::to_string(g_hookCalls) + ")";
  }
  // 2. The hook sees the registers and rip = the site; a register it does not touch survives; flags survive the call.
  //    mov rax, 1 ; cmp rax, 1 ; mov rcx, 7 ; H: mov rdx, 3 ; jne bad ; mov rax, 99 ; hlt ; bad: mov rax, 1 ; hlt
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0x48, 0x83, 0xf8, 1,  0x48, 0xc7, 0xc1, 7, 0, 0, 0,
                           0x48, 0xc7, 0xc2, 3, 0, 0, 0,  0x75, 8,  0x48, 0xc7, 0xc0, 99, 0, 0, 0,  0xf4,  0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0xf4};
    const uint64_t H = CodeBase + 18;
    hr.sites = site(H);
    static uint64_t seenRip, seenRcx;
    g_hookFn = [](CPUState* s, uint64_t) -> uint64_t { seenRip = s->rip; seenRcx = s->gregs[1]; s->gregs[1] = 55; return 0; };
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 2: " + e;
    if (!(e = expect("2 flags survive the hook: rax", st.gregs[0], 99)).empty()) return e;
    if (!(e = expect("2 rip at the hook", seenRip, H)).empty()) return e;
    if (!(e = expect("2 rcx seen", seenRcx, 7)).empty()) return e;
    if (!(e = expect("2 rcx edited", st.gregs[1], 55)).empty()) return e;
    if (!(e = expect("2 rdx (the hooked instruction ran)", st.gregs[2], 3)).empty()) return e;
  }
  // 3. Return from a function at its entry (act 2): the callback sets rax, pops the return address.
  //    mov rax, 0 ; call f ; add rax, 1000 ; hlt ; f: H: mov rax, 1 ; ret
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc0, 0, 0, 0, 0,  0xe8, 8, 0, 0, 0,  0x48, 0x05, 0xe8, 3, 0, 0,  0xf4, 0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0xc3};
    // (the add rax, imm32 is 6 bytes: 48 05 imm32; fix the call offset below)
    hr.code = {0x48, 0xc7, 0xc0, 0, 0, 0, 0,  0xe8, 7, 0, 0, 0,  0x48, 0x05, 0xe8, 3, 0, 0,  0xf4,  0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0xc3};
    const uint64_t F = CodeBase + 19;
    hr.sites = site(F);
    g_hookFn = [](CPUState* s, uint64_t) -> uint64_t { s->gregs[0] = 42; s->rip = *(uint64_t*)s->gregs[4]; s->gregs[4] += 8; return 2; };
    if (!(e = runHookCase(hr, opt, st, false)).empty()) return "FAIL 3a: " + e;
    if (!(e = expect("3a without the hook: rax", st.gregs[0], 1001)).empty()) return e;
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 3b: " + e;
    if (!(e = expect("3b the hook returned: rax", st.gregs[0], 1042)).empty()) return e;
  }
  // 4. The callback replaces an instruction and continues after it (act 1): a resume point in the middle of a block, then one at a block entry.
  //    mov rax, 0 ; H: mov rax, 1 ; R: add rax, 10 ; hlt        (hook: rax = 5, rip = R)
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc0, 0, 0, 0, 0,  0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0x48, 0x83, 0xc0, 10,  0xf4};
    const uint64_t H = CodeBase + 7, R = CodeBase + 14;
    hr.sites = site(H, {R});
    g_hookFn = [R](CPUState* s, uint64_t) -> uint64_t { s->gregs[0] = 5; s->rip = R; return 1; };
    if (!(e = runHookCase(hr, opt, st, false)).empty()) return "FAIL 4a: " + e;
    if (!(e = expect("4a without the hook: rax", st.gregs[0], 11)).empty()) return e;
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 4b: " + e;
    if (!(e = expect("4b resume in the middle of a block: rax", st.gregs[0], 15)).empty()) return e;
  }
  {
    //    mov rax, 0 ; H: je +7 (not taken: rax is 0... use cmp) ...
    //    xor eax, eax ; H: jne T ; mov rbx, 1 ; hlt ; T: mov rbx, 2 ; hlt      (ZF = 1: the branch is not taken; the hook sends it to T)
    HookRun hr; hr.code = {0x31, 0xc0,  0x75, 8,  0x48, 0xc7, 0xc3, 1, 0, 0, 0,  0xf4,  0x48, 0xc7, 0xc3, 2, 0, 0, 0,  0xf4};
    const uint64_t H = CodeBase + 2, T = CodeBase + 12;
    hr.sites = site(H, {T, CodeBase + 4});
    g_hookFn = [T](CPUState* s, uint64_t) -> uint64_t { s->rip = T; return 1; };
    if (!(e = runHookCase(hr, opt, st, false)).empty()) return "FAIL 4c: " + e;
    if (!(e = expect("4c without the hook: rbx", st.gregs[3], 1)).empty()) return e;
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 4d: " + e;
    if (!(e = expect("4d resume at a block entry: rbx", st.gregs[3], 2)).empty()) return e;
  }
  // 5. A jump the site does not list goes through the dispatcher (the code there is compiled on demand).
  //    H: mov rax, 1 ; hlt ; T: mov rax, 77 ; hlt       (hook: rip = T, not a resume point)
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc0, 1, 0, 0, 0,  0xf4,  0x48, 0xc7, 0xc0, 77, 0, 0, 0,  0xf4};
    const uint64_t T = CodeBase + 8;
    hr.sites = site(CodeBase);
    g_hookFn = [T](CPUState* s, uint64_t) -> uint64_t { s->rip = T; return 1; };
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 5: " + e;
    if (!(e = expect("5 dispatcher: rax", st.gregs[0], 77)).empty()) return e;
  }
  // 6. A site in a loop, the callback counts and clears nothing: it runs once per iteration; xmm registers survive.
  //    mov rcx, 3 ; movd xmm1, ecx ; L: H: dec rcx ; jnz L ; movq rax, xmm1 ; hlt
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc1, 3, 0, 0, 0,  0x66, 0x0f, 0x6e, 0xc9,  0x48, 0xff, 0xc9,  0x75, 0xfb,  0x66, 0x48, 0x0f, 0x7e, 0xc8,  0xf4};
    hr.sites = site(CodeBase + 11);
    g_hookFn = [](CPUState* s, uint64_t) -> uint64_t { s->xmm.sse.data[1][0] += 10; return 0; };
    g_hookCalls = 0;
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 6: " + e;
    if (!(e = expect("6 hook calls in the loop", g_hookCalls, 3)).empty()) return e;
    if (!(e = expect("6 xmm1 edited by the hook", st.gregs[0], 33)).empty()) return e;
  }
  // 7. A value site: `mov eax, imm32` (the scene size of bbport_display) replaced by a callback that loads rax and goes on after the 5 bytes.
  //    mov rax, -1 ; H: mov eax, 1280 ; R: mov rbx, rax ; hlt
  {
    HookRun hr; hr.code = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff,  0xb8, 0x00, 0x05, 0x00, 0x00,  0x48, 0x89, 0xc3,  0xf4};
    const uint64_t H = CodeBase + 7, R = CodeBase + 12;
    hr.sites = site(H, {R});
    g_hookFn = [R](CPUState* s, uint64_t) -> uint64_t { s->gregs[0] = 1707; s->rip = R; return 1; };
    if (!(e = runHookCase(hr, opt, st, false)).empty()) return "FAIL 7a: " + e;
    if (!(e = expect("7a without the hook: rbx", st.gregs[3], 1280)).empty()) return e;
    if (!(e = runHookCase(hr, opt, st, true)).empty()) return "FAIL 7b: " + e;
    if (!(e = expect("7b value site: rbx", st.gregs[3], 1707)).empty()) return e;
  }
  return "PASS";
}


// Variants (docs/RECOMP_ABI.md, "Variants"): the functions a catalog's patch groups touch are lifted again from the patched bytes and lowered as
// alternate units bbrv<k>_<vaddr>; the table entries carry the hash of the patched bytes. A tiny image in this process:
//   F  0x100: mov eax, 1 ; ret                  group g1 patches the immediate (mov eax, 2)
//   H  0x300: mov eax, 5 ; nop*5 ; add eax, 7 ; ret    group g3 puts `jmp cave` over the nops and writes the cave at 0x2000:
//   cave 0x2000: add eax, 100 ; jmp H+10        (new code, and a unit inside H that only the variant has)
//   K  0x400: mov eax, 1 ; mov ebx, 1 ; ret     alternatives g4 / g5 (excl=x) patch the two immediates; g4+g5 is never built
//   R  0x500: mov eax, [rip + disp] ; ret       the operand redirect: group g6 rewrites the displacement to a fixed data address (the frame rate's `live`)
std::string runVariantTest(int opt) {
  constexpr uint64_t Base = 0x200000000000ull, Size = 0x10000; // (high memory: also outside the shadow gap of ASan builds)
  mapFixed(Base, Size);
  mapFixed(0xe8000000 - PageSz, PageSz * 2);
  auto put = [&](uint64_t v, std::initializer_list<uint8_t> b) { size_t i = 0; for (uint8_t x : b) ((uint8_t*)(Base + v))[i++] = x; };
  put(0x100, {0xb8, 1, 0, 0, 0, 0xc3});
  put(0x300, {0xb8, 5, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x05, 7, 0, 0, 0, 0xc3});
  put(0x400, {0xb8, 1, 0, 0, 0, 0xbb, 1, 0, 0, 0, 0xc3});
  put(0x500, {0x8b, 0x05, 0xfa, 0x00, 0x00, 0x00, 0xc3}); // mov eax, [rip + 0xfa] -> 0x600
  put(0x600, {7, 0, 0, 0});
  put(0x700, {99, 0, 0, 0});                              // the redirect's target (live)
  static ElfImage img;
  img.base = Base; img.loads.assign(1, ElfSegment{0, Size, Size, 0, 5}); img.textLo = 0; img.textHi = Size;
  const int32_t toCave = int32_t(0x2000 - (0x305 + 5)), back = int32_t((0x300 + 10) - (0x2005 + 5));
  char cat[1024];
  auto hexle = [](int32_t v) { char b[16]; snprintf(b, sizeof b, "%02x%02x%02x%02x", v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255); return std::string(b); };
  snprintf(cat, sizeof cat, "group g1\nw 101 02\ngroup g3\nw 305 e9%s\nc 2000 0564000000e9%s\ngroup g4 excl=x\nw 401 02\ngroup g5 excl=x\nw 406 07\ngroup g6\nw 502 %s\n", hexle(toCave).c_str(), hexle(back).c_str(), hexle(int32_t(0x700 - 0x506)).c_str());
  Catalog catalog; std::string err;
  if (!parseCatalog(cat, "test", catalog, err)) return "FAIL catalog: " + err;
  std::vector<Func> funcs = {{0x100, 0x106, 0, 0}, {0x300, 0x310, 0, 0}, {0x400, 0x40b, 0, 0}, {0x500, 0x507, 0, 0}};
  std::vector<uint64_t> starts = {0x100, 0x300, 0x400, 0x500}, traps;
  LiftOptions lo; lo.gapUnits = false; lo.multiblock = true;
  VariantJob vj;
  vj.img = &img; vj.funcs = &funcs; vj.starts = &starts; vj.trapUnits = &traps; vj.catalog = &catalog; vj.lift = lo;
  vj.lower.liftBase = Base; vj.lower.imageSize = Size; vj.lower.fixedImageBase = true; vj.lower.imageBase = Base; vj.lower.aarch64 = false; vj.lower.hiddenUnits = false;
  vj.lower.calleeSaved = false; vj.lower.storeFlagsAtCalls = true;
  vj.target.aarch64 = false; vj.target.opt = opt; vj.jobs = 2; vj.verify = true;
  char dir[] = "/tmp/bbr-vartest-XXXXXX";
  if (!mkdtemp(dir)) return "FAIL mkdtemp";
  struct Rm { std::string d; ~Rm() { std::error_code ec; fs::remove_all(d, ec); } } rm{dir};
  vj.objBase = std::string(dir) + "/v";
  VariantResult vr;
  if (!compileVariants(vj, vr)) return "FAIL compileVariants";
  // F: g1; H: g3 (3 units: H, the cave, the jump-back target inside H); K: g4, g5 (not both): 3 variants
  if (vr.table.funcs.size() != 5) { std::string n; for (auto& f : vr.table.funcs) n += " " + f.name; return "FAIL expected 5 variants, got " + std::to_string(vr.table.funcs.size()) + ":" + n; }
  const VFunc* vf = nullptr;
  for (auto& f : vr.table.funcs) if (f.funcStart == 0x300) vf = &f;
  if (!vf || vf->unitCount != 3) return "FAIL the variant of H must have 3 units (H, the cave, the unit inside H)";
  std::set<uint64_t> entries;
  for (uint32_t i = 0; i < vf->unitCount; ++i) entries.insert(vr.table.units[vf->firstUnit + i].vaddr);
  if (!entries.count(0x300) || !entries.count(0x2000) || !entries.count(0x30a)) return "FAIL the variant of H lacks one of the units 0x300, 0x2000, 0x30a";
  for (uint32_t i = 0; i < vf->unitCount; ++i) {
    const VUnit& u = vr.table.units[vf->firstUnit + i];
    if ((u.vaddr == 0x300) != (u.orig == 1)) return "FAIL original unit index of " + hex(u.vaddr) + ": " + std::to_string(u.orig);
  }
  for (auto& f : vr.table.funcs) if (f.name.find("g4+g5") != std::string::npos) return "FAIL alternatives g4, g5 were combined";
  // The bytes the runtime will see after the groups were applied hash to the table's values; the original bytes do not.
  auto apply = [&](const std::string& g) { for (auto& grp : catalog.groups) if (grp.name == g) for (auto& w : grp.writes) memcpy((void*)(Base + w.vaddr), w.bytes.data(), w.bytes.size()); };
  apply("g1"); apply("g3");
  for (auto& f : vr.table.funcs) {
    if (f.funcStart != 0x100 && f.funcStart != 0x300) continue;
    for (uint32_t i = 0; i < f.unitCount; ++i) {
      const VUnit& u = vr.table.units[f.firstUnit + i];
      if (hashRange((const uint8_t*)(Base + u.vaddr), u.end - u.vaddr) != u.ehash) return "FAIL the patched bytes do not hash to the table's value at " + hex(u.vaddr);
    }
  }
  const uint64_t pristineF = hashRange((const uint8_t*)(Base + 0x100), 6);
  (void)pristineF;
  // Run the variants (in a JIT with the objects added) and the originals (lowered from the pristine bytes): behaviour differs as patched.
  TargetSpec ts; ts.aarch64 = false; ts.opt = opt;
  Jit jit(img, lo, ts);
  if (!jit.err.empty()) return "FAIL jit init: " + jit.err;
  g_jit = &jit;
  jit.liftBase = Base; jit.imageSize = Size;
  for (auto& o : vr.objFiles) {
    auto buf = MemoryBuffer::getFile(o);
    if (!buf) return "FAIL cannot read " + o;
    if (auto e = jit.jit->addObjectFile(std::move(*buf))) return "FAIL add object: " + toString(std::move(e));
  }
  // (the pristine units: undo the patches for the lift)
  auto revertAll = [&] { memset((void*)(Base + 0x2000), 0, 16); put(0x100, {0xb8, 1, 0, 0, 0, 0xc3}); put(0x300, {0xb8, 5, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x05, 7, 0, 0, 0, 0xc3}); };
  revertAll();
  jit.rangeEnd = [](uint64_t) -> uint64_t { return 0; };
  if (!jit.compile(Base + 0x100, Base + 0x106) || !jit.compile(Base + 0x300, Base + 0x310)) return "FAIL compile originals: " + jit.err + jit.liftFailure;
  alignas(64) static CPUState st;
  static CPUState::gdt_segment gdt[32];
  struct sigaction sa {}; sa.sa_handler = jitOnSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER | SA_ONSTACK;
  static char altstack[1 << 16];
  stack_t ss {}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
  for (int sg : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(sg, &sa, nullptr);
  alarm(20);
  auto call = [&](Jit::Fn fn, uint64_t* eax) -> std::string {
    new (&st) CPUState();
    st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt; st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
    st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
    auto* cs = CPUState::GetSegmentFromIndex(st, st.cs_idx);
    CPUState::SetGDTBase(cs, 0); CPUState::SetGDTLimit(cs, 0xFFFFFu); cs->L = 1; cs->D = 0;
    st.gregs[4] = 0xe8000000; *(uint64_t*)st.gregs[4] = 0xdead0;
    g_haltKind = 0;
    if (sigsetjmp(g_halt, 1) == 0) { fn(&st); } else return "signal";
    if (st.rip != 0xdead0) return "did not return";
    *eax = st.gregs[0];
    return "";
  };
  auto variantFn = [&](uint64_t funcStart, uint64_t vaddr) -> Jit::Fn {
    for (auto& f : vr.table.funcs) {
      if (f.funcStart != funcStart) continue;
      for (uint32_t i = 0; i < f.unitCount; ++i) if (vr.table.units[f.firstUnit + i].vaddr == vaddr) {
        auto r = jit.jit->lookup(vr.table.units[f.firstUnit + i].symbol);
        return r ? r->toPtr<Jit::Fn>() : nullptr;
      }
    }
    return nullptr;
  };
  uint64_t eax = 0;
  std::string e;
  if (!(e = call(jit.find(Base + 0x100), &eax)).empty() || eax != 1) return "FAIL original F: " + e + " eax " + std::to_string(eax);
  Jit::Fn vF = variantFn(0x100, 0x100);
  if (!vF) return "FAIL no variant function for F";
  if (!(e = call(vF, &eax)).empty() || eax != 2) return "FAIL variant of F: " + e + " eax " + std::to_string(eax);
  if (!(e = call(jit.find(Base + 0x300), &eax)).empty() || eax != 12) return "FAIL original H: " + e + " eax " + std::to_string(eax);
  Jit::Fn vH = variantFn(0x300, 0x300);
  if (!vH) return "FAIL no variant function for H";
  if (!(e = call(vH, &eax)).empty() || eax != 112) return "FAIL variant of H (through the cave): " + e + " eax " + std::to_string(eax);
  // The operand redirect: the original reads 7 at 0x600, the variant 99 at 0x700 (a displacement is all that changed), both in the lowered code.
  revertAll();
  put(0x500, {0x8b, 0x05, 0xfa, 0x00, 0x00, 0x00, 0xc3});
  if (!jit.compile(Base + 0x500, Base + 0x507)) return "FAIL compile R: " + jit.err + jit.liftFailure;
  if (!(e = call(jit.find(Base + 0x500), &eax)).empty() || eax != 7) return "FAIL original R: " + e + " eax " + std::to_string(eax);
  Jit::Fn vR = variantFn(0x500, 0x500);
  if (!vR) return "FAIL no variant function for R";
  if (!(e = call(vR, &eax)).empty() || eax != 99) return "FAIL variant of R (operand redirect): " + e + " eax " + std::to_string(eax);
  alarm(0);
  return "PASS";
}

// ------------------------------------------------------------------------------------ the parent ----
struct Counts { unsigned pass = 0, fail = 0, unsup = 0, lift = 0, skip = 0, crash = 0; };

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

// FEX runs unittests/ASM/X87 (and every other x87 test without an Env key) in its default x87 mode, 80-bit precision
// (X87ReducedPrecision=0), and the variants in X87_F64 with "Env": {"FEX_X87REDUCEDPRECISION": "1"}. The game runs reduced
// (tools/android/fex-compatible.json) and the lifter is configured like that, so the tests that apply are the ones with that Env
// key (X87_F64 and a few others); the 80-bit ones expect extended significands in MM registers, status-word exceptions and
// 80-bit rounding that the reduced x87 does not have by design. They still run, in the category "x87 80-bit (info)", which is
// not part of the totals.
bool usesX87(const std::string& text) {
  static const std::regex re(R"((^|\n)[ \t]*(f[a-z0-9]{1,9}|xsave[a-z]*|xrstor[a-z]*)\b)");
  return std::regex_search(text, re);
}

std::string skipReason(const Test& t, const std::set<std::string>& disabled) {
  if (disabled.count("Test_" + t.rel)) return "disabled or known failure in FEX";
  if (t.cfg.is32) return "32-bit";
  if (t.category == "MMX" || t.category == "3DNow") return "MMX/3DNow";
  if (t.category == "SelfModifyingCode") return "self-modifying code";
  for (auto& f : t.cfg.features) if (f == "AVX512" || f == "SHA" || f == "CLZERO" || f == "EMMI" || f == "RAND") return "needs host feature " + f;
  return "";
}

} // namespace

int main(int argc, char** argv) {
  setpriority(PRIO_PROCESS, 0, 19);
  std::string fexDir = "out/recomp-fex", nasm = BBR_NASM, outFile = "out/recomp/asmtest.txt", filter;
  int opt = 1; unsigned jobs = 22; bool verbose = false; bool listAll = false, resumeTest = false, hookTest = false, variantTest = false;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* s) { return !strcmp(argv[i], s); };
    auto arg = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing argument\n"); exit(2); } return argv[++i]; };
    if (is("--fex")) fexDir = arg();
    else if (is("--nasm")) nasm = arg();
    else if (is("--out")) outFile = arg();
    else if (is("--filter")) filter = arg();
    else if (is("--jobs")) jobs = std::max(1, atoi(arg()));
    else if (is("-O0")) opt = 0; else if (is("-O1")) opt = 1; else if (is("-O2")) opt = 2;
    else if (is("-v")) verbose = true;
    else if (is("--list")) listAll = true;
    else if (is("--resume-selftest")) resumeTest = true;
    else if (is("--hook-selftest")) hookTest = true;
    else if (is("--variant-selftest")) variantTest = true;
    else { fprintf(stderr, "usage: bb-recomp-asmtest [--fex <FEX tree>] [--nasm <nasm>] [--filter <substr>] [--out <file>] [--jobs n] [-O0|-O1|-O2] [-v]\n"); return 2; }
  }
  jobs = std::min(jobs, 22u);
  if (resumeTest) {
    const pid_t pid = fork();
    if (pid == 0) { const std::string v = runResumeTest(opt); puts(v.c_str()); _exit(v == "PASS" ? 0 : 1); }
    int st = 0; waitpid(pid, &st, 0);
    printf("resume entries self-test: %s\n", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "PASS" : "FAIL");
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
  }
  if (variantTest) {
    const pid_t pid = fork();
    if (pid == 0) { const std::string v = runVariantTest(opt); puts(v.c_str()); fflush(stdout); _exit(v == "PASS" ? 0 : 1); }
    int st = 0; waitpid(pid, &st, 0);
    printf("variant self-test: %s\n", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "PASS" : "FAIL");
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
  }
  if (hookTest) {
    const pid_t pid = fork();
    if (pid == 0) { const std::string v = runHookTest(opt); puts(v.c_str()); fflush(stdout); _exit(v == "PASS" ? 0 : 1); }
    int st = 0; waitpid(pid, &st, 0);
    printf("hook slot self-test: %s\n", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "PASS" : "FAIL");
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
  }
  const fs::path asmDir = fs::path(fexDir) / "unittests/ASM";
  if (!fs::exists(asmDir)) { fprintf(stderr, "no %s\n", asmDir.c_str()); return 1; }
  std::set<std::string> disabled;
  for (const char* f : {"Disabled_Tests", "Known_Failures", "Known_Failures_jit"}) { // (the second: FEX itself expects these to fail)
    std::istringstream ss(slurp(asmDir / f)); std::string l;
    while (std::getline(ss, l)) if (!l.empty() && l[0] != '#') disabled.insert(l.rfind("Test_", 0) == 0 ? l : "Test_" + l);
  }

  std::vector<Test> tests;
  for (auto& e : fs::recursive_directory_iterator(asmDir)) {
    if (!e.is_regular_file() || e.path().extension() != ".asm") continue;
    Test t;
    t.rel = fs::relative(e.path(), asmDir).string();
    if (!filter.empty() && t.rel.find(filter) == std::string::npos) continue;
    const size_t sl = t.rel.find('/');
    t.category = sl == std::string::npos ? "(top level)" : t.rel.substr(0, sl);
    t.text = slurp(e.path());
    t.configOk = parseConfig(t.text, t.cfg);
    t.x87Full = !t.cfg.x87Reduced && t.category != "X87_F64" && usesX87(t.text); // (four X87_F64 tests have a mistyped Env key)
    if (t.x87Full) t.category = "x87 80-bit (info)";
    tests.push_back(std::move(t));
  }
  std::sort(tests.begin(), tests.end(), [](const Test& a, const Test& b) { return a.rel < b.rel; });
  if (listAll) { for (auto& t : tests) printf("%s\n", t.rel.c_str()); return 0; }

  // Assemble everything with nasm first, in parallel.
  const fs::path tmp = fs::temp_directory_path() / ("bbr-asmtest-" + std::to_string(getpid()));
  fs::create_directories(tmp);
  std::atomic<size_t> nextIdx {0};
  std::vector<std::string> asmErr(tests.size());
  auto asmWorker = [&] {
    for (;;) {
      const size_t i = nextIdx++;
      if (i >= tests.size()) break;
      Test& t = tests[i];
      if (!skipReason(t, disabled).empty()) continue;
      const fs::path src = tmp / (std::to_string(i) + ".asm"), bin = tmp / (std::to_string(i) + ".bin");
      { std::ofstream f(src); f << "BITS 64\n" << t.text << "\nret\n"; }
      const std::string cmd = nasm + " -i " + (asmDir / "Includes").string() + "/ " + src.string() + " -o " + bin.string() + " 2>" + (tmp / (std::to_string(i) + ".err")).string();
      if (system(cmd.c_str()) != 0) asmErr[i] = slurp(tmp / (std::to_string(i) + ".err")).substr(0, 200);
      else t.bin = slurp(bin);
    }
  };
  { std::vector<std::thread> th; for (unsigned i = 0; i < jobs; ++i) th.emplace_back(asmWorker); for (auto& x : th) x.join(); }

  // Run each test in a forked child, up to `jobs` at a time.
  struct Run { pid_t pid; int fd; size_t idx; };
  std::vector<std::string> verdict(tests.size());
  std::map<pid_t, Run> running;
  size_t nextTest = 0, done = 0;
  auto spawn = [&](size_t i) {
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); exit(1); }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      close(p[0]);
      std::string v = runTest(tests[i], opt);
      if (write(p[1], v.data(), v.size()) < 0) _exit(98);
      _exit(0);
    }
    close(p[1]);
    running[pid] = {pid, p[0], i};
  };
  while (done < tests.size()) {
    while (running.size() < jobs && nextTest < tests.size()) {
      const size_t i = nextTest++;
      Test& t = tests[i];
      std::string sk = skipReason(t, disabled);
      if (!sk.empty()) { verdict[i] = "SKIP " + sk; ++done; continue; }
      if (!t.configOk) { verdict[i] = "SKIP unparsable CONFIG"; ++done; continue; }
      if (!asmErr[i].empty()) { verdict[i] = "SKIP nasm failed: " + asmErr[i]; ++done; continue; }
      spawn(i);
    }
    if (running.empty()) continue;
    int status;
    const pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) break;
    auto it = running.find(pid);
    if (it == running.end()) continue;
    char buf[4096]; std::string v;
    ssize_t n;
    while ((n = read(it->second.fd, buf, sizeof buf)) > 0) v.append(buf, n);
    close(it->second.fd);
    if (v.empty()) v = WIFSIGNALED(status) ? "CRASH killed by signal " + std::to_string(WTERMSIG(status)) : "CRASH exit " + std::to_string(WEXITSTATUS(status));
    verdict[it->second.idx] = v;
    running.erase(it);
    ++done;
  }
  fs::remove_all(tmp);

  // Report.
  std::map<std::string, Counts> cats;
  std::map<std::string, unsigned> unsupOps, skipReasons;
  std::string failures;
  for (size_t i = 0; i < tests.size(); ++i) {
    Counts& c = cats[tests[i].category];
    const std::string& v = verdict[i];
    if (v.rfind("PASS", 0) == 0) ++c.pass;
    else if (v.rfind("SKIP", 0) == 0) { ++c.skip; ++skipReasons[v.substr(5)]; }
    else if (v.rfind("UNSUP", 0) == 0) {
      ++c.unsup;
      std::istringstream ss(v.substr(5)); std::string o; while (ss >> o) ++unsupOps[o];
    } else if (v.rfind("LIFT", 0) == 0) { ++c.lift; failures += "LIFT   " + tests[i].rel + ": " + v.substr(std::min<size_t>(5, v.size())) + "\n"; }
    else if (v.rfind("CRASH", 0) == 0) { ++c.crash; failures += "CRASH  " + tests[i].rel + ": " + v.substr(std::min<size_t>(6, v.size())) + "\n"; }
    else { ++c.fail; failures += "FAIL   " + tests[i].rel + ": " + (v.size() > 5 ? v.substr(5) : v) + "\n"; }
    if (verbose) printf("%-60s %s\n", tests[i].rel.c_str(), v.c_str());
  }
  std::ostringstream out;
  char line[256];
  snprintf(line, sizeof line, "%-22s %6s %6s %6s %6s %6s %6s   %s\n", "category", "pass", "fail", "unsup", "liftx", "crash", "skip", "pass of runnable");
  out << line;
  Counts tot; unsigned allRun = 0;
  Counts info;
  for (auto& [name, c] : cats) {
    if (name == "x87 80-bit (info)") { info = c; continue; }
    const unsigned run = c.pass + c.fail + c.crash;
    snprintf(line, sizeof line, "%-22s %6u %6u %6u %6u %6u %6u   %.1f%% (%u/%u)\n", name.c_str(), c.pass, c.fail, c.unsup, c.lift, c.crash, c.skip,
             run ? 100.0 * c.pass / run : 0.0, c.pass, run);
    out << line;
    tot.pass += c.pass; tot.fail += c.fail; tot.unsup += c.unsup; tot.lift += c.lift; tot.crash += c.crash; tot.skip += c.skip;
    allRun += run;
  }
  snprintf(line, sizeof line, "%-22s %6u %6u %6u %6u %6u %6u   %.1f%% (%u/%u)\n", "TOTAL", tot.pass, tot.fail, tot.unsup, tot.lift, tot.crash, tot.skip,
           allRun ? 100.0 * tot.pass / allRun : 0.0, tot.pass, allRun);
  out << line;
  {
    const unsigned run = info.pass + info.fail + info.crash;
    snprintf(line, sizeof line, "%-22s %6u %6u %6u %6u %6u %6u   %.1f%% (%u/%u)   (not in TOTAL: written for 80-bit x87)\n", "x87 80-bit (info)", info.pass, info.fail, info.unsup, info.lift, info.crash,
             info.skip, run ? 100.0 * info.pass / run : 0.0, info.pass, run);
    out << line;
  }
  out << "\n'pass of runnable' = pass / (pass + fail + crash): the tests whose ops are all lowered. unsup = an IR op still lowers to bbr_trap;\n"
         "liftx = FEX's front end could not lift the test; skip = not attempted (see below).\n";
  out << "\nskipped:\n";
  for (auto& [r, n] : skipReasons) { snprintf(line, sizeof line, "  %5u  %s\n", n, r.c_str()); out << line; }
  std::vector<std::pair<unsigned, std::string>> uo;
  for (auto& [o, n] : unsupOps) uo.push_back({n, o});
  std::sort(uo.rbegin(), uo.rend());
  out << "\nunsupported ops (tests blocked):\n";
  for (auto& [n, o] : uo) { snprintf(line, sizeof line, "  %5u  %s\n", n, o.c_str()); out << line; }
  out << "\nfailures:\n" << failures;
  fputs(out.str().c_str(), stdout);
  fs::create_directories(fs::path(outFile).parent_path());
  std::ofstream(outFile) << out.str();
  return 0;
}
