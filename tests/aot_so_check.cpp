// SPDX-License-Identifier: GPL-2.0-or-later
// Dev check of a real bb-recomp .so against the real image, without running the game (needs the game's prepared files, so it is not part of the
// automatic tests): maps the flat image (out/boot-linked.bin's memory image), applies BBPATCH2 patch parts as the loader does, runs the engine's
// start-up classification (the .so's tables, unit hashes, variants) and prints what runs as an original, as a variant and what has no native code.
//   x86-64 .so: bb-recomp --target x86_64 --so out/aot/libbbaot_x86.so --variants tools/recomp/variants ...
//   c++ -std=c++20 -O1 -g -pthread -Icpu -Isrc -Igpu/shim -Igpu/shadps4 tests/aot_so_check.cpp gpu/shim/bbport_frame_rate.cpp gpu/shim/bbport_display.cpp \
//       gpu/shim/bbport_guest_guards.cpp -ldl -rdynamic -o out/aot-so-check
//   out/aot-so-check libbbaot_x86.so image.bin [--patchers] [--hooks catalog.txt ...] [patch-part.bin ...]
// --patchers runs the port's own patchers (frame rate, display, guards, scene size: their environment is the process's: BB_OUTPUT_RES, BB_RENDER_RES ...)
// against the engine, as bbgpu_patch_image does; --hooks puts a (do-nothing) hook slot at every `hook` line of a catalog, as the online module does.
#include "../cpu/guest_cpu_aot.cpp"

#include <fcntl.h>

#include <fstream>
#include <sstream>

#include "bbport_display.h"
#include "bbport_frame_rate.h"
#include "bbport_guest_guards.h"

extern "C" int runtime_memory_is_mapped(uintptr_t, uint64_t) { return 0; }
bool BbSubmitFxInitZero(std::uint64_t, std::uint32_t) { return false; }

#include "../tests/aot_test_services.h"

static std::vector<uint8_t> ReadFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(2); }
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> d(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) std::exit(2);
    std::fclose(f);
    return d;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: aot-so-check <bb-recomp .so> <image.bin> [patch part.bin ...]\n"); return 2; }
    const std::vector<uint8_t> image = ReadFile(argv[2]);
    constexpr uintptr_t Base = 0x800000000ull;
    void* mem = mmap(reinterpret_cast<void*>(Base), image.size() + 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem != reinterpret_cast<void*>(Base)) { std::perror("mmap"); return 2; }
    std::memcpy(mem, image.data(), image.size());
    Load(argv[1]);
    if (!E.on) return 1;
    OpenImage(Base, image.size());
    bool patchers = false;
    std::vector<const char*> catalogs;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--patchers")) { patchers = true; continue; }
        if (!std::strcmp(argv[i], "--hooks") && i + 1 < argc) { catalogs.push_back(argv[++i]); continue; }
        // BBPATCH2: magic, base, count, {offset, length, bytes}
        const std::vector<uint8_t> p = ReadFile(argv[i]);
        uint64_t count; std::memcpy(&count, p.data() + 16, 8);
        size_t pos = 24;
        for (uint64_t k = 0; k < count; ++k) {
            uint64_t off, len; std::memcpy(&off, p.data() + pos, 8); std::memcpy(&len, p.data() + pos + 8, 8);
            std::memcpy(reinterpret_cast<uint8_t*>(Base) + off, p.data() + pos + 16, len);
            pos += 16 + len;
        }
        std::printf("applied %s\n", argv[i]);
    }
    if (patchers) {
        BbFrameRate::PatchImage(static_cast<unsigned char*>(mem), image.size());
        BbDisplay::PatchImage(static_cast<unsigned char*>(mem), image.size());
        BbGuestGuards::PatchImage(static_cast<unsigned char*>(mem), image.size());
    }
    for (const char* path : catalogs) {  // a do-nothing slot hook at every site the catalog declares
        std::ifstream in(path);
        std::string line;
        unsigned set = 0, refused = 0;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string word, addr;
            if (!(ls >> word >> addr) || word != "hook") continue;
            const uintptr_t at = Base + std::strtoull(addr.c_str(), nullptr, 16);
            if (guest_cpu_hook_slot(at, 5, [](void*, GuestRegsEx*) -> uint32_t { return GUEST_HOOK_CONTINUE; }, nullptr)) ++set; else ++refused;
        }
        std::printf("hooks %s: %u set, %u refused\n", path, set, refused);
    }
    PrepareImage();
    std::printf("units %llu, dirty %llu (trap units %llu); functions: %llu variant activations, %llu dirty\n", (unsigned long long)E.count,
                (unsigned long long)E.dirty_count.load(), (unsigned long long)E.trap_units, (unsigned long long)V.to_variant.load(), (unsigned long long)V.to_dirty.load());
    if (std::getenv("BB_AOT_DEBUG")) {
        for (uint64_t f = 0; f < V.nfuncs; ++f)
            if (V.state[f] && !V.locked[f]) {
                std::printf("  %s +%#x", V.state[f] == 1 ? "variant" : "DIRTY  ", V.funcs[f].start);
                if (V.state[f] == 1) std::printf(" %s", V.vnames + V.vfuncs[V.active[f]].nameOff);
                std::printf("\n");
            }
    }
    return 0;
}
