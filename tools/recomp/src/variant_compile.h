// Compiles the variants of a catalog (variants.h): for every function a group of patches touches, and every consistent subset of the
// groups that touch it, the function is lifted again from the patched bytes and lowered as alternate units named bbrv<k>_<vaddr>.
#pragma once
#include "codegen.h"
#include "elf_image.h"
#include "fde_scan.h"
#include "lifter.h"
#include "lower_llvm.h"
#include "variants.h"

#include <map>
#include <string>
#include <vector>

struct VariantJob {
  const ElfImage* img = nullptr;
  const std::vector<Func>* funcs = nullptr;        // compiled functions, sorted
  const std::vector<uint64_t>* starts = nullptr;   // sorted unit entries of the main compile (bbr_units[])
  const std::vector<uint64_t>* trapUnits = nullptr;// sorted units that start dirty (functions holding one get no variant)
  const Catalog* catalog = nullptr;
  LiftOptions lift;                                 // as the main compile (forceGuestOpcode included); entryUnits is copied
  LowerOptions lower;                               // as the main compile (dirtyCheck is turned off for variants)
  TargetSpec target;
  unsigned jobs = 4;
  std::string objBase;                              // objects are written to <objBase>.varNNN.o
  bool verify = false;
  size_t batchBytes = 512 << 10;                    // code bytes per module
};

struct VariantResult {
  VariantTable table;
  std::vector<std::string> objFiles;
  uint64_t groupsUsed = 0, variantsPlanned = 0, variantsBuilt = 0, variantsSkipped = 0, unitsBuilt = 0, unitsNew = 0, dataWrites = 0, cavePieces = 0;
  uint64_t codeBytes = 0;
  double seconds = 0;
  std::vector<std::string> notes;                   // one line per skipped variant and other oddities
};

bool compileVariants(const VariantJob& job, VariantResult& out);
uint64_t hashRange(const uint8_t* p, uint64_t n); // FNV-1a 64, byte 0x65 hashed as 0x64 (what the runtime does)
