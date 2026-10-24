// Function list from .eh_frame_hdr: one FDE per function, so starts and ends come for free.
#pragma once
#include "elf_image.h"

struct Func { uint64_t start, end; uint64_t fde = 0; uint8_t kind = 0; }; // kind: 0 FDE function, 1 non-FDE entry (module start), 2 PLT stub // guest vaddrs (image-relative), [start, end); fde: vaddr of its FDE record in .eh_frame

// Parses the binary search table of PT_GNU_EH_FRAME and the pc range of each FDE.
// Returns false and sets err when the encodings are not the ones PS4 binaries use.
bool scanFdes(const ElfImage& img, std::vector<Func>& out, std::string& err);
// The same for the .eh_frame_hdr at image vaddr `hdr` (a linked module's); appends to out.
bool scanFdesAt(const ElfImage& img, uint64_t hdr, std::vector<Func>& out, std::string& err);
// Every module of the image (the eboot, then the linked modules), sorted by address.
bool scanAllFdes(const ElfImage& img, std::vector<Func>& out, std::string& err);
// Code the runtime enters that no FDE covers: the eboot entry and the linked modules' start addresses (hand-written
// startup code), and the PLT stubs (`jmp [rip+slot]` through a relocated slot). Each becomes a pseudo-function that ends
// at the next FDE (or entry, or 16 bytes for a stub). Appends to `funcs` (kept sorted). Boot images only for the stubs.
struct NonFde { size_t entries = 0, stubs = 0; };
NonFde addNonFdeEntries(const ElfImage& img, std::vector<Func>& funcs);
