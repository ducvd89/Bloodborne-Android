#include "fde_scan.h"
#include <cstring>

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

bool scanFdes(const ElfImage& img, std::vector<Func>& out, std::string& err) {
  if (!img.ehFrameHdrVaddr) { err = "no PT_GNU_EH_FRAME"; return false; }
  Reader r{img, img.ehFrameHdrVaddr};
  uint64_t p = img.ehFrameHdrVaddr;
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
    out.push_back({(uint64_t)start, (uint64_t)start + range});
  }
  return true;
}
