// Lifter: drives FEX's x86 front end (decoder, OpcodeDispatcher, IR passes) over one guest function
// at a time, without a JIT and without ever running guest code. No FEX headers here: the FEX types
// stay inside lifter.cpp.
#pragma once
#include "elf_image.h"
#include "fde_scan.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct LiftOptions {
  bool multiblock = true;     // whole function as one IR unit (the shipped config, Multiblock=1)
  bool tso = false;           // TSOEnabled; the game runs with it off (fex-compatible.json)
  bool gapUnits = true;       // lift code the entry unit does not reach (switch case bodies) as extra units
  bool validate = false;      // also run FEX's IR validation pass (errors become a failure reason)
  bool passes = true;         // x87 stack optimisation + dead flag calculation elimination
  std::string host = "armv8.4"; // HostFeatures profile of the target: armv8.2 | armv8.4 | armv9
  int64_t injectCrash = -1;   // test hooks (crash containment): fault / FEX assert inside the lift of the function at this vaddr
  int64_t injectAssert = -1;
  uint64_t maxInst = 0x10000;  // decoder limit per unit (FEX's own buffer limit is 0x10000)
};

enum class LiftStatus : uint8_t {
  Clean,   // every decoded instruction was lifted
  Partial, // the unit lifted but a block was cut short (dispatch failure after the first instruction)
  Failed,  // nothing could be lifted (entry instruction invalid or unhandled)
  Crashed, // FEX asserted or faulted; the lifter state was rebuilt
};
const char* statusName(LiftStatus s);

struct FuncResult {
  LiftStatus status = LiftStatus::Failed;
  uint32_t insts = 0;            // x86 instructions lifted
  uint32_t blocks = 0;           // IR code blocks
  uint32_t irOps = 0;            // IR ops, all blocks (CodeBlock nodes included)
  uint64_t coveredBytes = 0;     // bytes of the function inside decoded blocks
  uint64_t gapBytes = 0;         // bytes of the function no block covers (padding, dead code, dropped blocks)
  uint32_t gapInsts = 0;         // gap bytes decoded as instructions
  uint32_t gapPad = 0;           // ... of which padding (int3, nop, ud2)
  uint32_t gapData = 0;          // ... of which data (rel32 switch table entries kept in .text)
  uint32_t gapInvalid = 0;       // gap bytes that did not decode and are not table data
  uint32_t extraUnits = 0;       // extra units lifted for code the entry unit does not reach
  uint32_t extraFailed = 0;      // ... of which cut short or not liftable
  uint32_t extraInsts = 0;       // x86 instructions lifted by extra units (not in `insts`)
  std::string extraReason;       // first extra-unit failure
  bool spansPastEnd = false;     // decoded bytes ran past the FDE end (should not happen: the range is bounded)
  std::string reason;            // first failure, "category: detail"; empty when Clean
  std::vector<std::string> gapReasons; // "category: detail" for undecodable gap bytes
};

class Lifter {
 public:
  Lifter(const ElfImage& img, const LiftOptions& opt);
  ~Lifter();
  Lifter(const Lifter&) = delete;

  // Lifts f (image vaddrs). When opCounts is non-null it receives the per-op IR op counts (size numOps()).
  // sizeHist, when non-null, accumulates (op, size, element size) -> count.
  // irText, when non-null, receives FEX's IR dump of the function.
  void lift(const Func& f, FuncResult& r, std::vector<uint32_t>* opCounts,
            std::vector<uint64_t>* sizeHist = nullptr, std::string* irText = nullptr);

  static size_t numOps();               // IR op kinds (IR.json), without the sentinel
  static const char* opName(size_t op);
  // Op index -> true for ops that exist only to structure the list (Dummy, IRHeader).
  static bool isPseudoOp(size_t op);
  // Ops that are no operation of their own: Inline* immediates, block structure, GuestOpcode markers.
  static bool isStructuralOp(size_t op);
  // Slot count of sizeHist and the (op, size, elementSize) <-> slot mapping.
  static size_t sizeHistSlots();
  static void sizeSlotDecode(size_t slot, size_t& op, unsigned& sizeBits, unsigned& elemBits);

  // Messages FEX logged during the last lift (decode errors, "Invalid or Unknown instruction"...).
  const std::string& log() const { return log_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string log_;
};
