// SPDX-License-Identifier: GPL-2.0-or-later
// Prints the variant catalog entries of the port's own code patchers (frame rate, display, guest guards), by running them: the real patcher sources
// (gpu/shim/bbport_frame_rate.cpp, bbport_display.cpp, bbport_guest_guards.cpp) are built with BB_HOOK_SITE_DUMP, which makes bbport_hook_sites.h print
// every native hook site they declare (`hook <image offset> <length> snap [resume=...]`) instead of installing it, and run against the game's flat
// image (out/boot-linked.bin's memory image). The bytes the frame-rate patcher writes in that mode (the operand redirects to `live` at its fixed
// place in the image, the nops and the run speed's divss) are printed as a group (`@group` / `@w` lines). Run by gen_patchers.py with the settings
// that install each patcher; the environment is the patchers' own (BB_FPS, BB_RENDER_RES, BB_OUTPUT_RES ...).
//   usage: gen_patchers IMAGE.bin [BBPATCH2 part ...]   (the parts are applied first, as the loader does, and are not part of what is printed)
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../../src/guest_cpu.h"
#include "bbport_display.h"
#include "bbport_frame_rate.h"
#include "bbport_guest_guards.h"

extern "C" {
void guest_cpu_code(uintptr_t, size_t) {}
uintptr_t guest_cpu_host_function(void* fn) { return uintptr_t(fn); }
uint64_t guest_cpu_call(uintptr_t, unsigned, const uint64_t*) { return 0; }
int guest_cpu_has_hook_slot(uintptr_t) { return 1; }
int guest_cpu_hook_slot(uintptr_t, size_t, GuestHookEx, void*) { return 1; }
int runtime_memory_is_mapped(uintptr_t, uint64_t) { return 0; }
}
bool BbSubmitFxInitZero(std::uint64_t, std::uint32_t) { return false; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: gen_patchers IMAGE.bin\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 2; }
    std::fseek(f, 0, SEEK_END);
    const size_t size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    constexpr uintptr_t Base = 0x800000000ull;
    auto* image = static_cast<unsigned char*>(mmap(reinterpret_cast<void*>(Base), size + 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    if (image == MAP_FAILED || reinterpret_cast<uintptr_t>(image) != Base) { std::perror("mmap"); return 2; }
    if (std::fread(image, 1, size, f) != size) return 2;
    std::fclose(f);
    for (int i = 2; i < argc; ++i) {  // BBPATCH2: magic, base, count, {offset, length, bytes}
        FILE* p = std::fopen(argv[i], "rb");
        if (!p) { std::perror(argv[i]); return 2; }
        char magic[8];
        std::uint64_t base, count;
        if (std::fread(magic, 1, 8, p) != 8 || std::memcmp(magic, "BBPATCH2", 8) || std::fread(&base, 8, 1, p) != 1 || std::fread(&count, 8, 1, p) != 1) return 2;
        for (std::uint64_t k = 0; k < count; ++k) {
            std::uint64_t off, len;
            if (std::fread(&off, 8, 1, p) != 1 || std::fread(&len, 8, 1, p) != 1 || off + len > size || std::fread(image + off, 1, len, p) != len) return 2;
        }
        std::fclose(p);
    }
    std::vector<unsigned char> before(image, image + size);
    BbFrameRate::PatchImage(image, size);
    BbDisplay::PatchImage(image, size);
    BbGuestGuards::PatchImage(image, size);
    // The bytes the patchers wrote.
    std::printf("@group\n");
    for (size_t at = 0; at < size;) {
        if (image[at] == before[at]) { ++at; continue; }
        size_t end = at;
        while (end < size && image[end] != before[end]) ++end;
        std::printf("@w %zx ", at);
        for (size_t k = at; k < end; ++k) std::printf("%02x", image[k]);
        std::printf("\n");
        at = end;
    }
    return 0;
}
