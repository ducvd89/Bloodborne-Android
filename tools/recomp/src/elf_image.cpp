#include "elf_image.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
constexpr uint32_t PT_LOAD_ = 1, PT_GNU_EH_FRAME_ = 0x6474e550;
constexpr uint64_t Page = 0x4000; // PS4 segment alignment; also a multiple of the host page size

// Program headers of an ELF file, plain or wrapped in a (plaintext) PS4 SELF: magic 4f 15 3d 1d, u16 segment count at
// +24, the ELF image after the 32-byte header and `count` 32-byte segment entries.
bool programHeaders(const std::vector<uint8_t>& f, std::vector<Phdr>& out, std::string& err) {
  size_t base = 0;
  if (f.size() >= 4 && !memcmp(f.data(), "\x4f\x15\x3d\x1d", 4)) {
    uint16_t count; if (f.size() < 26) { err = "short SELF"; return false; }
    memcpy(&count, f.data() + 24, 2);
    base = 32 + (size_t)count * 32;
  }
  if (f.size() < base + sizeof(Ehdr) || memcmp(f.data() + base, "\x7f" "ELF", 4)) { err = "not an ELF/SELF file"; return false; }
  Ehdr eh; memcpy(&eh, f.data() + base, sizeof eh);
  if (eh.machine != 0x3e) { err = "not x86-64"; return false; }
  if (eh.phentsize < sizeof(Phdr) || base + eh.phoff + (size_t)eh.phnum * eh.phentsize > f.size()) { err = "bad program headers"; return false; }
  for (unsigned i = 0; i < eh.phnum; ++i) { Phdr ph; memcpy(&ph, f.data() + base + eh.phoff + i * eh.phentsize, sizeof ph); out.push_back(ph); }
  return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END); out.resize(ftell(f)); fseek(f, 0, SEEK_SET);
  const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

void* mapFixed(uint64_t addr, uint64_t size) {
#ifdef _WIN32
  return VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void* p = mmap((void*)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}
} // namespace

bool ElfImage::load(const std::string& path, uint64_t baseAddr, std::string& err) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { err = "cannot open " + path; return false; }
  fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
  if (fread(file.data(), 1, file.size(), f) != file.size()) { fclose(f); err = "short read"; return false; }
  fclose(f);
  if (file.size() < sizeof(Ehdr) || memcmp(file.data(), "\x7f" "ELF", 4)) { err = "not an ELF file"; return false; }
  Ehdr eh; memcpy(&eh, file.data(), sizeof eh);
  if (eh.machine != 0x3e) { err = "not x86-64"; return false; }
  entry = eh.entry; base = baseAddr;
  uint64_t top = 0;
  for (unsigned i = 0; i < eh.phnum; ++i) {
    Phdr ph; memcpy(&ph, file.data() + eh.phoff + i * eh.phentsize, sizeof ph);
    if (ph.type == PT_LOAD_) {
      loads.push_back({ph.vaddr, ph.filesz, ph.memsz, ph.offset, ph.flags});
      top = std::max(top, ph.vaddr + ph.memsz);
    } else if (ph.type == PT_GNU_EH_FRAME_) {
      ehFrameHdrVaddr = ph.vaddr; ehFrameHdrSize = ph.filesz;
    }
  }
  if (loads.empty()) { err = "no PT_LOAD"; return false; }
  modules.assign(1, ModuleInfo {"eboot", 0, top, ehFrameHdrVaddr, ehFrameHdrSize, entry});
  for (auto& s : loads) {
    if (s.flags & 1) { textLo = s.vaddr; textHi = s.vaddr + s.memsz; }
    uint64_t a = (baseAddr + s.vaddr) & ~(Page - 1), e = (baseAddr + s.vaddr + s.memsz + Page - 1) & ~(Page - 1);
    if (!mapFixed(a, e - a)) {
      // Segments may share a page boundary only if they overlap; PS4 segments are page aligned, so a
      // failure here means the base is already in use.
      err = "cannot map segment at " + std::to_string(a); return false;
    }
    memcpy((void*)(baseAddr + s.vaddr), file.data() + s.offset, s.filesz);
  }
  return true;
}

const uint8_t* ElfImage::fileBytes(uint64_t vaddr, uint64_t len) const {
  for (auto& s : loads)
    if (vaddr >= s.vaddr && vaddr + len <= s.vaddr + s.filesz) return file.data() + s.offset + (vaddr - s.vaddr);
  return nullptr;
}

const ModuleInfo* ElfImage::moduleOf(uint64_t vaddr) const {
  for (auto& m : modules) if (vaddr >= m.base && vaddr < m.base + m.size) return &m;
  return nullptr;
}

bool ElfImage::loadBoot(const std::string& bootPath, const std::string& ebootElf, const std::string& prxDir, uint64_t baseAddr, std::string& err) {
  std::vector<uint8_t> bf;
  if (!readFile(bootPath, bf)) { err = "cannot read " + bootPath; return false; }
  size_t pos = 0;
  auto u64 = [&](uint64_t& v) { if (pos + 8 > bf.size()) { err = "truncated boot file"; return false; } memcpy(&v, bf.data() + pos, 8); pos += 8; return true; };
  if (bf.size() < 8 || memcmp(bf.data(), "BBPROBE5", 8)) { err = "not a BBPROBE5 boot-linked image (scripts/link_modules.py)"; return false; }
  pos = 8;
  uint64_t size, entryOff, ns, nr, ni, caps, procparam, tls[4], mc;
  for (uint64_t* v : {&size, &entryOff, &ns, &nr, &ni, &caps, &procparam, &tls[0], &tls[1], &tls[2], &tls[3], &mc}) if (!u64(*v)) return false;
  if (!size || size > (512u << 20) || !ns || ns > 64 || mc == 0 || mc > 16 || ni > 100000 || nr > 4000000) { err = "boot file limits exceeded"; return false; }
  struct Mod { uint64_t base, size, init, ta, tm, tf, tmod; };
  std::vector<Mod> mods(mc);
  for (auto& m : mods) for (uint64_t* v : {&m.base, &m.size, &m.init, &m.ta, &m.tm, &m.tf, &m.tmod}) if (!u64(*v)) return false;
  uint64_t nb; if (!u64(nb) || nb > ni) { err = "bad binding count"; return false; }
  links.bindings.clear(); links.imports.clear(); links.relocs.clear();
  for (uint64_t i = 0; i < nb; ++i) { BootLinks::Binding b; if (!u64(b.index) || !u64(b.address) || !u64(b.kind)) return false; links.bindings.push_back(b); }
  loads.clear();
  for (uint64_t i = 0; i < ns; ++i) {
    uint64_t a, sz, fl; if (!u64(a) || !u64(sz) || !u64(fl)) return false;
    if (a > size || sz > size - a) { err = "bad segment"; return false; }
    loads.push_back({a, sz, sz, a, (uint32_t)fl}); // the flat image is the file: offset = vaddr
  }
  if (pos + ni * 128 + nr * 32 + size != bf.size()) { err = "boot file size does not match its header"; return false; }
  for (uint64_t i = 0; i < ni; ++i) { links.imports.push_back(std::string((const char*)bf.data() + pos, strnlen((const char*)bf.data() + pos, 128))); pos += 128; }
  for (uint64_t i = 0; i < nr; ++i) { BootLinks::Reloc r; if (!u64(r.target) || !u64(r.kind) || !u64(r.value) || !u64(r.addend)) return false; links.relocs.push_back(r); }
  file.assign(bf.begin() + pos, bf.end()); // the flat memory image, before relocations and patches
  std::vector<uint8_t>().swap(bf);
  boot = true;
  entry = entryOff; base = baseAddr;

  // Module list: the eboot, then the linked modules in link order (names from link.json when present).
  std::vector<std::string> names;
  {
    std::vector<uint8_t> j; std::string dir = bootPath.substr(0, bootPath.rfind('/') == std::string::npos ? 0 : bootPath.rfind('/') + 1);
    if (readFile(dir + "link.json", j)) {
      std::string t(j.begin(), j.end());
      for (size_t at = 0; (at = t.find("\"file\"", at)) != std::string::npos; ) {
        size_t q1 = t.find('"', t.find(':', at) + 1), q2 = t.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        names.push_back(t.substr(q1 + 1, q2 - q1 - 1)); at = q2;
      }
    }
  }
  std::vector<uint8_t> eb;
  std::vector<Phdr> ph;
  if (!readFile(ebootElf, eb)) { err = "cannot read " + ebootElf; return false; }
  if (!programHeaders(eb, ph, err)) { err = ebootElf + ": " + err; return false; }
  modules.clear();
  ModuleInfo main {"eboot", 0, 0, 0, 0, 0};
  main.init = entryOff;
  for (auto& p : ph) if (p.type == PT_GNU_EH_FRAME_) { main.ehFrameHdrVaddr = p.vaddr; main.ehFrameHdrSize = p.filesz; }
  main.size = mods[0].base; // the eboot ends where the first module starts (the module table is in link order)
  ehFrameHdrVaddr = main.ehFrameHdrVaddr; ehFrameHdrSize = main.ehFrameHdrSize;
  modules.push_back(main);
  for (size_t m = 0; m < mods.size(); ++m) {
    ModuleInfo mi; mi.base = mods[m].base; mi.size = mods[m].size; mi.init = mods[m].init;
    mi.name = m < names.size() ? names[m] : (m == 0 ? "libc.prx" : "libSceFios2.prx");
    std::vector<uint8_t> f; std::vector<Phdr> mp;
    if (!readFile(prxDir + "/" + mi.name, f)) { err = "cannot read " + prxDir + "/" + mi.name; return false; }
    if (!programHeaders(f, mp, err)) { err = mi.name + ": " + err; return false; }
    uint64_t end = 0;
    for (auto& p : mp) {
      if (p.type == PT_GNU_EH_FRAME_) { mi.ehFrameHdrVaddr = mi.base + p.vaddr; mi.ehFrameHdrSize = p.filesz; }
      if (p.type == PT_LOAD_ || p.type == 0x61000010) end = std::max(end, p.vaddr + p.memsz);
    }
    if (end != mi.size) { err = mi.name + ": size " + std::to_string(end) + " does not match the boot image module (" + std::to_string(mi.size) + "): wrong .prx for this boot file?"; return false; }
    modules.push_back(mi);
  }
  textLo = 0; textHi = 0;
  for (auto& s : loads) if (s.flags & 1) { if (!textHi) textLo = s.vaddr; textHi = std::max(textHi, s.vaddr + s.memsz); }
  uint64_t a = baseAddr & ~(Page - 1), e = (baseAddr + size + Page - 1) & ~(Page - 1);
  if (!mapFixed(a, e - a)) { err = "cannot map the image at " + std::to_string(a); return false; }
  memcpy((void*)baseAddr, file.data(), file.size());
  return true;
}
