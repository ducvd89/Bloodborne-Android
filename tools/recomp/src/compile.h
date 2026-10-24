// Whole-image (or partial) compilation: lift -> lower -> optimise -> object files, in parallel shards;
// the unit table module; linking an Android .so with the NDK linker.
#pragma once
#include "codegen.h"
#include "lifter.h"
#include "variants.h"
#include <cstdint>
#include <string>
#include <vector>

struct CompileOptions {
  TargetSpec target;
  LiftOptions lift;
  unsigned shards = 64;          // modules compiled in parallel (a minimum: more shards are used when the memory budget needs them)
  unsigned jobs = 22;            // worker threads (an upper bound: fewer are used when the memory budget needs it)
  bool jobsFixed = false, shardsFixed = false; // set when the user chose them: the memory plan then leaves them alone
  bool planOnly = false;         // print the memory plan and stop
  double memFraction = 0.6;      // peak memory target: this fraction of the available RAM
  uint64_t limit = 0;            // only the first N functions
  int64_t onlyVaddr = -1;        // only the function containing this vaddr
  std::string objPath;           // --obj: object files (<base>.NNN.o per shard, <base>.table.o)
  std::string llPath;            // --emit-ll: textual IR (<base>.NNN.ll per shard when shards > 1)
  std::string soPath;            // --so: link an Android ARM64 shared object
  std::string statsPath;         // where the summary is also written
  std::string ndk = "/home/cid/BloodbornePC/.local-deps/android/sdk/ndk/29.0.14206865";
  std::string helpersDir;        // the x87 helper sources (tools/recomp/helpers); default: the directory compiled into the tool
  bool table = true;             // emit the unit table object
  bool verify = false;           // run the LLVM verifier on every module
  bool storeFlagsAtCalls = false; // see LowerOptions
  bool calleeSaved = true;        // see LowerOptions
  bool dirtyCheck = true;         // every unit tests bbr_dirty[its index] first (--no-dirty-check)
  bool resumeEntries = false;     // see LowerOptions
  // Granular entries (docs/RECOMP_ABI.md, "Granular entries"): units that can be entered at every instruction boundary. Jumps (default): the functions
  // with a table-like indirect jump whose cases are not known for sure; All: every function (a size and compile time experiment); Off.
  enum class Granular { Off, Jumps, All } granular = Granular::Jumps;
  std::string granularList;       // --granular-funcs: a file of image addresses; the functions holding them are granular too
  std::string unitsDump;          // --dump-units: the unit table (entry, end, extra, function, granular) as text
  std::vector<uint64_t> checkEntries; // --check-entries: image addresses that must be a unit entry or a granular entry (exit status 3 when one is not)
  bool unitsOnly = false;         // --units-only: stop after the unit set is known (dump, check), no code
  std::vector<std::string> variantPaths; // variant catalogs (files or directories of *.txt): patch variants and native hook sites (docs/RECOMP_ABI.md)
  bool variantReport = false;     // list every variant and hook site in the summary
};

int runCompile(const ElfImage& img, const std::vector<Func>& funcs, const CompileOptions& opt);
