// Lifter: drives FEX's x86 front end (decoder, OpcodeDispatcher, IR passes) over one guest function
// at a time, without a JIT and without ever running guest code. No FEX headers here: the FEX types
// stay inside lifter.cpp.
#pragma once
#include "elf_image.h"
#include "fde_scan.h"
#include "jumptable.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct LiftOptions {
  bool multiblock = true;     // whole function as one IR unit (the shipped config, Multiblock=1)
  bool tso = false;           // TSOEnabled; the game runs with it off (fex-compatible.json)
  bool gapUnits = true;       // lift code the entry unit does not reach (switch case bodies) as extra units
  bool switchTables = true;   // resolve jump tables: case targets become blocks of the unit, the jump a `switch`
  bool validate = false;      // also run FEX's IR validation pass (errors become a failure reason)
  bool passes = true;         // x87 stack optimisation + dead flag calculation elimination
  std::string host = "armv8.4"; // HostFeatures profile of the target: armv8.2 | armv8.4 | armv9
  int64_t injectCrash = -1;   // test hooks (crash containment): fault / FEX assert inside the lift of the function at this vaddr
  int64_t injectAssert = -1;
  // Secondary entries (image vaddrs; shared, owned by the caller, must outlive the lifters): the unit set also gets a unit at
  // every landing pad of the function (key: function start) and at the return address of every call to a returns-twice target.
  const std::map<uint64_t, std::vector<uint64_t>>* landingPads = nullptr;
  const std::set<uint64_t>* retTwiceTargets = nullptr;  // direct call targets (setjmp, its PLT stubs)
  const std::set<uint64_t>* retTwiceSlots = nullptr;    // GOT slots of `call [rip+slot]`
  const std::map<uint64_t, std::vector<uint64_t>>* entryUnits = nullptr; // function start -> extra unit entries (direct call/jmp targets inside it that no unit starts at)
  bool collectTargets = false;                          // FuncResult::outTargets: direct call/jmp/jcc targets outside the function (discovery)
  const std::set<uint64_t>* forceGuestOpcode = nullptr; // image vaddrs of instructions that always get a GuestOpcode marker (hook sites, hook resume points; docs/RECOMP_ABI.md, "Hook slots")
  uint64_t maxInst = 0x10000;  // decoder limit per unit (FEX's own buffer limit is 0x10000)
  // Granular entries (docs/RECOMP_ABI.md, "Granular entries"): every unit of these functions can be entered at every instruction boundary it holds
  // (a dispatch switch at the unit entry, like the resume entries). granularFuncs: function starts (image vaddrs); granularAll: every function.
  const std::set<uint64_t>* granularFuncs = nullptr;
  bool granularAll = false;
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
  uint32_t padUnits = 0, setjmpUnits = 0; // secondary entry units lifted for landing pads / setjmp return sites (also counted in extraUnits? no: separate)
  uint32_t secondaryFailed = 0;  // ... of which cut short or not liftable
  uint32_t secondaryInsts = 0;   // x86 instructions in them
  uint32_t padsKnown = 0, padsInUnit = 0; // landing pads of the function, and how many already were a unit entry (no extra unit needed)
  std::string extraReason;       // first extra-unit failure
  bool spansPastEnd = false;     // decoded bytes ran past the FDE end (should not happen: the range is bounded)
  std::string reason;            // first failure, "category: detail"; empty when Clean
  std::vector<std::string> gapReasons; // "category: detail" for undecodable gap bytes
  // Indirect jumps (`jmp reg`, `jmp [mem]`) of the function, counted once per address over all its units.
  uint32_t indJmps = 0;          // distinct indirect jmp instructions
  uint32_t jmpResolved = 0;      // ... resolved as a bounded jump table (cmp/ja or and mask)
  uint32_t jmpScanned = 0;       // ... resolved by scanning the table to its first non-target entry (no bound found)
  uint32_t jmpMem = 0;           // ... jmp [mem] (GOT stubs and vtable tail calls): not tables
  uint32_t jmpMemRip = 0, jmpMemIdx = 0; // ... of which [rip+x] (import stubs) and [base+index*scale+x] (absolute tables)
  uint32_t jmpReg = 0;           // ... jmp reg that is no table (register tail calls)
  uint32_t jmpFailed = 0;        // ... table-like but not resolved (reasons in jmpExamples)
  uint32_t tableEntries = 0;     // case targets read from resolved tables (before de-duplication)
  uint64_t tableBytes = 0;       // bytes of resolved tables
  std::vector<uint64_t> outTargets;      // collectTargets: direct branch/call targets outside [start, end) inside the image (image vaddrs, sorted, unique)
  std::vector<uint64_t> leaTargets;      // collectTargets: `lea reg, [rip+x]` targets (address-taken code), image vaddrs, sorted, unique
  uint32_t entryUnitsLifted = 0;         // units lifted at entryUnits addresses
  std::vector<std::string> jmpExamples; // "vaddr reason" of unresolved jmp reg / table-like jumps (first few)
  // Table-like indirect jumps whose case set is not known for sure: unresolved (the table was not found or has no usable bound) or resolved only by scanning
  // the table to its first non-target entry. A function with one gets granular entries by default.
  struct WeakJump { uint64_t pc; uint8_t why; uint64_t table; uint32_t entries; bool scanned; };
  std::vector<WeakJump> weakJumps;
  bool granular = false;                 // the function was lifted with granular entries
};

// What the lowering needs to know about a unit besides its IR: the resolved jump tables (image vaddrs).
struct SwitchInfo { uint64_t jmpVaddr; std::vector<uint64_t> targets; };
struct UnitMeta {
  std::vector<SwitchInfo> switches;
  uint64_t end = 0; /* image vaddr just past the last decoded byte of the unit (0: unknown) */
  bool granular = false;          // the unit gets an entry at every instruction boundary it holds (insts)
  std::vector<uint64_t> insts;    // image vaddrs of the instruction boundaries the unit decoded (granular units only), sorted, unique
};

// Receives each lifted unit while its IR is alive (valid only inside the call): entry is the image vaddr,
// irView a `const FEXCore::IR::IRListView*` (the lowering casts it back).
struct UnitSink {
  virtual ~UnitSink() = default;
  virtual void unit(uint64_t entryVaddr, bool extra, const void* irView, const UnitMeta& meta) = 0;
};

class Lifter {
 public:
  Lifter(const ElfImage& img, const LiftOptions& opt);
  ~Lifter();
  Lifter(const Lifter&) = delete;

  // Lifts f (image vaddrs). When opCounts is non-null it receives the per-op IR op counts (size numOps()).
  // sizeHist, when non-null, accumulates (op, size, element size) -> count.
  // irText, when non-null, receives FEX's IR dump of the function.
  // sink, when non-null, gets every unit (the entry unit and the extra units) as it is lifted.
  void lift(const Func& f, FuncResult& r, std::vector<uint32_t>* opCounts,
            std::vector<uint64_t>* sizeHist = nullptr, std::string* irText = nullptr, UnitSink* sink = nullptr);

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

  // FEX's own CPUID model (FEXCore CPUIDEmu, x86-64 build of the tool) for the Thor's host features (the armv9 profile with SSE4a off and CLZERO on, which is
  // what FEX computes there): answers[i] = {eax, ebx, ecx, edx} of query i = {function, leaf}. For the differential test of cpu/guest_cpu_cpuid.h
  // (bb-recomp-asmtest --cpuid-diff). The brand string, leaf 0x15 and the host nibble of leaf 0x40000001 are AArch64-only code in FEX: not meaningful here.
  static std::vector<std::array<uint32_t, 4>> fexCpuidReference(const std::vector<std::pair<uint32_t, uint32_t>>& queries);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string log_;
};
