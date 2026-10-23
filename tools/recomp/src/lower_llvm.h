// FEX IR -> LLVM IR lowering (docs/RECOMP_ABI.md). One lifted unit becomes one LLVM function
// `void bbr_<hex vaddr>(GuestState*)`. The FEX types stay inside lower_*.cpp: the unit's IR arrives as an
// opaque `const FEXCore::IR::IRListView*`.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace llvm { class LLVMContext; class Module; }

struct LowerOptions {
  uint64_t liftBase = 0;       // host address the lifter mapped image vaddr 0 at: IR addresses are liftBase + vaddr
  uint64_t imageSize = 0;      // bytes from liftBase that hold the image: a `Constant` inside is a relocatable address
  bool fixedImageBase = false; // true: addresses become constants (imageBase); false: the global bbr_image_base
  uint64_t imageBase = 0;
  bool aarch64 = true;         // target: picks the few target-specific bits (yield hint)
  bool storeFlagsAtCalls = true; // also store NZCV to the context before calls (always done at exits)
  bool hiddenUnits = true;       // hidden visibility for the unit functions (off for the in-process JIT)
  std::function<bool(uint64_t)> isKnownUnit; // image vaddr -> a unit with that entry exists (direct call)
};

struct LowerResult {
  std::vector<std::string> unsupported; // distinct IR op names that became bbr_trap calls
  uint32_t trapSites = 0;               // number of bbr_trap(unsupported) calls emitted
  uint32_t directCalls = 0, dispatchCalls = 0, relocConstants = 0;
  uint32_t irOps = 0;                   // IR ops lowered (all blocks)
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
  void lower(uint64_t entry, const void* irView, LowerResult* res);
  // A stub for a unit that could not be lifted: bbr_trap(s, BBR_TRAP_UNLIFTABLE | vaddr).
  void stub(uint64_t entry);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string unitName(uint64_t vaddr); // "bbr_<hex>"
