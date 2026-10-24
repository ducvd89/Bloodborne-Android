// FEX IR -> LLVM IR lowering (docs/RECOMP_ABI.md). One lifted unit becomes one LLVM function
// `void bbr_<hex vaddr>(GuestState*)`. The FEX types stay inside lower_*.cpp: the unit's IR arrives as an
// opaque `const FEXCore::IR::IRListView*`.
#pragma once
#include "lifter.h" // UnitMeta
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm { class LLVMContext; class Module; }

// A native hook site (docs/RECOMP_ABI.md, "Hook slots"): at the start of the guest instruction at image vaddr `vaddr` the unit tests
// bbr_hook_slot[index]; when set it spills the state, calls the slot (GuestState*, index) and acts on the result: 0 = run the instruction as
// usual, 1 = the callback set rip: continue there (one of `resume`, else through the dispatcher), 2 = the callback made a `ret`.
struct HookSite {
  uint64_t vaddr = 0;
  uint32_t index = 0;
  std::vector<uint64_t> resume; // addresses the callback may continue at inside the unit (instruction boundaries or block entries)
};
using HookSites = std::map<uint64_t, HookSite>; // by vaddr

struct LowerOptions {
  uint64_t liftBase = 0;       // host address the lifter mapped image vaddr 0 at: IR addresses are liftBase + vaddr
  uint64_t imageSize = 0;      // bytes from liftBase that hold the image: a `Constant` inside is a relocatable address
  bool fixedImageBase = false; // true: addresses become constants (imageBase); false: the global bbr_image_base
  uint64_t imageBase = 0;
  bool aarch64 = true;         // target: picks the few target-specific bits (yield hint)
  bool storeFlagsAtCalls = false; // also store NZCV before calls and returns (x86 flags are dead there; tail jumps always store)
  bool calleeSaved = true;       // after a call, rbx rbp r12-r15 are still what they were (the SysV ABI), so they are not reloaded
  bool dirtyCheck = false;       // each unit starts with `if (bbr_dirty[index]) bbr_dirty_entry(s, vaddr)` (docs/RECOMP_ABI.md, "Dirty units")
  std::function<int64_t(uint64_t)> unitIndex; // image vaddr -> index in bbr_units[] (dirtyCheck); -1: not a unit
  bool resumeEntries = false;    // every call-return block is a resume entry of its unit (docs/RECOMP_ABI.md, "Resume entries")
  bool hiddenUnits = true;       // hidden visibility for the unit functions (off for the in-process JIT)
  std::function<bool(uint64_t)> isKnownUnit; // image vaddr -> a unit with that entry exists (direct call)
  std::function<std::string(uint64_t)> callName; // direct-call symbol of a target (isKnownUnit must be true for it); default unitName(vaddr)
  std::string defPrefix;                        // prefix of the functions defined here ("bbr_" when empty); variants use "bbrv<k>_"
  const HookSites* hookSites = nullptr;         // native hook sites (every unit that holds the instruction gets the slot test)
};

struct LowerResult {
  std::vector<std::string> unsupported; // distinct IR op names that became bbr_trap calls
  uint32_t trapSites = 0;               // number of bbr_trap(unsupported) calls emitted
  uint32_t directCalls = 0, dispatchCalls = 0, relocConstants = 0;
  uint32_t callExits = 0;               // ExitFunction with the Call hint (of directCalls + dispatchCalls)
  uint32_t irOps = 0;                   // IR ops lowered (all blocks)
  uint32_t resumeCases = 0;             // call-return blocks reachable through the resume switch at the unit entry
  uint32_t hookSites = 0;               // hook slot tests emitted
  std::vector<uint32_t> hookIdx;        // ... their site indices
  uint32_t switches = 0, switchCases = 0; // indirect jumps lowered as an LLVM switch over the unit's blocks, and their cases
  uint32_t volatileLoads = 0;           // guest loads in loops without stores, atomics or exits (spin-waits), lowered volatile
};

// bbr_trap kinds (second argument of bbr_trap). Bit 63 set: unsupported IR op, low bits = op index.
// Clear: a guest Break (hlt, int3, ud2, ...): Signal | TrapNumber << 8 | si_code << 16 | ErrorRegister << 32.
constexpr uint64_t BBR_TRAP_UNSUPPORTED = 1ull << 63;
constexpr uint64_t BBR_TRAP_UNLIFTABLE = 1ull << 62; // stub unit for a function the lifter could not lift

class UnitLowerer {
 public:
  UnitLowerer(llvm::LLVMContext& ctx, llvm::Module& mod, const LowerOptions& opt);
  ~UnitLowerer();
  // Lowers the unit entered at image vaddr `entry` (irView: const FEXCore::IR::IRListView*) into the module.
  void lower(uint64_t entry, const void* irView, LowerResult* res, const UnitMeta* meta = nullptr);
  // A stub for a unit that could not be lifted: bbr_trap(s, BBR_TRAP_UNLIFTABLE | vaddr).
  void stub(uint64_t entry);
  // Names for the units lowered from now on (variants share one module): the functions defined get `defPrefix` + hex vaddr, a direct call
  // goes to the symbol callName(target) (empty: not a direct target: the dispatcher).
  void reconfigure(const std::string& defPrefix, std::function<std::string(uint64_t)> callName);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string unitName(uint64_t vaddr); // "bbr_<hex>"
std::string unitNameWith(const std::string& prefix, uint64_t vaddr); // "<prefix><hex>"
