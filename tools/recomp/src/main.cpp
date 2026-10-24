// bb-recomp: lifts the game's x86-64 code with FEX's front end. Step 1: IR census (no LLVM yet).
//   bb-recomp --elf out/eboot.elf                    census over every FDE function -> out/recomp/
//   bb-recomp --elf out/eboot.elf --dump 0x1234560   FEX IR of the function containing that vaddr
//   bb-recomp --elf out/eboot.elf --scan             just the function list
#include "census.h"
#include "discover.h"
#include "eh_frame.h"
#include "fde_scan.h"
#ifdef BB_RECOMP_LLVM
#include "compile.h"
#endif
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif

static void usage() {
  fputs("usage: bb-recomp --elf <eboot.elf> [options]\n"
        "  --boot <file>        boot-linked image (scripts/link_modules.py, out/boot-linked.bin): the eboot plus the linked\n"
        "                       system modules (libc.prx ...) at their image offsets, one image, one unit table. --elf then only\n"
        "                       supplies the eboot's program headers\n"
        "  --prx-dir <dir>      where the .prx files of --boot are (default: <game>/sce_module next to out/)\n"
        "  --dump <hex vaddr>   print the FEX IR of the function containing the address\n"
        "  --scan               list function count and range statistics only\n"
        "  --funcs <file>       compare the FDE list with a 'start end' hex list (ehscan.py output)\n"
        "  --out <dir>          census output directory (default out/recomp)\n"
        "  --jobs <n>           worker threads (default: cores - 2, at most 22; fewer when the memory budget needs it)\n"
        "  --limit <n>, --every <n>   census only the first n functions / every nth function\n"
        "  --base <hex>         host address of image vaddr 0 (default 0x3a7700000000: no plain constant of the game falls in it)\n"
        "  --host <profile>     target features: armv8.2 | armv8.4 (default) | armv9\n"
        "  --no-multiblock      one basic block per unit (FEX default is multiblock; the game ships Multiblock=1)\n"
        "  --tso                lift with x86 TSO memory ordering (the game runs with it off)\n"
        "  --no-gap-units       do not lift code the entry unit does not reach as extra units\n"
        "  --no-switch-tables   do not resolve jump tables (indirect jmps go through the dispatcher)\n"
        "  --no-discovery       skip the search for code without an FDE (data pointers, branch targets outside every function)\n"
        "  --no-secondary       no extra units for C++ landing pads and setjmp return sites\n"
        "  --validate           run FEX's IR validation pass on every unit\n"
        "  --no-passes          skip the x87 and dead-flag passes\n"
        "  --no-size-hist       skip ir_ops_by_size.csv\n"
        "  --inject-crash <hex>, --inject-assert <hex>   test hooks: fault / FEX assert while lifting that function\n"
        "  --no-nice            do not lower the process priority to 19\n"
        "LLVM modes (need -DBB_RECOMP_LLVM=ON):\n"
        "  --emit-ll <hex vaddr|all> <file>   LLVM IR text (one function, or --limit N / all functions)\n"
        "  --obj <file>         compile to object files (<file> for 1 shard, else <base>.NNN.o + <base>.table.o)\n"
        "  --so <file>          compile and link a shared object: Android ARM64 (NDK ld.lld) or, with --target x86_64, a PC one (clang + lld)\n"
        "  --target aarch64|x86_64   (default aarch64)     -O0 | -O1 (default) | -O2\n"
        "  --shards <n>         modules compiled in parallel (default 64 or more: the memory plan makes the shards smaller, then the\n"
        "                       worker count lower, until the peak fits in 60% of the available RAM; 1 for --emit-ll)\n"
        "  --plan-only          print the memory plan (jobs, shards, estimated peak) and stop\n"
        "  --mem-fraction <f>   share of the available memory the compile may use (default 0.6)\n"
        "  --verify-llvm        run the LLVM verifier on every module\n"
        "  --flags-at-calls     also store NZCV before calls and returns (x86 flags are dead there; off by default)\n"
        "  --no-dirty-check     no `if (bbr_dirty[unit]) bbr_dirty_entry()` at the start of every unit (default: on)\n"
        "  --resume-entries     every call-return block is also an entry of its unit (resume flag in the guest state)\n"
        "  --variants <file|dir>   a variant catalog (tools/recomp/variants/*.txt; repeatable): patch groups compiled as variants, native hook sites\n"
        "  --variant-report     list every variant and hook site in the summary\n"
        "  --no-callee-saved    reload rbx, rbp, r12-r15 after calls too (default: the SysV callee-saved set is trusted)\n"
        "  --ndk <dir>          Android NDK for --so\n"
        "  --helpers-dir <dir>  the x87 helper sources (default: tools/recomp/helpers of the build)\n"
        "  --stats <file>       also write the compile summary here\n", stderr);
}

int main(int argc, char** argv) {
  std::string elf = "out/eboot.elf", funcsFile, dumpArg, bootFile, prxDir;
  // The lift base. The lowering takes a 64-bit IR constant inside [base, base + image size) for an image address (FEX folds
  // rip-relative arithmetic into such constants) and relocates it to the runtime's image base. With the old base 0x200000000
  // two packed 32-bit integers such as `movabs rax, 0x200000002` (2 and 2) became image + 2 on the Thor (a count of
  // 0x104002 -> a 1 GB allocation -> null). Nothing but rip-derived values lands in this high, odd window.
  uint64_t base = 0x3a7700000000ull;
  bool scanOnly = false, nice = true, dump = false, noSecondary = false, noDiscovery = false;
  CensusOptions co;
#ifdef BB_RECOMP_LLVM
  CompileOptions cc;
  bool shardsSet = false, jobsSet = false;
  cc.statsPath = "out/recomp/compile.txt";
#endif
  unsigned hw = std::thread::hardware_concurrency();
  co.jobs = std::min(22u, hw > 2 ? hw - 2 : 1u);
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* s) { return !strcmp(argv[i], s); };
    auto arg = [&]() -> const char* { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
    if (is("--elf")) elf = arg();
    else if (is("--boot")) bootFile = arg();
    else if (is("--prx-dir")) prxDir = arg();
    else if (is("--dump")) { dump = true; dumpArg = arg(); }
    else if (is("--scan")) scanOnly = true;
    else if (is("--funcs")) funcsFile = arg();
    else if (is("--out")) co.outDir = arg();
    else if (is("--jobs")) { co.jobs = std::max(1, atoi(arg())); jobsSet = true; }
    else if (is("--limit")) co.limit = strtoull(arg(), nullptr, 0);
    else if (is("--every")) co.every = std::max<uint64_t>(1, strtoull(arg(), nullptr, 0));
    else if (is("--base")) base = strtoull(arg(), nullptr, 16);
    else if (is("--host")) co.lift.host = arg();
    else if (is("--no-multiblock")) co.lift.multiblock = false;
    else if (is("--tso")) co.lift.tso = true;
    else if (is("--no-gap-units")) co.lift.gapUnits = false;
    else if (is("--no-switch-tables")) co.lift.switchTables = false;
    else if (is("--no-secondary")) noSecondary = true;
    else if (is("--no-discovery")) noDiscovery = true;
    else if (is("--validate")) co.lift.validate = true;
    else if (is("--no-passes")) co.lift.passes = false;
    else if (is("--no-size-hist")) co.sizeHist = false;
    else if (is("--inject-crash")) co.lift.injectCrash = strtoll(arg(), nullptr, 16);
    else if (is("--inject-assert")) co.lift.injectAssert = strtoll(arg(), nullptr, 16);
    else if (is("--no-nice")) nice = false;
#ifdef BB_RECOMP_LLVM
    else if (is("--emit-ll")) { std::string a = arg(); cc.llPath = arg(); if (a == "all") cc.onlyVaddr = -1; else cc.onlyVaddr = strtoll(a.c_str(), nullptr, 16); }
    else if (is("--obj")) cc.objPath = arg();
    else if (is("--so")) cc.soPath = arg();
    else if (is("--target")) { std::string t = arg(); if (t == "aarch64") cc.target.aarch64 = true; else if (t == "x86_64") cc.target.aarch64 = false; else { usage(); return 2; } }
    else if (is("-O0")) cc.target.opt = 0;
    else if (is("-O1")) cc.target.opt = 1;
    else if (is("-O2")) cc.target.opt = 2;
    else if (is("--shards")) { cc.shards = std::max(1, atoi(arg())); shardsSet = true; }
    else if (is("--verify-llvm")) cc.verify = true;
    else if (is("--plan-only")) cc.planOnly = true;
    else if (is("--mem-fraction")) cc.memFraction = std::max(0.05, atof(arg()));
    else if (is("--flags-at-calls")) cc.storeFlagsAtCalls = true;
    else if (is("--resume-entries")) cc.resumeEntries = true;
    else if (is("--no-dirty-check")) cc.dirtyCheck = false;
    else if (is("--no-callee-saved")) cc.calleeSaved = false;
    else if (is("--variants")) cc.variantPaths.push_back(arg());
    else if (is("--variant-report")) cc.variantReport = true;
    else if (is("--ndk")) cc.ndk = arg();
    else if (is("--helpers-dir")) cc.helpersDir = arg();
    else if (is("--stats")) cc.statsPath = arg();
    else if (is("--func")) cc.onlyVaddr = strtoll(arg(), nullptr, 16);
#endif
    else { usage(); return 2; }
  }
  co.jobs = std::min(co.jobs, 22u); // 2 of 24 cores stay free on the dev PC
#ifndef _WIN32
  if (nice) setpriority(PRIO_PROCESS, 0, 19);
#endif
  ElfImage img; std::string err;
  if (bootFile.empty()) {
    if (!img.load(elf, base, err)) { fprintf(stderr, "bb-recomp: %s\n", err.c_str()); return 1; }
  } else {
    if (prxDir.empty()) prxDir = "/home/cid/BloodbornePC/game/CUSA03173/sce_module";
    if (!img.loadBoot(bootFile, elf, prxDir, base, err)) { fprintf(stderr, "bb-recomp: %s\n", err.c_str()); return 1; }
  }
  std::vector<Func> funcs;
  if (!scanAllFdes(img, funcs, err)) { fprintf(stderr, "bb-recomp: %s\n", err.c_str()); return 1; }
  uint64_t bytes = 0, mn = ~0ull, mx = 0; for (auto& f : funcs) { bytes += f.end - f.start; mn = std::min(mn, f.start); mx = std::max(mx, f.end); }
  if (img.boot) {
    printf("boot image %s: %zu modules\n", bootFile.c_str(), img.modules.size());
    for (auto& m : img.modules) {
      size_t n = 0; uint64_t b = 0;
      for (auto& f : funcs) if (f.start >= m.base && f.start < m.base + m.size) { ++n; b += f.end - f.start; }
      printf("  %-16s image +%#" PRIx64 " size %#" PRIx64 ": %zu FDE functions, %.2f MB\n", m.name.c_str(), m.base, m.size, n, b / 1e6);
    }
  }
  printf("image %s: text vaddr %#" PRIx64 "-%#" PRIx64 " mapped at base %#" PRIx64 "; %zu FDE functions, %.1f MB, range %#" PRIx64 "-%#" PRIx64 "\n",
         elf.c_str(), img.textLo, img.textHi, img.base, funcs.size(), bytes / 1e6, mn, mx);
  if (!funcsFile.empty()) {
    FILE* f = fopen(funcsFile.c_str(), "r"); if (!f) { perror(funcsFile.c_str()); return 1; }
    unsigned long long s, e; size_t n = 0, bad = 0;
    while (fscanf(f, "%llx %llx", &s, &e) == 2) { if (n >= funcs.size() || funcs[n].start != s || funcs[n].end != e) ++bad; ++n; }
    fclose(f);
    printf("compared with %s: %zu entries, %zu differ%s\n", funcsFile.c_str(), n, bad, n != funcs.size() ? " (count differs)" : "");
    if (bad || n != funcs.size()) return 1;
  }
  {
    const size_t nFde = funcs.size();
    const NonFde nf = addNonFdeEntries(img, funcs);
    if (nf.entries || nf.stubs) printf("non-FDE code the runtime enters: %zu start entries (eboot entry, module starts), %zu PLT stubs (%zu pseudo-functions added to the %zu FDE functions)\n", nf.entries, nf.stubs, funcs.size() - nFde, nFde);
  }
  static std::map<uint64_t, std::vector<uint64_t>> entryUnits;
  static EhScan eh;
  static ReturnsTwice rt;
  if (!noSecondary && !scanOnly) {
    scanLandingPads(img, funcs, eh);
    rt = findReturnsTwice(img);
    printf("secondary entries: %" PRIu64 " FDEs, %" PRIu64 " with an LSDA, %" PRIu64 " call-site records, %" PRIu64 " landing pads in %zu functions%s%s\n", eh.fdes, eh.fdesWithLsda, eh.callSites, eh.pads, eh.landingPads.size(),
           eh.badLsda ? " (unparsable: " : "", eh.badLsda ? (std::to_string(eh.badLsda) + " " + eh.firstError + ")").c_str() : "");
    if (img.boot) {
      printf("  returns-twice: %zu import(s) %s, %zu call targets (exports and PLT stubs), %zu GOT slots\n", rt.names.size(), rt.names.empty() ? "" : rt.names[0].c_str(), rt.targets.size(), rt.gotSlots.size());
    } else printf("  returns-twice: not looked for (plain eboot.elf has no import information; use --boot)\n");
    co.lift.landingPads = &eh.landingPads;
    if (!rt.targets.empty() || !rt.gotSlots.empty()) { co.lift.retTwiceTargets = &rt.targets; co.lift.retTwiceSlots = &rt.gotSlots; }
  }
  co.lift.entryUnits = &entryUnits;
  if (!noDiscovery && !scanOnly) {
    const DiscoverResult d = discoverEntries(img, funcs, entryUnits, co.lift, co.jobs);
    printf("discovery (%u rounds): %zu relocation pointers into code outside any FDE function (%zu did not decode), %zu more through rip-relative lea, %zu through branches: %zu functions added (%.2f MB), %zu call/jmp targets inside functions that no unit started at\n",
           d.rounds, d.relocCandidates, d.relocRejected, d.leaCandidates, d.branchCandidates, d.pseudoFuncs, d.pseudoBytes / 1e6, d.midEntries);
  }
  if (scanOnly) return 0;
  if (dump) return dumpFunction(img, funcs, strtoull(dumpArg.c_str(), nullptr, 16), co.lift);
#ifdef BB_RECOMP_LLVM
  if (!cc.llPath.empty() || !cc.objPath.empty() || !cc.soPath.empty() || cc.planOnly) {
    cc.lift = co.lift; cc.jobs = co.jobs; cc.limit = co.limit; cc.jobsFixed = jobsSet; cc.shardsFixed = shardsSet;
    if (!shardsSet && !cc.llPath.empty() && cc.objPath.empty() && cc.soPath.empty()) cc.shards = 1;
    return runCompile(img, funcs, cc);
  }
#endif
  return runCensus(img, funcs, co);
}
