#include "census.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <thread>

namespace {
struct Group { uint64_t count = 0; uint64_t funcs = 0; std::string example; uint64_t exampleAddr = 0; };

// One per worker, merged at the end (no sharing while lifting).
struct Agg {
  std::vector<uint64_t> ops, funcsUsing, sizeHist;
  uint64_t byStatus[4] = {};
  uint64_t insts = 0, irOps = 0, blocks = 0, bytes = 0, covered = 0, gap = 0, gapInsts = 0, gapPad = 0, gapData = 0, gapInvalid = 0, pastEnd = 0;
  uint64_t realOps = 0, extraUnits = 0, extraFailed = 0, extraInsts = 0, funcsWithExtra = 0, funcsExtraFailed = 0;
  uint64_t instsClean = 0;
  uint64_t padUnits = 0, setjmpUnits = 0, secondaryFailed = 0, secondaryInsts = 0, padsKnown = 0, padsInUnit = 0;
  uint64_t indJmps = 0, jmpResolved = 0, jmpScanned = 0, jmpMem = 0, jmpMemRip = 0, jmpMemIdx = 0, jmpReg = 0, jmpFailed = 0, tableEntries = 0, tableBytes = 0;
  std::map<std::string, Group> fail, gapFail, jmpFail;
  Agg() : ops(Lifter::numOps()), funcsUsing(Lifter::numOps()) {}
};

// "category: NAME [b0 b1 b2 ...] @addr" -> key "category: NAME [b0 b1 b2]"
std::string keyOf(const std::string& r) {
  auto b = r.find('['), e = r.find(']');
  if (b == std::string::npos || e == std::string::npos) return r;
  std::string bytes = r.substr(b + 1, e - b - 1);
  size_t cut = 0; for (int n = 0; n < 3 && cut != std::string::npos; ++n) cut = bytes.find(' ', cut ? cut + 1 : 0);
  if (cut != std::string::npos) bytes.resize(cut);
  return r.substr(0, b) + "[" + bytes + "]";
}
uint64_t addrOf(const std::string& r) { auto a = r.rfind('@'); return a == std::string::npos ? 0 : strtoull(r.c_str() + a + 1, nullptr, 16); }

void addGroup(std::map<std::string, Group>& m, const std::string& reason) {
  auto& g = m[keyOf(reason)];
  if (!g.count++) { g.example = reason; g.exampleAddr = addrOf(reason); }
}
}

static FILE* g_sf;
__attribute__((format(printf, 1, 2))) static void both(const char* fmt, ...) {
  va_list a, b; va_start(a, fmt); va_copy(b, a);
  vfprintf(stdout, fmt, a); vfprintf(g_sf, fmt, b); va_end(a); va_end(b);
}

int runCensus(const ElfImage& img, const std::vector<Func>& funcs, const CensusOptions& opt) {
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  std::vector<size_t> todo;
  for (size_t i = 0; i < funcs.size(); i += opt.every) { if (opt.limit && todo.size() >= opt.limit) break; todo.push_back(i); }
  std::vector<FuncResult> results(todo.size());
  std::vector<Agg> aggs(opt.jobs);
  const size_t nOps = Lifter::numOps();
  std::atomic<size_t> next {0}, done {0};

  auto worker = [&](unsigned w) {
    Lifter lifter(img, opt.lift);
    Agg& a = aggs[w];
    if (opt.sizeHist) a.sizeHist.assign(Lifter::sizeHistSlots(), 0);
    std::vector<uint32_t> cnt;
    for (;;) {
      size_t k = next.fetch_add(1);
      if (k >= todo.size()) break;
      const Func& f = funcs[todo[k]];
      FuncResult& r = results[k];
      lifter.lift(f, r, &cnt, opt.sizeHist ? &a.sizeHist : nullptr, nullptr);
      ++a.byStatus[(int)r.status];
      a.bytes += f.end - f.start; a.covered += r.coveredBytes; a.gap += r.gapBytes;
      a.gapInsts += r.gapInsts; a.gapPad += r.gapPad; a.gapData += r.gapData; a.gapInvalid += r.gapInvalid; a.pastEnd += r.spansPastEnd;
      a.extraUnits += r.extraUnits; a.extraFailed += r.extraFailed; a.extraInsts += r.extraInsts; a.funcsWithExtra += r.extraUnits != 0; a.funcsExtraFailed += r.extraFailed != 0;
      a.insts += r.insts; a.irOps += r.irOps; a.blocks += r.blocks;
      a.padUnits += r.padUnits; a.setjmpUnits += r.setjmpUnits; a.secondaryFailed += r.secondaryFailed; a.secondaryInsts += r.secondaryInsts; a.padsKnown += r.padsKnown; a.padsInUnit += r.padsInUnit;
      a.indJmps += r.indJmps; a.jmpResolved += r.jmpResolved; a.jmpScanned += r.jmpScanned; a.jmpMem += r.jmpMem; a.jmpMemRip += r.jmpMemRip; a.jmpMemIdx += r.jmpMemIdx; a.jmpReg += r.jmpReg; a.jmpFailed += r.jmpFailed; a.tableEntries += r.tableEntries; a.tableBytes += r.tableBytes;
      for (auto& e : r.jmpExamples) { auto& g = a.jmpFail[e.substr(e.find(' ') + 1)]; if (!g.count++) g.example = "jmp at vaddr " + e.substr(0, e.find(' ')) + " in function " + std::to_string(f.start); }
      if (r.status == LiftStatus::Clean) a.instsClean += r.insts;
      for (size_t o = 0; o < nOps; ++o) if (cnt[o]) { a.ops[o] += cnt[o]; ++a.funcsUsing[o]; if (!Lifter::isStructuralOp(o)) a.realOps += cnt[o]; }
      if (!r.reason.empty()) addGroup(a.fail, std::string(statusName(r.status)) + " " + r.reason);
      if (!r.extraReason.empty()) addGroup(a.fail, "gap-unit " + r.extraReason);
      for (auto& g : r.gapReasons) addGroup(a.gapFail, g);
      size_t d = ++done;
      if (d % 5000 == 0) { fprintf(stderr, "\r%zu / %zu functions", d, todo.size()); fflush(stderr); }
    }
  };
  std::vector<std::thread> pool;
  for (unsigned w = 0; w < opt.jobs; ++w) pool.emplace_back(worker, w);
  for (auto& t : pool) t.join();
  fprintf(stderr, "\r%zu / %zu functions\n", done.load(), todo.size());
  double secs = std::chrono::duration<double>(clk::now() - t0).count();

  // merge
  Agg t;
  if (opt.sizeHist) t.sizeHist.assign(Lifter::sizeHistSlots(), 0);
  for (auto& a : aggs) {
    for (size_t o = 0; o < nOps; ++o) { t.ops[o] += a.ops[o]; t.funcsUsing[o] += a.funcsUsing[o]; }
    for (size_t o = 0; o < a.sizeHist.size(); ++o) t.sizeHist[o] += a.sizeHist[o];
    for (int s = 0; s < 4; ++s) t.byStatus[s] += a.byStatus[s];
    t.insts += a.insts; t.irOps += a.irOps; t.blocks += a.blocks; t.bytes += a.bytes; t.covered += a.covered; t.gap += a.gap;
    t.gapInsts += a.gapInsts; t.gapPad += a.gapPad; t.gapData += a.gapData; t.gapInvalid += a.gapInvalid; t.pastEnd += a.pastEnd; t.instsClean += a.instsClean; t.realOps += a.realOps; t.extraUnits += a.extraUnits; t.extraFailed += a.extraFailed; t.extraInsts += a.extraInsts; t.funcsWithExtra += a.funcsWithExtra; t.funcsExtraFailed += a.funcsExtraFailed;
    for (auto& [k, g] : a.fail) { auto& d = t.fail[k]; if (!d.count) { d.example = g.example; d.exampleAddr = g.exampleAddr; } d.count += g.count; }
    t.padUnits += a.padUnits; t.setjmpUnits += a.setjmpUnits; t.secondaryFailed += a.secondaryFailed; t.secondaryInsts += a.secondaryInsts; t.padsKnown += a.padsKnown; t.padsInUnit += a.padsInUnit;
    t.indJmps += a.indJmps; t.jmpResolved += a.jmpResolved; t.jmpScanned += a.jmpScanned; t.jmpMem += a.jmpMem; t.jmpMemRip += a.jmpMemRip; t.jmpMemIdx += a.jmpMemIdx; t.jmpReg += a.jmpReg; t.jmpFailed += a.jmpFailed; t.tableEntries += a.tableEntries; t.tableBytes += a.tableBytes;
    for (auto& [k, g] : a.jmpFail) { auto& d = t.jmpFail[k]; if (!d.count) { d.example = g.example; d.exampleAddr = g.exampleAddr; } d.count += g.count; }
    for (auto& [k, g] : a.gapFail) { auto& d = t.gapFail[k]; if (!d.count) { d.example = g.example; d.exampleAddr = g.exampleAddr; } d.count += g.count; }
  }

  std::string dir = opt.outDir;
  auto open = [&](const char* name) { std::string p = dir + "/" + name; FILE* f = fopen(p.c_str(), "w"); if (!f) { perror(p.c_str()); exit(1); } return f; };
  std::string mk = "mkdir -p '" + dir + "'"; if (system(mk.c_str())) return 1;

  // functions.csv
  { FILE* f = open("functions.csv"); fprintf(f, "start,end,size,status,insts,blocks,ir_ops,covered,gap,gap_pad,gap_data,gap_invalid,extra_units,extra_failed,extra_insts,ind_jmps,jmp_resolved,jmp_scanned,jmp_unresolved,reason\n");
    for (size_t k = 0; k < todo.size(); ++k) { auto& fn = funcs[todo[k]]; auto& r = results[k];
      fprintf(f, "%" PRIx64 ",%" PRIx64 ",%" PRIu64 ",%s,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,\"%s\"\n", fn.start, fn.end, fn.end - fn.start, statusName(r.status),
              r.insts, r.blocks, r.irOps, r.coveredBytes, r.gapBytes, r.gapPad, r.gapData, r.gapInvalid, r.extraUnits, r.extraFailed, r.extraInsts, r.indJmps, r.jmpResolved, r.jmpScanned, r.jmpReg + r.jmpFailed, r.reason.c_str()); }
    fclose(f); }

  // ir_ops.csv sorted by count
  std::vector<size_t> order(nOps); for (size_t i = 0; i < nOps; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return t.ops[a] > t.ops[b]; });
  size_t used = 0; for (size_t o = 0; o < nOps; ++o) used += t.ops[o] != 0;
  { FILE* f = open("ir_ops.csv"); fprintf(f, "op,count,pct_of_ops,functions\n");
    for (size_t o : order) fprintf(f, "%s,%" PRIu64 ",%.4f,%" PRIu64 "\n", Lifter::opName(o), t.ops[o], 100.0 * t.ops[o] / std::max<uint64_t>(t.irOps, 1), t.funcsUsing[o]);
    fclose(f); }
  { FILE* f = open("ir_ops_unused.txt");
    for (size_t o = 0; o < nOps; ++o) if (!t.ops[o]) fprintf(f, "%s%s\n", Lifter::opName(o), Lifter::isPseudoOp(o) ? "  (pseudo op, not emitted into blocks)" : "");
    fclose(f); }
  if (opt.sizeHist) {
    std::vector<size_t> slots; for (size_t s = 0; s < t.sizeHist.size(); ++s) if (t.sizeHist[s]) slots.push_back(s);
    std::sort(slots.begin(), slots.end(), [&](size_t a, size_t b) { return t.sizeHist[a] > t.sizeHist[b]; });
    FILE* f = open("ir_ops_by_size.csv"); fprintf(f, "op,size_bits,element_bits,count\n");
    for (size_t s : slots) { size_t o; unsigned sb, eb; Lifter::sizeSlotDecode(s, o, sb, eb); fprintf(f, "%s,%u,%u,%" PRIu64 "\n", Lifter::opName(o), sb, eb, t.sizeHist[s]); }
    fclose(f); }
  // failures.txt
  auto dumpGroups = [&](FILE* f, std::map<std::string, Group>& m) {
    std::vector<std::pair<std::string, Group>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    for (auto& [k, g] : v) fprintf(f, "%8" PRIu64 "  %s\n          e.g. %s\n", g.count, k.c_str(), g.example.c_str());
  };
  { FILE* f = open("failures.txt");
    fprintf(f, "Functions not lifted cleanly (grouped by first failure; count = functions)\n\n"); dumpGroups(f, t.fail);
    fprintf(f, "\nIndirect jumps that look like a table but are not resolved, and register jumps without a table (count = jumps)\n\n"); dumpGroups(f, t.jmpFail);
    fprintf(f, "\nUndecodable bytes inside function ranges outside every lifted block (count = bytes; includes dead code after noreturn calls)\n\n"); dumpGroups(f, t.gapFail);
    fclose(f); }

  // summary
  uint64_t nf = todo.size();
  FILE* sf = g_sf = open("census.txt");
  both("== bb-recomp census ==\n");
  {
    size_t pseudo = 0; for (auto& f : funcs) pseudo += f.kind != 0;
    both("image:            %zu functions (%zu FDE functions + %zu without an FDE: module starts, PLT stubs, discovered thunks) in %.1f MB; %" PRIu64 " functions censused (every %" PRIu64 ")\n", funcs.size(), funcs.size() - pseudo, pseudo, t.bytes / 1e6, nf, opt.every);
  }
  both("config:           multiblock=%d tso=%d passes=%d host=%s jobs=%u\n", opt.lift.multiblock, opt.lift.tso, opt.lift.passes, opt.lift.host.c_str(), opt.jobs);
  both("time:             %.2f s wall (%.0f functions/s, %.0f x86 instr/s)\n", secs, nf / secs, (t.insts + t.extraInsts) / secs);
  both("clean:            %" PRIu64 " (%.3f%%)\n", t.byStatus[0], 100.0 * t.byStatus[0] / nf);
  both("partial:          %" PRIu64 " (%.3f%%)  [lifted, but a block was cut short at an unhandled instruction]\n", t.byStatus[1], 100.0 * t.byStatus[1] / nf);
  both("failed:           %" PRIu64 " (%.3f%%)\n", t.byStatus[2], 100.0 * t.byStatus[2] / nf);
  both("crashed:          %" PRIu64 " (%.3f%%)  [FEX assert or fault, caught per function]\n", t.byStatus[3], 100.0 * t.byStatus[3] / nf);
  both("x86 instructions: %" PRIu64 " lifted (entry units %" PRIu64 " + extra units %" PRIu64 ")\n", t.insts + t.extraInsts, t.insts, t.extraInsts);
  both("extra units:      %" PRIu64 " in %" PRIu64 " functions for code the entry unit does not reach; %" PRIu64 " in %" PRIu64 " functions not clean\n", t.extraUnits, t.funcsWithExtra, t.extraFailed, t.funcsExtraFailed);
  both("IR ops:           %" PRIu64 " total in %" PRIu64 " code blocks = %.2f per x86 instruction\n", t.irOps, t.blocks, t.insts ? (double)t.irOps / (t.insts + t.extraInsts) : 0.0);
  both("  excluding Inline* constants, block markers and GuestOpcode: %" PRIu64 " = %.2f per x86 instruction\n", t.realOps, t.insts ? (double)t.realOps / (t.insts + t.extraInsts) : 0.0);
  both("distinct IR ops:  %zu of %zu in IR.json used; %zu never emitted\n", used, nOps, nOps - used);
  both("bytes:            %.1f MB in FDEs, %.1f MB covered by lifted blocks (%.2f%%), %.1f MB gap\n", t.bytes / 1e6, t.covered / 1e6, 100.0 * t.covered / t.bytes, t.gap / 1e6);
  { uint64_t fb = 0, fc = 0;
    for (size_t k = 0; k < todo.size(); ++k) if (funcs[todo[k]].kind == 0) { fb += funcs[todo[k]].end - funcs[todo[k]].start; fc += results[k].coveredBytes; }
    both("  FDE functions only: %.1f MB, %.1f MB covered (%.2f%%)\n", fb / 1e6, fc / 1e6, fb ? 100.0 * fc / fb : 0.0); }
  both("  gap contents:   %.2f MB padding (int3/nop/ud2), %.2f MB switch table data, %.2f MB other instructions (dead or dropped), %" PRIu64 " undecodable bytes\n", t.gapPad / 1e6, t.gapData / 1e6, t.gapInsts / 1e6, t.gapInvalid);
  both("  units past FDE end: %" PRIu64 "\n", t.pastEnd);
  both("secondary units: %" PRIu64 " landing pads (of %" PRIu64 " in the functions: %" PRIu64 " were already a unit entry), %" PRIu64 " setjmp return sites; %" PRIu64 " not clean; %" PRIu64 " x86 instructions\n",
       t.padUnits, t.padsKnown, t.padsInUnit, t.setjmpUnits, t.secondaryFailed, t.secondaryInsts);
  if (img.modules.size() > 1) {
    both("per module (functions incl. pseudo-functions for module starts and PLT stubs):\n");
    for (auto& m : img.modules) {
      uint64_t n = 0, fb = 0, cov = 0, ins = 0, units = 0, extra = 0, pseudo = 0, bad = 0;
      for (size_t k = 0; k < todo.size(); ++k) {
        const Func& fn = funcs[todo[k]];
        if (fn.start < m.base || fn.start >= m.base + m.size) continue;
        const FuncResult& r = results[k];
        ++n; fb += fn.end - fn.start; cov += r.coveredBytes; ins += r.insts + r.extraInsts + r.secondaryInsts; units += 1 + r.extraUnits + r.padUnits + r.setjmpUnits;
        extra += r.extraUnits + r.padUnits + r.setjmpUnits; pseudo += fn.kind != 0; bad += r.status != LiftStatus::Clean;
      }
      both("  %-16s %7" PRIu64 " functions (%" PRIu64 " pseudo), %6.2f MB, %5.2f%% of the bytes in lifted blocks, %8" PRIu64 " x86 instructions, %7" PRIu64 " units (%" PRIu64 " extra), %" PRIu64 " not clean\n",
           m.name.c_str(), n, pseudo, fb / 1e6, fb ? 100.0 * cov / fb : 0.0, ins, units, extra, bad);
    }
  }
  both("indirect jmps:    %" PRIu64 " (jmp reg/mem): %" PRIu64 " jump tables resolved (%" PRIu64 " bounded by cmp/ja or and, %" PRIu64 " scanned without a bound; %" PRIu64 " case entries, %.2f MB of tables),\n"
       "                  %" PRIu64 " jmp [mem] (%" PRIu64 " [rip+x] import stubs, %" PRIu64 " [base+index*scale+x], rest [reg+x] vtable tail calls), %" PRIu64 " jmp reg that is no table (register tail calls), %" PRIu64 " table-like but unresolved\n",
       t.indJmps, t.jmpResolved + t.jmpScanned, t.jmpResolved, t.jmpScanned, t.tableEntries, t.tableBytes / 1e6, t.jmpMem, t.jmpMemRip, t.jmpMemIdx, t.jmpReg, t.jmpFailed);
  both("\ntop IR ops by count:\n  %-28s %12s %7s %9s\n", "op", "count", "%", "functions");
  for (size_t i = 0; i < 40 && i < order.size(); ++i) { size_t o = order[i]; both("  %-28s %12" PRIu64 " %6.2f%% %9" PRIu64 "\n", Lifter::opName(o), t.ops[o], 100.0 * t.ops[o] / std::max<uint64_t>(t.irOps, 1), t.funcsUsing[o]); }
  fclose(sf);
  fprintf(stdout, "\nfiles under %s: census.txt functions.csv ir_ops.csv ir_ops_by_size.csv ir_ops_unused.txt failures.txt\n", dir.c_str());
  return 0;
}

int dumpFunction(const ElfImage& img, const std::vector<Func>& funcs, uint64_t vaddr, const LiftOptions& opt) {
  const Func* hit = nullptr;
  for (auto& f : funcs) if (vaddr >= f.start && vaddr < f.end) { hit = &f; break; }
  if (!hit) { fprintf(stderr, "no FDE covers %#" PRIx64 "\n", vaddr); return 1; }
  Lifter l(img, opt);
  FuncResult r; std::string ir;
  l.lift(*hit, r, nullptr, nullptr, &ir);
  printf("; function %#" PRIx64 "-%#" PRIx64 " (%" PRIu64 " bytes): %s, %u x86 instructions, %u blocks, %u IR ops\n", hit->start, hit->end, hit->end - hit->start,
         statusName(r.status), r.insts, r.blocks, r.irOps);
  printf("; covered %" PRIu64 " bytes, gap %" PRIu64 " (padding %u, table data %u, other %u, undecodable %u)\n", r.coveredBytes, r.gapBytes, r.gapPad, r.gapData, r.gapInsts, r.gapInvalid);
  printf("; guest addresses in the IR are image vaddr + base %#" PRIx64 "\n", img.base);
  if (!r.reason.empty()) printf("; reason: %s\n", r.reason.c_str());
  for (auto& g : r.gapReasons) printf("; gap: %s\n", g.c_str());
  if (!l.log().empty()) printf("; FEX log:\n;   %s", l.log().c_str());
  printf("%s", ir.c_str());
  return 0;
}
