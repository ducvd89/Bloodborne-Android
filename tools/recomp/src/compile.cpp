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
    if (!lr.unsupported.empty()) { ++st.unitsWithTrap; for (auto& n : lr.unsupported) ++st.opUnits[n]; }
  }
};

uint64_t fnv1a(const std::vector<uint8_t>& d) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : d) { h ^= b; h *= 1099511628211ull; }
  return h;
}

// bbr_units[] (sorted), bbr_unit_count, bbr_image_hash, bbr_abi_version, and the bbr_image_base the loader sets.
bool emitTable(const std::vector<uint64_t>& units, const std::vector<Func>& funcs, uint64_t imageHash, const CompileOptions& opt, TargetMachine& tm, const std::string& path) {
  LLVMContext ctx;
  Module m("bbr_table", ctx);
  setModuleTarget(m, tm);
  auto* i64 = Type::getInt64Ty(ctx); auto* i32 = Type::getInt32Ty(ctx); auto* ptr = PointerType::getUnqual(ctx);
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
  // bbr_funcs[]: the FDE functions in address order with the contiguous run of bbr_units[] entries inside each (the entry unit
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
  new GlobalVariable(m, i64, true, GlobalValue::ExternalLinkage, ConstantInt::get(i64, opt.resumeEntries ? 1 : 0), "bbr_abi_flags");
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

  // Direct calls go to function starts.
  std::vector<uint64_t> starts;
  starts.reserve(funcs.size());
  for (auto& f : funcs) starts.push_back(f.start);
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
  std::vector<uint64_t> units;
  for (auto& s : stats) {
    tot.liftLower += s.liftLower; tot.opt += s.opt; tot.codegen += s.codegen;
    tot.funcs += s.funcs; tot.units += s.units; tot.unitsWithTrap += s.unitsWithTrap; tot.trapSites += s.trapSites; tot.irOps += s.irOps;
    tot.directCalls += s.directCalls; tot.dispatchCalls += s.dispatchCalls; tot.relocConsts += s.relocConsts; tot.liftFailed += s.liftFailed;
    tot.objBytes += s.objBytes; tot.textBytes += s.textBytes;
    tot.resumeCases += s.resumeCases; tot.switches += s.switches; tot.switchCases += s.switchCases; tot.indJmps += s.indJmps; tot.jmpResolved += s.jmpResolved; tot.jmpScanned += s.jmpScanned;
    tot.jmpMem += s.jmpMem; tot.jmpReg += s.jmpReg; tot.jmpFailed += s.jmpFailed; tot.tableBytes += s.tableBytes;
    for (auto& [n, c] : s.opUnits) tot.opUnits[n] += c;
    units.insert(units.end(), s.unitVaddrs.begin(), s.unitVaddrs.end());
  }
  std::sort(units.begin(), units.end());
  units.erase(std::unique(units.begin(), units.end()), units.end());

  double tableTime = 0;
  std::string tablePath;
  if (wantCode && opt.table) {
    auto t = clk::now();
    auto tm = makeTargetMachine(opt.target);
    tablePath = (opt.objPath.empty() ? opt.soPath : opt.objPath);
    const size_t dot = tablePath.rfind('.');
    tablePath = (dot == std::string::npos ? tablePath : tablePath.substr(0, dot)) + ".table.o";
    std::vector<Func> compiled;
    compiled.reserve(todo.size());
    for (size_t i : todo) compiled.push_back(funcs[i]);
    if (!emitTable(units, compiled, fnv1a(img.file), opt, *tm, tablePath)) return 1;
    tableTime = since(t);
  }

  double linkTime = 0;
  uint64_t soBytes = 0;
  if (!opt.soPath.empty()) {
    if (!opt.target.aarch64) { fprintf(stderr, "bb-recomp: --so needs --target aarch64\n"); return 1; }
    auto t = clk::now();
    const std::string bin = opt.ndk + "/toolchains/llvm/prebuilt/linux-x86_64";
    std::string cmd = "nice -n 19 " + bin + "/bin/ld.lld -shared -m aarch64linux --hash-style=gnu -z noexecstack -z max-page-size=16384 -Bsymbolic --build-id=none "
                      "-soname " + opt.soPath.substr(opt.soPath.rfind('/') + 1) + " -o " + opt.soPath;
    for (auto& o : objFiles) cmd += " " + o;
    if (!tablePath.empty()) cmd += " " + tablePath;
    cmd += " " + bin + "/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a";
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
    say("CPU time: lift+lower %.1f s, optimise %.1f s, codegen %.1f s (sum over threads); table %.1f s, link %.1f s\n", tot.liftLower, tot.opt, tot.codegen, tableTime, linkTime);
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
