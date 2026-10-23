// Function list from .eh_frame_hdr: one FDE per function, so starts and ends come for free.
#pragma once
#include "elf_image.h"

struct Func { uint64_t start, end; }; // guest vaddrs (image-relative), [start, end)

// Parses the binary search table of PT_GNU_EH_FRAME and the pc range of each FDE.
// Returns false and sets err when the encodings are not the ones PS4 binaries use.
bool scanFdes(const ElfImage& img, std::vector<Func>& out, std::string& err);
