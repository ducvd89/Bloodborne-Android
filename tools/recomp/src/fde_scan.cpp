#include "fde_scan.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace {
// DW_EH_PE_* value encodings used by the table (low nibble: width and sign; high nibble: base).
constexpr uint8_t PE_udata4 = 0x03, PE_sdata4 = 0x0b, PE_pcrel = 0x10, PE_datarel = 0x30;

struct Reader {
  const ElfImage& img; // reads by image vaddr
  uint64_t va;         // vaddr of the .eh_frame_hdr start (datarel base)
  bool ok = true;
  template <class T> T get(uint64_t at) { auto* p = img.fileBytes(at, sizeof(T)); if (!p) { ok = false; return 0; } T v; memcpy(&v, p, sizeof v); return v; }
  // Reads an encoded 4-byte value at vaddr `at`, returns the resolved value.
  int64_t enc(uint8_t e, uint64_t at) {
    if (e == PE_udata4) return get<uint32_t>(at);
    if ((e & 0x0f) == PE_sdata4) {
      int64_t v = get<int32_t>(at);
      if ((e & 0x70) == PE_pcrel) v += (int64_t)at;
      else if ((e & 0x70) == PE_datarel) v += (int64_t)va;
      else if (e & 0x70) ok = false;
      return v;
    }
    ok = false; return 0;
  }
};
} // namespace

bool scanFdes(const ElfImage& img, std::vector<Func>& out, std::string& err) { return scanFdesAt(img, img.ehFrameHdrVaddr, out, err); }

bool scanAllFdes(const ElfImage& img, std::vector<Func>& out, std::string& err) {
  if (img.modules.empty()) return scanFdes(img, out, err);
  for (auto& m : img.modules) {
    if (!scanFdesAt(img, m.ehFrameHdrVaddr, out, err)) { err = m.name + ": " + err; return false; }
  }
  std::sort(out.begin(), out.end(), [](const Func& a, const Func& b) { return a.start < b.start; });
  return true;
}

bool scanFdesAt(const ElfImage& img, uint64_t hdr, std::vector<Func>& out, std::string& err) {
  if (!hdr) { err = "no PT_GNU_EH_FRAME"; return false; }
  Reader r{img, hdr};
  uint64_t p = hdr;
  uint8_t ver = r.get<uint8_t>(p), pe = r.get<uint8_t>(p + 1), fce = r.get<uint8_t>(p + 2), te = r.get<uint8_t>(p + 3);
  if (!r.ok || ver != 1) { err = "bad .eh_frame_hdr"; return false; }
  p += 4;
  r.enc(pe, p); p += 4;                       // eh_frame_ptr
  int64_t count = r.enc(fce, p); p += 4;      // fde_count
  if (!r.ok) { err = "unsupported .eh_frame_hdr encodings"; return false; }
  out.reserve(count);
  for (int64_t i = 0; i < count; ++i, p += 8) {
    int64_t start = r.enc(te, p), fde = r.enc(te, p + 4);
    // FDE: u32 length, u32 CIE pointer, pc_begin (4), pc_range (4). PS4's compiler uses sdata4 for both.
    int32_t range = r.get<int32_t>(fde + 12);
    if (!r.ok || range <= 0) { err = "bad FDE #" + std::to_string(i); return false; }
    out.push_back({(uint64_t)start, (uint64_t)start + range, (uint64_t)fde});
  }
  return true;
}

NonFde addNonFdeEntries(const ElfImage& img, std::vector<Func>& funcs) {
  NonFde r;
  std::sort(funcs.begin(), funcs.end(), [](const Func& a, const Func& b) { return a.start < b.start; });
  // Is `a` inside an FDE function? Returns the index of the first function that starts after `a` in `next`.
  auto covered = [&](uint64_t a, size_t& next) {
    auto it = std::upper_bound(funcs.begin(), funcs.end(), a, [](uint64_t v, const Func& f) { return v < f.start; });
    next = it - funcs.begin();
    return it != funcs.begin() && (it - 1)->end > a;
  };
  auto execEnd = [&](uint64_t a) { for (auto& s : img.loads) if ((s.flags & 1) && a >= s.vaddr && a < s.vaddr + s.filesz) return s.vaddr + s.filesz; return (uint64_t)0; };
  std::vector<Func> add;
  auto addEntry = [&](uint64_t a, uint8_t kind, uint64_t maxLen) {
    size_t next;
    if (!a || covered(a, next)) return;
    const uint64_t se = execEnd(a);
    if (!se) return;
    uint64_t end = std::min(se, next < funcs.size() ? funcs[next].start : se);
    end = std::min(end, a + maxLen);
    if (end > a) add.push_back({a, end, 0, kind});
  };
  for (auto& m : img.modules) { if (m.init) { size_t b = add.size(); addEntry(m.init, 1, 1 << 16); r.entries += add.size() - b; } }
  if (img.boot) {
    std::set<uint64_t> slots;
    for (auto& rl : img.links.relocs) slots.insert(rl.target);
    for (auto& s : img.loads) {
      if (!(s.flags & 1)) continue;
      const uint8_t* p = img.file.data() + s.offset;
      for (uint64_t i = 0; i + 6 <= s.filesz; ++i) {
        if (p[i] != 0xff || p[i + 1] != 0x25) continue;
        int32_t d; memcpy(&d, p + i + 2, 4);
        if (!slots.count(s.vaddr + i + 6 + (int64_t)d)) continue;
        size_t b = add.size(); addEntry(s.vaddr + i, 2, 6); r.stubs += add.size() - b;
      }
    }
  }
  std::sort(add.begin(), add.end(), [](const Func& a, const Func& b) { return a.start < b.start; });
  add.erase(std::unique(add.begin(), add.end(), [](const Func& a, const Func& b) { return a.start == b.start; }), add.end());
  // pseudo-functions must not overlap each other either: a stub ends where the next entry starts
  for (size_t i = 0; i + 1 < add.size(); ++i) add[i].end = std::min(add[i].end, add[i + 1].start);
  funcs.insert(funcs.end(), add.begin(), add.end());
  std::sort(funcs.begin(), funcs.end(), [](const Func& a, const Func& b) { return a.start < b.start; });
  return r;
}
