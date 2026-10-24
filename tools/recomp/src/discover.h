// Discovery of code that has no FDE: functions the runtime enters through data pointers (vtable thunks, static initializer
// tables) or direct branches, and call/jmp targets that land inside another function. Runs the lifter over the image
// (about a second) and iterates to a fixpoint; the result extends the function list and the per-function extra entries.
#pragma once
#include "lifter.h"

struct DiscoverResult {
  size_t relocCandidates = 0;   // relocation values (data pointers) into code outside any FDE function (distinct)
  size_t relocRejected = 0;     // ... that do not decode as code
  size_t leaCandidates = 0;     // `lea reg,[rip+f]` targets outside any FDE function (address-taken code) not already found through a relocation
  size_t branchCandidates = 0;  // direct call/jmp/jcc targets outside any FDE function, found by lifting
  size_t pseudoFuncs = 0;       // functions added (kind 3)
  size_t pseudoBytes = 0;
  size_t midEntries = 0;        // call/jmp targets in the middle of a function that no unit starts at (units added)
  size_t extraGiven = 0, extraFuncStarts = 0, extraMid = 0, extraNew = 0, extraRejected = 0; // --extra-entries: addresses, already function starts, mid-function units added, new pseudo-functions, did not decode
  unsigned rounds = 0;
};

// funcs: sorted; gets the pseudo-functions (Func::kind 3) added and stays sorted. entryUnits: function start -> extra unit entries.
// extra: addresses the runtime was seen to enter that no unit starts at (a misses log, --extra-entries): each becomes a unit entry like a discovered target.
DiscoverResult discoverEntries(const ElfImage& img, std::vector<Func>& funcs, std::map<uint64_t, std::vector<uint64_t>>& entryUnits, const LiftOptions& lift, unsigned jobs,
                               const std::vector<uint64_t>* extra = nullptr);
// Reads a list of image offsets: the runtime's misses log (`miss +0x210ebc0 ...` lines, docs/RECOMP_ABI.md "Strict mode"), or one hex address per line
// ('+0x' and '0x' prefixes optional, '#' comments).
std::vector<uint64_t> readEntryList(const char* path, std::string& err);
