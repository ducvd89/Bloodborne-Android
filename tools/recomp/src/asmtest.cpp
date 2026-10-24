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
  int opt = 1; unsigned jobs = 22; bool verbose = false; bool listAll = false, resumeTest = false;
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
