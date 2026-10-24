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

// One module of the image: the eboot (base 0) or a linked system module (libc.prx ...). Module code lives at
// `base` + the module's own vaddr, so every address in the tool, the units and the table is relative to the one
// image base the runtime maps (the same layout as scripts/link_modules.py's boot-linked.bin).
struct ModuleInfo {
  std::string name;
  uint64_t base = 0, size = 0;                      // image-relative
  uint64_t ehFrameHdrVaddr = 0, ehFrameHdrSize = 0; // image-relative (PT_GNU_EH_FRAME of the module)
  uint64_t init = 0;                                // image-relative entry the runtime starts the module at (boot mode; 0: none)
};

// Imports and relocations of a boot-linked image: which GOT slot belongs to which import, and the native
// exports (libc.prx's) that satisfy them. Used to find setjmp-like functions by name.
struct BootLinks {
  std::vector<std::string> imports;                         // "NID#lib#mod"
  struct Binding { uint64_t index, address, kind; };        // import index -> export address in the image (kind 1 function, 2 data)
  std::vector<Binding> bindings;
  struct Reloc { uint64_t target, kind, value, addend; };   // kind 0: *target = image+value; 1: function import; 2: data import
  std::vector<Reloc> relocs;
};

struct ElfImage {
  std::vector<ElfSegment> loads;         // PT_LOAD
  uint64_t ehFrameHdrVaddr = 0, ehFrameHdrSize = 0; // PT_GNU_EH_FRAME
  uint64_t entry = 0;
  uint64_t base = 0;                     // host address of vaddr 0
  uint64_t textLo = 0, textHi = 0;       // executable segment [vaddr, vaddr+memsz)
  std::vector<uint8_t> file;             // the whole file (kept for the .eh_frame_hdr parser); boot mode: the flat image
  std::vector<ModuleInfo> modules;       // module 0 is the eboot; boot mode adds the linked modules
  BootLinks links;                       // boot mode only
  bool boot = false;

  // Reads the file and maps every PT_LOAD at base + vaddr (anonymous memory, filled from the file).
  // The mapping stays for the life of the process. Returns false and sets err on failure.
  bool load(const std::string& path, uint64_t baseAddr, std::string& err);

  // Boot mode: the memory image the runtime builds (scripts/link_modules.py output, BBPROBE5: eboot + linked modules),
  // mapped at base + vaddr. `ebootElf` supplies the eboot's PT_GNU_EH_FRAME, the .prx files (in prxDir, named in
  // link.json next to the boot file, else libc.prx and libSceFios2.prx) theirs.
  bool loadBoot(const std::string& bootPath, const std::string& ebootElf, const std::string& prxDir, uint64_t baseAddr, std::string& err);
  const ModuleInfo* moduleOf(uint64_t vaddr) const;

  // Bytes of the file for a vaddr (inside a PT_LOAD), or nullptr.
  const uint8_t* fileBytes(uint64_t vaddr, uint64_t len) const;
};
