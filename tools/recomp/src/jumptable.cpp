#include "jumptable.h"
#include <algorithm>
#include <unordered_map>

namespace jt {

const char* whyName(Why w) {
  switch (w) {
  case Why::Resolved: return "resolved";
  case Why::NotTable: return "not a table jump (register tail call or computed target)";
  case Why::NoLoad: return "add found, no movsxd [base+idx*4] feeding it";
  case Why::NoBase: return "no lea of the table base";
  case Why::NoBound: return "table found, index not bounded by cmp/ja or and";
  case Why::BadTable: return "table entries do not point into the function";
  case Why::MemTarget: return "jmp [mem]";
  }
  return "?";
}

namespace {
constexpr unsigned MaxBack = 64;     // instructions to look back
constexpr uint32_t MaxEntries = 4096; // unbounded fallback scan

uint32_t writeMask(const Ins& p) {
  switch (p.kind) {
  case K::Clobber: return p.clobber;
  case K::NoWrite: case K::Cmp: case K::Jcc: case K::Jmp: case K::JmpInd: case K::Ret: case K::Call: case K::CallInd: return 0;
  default: return p.dst.k == Op::Reg ? 1u << p.dst.reg : 0;
  }
}

// Control-flow helpers over the sorted instruction list.
struct Flow {
  const std::vector<Ins>& ins;
  std::unordered_map<uint64_t, size_t> idxOf;                 // pc -> index
  std::unordered_multimap<uint64_t, size_t> takenFrom;        // branch target -> index of the conditional jump to it
  explicit Flow(const std::vector<Ins>& v) : ins(v) {
    for (size_t i = 0; i < v.size(); ++i) {
      idxOf[v[i].pc] = i;
      if (v[i].kind == K::Jcc && v[i].src.k == Op::Imm) takenFrom.emplace(v[i].pc + v[i].size + v[i].src.imm, i);
    }
  }
  // The instruction executed before ins[k]: the one right before it if it falls through, else the conditional jump
  // that branches to it. -1 when there is none (or several). `taken` tells which way the jump went.
  long prev(size_t k, bool& taken) const {
    taken = false;
    if (k > 0) {
      const Ins& p = ins[k - 1];
      if (p.pc + p.size == ins[k].pc && p.kind != K::Jmp && p.kind != K::JmpInd && p.kind != K::Ret) return (long)k - 1;
    }
    auto r = takenFrom.equal_range(ins[k].pc);
    if (r.first == r.second || std::next(r.first) != r.second) return -1;
    taken = true;
    return (long)r.first->second;
  }
};
} // namespace

std::vector<Jump> analyze(const std::vector<Ins>& ins, const Env& env) {
  std::vector<Jump> out;
  Flow w(ins);
  for (size_t i = 0; i < ins.size(); ++i) {
    const Ins& jmp = ins[i];
    if (jmp.kind != K::JmpInd) continue;
    Jump j; j.pc = jmp.pc;
    if (jmp.src.k == Op::Mem) { j.memForm = true; j.ripMem = jmp.src.rip; j.idxMem = jmp.src.index >= 0; j.why = Why::MemTarget; out.push_back(std::move(j)); continue; }
    if (jmp.src.k != Op::Reg) { out.push_back(std::move(j)); continue; }

    // 1. the jump register is the result of `add off, base` (either operand order).
    size_t k = i;
    const Ins* add = nullptr;
    unsigned n = 0;
    for (bool taken; n < MaxBack; ++n) {
      const long m = w.prev(k, taken);
      if (m < 0 || taken) break;
      k = m;
      if (writeMask(ins[k]) >> jmp.src.reg & 1) { add = &ins[k]; break; }
    }
    if (!add || add->kind != K::Add || add->dst.k != Op::Reg || add->dst.reg != jmp.src.reg || add->src.k != Op::Reg || add->src.reg == add->dst.reg) {
      out.push_back(std::move(j)); continue;
    }
    const uint8_t rd = add->dst.reg, rs = add->src.reg;

    // 2. the movsxd that loaded one of the two (the other one is the table base).
    const Ins* load = nullptr;
    for (bool taken; n < MaxBack; ++n) {
      const long m = w.prev(k, taken);
      if (m < 0 || taken) break;
      k = m;
      if (writeMask(ins[k]) & ((1u << rd) | (1u << rs))) { load = &ins[k]; break; }
    }
    if (!load || load->kind != K::Movsxd || load->dst.k != Op::Reg || load->src.k != Op::Mem || load->src.rip || load->src.index < 0 || load->src.scale != 4 ||
        load->src.base < 0 || (load->dst.reg != rd && load->dst.reg != rs) || load->src.base != (load->dst.reg == rd ? rs : rd)) {
      j.why = Why::NoLoad; out.push_back(std::move(j)); continue;
    }
    uint8_t base = (uint8_t)load->src.base;
    const uint8_t idx = (uint8_t)load->src.index;
    const int64_t disp = load->src.imm;
    uint32_t alias = 1u << idx;   // registers that hold the index value
    bool haveBase = false, haveBound = false, aliasDead = false, broken = false;
    uint64_t baseVal = 0;
    uint64_t bound = 0; // number of entries when haveBound
    std::vector<std::pair<uint8_t, uint64_t>> cands; // (register compared, entries) of the cmp/ja pairs seen

    // 3. back over the code that leads here: the base lea and the bound. Walks the control flow backwards: through
    // fall-through, and through the one conditional jump that branches to a block start. When that ends without
    // both, a loose pass goes on over the preceding instructions in address order (the lea is often hoisted out of
    // a loop): it can be wrong, which the table sanity check below catches (a wrong guess only costs dead blocks).
    auto visit = [&](const Ins* p, size_t at, bool taken) {
      uint32_t wm = writeMask(*p);
      if (p->kind == K::Call || p->kind == K::CallInd) wm = 0xfc7; // a call clobbers the caller-saved registers (SysV): rax rcx rdx rsi rdi r8-r11
      if ((wm >> base & 1) && !haveBase && p->kind == K::Mov && p->dst.k == Op::Reg && p->dst.reg == base && p->src.k == Op::Reg) {
        base = p->src.reg; // the base was copied (`lea rcx, [rip+T] ; mov rsi, rcx`): look for the lea of the source
        wm &= ~(1u << p->dst.reg); // (the mov itself is no obstacle; the new base is checked from the next instruction on)
      } else if ((wm >> base & 1) && !haveBase) {
        if ( p->kind != K::Lea || p->src.k != Op::Mem || !p->src.rip || p->dst.k != Op::Reg || p->dst.reg != base || p->src.base >= 0) { broken = true; return; }
        baseVal = p->pc + p->size + p->src.imm; haveBase = true;
      }
      // Candidate bounds: `cmp X, N ; ja/jae` (fall through = in range: N+1 / N cases) or `jbe/jb L` (taken = in range).
      // X need not be the index register itself: it may be the register the index was copied from (`mov ecx, esi ;
      // cmp esi, N ; ja ; ... [tbl+rcx*4]`); it counts once the walk reaches that copy with X untouched in between.
      if (!haveBound && p->kind == K::Jcc && ((!taken && (p->cc == 7 || p->cc == 3)) || (taken && (p->cc == 6 || p->cc == 2))) && at > 0) {
        // The cmp may not sit right before the jcc: the scheduler puts flag-preserving instructions (mov, lea ...) between them
        // (`cmp rax, 11 ; mov rdx, [rbp-0x680] ; ja default`), as long as they do not write the compared register.
        const Ins* q = nullptr;
        uint32_t between = 0;
        for (size_t b = at, back = 0; b > 0 && back < 8; --b, ++back) {
          const Ins& pi = ins[b - 1];
          if (pi.pc + pi.size != ins[b].pc) break;
          if (pi.kind == K::Cmp) { q = &pi; break; }
          if (!pi.flagSafe) break;
          between |= writeMask(pi);
        }
        if (q && q->kind == K::Cmp && q->dst.k == Op::Reg && q->src.k == Op::Imm && q->src.imm >= 0 && q->src.imm < (1 << 20) && !(between >> q->dst.reg & 1))
          cands.push_back({q->dst.reg, (uint64_t)q->src.imm + (p->cc == 7 || p->cc == 6 ? 1 : 0)});
      }
      if (!haveBound && wm)
        for (size_t c = 0; c < cands.size();) { if ((wm >> cands[c].first & 1) && !(alias >> cands[c].first & 1)) cands.erase(cands.begin() + c); else ++c; }
      if (!haveBound && !aliasDead && (wm & alias)) {
        if (p->kind == K::And && p->dst.k == Op::Reg && (alias >> p->dst.reg & 1) && p->src.k == Op::Imm && p->src.imm > 0 && ((p->src.imm + 1) & p->src.imm) == 0) {
          bound = (uint64_t)p->src.imm + 1; haveBound = true;
        } else if ((p->kind == K::Mov || p->kind == K::Movzx || p->kind == K::Movsxd) && p->dst.k == Op::Reg && p->src.k == Op::Reg) {
          alias = (alias & ~(1u << p->dst.reg)) | (1u << p->src.reg); // a copy of the value we track
        } else {
          alias &= ~wm;
          if (!alias) aliasDead = true;
        }
      }
      if (!haveBound && !aliasDead)
        for (auto& c : cands) if (alias >> c.first & 1) { bound = c.second; haveBound = true; break; }
    };
    const auto done = [&] { return haveBase && (haveBound || aliasDead); };
    size_t kk = k;
    for (bool taken; n < MaxBack && !broken && !done(); ++n) {
      const long m = w.prev(kk, taken);
      if (m < 0) break;
      kk = m;
      visit(&ins[kk], kk, taken);
    }
    if (!broken && !done()) {
      for (unsigned loose = 0; kk > 0 && loose < 32768 && !broken && !done(); ++loose) { --kk; visit(&ins[kk], kk, false); }
    }
    if (!haveBase || broken) { j.why = Why::NoBase; out.push_back(std::move(j)); continue; }

    j.table = baseVal + (uint64_t)disp;
    const uint32_t want = haveBound ? (uint32_t)std::min<uint64_t>(bound, 1 << 16) : MaxEntries;
    std::vector<uint64_t> t;
    uint32_t read = 0;
    // The table ends at the bound or at the first entry that is no code address of this function (the bound may be
    // too large: `and idx, 31` for a table of 13 entries; the next object in .text follows the table).
    for (uint32_t e = 0; e < want; ++e) {
      int32_t v;
      if (!env.readS32(j.table + 4ull * e, v)) break;
      const uint64_t target = baseVal + (int64_t)v;
      if (target < env.lo || target >= env.hi) break;
      t.push_back(target); ++read;
    }
    j.entries = read;
    if (read < 2 && !(haveBound && read >= 1 && want == 1)) { j.why = haveBound ? Why::BadTable : Why::NoBound; out.push_back(std::move(j)); continue; }
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    j.targets = std::move(t);
    j.why = haveBound && read == want ? Why::Resolved : Why::NoBound; // NoBound with targets: the table was scanned to its first non-target entry
    out.push_back(std::move(j));
  }
  return out;
}

} // namespace jt
