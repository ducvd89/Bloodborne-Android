#include "discover.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <set>
#include <thread>

namespace {
struct Index {
  const std::vector<Func>& f;
  // containing function, or -1
  long find(uint64_t a) const {
    auto it = std::upper_bound(f.begin(), f.end(), a, [](uint64_t v, const Func& x) { return v < x.start; });
    if (it == f.begin()) return -1;
    --it;
    return a < it->end ? (long)(it - f.begin()) : -1;
  }
};
} // namespace

std::vector<uint64_t> readEntryList(const char* path, std::string& err) {
  std::vector<uint64_t> out;
  FILE* f = fopen(path, "r");
  if (!f) { err = std::string("cannot open ") + path; return out; }
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
    if (!strncmp(p, "miss ", 5)) p += 5;
    while (*p == ' ') ++p;
    if (*p == '+') ++p;
    char* e = nullptr;
    const uint64_t a = strtoull(p, &e, 16);
    if (e == p) continue;
    out.push_back(a);
  }
  fclose(f);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

DiscoverResult discoverEntries(const ElfImage& img, std::vector<Func>& funcs, std::map<uint64_t, std::vector<uint64_t>>& entryUnits, const LiftOptions& liftOpt, unsigned jobs,
                               const std::vector<uint64_t>* extra) {
  DiscoverResult res;
  // Code range of each module: from its first FDE to its last (data pointers outside are not function pointers: rodata, strings).
  std::vector<std::pair<uint64_t, uint64_t>> code;
  for (auto& m : img.modules) {
    uint64_t lo = ~0ull, hi = 0;
    for (auto& f : funcs) if (f.kind == 0 && f.start >= m.base && f.start < m.base + m.size) { lo = std::min(lo, f.start); hi = std::max(hi, f.end); }
    if (hi) code.push_back({lo, hi});
  }
  auto inCode = [&](uint64_t a) { for (auto& c : code) if (a >= c.first && a < c.second) return true; return false; };
  // A direct call/jmp/jcc target found by lifting is certain control flow, not a data pointer that merely looks like one: it counts anywhere in
  // an executable segment of a module, also before the first FDE and after the last (crt start-up code has no FDE: the eboot's static
  // initialiser runner at +0x20, called from the entry point, lies before the first FDE function at +0xa0, and the units missed it).
  auto inExec = [&](uint64_t a) {
    if (inCode(a)) return true;
    for (auto& m : img.modules) {
      if (a < m.base || a >= m.base + m.size) continue;
      for (auto& sg : img.loads) if ((sg.flags & 1) && a >= sg.vaddr && a < sg.vaddr + sg.filesz) return true;
    }
    return false;
  };

  LiftOptions lo = liftOpt;
  // The units are lifted as they will be compiled (gap units, landing pads, setjmp sites and the entries found so far), because a
  // jump from one unit into the middle of another is a target outside its range that only shows up there.
  lo.collectTargets = true; lo.entryUnits = &entryUnits;

  // Candidate entries: data pointers (relocations) into code.
  std::set<uint64_t> pending;   // addresses to examine (image vaddrs)
  std::set<uint64_t> fromReloc, fromLea;
  if (img.boot)
    for (auto& r : img.links.relocs)
      if (r.kind == 0 && inCode(r.value)) fromReloc.insert(r.value);
  pending = fromReloc;
  std::set<uint64_t> fromExtra;
  if (extra)
    for (uint64_t a : *extra) {
      ++res.extraGiven;
      bool inImage = false;
      for (auto& m : img.modules) if (a >= m.base && a < m.base + m.size) inImage = true;
      if (!inImage) { ++res.extraRejected; continue; }
      fromExtra.insert(a); pending.insert(a);
    }

  std::vector<Func> frontier = funcs;   // round 0: every function, to collect its outgoing targets
  std::set<uint64_t> midSeen, holeSeen, touched; // touched: functions that got a new extra entry (lifted again)
  for (unsigned round = 0; round < 16; ++round) {
    res.rounds = round + 1;
    // Lift the frontier in parallel; gather the branch targets that leave their function.
    std::vector<std::vector<uint64_t>> targets(frontier.size()), leas(frontier.size());
    std::vector<uint8_t> okay(frontier.size(), 1);
    std::atomic<size_t> next {0};
    auto worker = [&] {
      Lifter l(img, lo);
      for (;;) {
        const size_t k = next++;
        if (k >= frontier.size()) break;
        FuncResult r;
        l.lift(frontier[k], r, nullptr, nullptr, nullptr, nullptr);
        if (r.status == LiftStatus::Failed || r.status == LiftStatus::Crashed) okay[k] = 0;
        if (okay[k] || frontier[k].kind == 0) { targets[k] = std::move(r.outTargets); leas[k] = std::move(r.leaTargets); } // (junk targets of a data pointer are not followed)
      }
    };
    std::vector<std::thread> th;
    for (unsigned j = 0; j < std::max(1u, jobs); ++j) th.emplace_back(worker);
    for (auto& t : th) t.join();

    // Pseudo-functions that did not decode were data: drop them again.
    if (round > 0) {
      std::set<uint64_t> bad;
      for (size_t k = 0; k < frontier.size(); ++k) if (!okay[k]) bad.insert(frontier[k].start);
      if (!bad.empty()) {
        funcs.erase(std::remove_if(funcs.begin(), funcs.end(), [&](const Func& f) { return f.kind == 3 && bad.count(f.start); }), funcs.end());
        for (uint64_t b : bad) { if (fromReloc.count(b)) ++res.relocRejected; if (fromExtra.count(b)) { ++res.extraRejected; if (res.extraNew) --res.extraNew; fprintf(stderr, "bb-recomp: extra entry +%#llx does not decode as code: ignored\n", (unsigned long long)b); } }
      }
    }

    // New candidates: the targets found plus the pending relocation pointers (round 0).
    std::set<uint64_t> cand = std::move(pending);
    pending.clear();
    for (auto& v : targets) for (uint64_t t : v) if (inExec(t)) cand.insert(t);
    // Address-taken code (`lea reg, [rip+f]`): only targets outside every function count (a lea inside a function is a label or a jump table).
    {
      Index li{funcs};
      for (auto& v : leas) for (uint64_t t : v) if (inCode(t) && li.find(t) < 0 && cand.insert(t).second) { fromLea.insert(t); }
    }

    Index idx{funcs};
    std::vector<Func> added;
    for (uint64_t t : cand) {
      const long i = idx.find(t);
      if (fromExtra.count(t)) {
        if (i >= 0 && funcs[i].start == t) ++res.extraFuncStarts;
        else if (i >= 0 && funcs[i].kind != 3) ++res.extraMid;
        else ++res.extraNew;
      }
      if (i < 0) {
        if (holeSeen.insert(t).second) { added.push_back({t, t + 1, 0, 3}); if (fromReloc.count(t)) ++res.relocCandidates; else if (fromLea.count(t)) ++res.leaCandidates; else ++res.branchCandidates; }
      } else if (funcs[i].kind == 3 && funcs[i].start != t) {
        // inside a pseudo-function whose end was only the next known entry: another function starts here
        if (holeSeen.insert(t).second) { added.push_back({t, t + 1, 0, 3}); ++res.branchCandidates; }
      } else if (funcs[i].start != t && midSeen.insert(t).second) {
        // a call/jmp/pointer into the middle of another function: a unit entry of that function
        entryUnits[funcs[i].start].push_back(t);
        touched.insert(funcs[i].start);
        ++res.midEntries;
      }
    }
    if (added.empty() && touched.empty()) break;
    // Bounds of the new pseudo-functions: up to the next function or candidate (an entry ends the previous one), at most the end of its code range.
    funcs.insert(funcs.end(), added.begin(), added.end());
    std::sort(funcs.begin(), funcs.end(), [](const Func& a, const Func& b) { return a.start < b.start; });
    for (size_t i = 0; i < funcs.size(); ++i) {
      Func& f = funcs[i];
      if (f.kind != 3) continue;
      uint64_t limit = i + 1 < funcs.size() ? funcs[i + 1].start : f.start + 0x10000;
      for (auto& c : code) if (f.start >= c.first && f.start < c.second) limit = std::min(limit, c.second);
      f.end = std::min(limit, f.start + 0x10000);
      if (f.end <= f.start) f.end = f.start + 1;
    }
    frontier.clear();
    for (uint64_t st : touched) { auto it = std::lower_bound(funcs.begin(), funcs.end(), st, [](const Func& f, uint64_t v) { return f.start < v; }); if (it != funcs.end() && it->start == st) frontier.push_back(*it); }
    touched.clear();
    for (auto& a : added) { auto it = std::lower_bound(funcs.begin(), funcs.end(), a.start, [](const Func& f, uint64_t v) { return f.start < v; }); if (it != funcs.end() && it->start == a.start) frontier.push_back(*it); }
  }
  for (auto& f : funcs) if (f.kind == 3) { ++res.pseudoFuncs; res.pseudoBytes += f.end - f.start; }
  for (auto& kv : entryUnits) { std::sort(kv.second.begin(), kv.second.end()); kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end()); }
  return res;
}
