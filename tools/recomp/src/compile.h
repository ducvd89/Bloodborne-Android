// Whole-image (or partial) compilation: lift -> lower -> optimise -> object files, in parallel shards;
// the unit table module; linking an Android .so with the NDK linker.
#pragma once
#include "codegen.h"
#include "lifter.h"
#include <cstdint>
#include <string>
#include <vector>

struct CompileOptions {
  TargetSpec target;
  LiftOptions lift;
  unsigned shards = 64;          // modules compiled in parallel
  unsigned jobs = 22;            // worker threads (2 of 24 cores stay free)
  uint64_t limit = 0;            // only the first N functions
  int64_t onlyVaddr = -1;        // only the function containing this vaddr
  std::string objPath;           // --obj: object files (<base>.NNN.o per shard, <base>.table.o)
  std::string llPath;            // --emit-ll: textual IR (<base>.NNN.ll per shard when shards > 1)
  std::string soPath;            // --so: link an Android ARM64 shared object
  std::string statsPath;         // where the summary is also written
  std::string ndk = "/home/cid/BloodbornePC/.local-deps/android/sdk/ndk/29.0.14206865";
  bool table = true;             // emit the unit table object
  bool verify = false;           // run the LLVM verifier on every module
  bool storeFlagsAtCalls = true; // see LowerOptions
};

int runCompile(const ElfImage& img, const std::vector<Func>& funcs, const CompileOptions& opt);
