// bb-recomp: lifts the game's x86-64 code with FEX's front end. Step 1: IR census (no LLVM yet).
//   bb-recomp --elf out/eboot.elf                    census over every FDE function -> out/recomp/
//   bb-recomp --elf out/eboot.elf --dump 0x1234560   FEX IR of the function containing that vaddr
//   bb-recomp --elf out/eboot.elf --scan             just the function list
#include "census.h"
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
        "  --jobs <n>           worker threads (default: cores - 2, at most 22)\n"
        "  --limit <n>, --every <n>   census only the first n functions / every nth function\n"
        "  --base <hex>         host address of image vaddr 0 (default 0x200000000)\n"
        "  --host <profile>     target features: armv8.2 | armv8.4 (default) | armv9\n"
        "  --no-multiblock      one basic block per unit (FEX default is multiblock; the game ships Multiblock=1)\n"
        "  --tso                lift with x86 TSO memory ordering (the game runs with it off)\n"
        "  --no-gap-units       do not lift code the entry unit does not reach as extra units\n"
        "  --no-switch-tables   do not resolve jump tables (indirect jmps go through the dispatcher)\n"
        "  --no-secondary       no extra units for C++ landing pads and setjmp return sites\n"
        "  --validate           run FEX's IR validation pass on every unit\n"
        "  --no-passes          skip the x87 and dead-flag passes\n"
        "  --no-size-hist       skip ir_ops_by_size.csv\n"
        "  --inject-crash <hex>, --inject-assert <hex>   test hooks: fault / FEX assert while lifting that function\n"
        "  --no-nice            do not lower the process priority to 19\n"
        "LLVM modes (need -DBB_RECOMP_LLVM=ON):\n"
        "  --emit-ll <hex vaddr|all> <file>   LLVM IR text (one function, or --limit N / all functions)\n"
        "  --obj <file>         compile to object files (<file> for 1 shard, else <base>.NNN.o + <base>.table.o)\n"
        "  --so <file>          compile and link an Android ARM64 shared object (needs --target aarch64)\n"
        "  --target aarch64|x86_64   (default aarch64)     -O0 | -O1 (default) | -O2\n"
        "  --shards <n>         modules compiled in parallel (default 64; 1 for --emit-ll)\n"
        "  --verify-llvm        run the LLVM verifier on every module\n"
        "  --flags-at-calls     also store NZCV before calls and returns (x86 flags are dead there; off by default)\n"
        "  --no-callee-saved    reload rbx, rbp, r12-r15 after calls too (default: the SysV callee-saved set is trusted)\n"
        "  --ndk <dir>          Android NDK for --so\n"
        "  --stats <file>       also write the compile summary here\n", stderr);
}

int main(int argc, char** argv) {
  std::string elf = "out/eboot.elf", funcsFile, dumpArg, bootFile, prxDir;
  uint64_t base = 0x200000000ull;
  bool scanOnly = false, nice = true, dump = false, noSecondary = false;
  CensusOptions co;
#ifdef BB_RECOMP_LLVM
  CompileOptions cc;
  bool shardsSet = false;
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
    else if (is("--jobs")) co.jobs = std::max(1, atoi(arg()));
    else if (is("--limit")) co.limit = strtoull(arg(), nullptr, 0);
    else if (is("--every")) co.every = std::max<uint64_t>(1, strtoull(arg(), nullptr, 0));
    else if (is("--base")) base = strtoull(arg(), nullptr, 16);
    else if (is("--host")) co.lift.host = arg();
    else if (is("--no-multiblock")) co.lift.multiblock = false;
    else if (is("--tso")) co.lift.tso = true;
    else if (is("--no-gap-units")) co.lift.gapUnits = false;
    else if (is("--no-switch-tables")) co.lift.switchTables = false;
    else if (is("--no-secondary")) noSecondary = true;
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
    else if (is("--flags-at-calls")) cc.storeFlagsAtCalls = true;
    else if (is("--no-callee-saved")) cc.calleeSaved = false;
    else if (is("--ndk")) cc.ndk = arg();
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
  if (scanOnly) return 0;
  if (dump) return dumpFunction(img, funcs, strtoull(dumpArg.c_str(), nullptr, 16), co.lift);
#ifdef BB_RECOMP_LLVM
  if (!cc.llPath.empty() || !cc.objPath.empty() || !cc.soPath.empty()) {
    cc.lift = co.lift; cc.jobs = co.jobs; cc.limit = co.limit;
    if (!shardsSet && !cc.llPath.empty() && cc.objPath.empty() && cc.soPath.empty()) cc.shards = 1;
    return runCompile(img, funcs, cc);
  }
#endif
  return runCensus(img, funcs, co);
}
