// Function list from .eh_frame_hdr: one FDE per function, so starts and ends come for free.
#pragma once
#include "elf_image.h"

struct Func { uint64_t start, end; uint64_t fde = 0; }; // guest vaddrs (image-relative), [start, end); fde: vaddr of its FDE record in .eh_frame

// Parses the binary search table of PT_GNU_EH_FRAME and the pc range of each FDE.
// Returns false and sets err when the encodings are not the ones PS4 binaries use.
bool scanFdes(const ElfImage& img, std::vector<Func>& out, std::string& err);
// The same for the .eh_frame_hdr at image vaddr `hdr` (a linked module's); appends to out.
bool scanFdesAt(const ElfImage& img, uint64_t hdr, std::vector<Func>& out, std::string& err);
// Every module of the image (the eboot, then the linked modules), sorted by address.
bool scanAllFdes(const ElfImage& img, std::vector<Func>& out, std::string& err);
