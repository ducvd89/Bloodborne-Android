#include "variant_compile.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace llvm;

uint64_t hashRange(const uint8_t* p, uint64_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < n; ++i) {
    uint8_t b = p[i];
    if (b == 0x65) b = 0x64;
    h ^= b; h *= 1099511628211ull;
  }
  return h;
}

namespace {

struct Unit { uint64_t entry, end; bool extra; };
struct RecSink : UnitSink {
  std::vector<Unit>& out;
  explicit RecSink(std::vector<Unit>& o) : out(o) {}
  void unit(uint64_t entry, bool extra, const void*, const UnitMeta& meta) override { out.push_back({entry, meta.end, extra}); }
};

struct Batch {
  std::unique_ptr<LLVMContext> ctx;
  std::unique_ptr<Module> mod;
  std::unique_ptr<UnitLowerer> ul;
  size_t bytes = 0;
  unsigned index = 0;
};

// Optimise + codegen on a small pool; at most `depth` modules wait at a time (a module is big in memory).
class Pool {
 public:
  Pool(const VariantJob& j, unsigned workers) : job(j) {
    for (unsigned i = 0; i < workers; ++i) th.emplace_back([this] { run(); });
    depth = workers;
  }
  void submit(Batch&& b) {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return q.size() < depth; });
    q.push_back(std::move(b));
    cv.notify_all();
  }
  bool finish() {
    { std::lock_guard<std::mutex> lk(mu); done = true; }
    cv.notify_all();
    for (auto& t : th) t.join();
    return !failed;
  }
  std::vector<std::string> files;
  std::mutex mu;
 private:
  void run() {
    auto tm = makeTargetMachine(job.target);
    for (;;) {
      Batch b;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return !q.empty() || done; });
        if (q.empty()) return;
        b = std::move(q.front()); q.pop_front();
        cv.notify_all();
      }
      if (!tm) { failed = true; continue; }
      if (job.verify) {
        std::string e; raw_string_ostream os(e);
        if (verifyModule(*b.mod, &os)) { fprintf(stderr, "bb-recomp: variant module %u: LLVM verifier: %s\n", b.index, e.substr(0, 2000).c_str()); failed = true; continue; }
      }
      optimizeModule(*b.mod, *tm, job.target.opt);
      std::string obj;
      if (!emitObject(*b.mod, *tm, obj)) { failed = true; continue; }
      char nm[32]; snprintf(nm, sizeof nm, ".var%03u.o", b.index);
      const std::string path = job.objBase + nm;
      FILE* f = fopen(path.c_str(), "wb");
      if (!f) { perror(path.c_str()); failed = true; continue; }
      fwrite(obj.data(), 1, obj.size(), f);
      fclose(f);
      std::lock_guard<std::mutex> lk(mu);
      files.push_back(path);
    }
  }
  const VariantJob& job;
  std::vector<std::thread> th;
  std::deque<Batch> q;
  std::condition_variable cv;
  size_t depth = 2;
  bool done = false;
  std::atomic<bool> failed {false};
};

} // namespace

bool compileVariants(const VariantJob& job, VariantResult& out) {
  const auto t0 = std::chrono::steady_clock::now();
  const ElfImage& img = *job.img;
  const std::vector<Func>& funcs = *job.funcs;
  const Catalog& cat = *job.catalog;
  uint64_t imageSize = job.lower.imageSize;

  auto findFunc = [&](uint64_t a) -> long {
    auto it = std::upper_bound(funcs.begin(), funcs.end(), a, [](uint64_t v, const Func& f) { return v < f.start; });
    if (it == funcs.begin()) return -1;
    --it;
    return a < it->end ? long(it - funcs.begin()) : -1;
  };

  // Which functions each group writes into; new code (caves) per group.
  std::vector<std::set<int>> touch(funcs.size());
  std::vector<std::vector<Func>> pseudo(cat.groups.size());
  std::set<int> used;
  for (size_t gi = 0; gi < cat.groups.size(); ++gi) {
    for (auto& w : cat.groups[gi].writes) {
      const uint64_t end = w.vaddr + w.bytes.size();
      if (end > imageSize) { out.notes.push_back("group " + cat.groups[gi].name + ": a write lies outside the image"); return false; }
      if (w.code) { pseudo[gi].push_back({w.vaddr, end, 0, 3}); ++out.cavePieces; continue; }
      bool any = false;
      for (uint64_t a = w.vaddr; a < end;) {
        const long fi = findFunc(a);
        if (fi < 0) {
          auto nx = std::upper_bound(funcs.begin(), funcs.end(), a, [](uint64_t v, const Func& f) { return v < f.start; });
          a = nx == funcs.end() ? end : std::min(end, nx->start);
          continue;
        }
        touch[fi].insert(int(gi)); used.insert(int(gi)); any = true;
        a = funcs[fi].end;
      }
      if (!any) ++out.dataWrites;
    }
  }
  out.groupsUsed = used.size();

  // The entry-unit map the lifter uses, as a private copy: a variant may need more entries than the original (a jump back into the
  // middle of the function from a cave).
  std::map<uint64_t, std::vector<uint64_t>> localEntry;
  if (job.lift.entryUnits) localEntry = *job.lift.entryUnits;
  LiftOptions lift = job.lift;
  lift.entryUnits = &localEntry;
  lift.collectTargets = true;
  Lifter lifter(img, lift);
  auto tm = makeTargetMachine(job.target);
  if (!tm) return false;

  const unsigned workers = std::max(1u, std::min(job.jobs, 8u));
  Pool pool(job, workers);
  Batch batch;
  unsigned nbatch = 0, nextId = 0;
  auto newBatch = [&] {
    batch = Batch();
    batch.ctx.reset(new LLVMContext());
    batch.mod.reset(new Module("bbr_variants_" + std::to_string(nbatch), *batch.ctx));
    setModuleTarget(*batch.mod, *tm);
    LowerOptions lo = job.lower;
    lo.dirtyCheck = false; lo.unitIndex = nullptr; lo.resumeEntries = job.lower.resumeEntries;
    batch.ul.reset(new UnitLowerer(*batch.ctx, *batch.mod, lo));
    batch.index = nbatch++;
  };
  newBatch();
  auto flush = [&] {
    if (batch.mod && batch.bytes) pool.submit(std::move(batch));
    batch = Batch();
  };

  auto unitSet = [&](const std::vector<Unit>& us) { std::set<uint64_t> s; for (auto& u : us) s.insert(u.entry); return s; };

  for (size_t fi = 0; fi < funcs.size(); ++fi) {
    if (touch[fi].empty()) continue;
    const Func& F = funcs[fi];
    {
      auto it = std::lower_bound(job.trapUnits->begin(), job.trapUnits->end(), F.start);
      if (it != job.trapUnits->end() && *it < F.end) { out.notes.push_back("skip function " + unitName(F.start) + ": a unit has unsupported ops (runs on the fallback)"); continue; }
    }
    std::vector<int> g(touch[fi].begin(), touch[fi].end());
    std::vector<unsigned> masks;
    const unsigned n = g.size();
    if (n <= 6) for (unsigned m = 1; m < (1u << n); ++m) masks.push_back(m);
    else { for (unsigned i = 0; i < n; ++i) masks.push_back(1u << i); masks.push_back((1u << n) - 1); out.notes.push_back("function " + unitName(F.start) + " is touched by " + std::to_string(n) + " groups: only single groups and all of them get variants"); }
    for (unsigned mask : masks) {
      std::vector<int> S;
      for (unsigned i = 0; i < n; ++i) if (mask >> i & 1) S.push_back(g[i]);
      bool ok = true;
      for (size_t a = 0; a < S.size() && ok; ++a)
        for (size_t b = a + 1; b < S.size(); ++b)
          if (!cat.groups[S[a]].excl.empty() && cat.groups[S[a]].excl == cat.groups[S[b]].excl) { ok = false; break; }
      if (!ok) continue;
      ++out.variantsPlanned;
      std::string name = unitName(F.start).substr(4);
      for (int gi : S) name += "+" + cat.groups[gi].name;

      // Apply the groups' bytes to the image (undone below).
      struct Saved { uint64_t vaddr; std::vector<uint8_t> old; };
      std::vector<Saved> saved;
      for (int gi : S)
        for (auto& w : cat.groups[gi].writes) {
          Saved s{w.vaddr, std::vector<uint8_t>(w.bytes.size())};
          memcpy(s.old.data(), (const void*)(img.base + w.vaddr), w.bytes.size());
          memcpy((void*)(img.base + w.vaddr), w.bytes.data(), w.bytes.size());
          saved.push_back(std::move(s));
        }
      auto revert = [&] { for (size_t i = saved.size(); i-- > 0;) memcpy((void*)(img.base + saved[i].vaddr), saved[i].old.data(), saved[i].old.size()); saved.clear(); };
      std::map<uint64_t, Func> pseudoByStart;
      for (int gi : S) for (auto& p : pseudo[gi]) pseudoByStart[p.start] = p;

      // The function set of the variant (the function, and every cave piece it reaches) and its unit set: a fixpoint, because a cave
      // jumps back into the middle of the function, which then needs a unit there.
      std::vector<Func> members {F};
      std::vector<std::vector<Unit>> munits;
      std::map<uint64_t, std::vector<uint64_t>> savedEntry; // keys touched, to put back
      bool fail = false;
      std::string why;
      for (int round = 0; round < 10; ++round) {
        munits.assign(members.size(), {});
        std::vector<uint64_t> targets;
        for (size_t mi = 0; mi < members.size(); ++mi) {
          RecSink rs(munits[mi]);
          FuncResult fr;
          lifter.lift(members[mi], fr, nullptr, nullptr, nullptr, &rs);
          if (fr.status == LiftStatus::Failed || fr.status == LiftStatus::Crashed) { fail = true; why = "lift " + std::string(statusName(fr.status)) + ": " + fr.reason; break; }
          targets.insert(targets.end(), fr.outTargets.begin(), fr.outTargets.end());
        }
        if (fail) break;
        bool changed = false;
        std::set<uint64_t> have;
        for (auto& us : munits) for (auto& u : us) have.insert(u.entry);
        std::sort(targets.begin(), targets.end()); targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (uint64_t t : targets) {
          auto ps = pseudoByStart.find(t);
          if (ps != pseudoByStart.end()) {
            bool known = false;
            for (auto& m : members) if (m.start == t) known = true;
            if (!known) { members.push_back(ps->second); changed = true; }
            continue;
          }
          for (auto& m : members) {
            if (t > m.start && t < m.end && !have.count(t)) {
              if (!savedEntry.count(m.start)) savedEntry[m.start] = localEntry.count(m.start) ? localEntry[m.start] : std::vector<uint64_t>();
              auto& v = localEntry[m.start];
              if (std::find(v.begin(), v.end(), t) == v.end()) { v.push_back(t); changed = true; }
            }
          }
        }
        if (!changed) break;
        if (round == 9) { fail = true; why = "unit set did not settle"; }
      }
      auto restoreEntries = [&] { for (auto& [k, v] : savedEntry) { if (v.empty()) localEntry.erase(k); else localEntry[k] = v; } savedEntry.clear(); };
      if (fail) { revert(); restoreEntries(); ++out.variantsSkipped; out.notes.push_back("skip variant " + name + ": " + why); continue; }

      // Lower.
      const std::string prefix = "bbrv" + std::to_string(nextId++) + "_";
      std::set<uint64_t> mine;
      for (auto& us : munits) for (auto& u : us) mine.insert(u.entry);
      const auto& starts = *job.starts;
      batch.ul->reconfigure(prefix, [&, prefix](uint64_t t) -> std::string {
        if (mine.count(t)) return unitNameWith(prefix, t);
        if (std::binary_search(starts.begin(), starts.end(), t)) return unitName(t);
        return std::string();
      });
      struct LowSink : UnitSink {
        UnitLowerer& ul; std::vector<VUnit>& units; const Func* member; const std::vector<uint64_t>& starts; const std::string& prefix; const ElfImage& img; std::string unsup; size_t& bytes;
        LowSink(UnitLowerer& u, std::vector<VUnit>& us, const std::vector<uint64_t>& st, const std::string& p, const ElfImage& i, size_t& b) : ul(u), units(us), member(nullptr), starts(st), prefix(p), img(i), bytes(b) {}
        void unit(uint64_t entry, bool, const void* ir, const UnitMeta& meta) override {
          LowerResult lr;
          ul.lower(entry, ir, &lr, &meta);
          if (!lr.unsupported.empty() && unsup.empty()) unsup = lr.unsupported[0];
          VUnit u;
          u.vaddr = entry;
          u.end = meta.end > entry ? meta.end : member->end;
          u.ehash = hashRange((const uint8_t*)(img.base + u.vaddr), u.end - u.vaddr);
          u.symbol = unitNameWith(prefix, entry);
          auto it = std::lower_bound(starts.begin(), starts.end(), entry);
          u.orig = it != starts.end() && *it == entry ? int64_t(it - starts.begin()) : -1;
          bytes += u.end - u.vaddr;
          units.push_back(std::move(u));
        }
      };
      std::vector<VUnit> vunits;
      size_t bytes = 0;
      LowSink ls(*batch.ul, vunits, starts, prefix, img, bytes);
      for (auto& m : members) {
        ls.member = &m;
        FuncResult fr;
        lifter.lift(m, fr, nullptr, nullptr, nullptr, &ls);
      }
      revert(); restoreEntries();
      if (!ls.unsup.empty()) { ++out.variantsSkipped; out.notes.push_back("skip variant " + name + ": unsupported IR op " + ls.unsup + " (the original is dirty-marked too)"); continue; }
      // (units of two members never share an entry; a variant unit reached twice would be a lifter bug)
      std::sort(vunits.begin(), vunits.end(), [](const VUnit& a, const VUnit& b) { return a.vaddr < b.vaddr; });
      VFunc vf;
      vf.funcStart = F.start; vf.name = name;
      vf.firstUnit = out.table.units.size(); vf.unitCount = vunits.size();
      for (auto& u : vunits) { if (u.orig < 0) ++out.unitsNew; out.table.units.push_back(u); }
      out.unitsBuilt += vunits.size();
      out.table.funcs.push_back(std::move(vf));
      ++out.variantsBuilt;
      batch.bytes += bytes;
      out.codeBytes += bytes;
      if (batch.bytes >= job.batchBytes) { flush(); newBatch(); }
    }
  }
  flush();
  const bool ok = pool.finish();
  out.objFiles = pool.files;
  std::sort(out.objFiles.begin(), out.objFiles.end());
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return ok;
}
