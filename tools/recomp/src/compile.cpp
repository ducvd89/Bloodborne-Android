#include "compile.h"
#include "lower_llvm.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#else
#include <windows.h>
#endif

using namespace llvm;
using clk = std::chrono::steady_clock;

namespace {
double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

struct ShardStats {
  double liftLower = 0, opt = 0, codegen = 0;
  uint64_t funcs = 0, units = 0, unitsWithTrap = 0, trapSites = 0, irOps = 0, directCalls = 0, dispatchCalls = 0, relocConsts = 0;
  uint64_t liftFailed = 0, objBytes = 0, textBytes = 0;
  uint64_t resumeCases = 0, switches = 0, switchCases = 0, indJmps = 0, jmpResolved = 0, jmpScanned = 0, jmpMem = 0, jmpReg = 0, jmpFailed = 0, tableBytes = 0;
  std::vector<uint64_t> unitVaddrs;
  std::vector<uint64_t> trapVaddrs; // units with an unsupported-op trap: bbr_dirty starts at 1 for them (the fallback CPU runs them)
  std::map<std::string, uint64_t> opUnits; // op name -> units containing a trap for it
};

std::string shardPath(const std::string& base, unsigned shards, unsigned k, const char* ext) {
  if (shards <= 1) return base;
  std::string s = base;
  const size_t dot = s.rfind('.');
  char b[32]; snprintf(b, sizeof b, ".%03u", k);
  if (dot == std::string::npos || dot < s.rfind('/')) return s + b + ext;
  return s.substr(0, dot) + b + s.substr(dot);
}

uint64_t textSize(const std::string& obj) {
  auto buf = MemoryBuffer::getMemBuffer(obj, "", false);
  auto o = object::ObjectFile::createObjectFile(buf->getMemBufferRef());
  if (!o) { consumeError(o.takeError()); return 0; }
  uint64_t n = 0;
  for (auto& s : (*o)->sections()) if (s.isText()) n += s.getSize();
  return n;
}

struct Sink : UnitSink {
  UnitLowerer& ul; ShardStats& st;
  Sink(UnitLowerer& u, ShardStats& s) : ul(u), st(s) {}
  void unit(uint64_t entry, bool, const void* ir, const UnitMeta& meta) override {
    LowerResult lr;
    ul.lower(entry, ir, &lr, &meta);
    ++st.units; st.unitVaddrs.push_back(entry);
    st.trapSites += lr.trapSites; st.irOps += lr.irOps; st.directCalls += lr.directCalls; st.dispatchCalls += lr.dispatchCalls;
    st.relocConsts += lr.relocConstants; st.switches += lr.switches; st.switchCases += lr.switchCases; st.resumeCases += lr.resumeCases;
    if (!lr.unsupported.empty()) { ++st.unitsWithTrap; st.trapVaddrs.push_back(entry); for (auto& n : lr.unsupported) ++st.opUnits[n]; }
  }
};

// What the pre-pass learns about each unit: whether it is a function start, and where its code ends.
struct UnitInfo { uint64_t entry; uint64_t end; bool extra; };
struct RecordSink : UnitSink {
  std::vector<UnitInfo>& out;
  explicit RecordSink(std::vector<UnitInfo>& o) : out(o) {}
  void unit(uint64_t entry, bool extra, const void*, const UnitMeta& meta) override { out.push_back({entry, meta.end, extra}); }
};

uint64_t fnv1a(const std::vector<uint8_t>& d) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : d) { h ^= b; h *= 1099511628211ull; }
  return h;
}

// unitHashes(ends)[i]: FNV-1a 64 of the image bytes [units[i], ends[i]) (clamped to the unit's executable segment) as the runtime sees them in memory. The runtime hashes the
// same ranges of the loaded image once, before the first guest call: a different hash means the code was patched (hooks, byte
// patches) or the image is another one, and that unit is dirty. bb-prepare patches fs -> gs segment prefixes (0x64 -> 0x65) in the
// image, so 0x65 hashes as 0x64. Units in every executable segment (the eboot and the linked modules) are covered.
std::vector<uint64_t> unitHashes(const ElfImage& img, const std::vector<uint64_t>& units, const std::vector<uint64_t>& ends) {
  std::vector<uint64_t> out(units.size(), 0);
  for (size_t i = 0; i < units.size(); ++i) {
    for (auto& s : img.loads) {
      if (!(s.flags & 1) || units[i] < s.vaddr || units[i] >= s.vaddr + s.memsz) continue;
      const uint64_t lo = units[i] - s.vaddr, hi = std::min(ends[i] - s.vaddr, s.memsz);
      uint64_t h = 1469598103934665603ull;
      for (uint64_t k = lo; k < hi; ++k) {
        uint8_t b = k < s.filesz ? img.file[s.offset + k] : 0;
        if (b == 0x65) b = 0x64;
        h ^= b; h *= 1099511628211ull;
      }
      out[i] = h;
      break;
    }
  }
  return out;
}

// bbr_units[] (sorted), bbr_unit_count, bbr_unit_end[], bbr_unit_hash[], bbr_unit_extra[], bbr_text_end, bbr_dirty[], bbr_funcs[],
// bbr_func_count, bbr_abi_flags, bbr_image_hash, bbr_abi_version, and the bbr_image_base the loader sets.
bool emitTable(const std::vector<uint64_t>& units, const std::vector<uint64_t>& ends, const std::vector<uint64_t>& hashes, const std::vector<uint64_t>& ehashes, const std::vector<uint8_t>& extra, uint64_t textEnd,
               const std::vector<uint8_t>& dirtyInit, const std::vector<Func>& funcs, uint64_t imageHash, const CompileOptions& opt, TargetMachine& tm, const std::string& path) {
  LLVMContext ctx;
  Module m("bbr_table", ctx);
  setModuleTarget(m, tm);
  auto* i64 = Type::getInt64Ty(ctx); auto* i32 = Type::getInt32Ty(ctx); auto* i8 = Type::getInt8Ty(ctx); auto* ptr = PointerType::getUnqual(ctx);
  auto* ent = StructType::get(ctx, {i64, ptr});
  std::vector<Constant*> es;
  es.reserve(units.size());
  auto* fty = FunctionType::get(Type::getVoidTy(ctx), {ptr}, false);
  for (uint64_t v : units) {
    auto* f = Function::Create(fty, GlobalValue::ExternalLinkage, unitName(v), &m);
    f->setVisibility(GlobalValue::HiddenVisibility);
    es.push_back(ConstantStruct::get(ent, {ConstantInt::get(i64, v), f}));
  }
  auto* aty = ArrayType::get(ent, es.size());
  new GlobalVariable(m, aty, true, GlobalValue::ExternalLinkage, ConstantArray::get(aty, es), "bbr_units");
  new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, units.size()), "bbr_unit_count");
  auto arr64 = [&](const std::vector<uint64_t>& v, const char* name) {
    std::vector<Constant*> xs; xs.reserve(v.size());
    for (uint64_t h : v) xs.push_back(ConstantInt::get(i64, h));
    auto* t = ArrayType::get(i64, xs.size());
    new GlobalVariable(m, t, true, GlobalValue::ExternalLinkage, ConstantArray::get(t, xs), name);
  };
  arr64(ends, "bbr_unit_end");       // end vaddr of each unit's code (just past its last decoded byte)
  arr64(hashes, "bbr_unit_hash");    // over [units[i], units[i+1]) (the last unit of a segment: to its end)
  arr64(ehashes, "bbr_unit_ehash");  // over [units[i], bbr_unit_end[i]): exact, for marking units dirty
  new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, textEnd), "bbr_text_end"); // end of the last executable segment
  {
    // bbr_unit_extra[i]: 1 for a unit that is not the start of a function (case bodies, landing pads, setjmp return sites, jump targets
    // into the middle of a function): it belongs to the function of the nearest preceding unit with 0, and is entered by jumps.
    std::vector<Constant*> xs; xs.reserve(extra.size());
    for (uint8_t e : extra) xs.push_back(ConstantInt::get(i8, e));
    auto* xty = ArrayType::get(i8, xs.size());
    new GlobalVariable(m, xty, true, GlobalValue::ExternalLinkage, ConstantArray::get(xty, xs), "bbr_unit_extra");
    // bbr_dirty[i]: written by the runtime (1: the code of unit i changed); every unit tests its byte first (opt.dirtyCheck)
    // A unit holding a bbr_trap for an IR op the lowering lacks starts dirty: a trap in the middle of a unit cannot be resumed,
    // so the runtime runs that unit's code on the fallback CPU from its entry (the unit itself never runs).
    auto* dty = ArrayType::get(i8, units.size());
    Constant* dinit = ConstantAggregateZero::get(dty);
    if (std::find(dirtyInit.begin(), dirtyInit.end(), 1) != dirtyInit.end()) {
      std::vector<Constant*> ds; ds.reserve(dirtyInit.size());
      for (uint8_t d : dirtyInit) ds.push_back(ConstantInt::get(i8, d));
      dinit = ConstantArray::get(dty, ds);
    }
    new GlobalVariable(m, dty, false, GlobalValue::ExternalLinkage, dinit, "bbr_dirty");
  }
  // bbr_funcs[]: the functions in address order with the contiguous run of bbr_units[] entries inside each (the entry unit
  // first, then gap and secondary units): how a dispatcher finds the units that may hold a mid-function address.
  {
    auto* fent = StructType::get(ctx, {i32, i32, i32, i32}); // start, end, first unit index, unit count
    std::vector<Constant*> fs;
    fs.reserve(funcs.size());
    for (const Func& f : funcs) {
      const size_t a = std::lower_bound(units.begin(), units.end(), f.start) - units.begin();
      const size_t b = std::lower_bound(units.begin(), units.end(), f.end) - units.begin();
      fs.push_back(ConstantStruct::get(fent, {ConstantInt::get(i32, f.start), ConstantInt::get(i32, f.end), ConstantInt::get(i32, a), ConstantInt::get(i32, b - a)}));
    }
    auto* faty = ArrayType::get(fent, fs.size());
    new GlobalVariable(m, faty, true, GlobalValue::ExternalLinkage, ConstantArray::get(faty, fs), "bbr_funcs");
    new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, fs.size()), "bbr_func_count");
  }
  new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, (opt.resumeEntries ? 1 : 0) | (opt.dirtyCheck ? 2 : 0)), "bbr_abi_flags");
  new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, imageHash), "bbr_image_hash");
  new GlobalVariable(m, i32, true, GlobalValue::ExternalLinkage, ConstantInt::get(i32, 1), "bbr_abi_version");
  new GlobalVariable(m, i64, false, GlobalValue::ExternalLinkage, ConstantInt::get(i64, 0), "bbr_image_base");
  std::string obj;
  if (!emitObject(m, tm, obj)) return false;
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { perror(path.c_str()); return false; }
  fwrite(obj.data(), 1, obj.size(), f);
  fclose(f);
  return true;
}

// Available memory in GB: MemAvailable (Linux), limited by a cgroup v2 memory.max; GlobalMemoryStatusEx on Windows. 0: unknown.
double availableMemoryGB() {
#ifdef _WIN32
  MEMORYSTATUSEX m; m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys / 1073741824.0 : 0.0;
#else
  double avail = 0;
  if (FILE* f = fopen("/proc/meminfo", "r")) {
    char line[256]; unsigned long long kb;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) { avail = kb / 1048576.0; break; }
    fclose(f);
  }
  auto readNum = [](const char* path, double& v) {
    FILE* f = fopen(path, "r"); if (!f) return false;
    char b[64] = {}; const bool ok = fgets(b, sizeof b, f) && isdigit((unsigned char)b[0]); fclose(f);
    if (ok) v = atof(b) / 1073741824.0;
    return ok;
  };
  double lim, cur;
  if (readNum("/sys/fs/cgroup/memory.max", lim) && readNum("/sys/fs/cgroup/memory.current", cur) && lim - cur > 0 && (avail == 0 || lim - cur < avail)) avail = lim - cur;
  return avail;
#endif
}

// Memory model, measured on the game (docs/STATIC_RECOMPILE_PLAN.md, "Compile time"): a worker holds one shard as an LLVM
// module plus its optimiser and code generator state, about 1.15 GB per MB of x86 code in the shard (+ 0.04 GB fixed);
// the process keeps the image (twice: the file and its mapping), the lifter tables and the FDE list, about 0.5 GB.
// jobs x (0.04 + 1.15 x shardMB) + 0.5 <= memFraction x available. Smaller shards cost nothing (64 -> 256 shards: same wall
// time), so the shard size shrinks before the worker count does.
struct MemPlan { unsigned jobs, shards; double availGB = 0, budgetGB = 0, shardMB = 0, estPeakGB = 0; bool limited = false; };
MemPlan planMemory(const CompileOptions& opt, double codeMB, size_t nfuncs) {
  constexpr double kBase = 0.5, kFixed = 0.04, kPerMB = 1.15, kMinShardMB = 0.08;
  MemPlan p;
  p.jobs = std::max(1u, opt.jobs);
  p.shards = std::max(1u, opt.shards);
  p.availGB = availableMemoryGB();
  if (const char* e = getenv("BBR_FAKE_MEM_GB")) p.availGB = atof(e); // test hook: pretend this much RAM is available
  p.budgetGB = p.availGB * opt.memFraction;
  const double want = std::max(codeMB / p.shards, 0.0001); // shard size of the requested configuration
  double shardMB = want;
  unsigned jobs = p.jobs;
  if (p.availGB > 0) {
    auto fit = [&](unsigned j) { return ((p.budgetGB - kBase) / j - kFixed) / kPerMB; }; // largest shard (MB) j workers can hold
    if (opt.shardsFixed) {
      if (!opt.jobsFixed) while (jobs > 1 && kBase + jobs * (kFixed + kPerMB * want) > p.budgetGB) --jobs;
    } else if (opt.jobsFixed) {
      shardMB = std::min(want, std::max(fit(jobs), 0.02));
    } else {
      while (jobs > 1 && fit(jobs) < kMinShardMB) --jobs;       // workers only come down when the shards cannot get smaller
      shardMB = std::min(want, std::max(fit(jobs), 0.02));
    }
    p.limited = jobs < p.jobs || shardMB < want * 0.999;
  }
  p.jobs = jobs;
  if (!opt.shardsFixed) p.shards = std::max<unsigned>(p.shards, (unsigned)std::ceil(codeMB / shardMB));
  p.shards = std::min<unsigned>(p.shards, (unsigned)std::max<size_t>(1, nfuncs));
  p.jobs = std::min(p.jobs, p.shards);
  p.shardMB = codeMB / p.shards;
  p.estPeakGB = kBase + p.jobs * (kFixed + kPerMB * p.shardMB);
  return p;
}

FILE* g_sf = nullptr;
__attribute__((format(printf, 1, 2))) void say(const char* fmt, ...) {
  va_list a, b; va_start(a, fmt); va_copy(b, a);
  vfprintf(stdout, fmt, a); if (g_sf) vfprintf(g_sf, fmt, b);
  va_end(a); va_end(b);
}
} // namespace

int runCompile(const ElfImage& img, const std::vector<Func>& funcs, const CompileOptions& opt) {
  const auto t0 = clk::now();
  initLlvmTargets();
  if (!opt.statsPath.empty()) g_sf = fopen(opt.statsPath.c_str(), "w");

  // Functions to compile.
  std::vector<size_t> todo;
  if (opt.onlyVaddr >= 0) {
    for (size_t i = 0; i < funcs.size(); ++i)
      if ((uint64_t)opt.onlyVaddr >= funcs[i].start && (uint64_t)opt.onlyVaddr < funcs[i].end) { todo.push_back(i); break; }
    if (todo.empty()) { fprintf(stderr, "bb-recomp: no function covers %#" PRIx64 "\n", (uint64_t)opt.onlyVaddr); return 1; }
  } else {
    for (size_t i = 0; i < funcs.size() && (!opt.limit || todo.size() < opt.limit); ++i) todo.push_back(i);
  }
  uint64_t totalBytes = 0;
  for (size_t i : todo) totalBytes += funcs[i].end - funcs[i].start;
  const bool wantCode0 = !opt.objPath.empty() || !opt.soPath.empty();
  MemPlan plan {std::max(1u, opt.jobs), std::max(1u, opt.shards)};
  if (wantCode0 || !opt.llPath.empty() || opt.planOnly) plan = planMemory(opt, totalBytes / 1e6, todo.size());
  if (opt.planOnly) {
    printf("memory plan: %.1f GB available, budget %.1f GB (%.0f%%): %u jobs, %u shards of %.2f MB (%.1f MB of code), estimated peak %.1f GB%s\n", plan.availGB, plan.budgetGB, opt.memFraction * 100,
           plan.jobs, plan.shards, plan.shardMB, totalBytes / 1e6, plan.estPeakGB, plan.limited ? " (reduced to fit)" : "");
    return 0;
  }
  const unsigned nshards = std::max(1u, std::min<unsigned>(plan.shards, todo.size()));
  // Shards of equal code size (compile time follows bytes, and the function sizes are very skewed).
  std::vector<size_t> bound(nshards + 1, 0);
  {
    uint64_t acc = 0; unsigned k = 1;
    for (size_t j = 0; j < todo.size(); ++j) {
      while (k < nshards && acc >= totalBytes * k / nshards) bound[k++] = j;
      acc += funcs[todo[j]].end - funcs[todo[j]].start;
    }
    while (k < nshards) bound[k++] = todo.size();
    bound[nshards] = todo.size();
  }

  // Pre-pass: lift every function once, only to learn the unit set (entries, ends, which are not function starts). Direct calls and
  // jumps then go straight to any unit, not only to function starts, and bbr_dirty[] gets a fixed index per unit. Lifting is about
  // a tenth of the work, and the units it makes are the ones the second pass makes (same lifter, same options).
  std::vector<UnitInfo> pre;
  {
    std::vector<std::vector<UnitInfo>> part(std::max(1u, plan.jobs));
    std::atomic<size_t> nextF {0};
    auto w = [&](unsigned id) {
      Lifter lifter(img, opt.lift);
      RecordSink rs(part[id]);
      for (;;) {
        const size_t j = nextF++;
        if (j >= todo.size()) break;
        const Func& f = funcs[todo[j]];
        FuncResult fr;
        lifter.lift(f, fr, nullptr, nullptr, nullptr, &rs);
        if (fr.status == LiftStatus::Failed || fr.status == LiftStatus::Crashed) part[id].push_back({f.start, f.end, false});
      }
    };
    std::vector<std::thread> th;
    for (unsigned i = 0; i < part.size(); ++i) th.emplace_back(w, i);
    for (auto& t : th) t.join();
    for (auto& p : part) pre.insert(pre.end(), p.begin(), p.end());
    std::sort(pre.begin(), pre.end(), [](const UnitInfo& a, const UnitInfo& b) { return a.entry < b.entry || (a.entry == b.entry && a.end > b.end); });
    pre.erase(std::unique(pre.begin(), pre.end(), [](const UnitInfo& a, const UnitInfo& b) { return a.entry == b.entry; }), pre.end());
  }
  std::vector<uint64_t> starts; // sorted unit entries
  starts.reserve(pre.size());
  for (auto& u : pre) starts.push_back(u.entry);
  const bool wantCode = !opt.objPath.empty() || !opt.soPath.empty();
  const bool emitLl = !opt.llPath.empty();
  LowerOptions lo;
  lo.liftBase = img.base;
  lo.imageSize = 0;
  for (auto& s : img.loads) lo.imageSize = std::max(lo.imageSize, s.vaddr + s.memsz);
  lo.aarch64 = opt.target.aarch64;
  lo.storeFlagsAtCalls = opt.storeFlagsAtCalls;
  lo.calleeSaved = opt.calleeSaved;
  lo.resumeEntries = opt.resumeEntries;
  lo.isKnownUnit = [&starts](uint64_t v) { return std::binary_search(starts.begin(), starts.end(), v); };
  lo.dirtyCheck = opt.dirtyCheck;
  lo.unitIndex = [&starts](uint64_t v) -> int64_t { auto it = std::lower_bound(starts.begin(), starts.end(), v); return it != starts.end() && *it == v ? it - starts.begin() : -1; };

  std::vector<ShardStats> stats(nshards);
  std::vector<std::string> objFiles(nshards);
  std::atomic<unsigned> next {0};
  std::atomic<bool> failed {false};
  const unsigned jobs = std::max(1u, std::min(plan.jobs, nshards));
  std::mutex errMu;

  auto worker = [&] {
    Lifter lifter(img, opt.lift);
    auto tm = makeTargetMachine(opt.target);
    if (!tm) { failed = true; return; }
    for (;;) {
      const unsigned k = next++;
      if (k >= nshards) break;
      ShardStats& st = stats[k];
      LLVMContext ctx;
      auto mod = std::make_unique<Module>("bbr_shard_" + std::to_string(k), ctx);
      setModuleTarget(*mod, *tm);
      UnitLowerer ul(ctx, *mod, lo);
      Sink sink(ul, st);
      auto t = clk::now();
      for (size_t j = bound[k]; j < bound[k + 1]; ++j) {
        const Func& f = funcs[todo[j]];
        FuncResult fr;
        lifter.lift(f, fr, nullptr, nullptr, nullptr, &sink);
        ++st.funcs;
        st.indJmps += fr.indJmps; st.jmpResolved += fr.jmpResolved; st.jmpScanned += fr.jmpScanned; st.jmpMem += fr.jmpMem; st.jmpReg += fr.jmpReg; st.jmpFailed += fr.jmpFailed; st.tableBytes += fr.tableBytes;
        if (fr.status == LiftStatus::Failed || fr.status == LiftStatus::Crashed) {
          ++st.liftFailed; ++st.units; st.unitVaddrs.push_back(f.start);
          ul.stub(f.start);
        }
      }
      st.liftLower = since(t);
      if (opt.verify) {
        std::string e; raw_string_ostream os(e);
        if (verifyModule(*mod, &os)) { std::lock_guard<std::mutex> g(errMu); fprintf(stderr, "bb-recomp: shard %u: LLVM verifier: %s\n", k, e.substr(0, 2000).c_str()); failed = true; }
      }
      if (emitLl) {
        std::error_code ec;
        raw_fd_ostream os(shardPath(opt.llPath, nshards, k, ".ll"), ec);
        if (ec) { fprintf(stderr, "bb-recomp: %s\n", ec.message().c_str()); failed = true; continue; }
        mod->print(os, nullptr);
      }
      if (wantCode) {
        t = clk::now();
        optimizeModule(*mod, *tm, opt.target.opt);
        st.opt = since(t);
        t = clk::now();
        std::string obj;
        if (!emitObject(*mod, *tm, obj)) { failed = true; continue; }
        st.codegen = since(t);
        st.objBytes = obj.size();
        st.textBytes = textSize(obj);
        const std::string path = shardPath(opt.objPath.empty() ? opt.soPath + ".o" : opt.objPath, nshards, k, ".o");
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { perror(path.c_str()); failed = true; continue; }
        fwrite(obj.data(), 1, obj.size(), f);
        fclose(f);
        objFiles[k] = path;
      }
    }
  };
  std::vector<std::thread> th;
  for (unsigned i = 0; i < jobs; ++i) th.emplace_back(worker);
  for (auto& t : th) t.join();
  if (failed) return 1;

  // Totals.
  ShardStats tot;
  std::vector<uint64_t> units, trapUnits;
  for (auto& s : stats) {
    tot.liftLower += s.liftLower; tot.opt += s.opt; tot.codegen += s.codegen;
    tot.funcs += s.funcs; tot.units += s.units; tot.unitsWithTrap += s.unitsWithTrap; tot.trapSites += s.trapSites; tot.irOps += s.irOps;
    tot.directCalls += s.directCalls; tot.dispatchCalls += s.dispatchCalls; tot.relocConsts += s.relocConsts; tot.liftFailed += s.liftFailed;
    tot.objBytes += s.objBytes; tot.textBytes += s.textBytes;
    tot.resumeCases += s.resumeCases; tot.switches += s.switches; tot.switchCases += s.switchCases; tot.indJmps += s.indJmps; tot.jmpResolved += s.jmpResolved; tot.jmpScanned += s.jmpScanned;
    tot.jmpMem += s.jmpMem; tot.jmpReg += s.jmpReg; tot.jmpFailed += s.jmpFailed; tot.tableBytes += s.tableBytes;
    for (auto& [n, c] : s.opUnits) tot.opUnits[n] += c;
    units.insert(units.end(), s.unitVaddrs.begin(), s.unitVaddrs.end());
    trapUnits.insert(trapUnits.end(), s.trapVaddrs.begin(), s.trapVaddrs.end());
  }
  std::sort(trapUnits.begin(), trapUnits.end());
  std::sort(units.begin(), units.end());
  units.erase(std::unique(units.begin(), units.end()), units.end());

  double tableTime = 0, helperTime = 0;
  std::string tablePath;
  if (wantCode && opt.table) {
    auto t = clk::now();
    auto tm = makeTargetMachine(opt.target);
    tablePath = (opt.objPath.empty() ? opt.soPath : opt.objPath);
    const size_t dot = tablePath.rfind('.');
    tablePath = (dot == std::string::npos ? tablePath : tablePath.substr(0, dot)) + ".table.o";
    uint64_t textEnd = 0;
    for (auto& sg : img.loads) if (sg.flags & 1) textEnd = std::max(textEnd, sg.vaddr + sg.memsz);
    if (units != starts) { fprintf(stderr, "bb-recomp: the units of the second pass (%zu) differ from the pre-pass (%zu)\n", units.size(), starts.size()); return 1; }
    std::vector<uint64_t> ends(pre.size());
    std::vector<uint8_t> extraFlags(pre.size());
    for (size_t i = 0; i < pre.size(); ++i) { ends[i] = pre[i].end > pre[i].entry ? pre[i].end : (i + 1 < pre.size() ? pre[i + 1].entry : textEnd); extraFlags[i] = pre[i].extra; }
    std::vector<Func> compiled;
    compiled.reserve(todo.size());
    for (size_t i : todo) compiled.push_back(funcs[i]);
    std::vector<uint64_t> nexts(pre.size());
    for (size_t i = 0; i < pre.size(); ++i) nexts[i] = i + 1 < pre.size() ? pre[i + 1].entry : textEnd;
    std::vector<uint8_t> dirtyInit(units.size(), 0);
    for (size_t i = 0; i < units.size(); ++i) dirtyInit[i] = std::binary_search(trapUnits.begin(), trapUnits.end(), units[i]);
    if (!emitTable(units, ends, unitHashes(img, units, nexts), unitHashes(img, units, ends), extraFlags, textEnd, dirtyInit, compiled, fnv1a(img.file), opt, *tm, tablePath)) return 1;
    tableTime = since(t);
  }

  // The x87 helpers (tools/recomp/helpers, docs/RECOMP_ABI.md): plain C, compiled for the target with its own clang and
  // linked into the .so (or written next to the objects), so that the runtime has nothing to provide for them.
  std::string helperObj;
  if (wantCode) {
    auto t = clk::now();
    std::string base = opt.objPath.empty() ? opt.soPath : opt.objPath;
    const size_t dot = base.rfind('.');
    helperObj = (dot == std::string::npos || dot < base.rfind('/') + 1 ? base : base.substr(0, dot)) + ".helpers.o";
    std::string dir = opt.helpersDir.empty() ? std::string(BBR_HELPERS_DIR) : opt.helpersDir;
    std::string cmd;
    if (opt.target.aarch64) {
      cmd = "nice -n 19 " + opt.ndk + "/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --target=aarch64-linux-android30";
    } else {
      cmd = "nice -n 19 clang";
    }
    cmd += " -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off -c " + dir + "/bbr_x87_all.c -o " + helperObj;
    if (system(cmd.c_str()) != 0) { fprintf(stderr, "bb-recomp: compiling the x87 helpers failed: %s\n", cmd.c_str()); return 1; }
    helperTime = since(t);
  }

  double linkTime = 0;
  uint64_t soBytes = 0;
  if (!opt.soPath.empty()) {
    auto t = clk::now();
    std::string cmd;
    if (opt.target.aarch64) {
      const std::string bin = opt.ndk + "/toolchains/llvm/prebuilt/linux-x86_64";
      cmd = "nice -n 19 " + bin + "/bin/ld.lld -shared -m aarch64linux --hash-style=gnu -z noexecstack -z max-page-size=16384 -Bsymbolic --build-id=none "
            "-soname " + opt.soPath.substr(opt.soPath.rfind('/') + 1) + " -o " + opt.soPath;
      for (auto& o : objFiles) cmd += " " + o;
      if (!tablePath.empty()) cmd += " " + tablePath;
      cmd += " " + helperObj;
      cmd += " -L" + bin + "/sysroot/usr/lib/aarch64-linux-android/30 -lm"; // the helpers call libm (sin, exp2, fmod ...)
      cmd += " " + bin + "/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a";
      // DT_NEEDED libbbcpu.so: the bbr_* helpers live there, and Android's linker resolves a dlopen()ed library's symbols only
      // against the global group (the executable, LD_PRELOAD) and the library's own DT_NEEDED tree, not against the
      // executable's DT_NEEDED libraries as glibc does (dlopen failed: cannot locate symbol "bbr_dispatch"). A soname stub
      // made here gives the entry; at run time it is the libbbcpu.so already loaded.
      const std::string stub = opt.soPath + ".libbbcpu-stub.so";
      const std::string mk = "nice -n 19 " + bin + "/bin/clang --target=aarch64-linux-android30 -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o " + stub;
      if (system(mk.c_str()) != 0) { fprintf(stderr, "bb-recomp: cannot make %s\n", stub.c_str()); return 1; }
      cmd += " --no-as-needed " + stub;
    } else {
      // The PC runtime (BB_AOT with an x86-64 host): the system clang links it with ld.lld and adds the builtins
      // (__divti3, __udivti3). The bbr_* helpers stay undefined here: the executable provides them.
      cmd = "nice -n 19 clang -shared -fuse-ld=lld -Wl,-Bsymbolic -Wl,-z,noexecstack -Wl,--build-id=none -o " + opt.soPath;
      for (auto& o : objFiles) cmd += " " + o;
      if (!tablePath.empty()) cmd += " " + tablePath;
      cmd += " " + helperObj + " -lm";
    }
    const int rc = system(cmd.c_str());
    linkTime = since(t);
    if (rc != 0) { fprintf(stderr, "bb-recomp: link failed (%d): %s\n", rc, cmd.c_str()); return 1; }
    FILE* f = fopen(opt.soPath.c_str(), "rb");
    if (f) { fseek(f, 0, SEEK_END); soBytes = ftell(f); fclose(f); }
  }

  const double wall = since(t0);
  long rssKb = 0;
#ifndef _WIN32
  struct rusage ru; if (getrusage(RUSAGE_SELF, &ru) == 0) rssKb = ru.ru_maxrss;
#endif
  say("\n== compile summary (%s, -O%d, %u shards, %u threads) ==\n", opt.target.aarch64 ? "aarch64" : "x86_64", opt.target.opt, nshards, jobs);
  if (plan.availGB > 0)
    say("memory plan: %.1f GB available, budget %.1f GB (%.0f%%): %u jobs, %u shards of %.2f MB, estimated peak %.1f GB%s\n", plan.availGB, plan.budgetGB, opt.memFraction * 100, jobs, nshards,
        plan.shardMB, plan.estPeakGB, plan.limited ? " (reduced to fit)" : "");
  say("functions %" PRIu64 " (%.1f MB), units %" PRIu64 " (lift failures %" PRIu64 "), IR ops lowered %" PRIu64 "\n", tot.funcs, totalBytes / 1e6, tot.units, tot.liftFailed, tot.irOps);
  say("units with bbr_trap (unsupported ops): %" PRIu64 " (%.2f%%), trap sites %" PRIu64 "\n", tot.unitsWithTrap, tot.units ? 100.0 * tot.unitsWithTrap / tot.units : 0.0, tot.trapSites);
  say("indirect jmps %" PRIu64 ": %" PRIu64 " jump tables (%" PRIu64 " bounded + %" PRIu64 " scanned, %.2f MB of tables), %" PRIu64 " jmp [mem], %" PRIu64 " jmp reg tail calls, %" PRIu64 " table-like unresolved; %" PRIu64 " lowered as switch (%" PRIu64 " cases)\n",
      tot.indJmps, tot.jmpResolved + tot.jmpScanned, tot.jmpResolved, tot.jmpScanned, tot.tableBytes / 1e6, tot.jmpMem, tot.jmpReg, tot.jmpFailed, tot.switches, tot.switchCases);
  if (opt.resumeEntries) say("resume entries: %" PRIu64 " call-return blocks reachable through the unit-entry resume switch\n", tot.resumeCases);
  say("direct calls %" PRIu64 ", dispatch calls/jumps %" PRIu64 ", relocated constants %" PRIu64 "\n", tot.directCalls, tot.dispatchCalls, tot.relocConsts);
  if (wantCode) {
    say("CPU time: lift+lower %.1f s, optimise %.1f s, codegen %.1f s (sum over threads); table %.1f s, x87 helpers %.1f s, link %.1f s\n", tot.liftLower, tot.opt, tot.codegen, tableTime, helperTime, linkTime);
    say(".text %.2f MB, objects %.2f MB%s\n", tot.textBytes / 1048576.0, tot.objBytes / 1048576.0, soBytes ? "" : "");
    if (soBytes) say("%s: %.2f MB\n", opt.soPath.c_str(), soBytes / 1048576.0);
  } else {
    say("CPU time: lift+lower %.1f s\n", tot.liftLower);
  }
  say("wall time %.1f s, peak RSS %.2f GB\n", wall, rssKb / 1048576.0);
  if (!tot.opUnits.empty()) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [n, c] : tot.opUnits) v.push_back({c, n});
    std::sort(v.rbegin(), v.rend());
    say("unsupported ops (units containing a trap for the op):\n");
    for (size_t i = 0; i < v.size() && i < 60; ++i) say("  %-28s %" PRIu64 "\n", v[i].second.c_str(), v[i].first);
  }
  if (g_sf) fclose(g_sf);
  return 0;
}
