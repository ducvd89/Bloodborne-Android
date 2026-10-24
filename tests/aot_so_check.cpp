// SPDX-License-Identifier: GPL-2.0-or-later
// Dev check of a real bb-recomp .so against the real image, without running the game (needs the game's prepared files, so it is not part of the
// automatic tests): maps the flat image (out/boot-linked.bin's memory image), applies BBPATCH2 patch parts as the loader does, runs the engine's
// start-up classification (the .so's tables, unit hashes, variants) and prints what runs as an original, as a variant and on the fallback.
//   x86-64 .so: bb-recomp --target x86_64 --so out/aot/libbbaot_x86.so --variants tools/recomp/variants ...
//   c++ -std=c++20 -O1 -g -pthread -Icpu -Isrc tests/aot_so_check.cpp -ldl -rdynamic -o out/aot-so-check
//   out/aot-so-check libbbaot_x86.so image.bin [patch-part.bin ...]
#include "../cpu/guest_cpu_aot.cpp"

#include <fcntl.h>

extern "C" {
void bb_fb_init(void* (*)(size_t, int)) {}
void bb_fb_code(uintptr_t, size_t) {}
uintptr_t bb_fb_host_function(void* fn) { return uintptr_t(fn); }
uint64_t bb_fb_call(uintptr_t, unsigned, const uint64_t*) { return 0; }
void bb_fb_thread_stack(void*, size_t) {}
void bb_fb_set_gs(void*) {}
void bb_fb_abandon(void) {}
void bb_fb_thread_end(void) {}
uintptr_t bb_fb_return_address(uintptr_t n) { return n; }
uintptr_t bb_fb_stack_pointer(void) { return 0; }
int bb_fb_signal_regs(const void*, GuestRegs*) { return 0; }
uintptr_t bb_fb_host_pc(const void*) { return 0; }
int bb_fb_handle_fault(int, siginfo_t*, void*) { return 0; }
int bb_fb_hook(uintptr_t, size_t, GuestHook) { return 0; }
void bb_fb_invalidate(uintptr_t, size_t) {}
uint64_t bb_fb_tsc(void) { return 0; }
uint64_t bb_fb_tsc_frequency(void) { return 1; }
}

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
    for (int i = 3; i < argc; ++i) {  // BBPATCH2: magic, base, count, {offset, length, bytes}
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
