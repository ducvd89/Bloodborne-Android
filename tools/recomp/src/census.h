// Census over every function of the image: lift success, failure reasons, IR op histogram.
#pragma once
#include "lifter.h"
#include <string>

struct CensusOptions {
  LiftOptions lift;
  unsigned jobs = 22;
  uint64_t limit = 0;           // lift only the first N functions (0 = all)
  uint64_t every = 1;           // lift every Nth function (sampling)
  bool sizeHist = true;         // also write ir_ops_by_size.csv
  std::string outDir = "out/recomp";
};

// Runs the census, writes the files under outDir and prints the summary. Returns the process exit code.
int runCensus(const ElfImage& img, const std::vector<Func>& funcs, const CensusOptions& opt);
// Lifts one function and prints its IR (the --dump mode).
int dumpFunction(const ElfImage& img, const std::vector<Func>& funcs, uint64_t vaddr, const LiftOptions& opt);
