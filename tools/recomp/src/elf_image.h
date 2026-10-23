// PS4 ELF (e_type 0xfe10) loader for bb-recomp: maps the load segments into host memory.
//
// FEX decodes guest code from host memory at the guest RIP (the Decoder reads *(uint8_t*)RIP), so the
// image has to live at the address the IR will name. Vaddr 0 cannot be mapped (mmap_min_addr), so the
// image is mapped at an offset: guest address = base + vaddr. The IR therefore holds absolute
// addresses (rip-relative targets, branch targets) in that space; the lowering subtracts the base.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ElfSegment {
  uint64_t vaddr, filesz, memsz, offset;
  uint32_t flags; // PF_X 1, PF_W 2, PF_R 4
};

struct ElfImage {
  std::vector<ElfSegment> loads;         // PT_LOAD
  uint64_t ehFrameHdrVaddr = 0, ehFrameHdrSize = 0; // PT_GNU_EH_FRAME
  uint64_t entry = 0;
  uint64_t base = 0;                     // host address of vaddr 0
  uint64_t textLo = 0, textHi = 0;       // executable segment [vaddr, vaddr+memsz)
  std::vector<uint8_t> file;             // the whole file (kept for the .eh_frame_hdr parser)

  // Reads the file and maps every PT_LOAD at base + vaddr (anonymous memory, filled from the file).
  // The mapping stays for the life of the process. Returns false and sets err on failure.
  bool load(const std::string& path, uint64_t baseAddr, std::string& err);

  // Bytes of the file for a vaddr (inside a PT_LOAD), or nullptr.
  const uint8_t* fileBytes(uint64_t vaddr, uint64_t len) const;
};
