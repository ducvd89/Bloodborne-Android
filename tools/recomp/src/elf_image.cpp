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
