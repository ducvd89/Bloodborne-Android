#include "eh_frame.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {
struct Cur {
  const ElfImage& img;
  uint64_t at;
  bool ok = true;
  template <class T> T get() { auto* p = img.fileBytes(at, sizeof(T)); if (!p) { ok = false; return 0; } T v; memcpy(&v, p, sizeof v); at += sizeof(T); return v; }
  uint64_t uleb() { uint64_t r = 0; for (unsigned sh = 0; sh < 64; sh += 7) { uint8_t b = get<uint8_t>(); r |= (uint64_t)(b & 0x7f) << sh; if (!(b & 0x80)) break; if (!ok) break; } return r; }
  int64_t sleb() { int64_t r = 0; unsigned sh = 0; uint8_t b = 0; do { b = get<uint8_t>(); r |= (int64_t)(b & 0x7f) << sh; sh += 7; } while ((b & 0x80) && ok && sh < 64); if (sh < 64 && (b & 0x40)) r |= -(1ll << sh); return r; }
  // DW_EH_PE value; `hdr` is the datarel base. Returns false for the omit encoding.
  bool enc(uint8_t e, uint64_t hdr, uint64_t& out) {
    if (e == 0xff) return false;
    const uint64_t field = at;
    int64_t v = 0;
    switch (e & 0x0f) {
    case 0x00: v = (int64_t)get<uint64_t>(); break;
    case 0x01: v = (int64_t)uleb(); break;
    case 0x02: v = get<uint16_t>(); break;
    case 0x03: v = get<uint32_t>(); break;
    case 0x04: v = (int64_t)get<uint64_t>(); break;
    case 0x09: v = sleb(); break;
    case 0x0a: v = (int16_t)get<uint16_t>(); break;
    case 0x0b: v = (int32_t)get<uint32_t>(); break;
    case 0x0c: v = (int64_t)get<uint64_t>(); break;
    default: ok = false; return false;
    }
    switch (e & 0x70) {
    case 0x00: break;
    case 0x10: v += (int64_t)field; break;     // pcrel
    case 0x30: v += (int64_t)hdr; break;       // datarel
    default: ok = false; return false;
    }
    out = (uint64_t)v;
    return ok;
  }
};

struct Cie { bool z = false, valid = false; uint8_t fdeEnc = 0, lsdaEnc = 0xff; };

Cie parseCie(const ElfImage& img, uint64_t cieAt, uint64_t hdr) {
  Cie c;
  Cur r{img, cieAt};
  const uint32_t len = r.get<uint32_t>(); r.get<uint32_t>(); // length, id (0)
  if (!r.ok || len == 0 || len == 0xffffffff) return c;
  const uint64_t end = cieAt + 4 + len;
  const uint8_t ver = r.get<uint8_t>();
  std::string aug;
  for (char ch; (ch = (char)r.get<uint8_t>()) && r.ok;) aug.push_back(ch);
  r.uleb(); r.sleb();
  if (ver == 1) r.get<uint8_t>(); else r.uleb();
  if (aug.empty() || aug[0] != 'z') { c.valid = r.ok; return c; }
  c.z = true;
  const uint64_t augLen = r.uleb(); const uint64_t augEnd = r.at + augLen;
  for (size_t i = 1; i < aug.size() && r.ok; ++i) {
    switch (aug[i]) {
    case 'R': c.fdeEnc = r.get<uint8_t>(); break;
    case 'L': c.lsdaEnc = r.get<uint8_t>(); break;
    case 'P': { uint8_t pe = r.get<uint8_t>(); uint64_t dummy; r.enc(pe, hdr, dummy); break; }
    case 'S': case 'B': break;
    default: r.ok = false;
    }
  }
  (void)end; (void)augEnd;
  c.valid = r.ok;
  return c;
}
} // namespace

void scanLandingPads(const ElfImage& img, const std::vector<Func>& funcs, EhScan& out) {
  std::unordered_map<uint64_t, Cie> cies;
  auto fail = [&](const std::string& m) { ++out.badLsda; if (out.firstError.empty()) out.firstError = m; };
  for (const Func& f : funcs) {
    if (!f.fde) continue;
    ++out.fdes;
    // Which module's .eh_frame_hdr the datarel base is: the FDE lies in the same module as its function.
    const ModuleInfo* mod = img.moduleOf(f.start);
    const uint64_t hdr = mod ? mod->ehFrameHdrVaddr : img.ehFrameHdrVaddr;
    Cur r{img, f.fde};
    const uint32_t len = r.get<uint32_t>();
    const uint64_t idAt = r.at;
    const uint32_t cieOff = r.get<uint32_t>();
    if (!r.ok || len == 0 || len == 0xffffffff) { fail("bad FDE header"); continue; }
    const uint64_t cieAt = idAt - cieOff;
    auto it = cies.find(cieAt);
    if (it == cies.end()) it = cies.emplace(cieAt, parseCie(img, cieAt, hdr)).first;
    const Cie& cie = it->second;
    if (!cie.valid) { fail("unparsable CIE"); continue; }
    if (!cie.z || cie.lsdaEnc == 0xff) continue;
    uint64_t pcBegin, pcRange;
    if (!r.enc(cie.fdeEnc, hdr, pcBegin) || !r.enc(cie.fdeEnc & 0x0f, hdr, pcRange)) { fail("bad FDE pc range"); continue; }
    const uint64_t augLen = r.uleb();
    const uint64_t augEnd = r.at + augLen;
    uint64_t lsda = 0;
    if (!r.enc(cie.lsdaEnc, hdr, lsda)) { if (!r.ok) fail("bad FDE LSDA pointer"); continue; }
    (void)augEnd;
    if (!lsda) continue;
    ++out.fdesWithLsda;
    // LSDA: LPStart encoding (+ value), TType encoding (+ uleb offset), call-site encoding, table length, records.
    Cur l{img, lsda};
    const uint8_t lpEnc = l.get<uint8_t>();
    uint64_t lpStart = f.start;
    if (lpEnc != 0xff) { if (!l.enc(lpEnc, hdr, lpStart)) { fail("bad LPStart"); continue; } }
    const uint8_t ttEnc = l.get<uint8_t>();
    if (ttEnc != 0xff) l.uleb();
    const uint8_t csEnc = l.get<uint8_t>();
    const uint64_t csLen = l.uleb();
    const uint64_t csEnd = l.at + csLen;
    if (!l.ok || csLen > 0x100000) { fail("bad call-site table"); continue; }
    auto& pads = out.landingPads[f.start];
    while (l.at < csEnd && l.ok) {
      uint64_t start, size, lp;
      // the call-site fields use csEnc without the pc-relative base (they are offsets): only the value format matters
      if (!l.enc(csEnc & 0x0f, hdr, start) || !l.enc(csEnc & 0x0f, hdr, size) || !l.enc(csEnc & 0x0f, hdr, lp)) break;
      l.uleb(); // action
      ++out.callSites;
      if (lp) pads.push_back(lpStart + lp);
    }
    if (!l.ok) fail("LSDA runs outside the image");
    std::sort(pads.begin(), pads.end());
    pads.erase(std::unique(pads.begin(), pads.end()), pads.end());
    // Pads must lie inside the function (or in its cold part, which has no FDE of its own here): keep those that do.
    pads.erase(std::remove_if(pads.begin(), pads.end(), [&](uint64_t a) { return a < f.start || a >= f.end; }), pads.end());
    out.pads += pads.size();
    if (pads.empty()) out.landingPads.erase(f.start);
  }
}

namespace {
struct NidName { const char* nid; const char* name; };
// NIDs (the import names of the eboot are "<nid>#<lib>#<module>"): the setjmp family (names from the public PS4 symbol
// lists, as src/import_names.inc has them). Only these return twice.
const NidName kReturnsTwice[] = {
  {"gNQ1V2vfXDE", "setjmp"},
};
}

ReturnsTwice findReturnsTwice(const ElfImage& img) {
  ReturnsTwice r;
  if (!img.boot) return r;
  std::set<uint64_t> imports;
  for (size_t i = 0; i < img.links.imports.size(); ++i) {
    const std::string& n = img.links.imports[i];
    for (auto& k : kReturnsTwice)
      if (k.name && n.compare(0, strlen(k.nid), k.nid) == 0 && n.size() > strlen(k.nid) && n[strlen(k.nid)] == '#') { imports.insert(i); r.names.push_back(k.name); }
  }
  for (auto& b : img.links.bindings) if (b.kind == 1 && imports.count(b.index)) r.targets.insert(b.address);
  for (auto& rl : img.links.relocs) if (rl.kind == 1 && imports.count(rl.value)) r.gotSlots.insert(rl.target);
  if (r.gotSlots.empty() && r.targets.empty()) return r;
  // PLT stubs: `jmp [rip+disp32]` (ff 25) through one of the slots, anywhere in the executable segments.
  for (auto& s : img.loads) {
    if (!(s.flags & 1)) continue;
    const uint8_t* p = img.file.data() + s.offset;
    for (uint64_t i = 0; i + 6 <= s.filesz; ++i) {
      if (p[i] != 0xff || p[i + 1] != 0x25) continue;
      int32_t d; memcpy(&d, p + i + 2, 4);
      if (r.gotSlots.count(s.vaddr + i + 6 + (int64_t)d)) r.targets.insert(s.vaddr + i);
    }
  }
  return r;
}
