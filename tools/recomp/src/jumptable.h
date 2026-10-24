// Switch table recognition (no FEX types here: lifter.cpp converts the decoded instructions).
//
// PS4 clang compiles a dense `switch` to
//     cmp  idx, N ; ja default          (or jae with N+1 -> N cases)
//     lea  base, [rip+T]
//     movsxd off, dword [base+idx*4]    (T = start of the rel32 table, kept in .text after the code)
//     add  off, base
//     jmp  off
// This finds such sequences by walking back over the straight-line instructions that precede the jmp,
// reads the table and returns the case targets, so the lifter can make them blocks of the same unit and
// the lowering can emit an LLVM `switch` over them. A wrong guess is harmless: the lowering only
// switches on addresses that are block entries of the unit and falls back to the dispatcher otherwise.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jt {

enum class K : uint8_t {
  Other,    // anything that is not interesting (writes its Reg destination, if any, in an unknown way)
  NoWrite,  // cmp, test, push, nop ...: leaves the destination alone
  Cmp,      // dst vs src[0]
  Jcc,      // conditional jump, cc in `cc`
  Jmp,      // direct jump
  JmpInd,   // indirect jump: operand in src[0]
  Call, CallInd, Ret,
  Lea, Mov, Movsxd, Movzx, Add, And,
  Clobber,  // writes registers we do not model (mul, div, cdq, xchg, rep movs, pop, leave ...): kills every alias
};

struct Op {
  enum Kind : uint8_t { None, Reg, Imm, Mem } k = None;
  uint8_t reg = 0;          // Reg: x86 register number 0-15
  int64_t imm = 0;          // Imm: the value; Mem: displacement
  int8_t base = -1, index = -1; // Mem: register numbers or -1
  uint8_t scale = 1;
  bool rip = false;         // Mem: [rip+imm], relative to the end of the instruction
};

struct Ins {
  uint64_t pc = 0;
  uint8_t size = 0;
  K kind = K::Other;
  uint8_t cc = 0;           // Jcc: x86 condition code (7 = A, 3 = AE ...)
  Op dst, src;
  bool flagSafe = false;    // leaves the flags alone (mov, lea, movzx, push, pop, nop ...): may sit between a cmp and its jcc
  uint16_t clobber = 0xffff; // Clobber: the registers it writes (bit n: x86 register n); everything when the lifter does not know
};

enum class Why : uint8_t {
  Resolved,
  NotTable,       // the jump register does not come from `add off, base` (tail call through a register, ...)
  NoLoad,         // add found, but not the movsxd [base+idx*4] feeding it
  NoBase,         // no `lea base, [rip+T]` for the base register
  NoBound,        // table found, but no cmp/ja (or and mask) bounds the index
  BadTable,       // the entries do not point into the function
  MemTarget,      // jmp [mem]: absolute table or tail call through memory
};
const char* whyName(Why w);

struct Jump {
  uint64_t pc = 0;
  Why why = Why::NotTable;
  uint64_t table = 0;       // address of the first entry (resolved or nearly)
  uint32_t entries = 0;     // entries read
  bool memForm = false;     // the jump operand is memory
  bool ripMem = false;      // ... [rip+x]: an import stub or a GOT call
  bool idxMem = false;      // ... [base+index*scale+x]: an absolute table or a vtable-style dispatch
  std::vector<uint64_t> targets; // distinct case targets, sorted
};

struct Env {
  uint64_t lo, hi;                                      // the unit's range: targets must lie inside
  std::function<bool(uint64_t, int32_t&)> readS32;      // guest memory
};

// ins: the unit's instructions sorted by pc, one per address. Returns one Jump per indirect jmp.
std::vector<Jump> analyze(const std::vector<Ins>& ins, const Env& env);

} // namespace jt
