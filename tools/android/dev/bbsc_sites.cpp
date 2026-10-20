// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android dev tool (run on an x86-64 PC, tools/android/dev/bbsc_sites.sh): which of BBSC's hooks and byte
// patches (gpu/bbnet20/bbsc) the game's code accepts, and which the port's own patches keep from installing.
// It maps the decrypted 1.09 eboot (out/eboot.elf) where the runtime maps the image, optionally applies the
// port's patches to it (the patch-parts files the launcher loads; the C++ patchers' sites are listed below, from
// gpu/shim/bbport_frame_rate.cpp, bbport_guest_guards.cpp, bbport_guest_hooks.cpp, bbport_display.cpp), then
// runs BBSC's own install code against it with the real InstallGuestCodeHook (gpu/shim/bbport_guest_code_hooks.cpp,
// the hook called directly: an x86-64 host) and prints what installed, what was refused and why, and every byte
// BBSC's patch tables wrote.
//   usage: bbsc_sites ELF [PATCH-PARTS-DIR|-] [--ports]      (--ports: also the C++ patchers' sites)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/mman.h>

#include "common/memory_patcher.h"
#include "core/bbsc_persistence.h"
#include "core/bloodborne_re.h"
#include "core/cpu_patches.h"
#include "core/file_format/psf.h"
#include "common/singleton.h"

// ---- Stand-ins for what BBSC asks of the rest of the module ----
extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end) {
    *prot = 7;
    *type = 0;
    *end = (address | 0xfff) + 1;
    return 1;
}
extern "C" void guest_cpu_code(uintptr_t, size_t) {}
extern "C" uintptr_t guest_cpu_host_function(void* fn) { return uintptr_t(fn); }
extern "C" void guest_cpu_invalidate(uintptr_t, size_t) {}
extern "C" uint64_t guest_cpu_call(uintptr_t, unsigned, const uint64_t*) { return 0; }
namespace BbNet { void EnsureGuestThread() {} }
namespace Core::Debugger { int GetCurrentPid() { return 1; } }
#include "common/path_util.h"
namespace Common::FS {
const std::filesystem::path& GetUserPath(PathType) {
    static const std::filesystem::path path = std::filesystem::temp_directory_path() / "bbsc-sites";
    return path;
}
std::string PathToUTF8String(const std::filesystem::path& p) { return p.string(); }
} // namespace Common::FS
namespace Libraries::Np::NpMatching2 { void BbscReplayRoomMembers(bool) {} }
namespace Libraries::Np::NpSignaling::Helpers {
void SendBbscFollowToPeers(unsigned, float, float, float, float, unsigned, int) {}
void SendBbscWorldFlagsToPeers(const void*, unsigned) {}
} // namespace Libraries::Np::NpSignaling::Helpers

namespace Core {
bool RealInstallGuestCodeHook(void*, std::span<const std::uint8_t>, std::uint64_t, GuestCodeHook, const std::uint8_t*);
}

namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;
constexpr u64 ImageBase = 0x800000000ull;

struct Owner {
    u64 start, end;
    std::string name;
};
std::vector<Owner> owners;

struct Hook {
    u64 offset;
    std::size_t length;
    u64 tag;
    bool skip;
    bool installed;
};
std::vector<Hook> hooks;

std::string Explain(std::uintptr_t address, std::size_t size) {
    const u64 offset = address - ImageBase;
    for (const auto& o : owners) {
        if (o.start < offset + size && offset < o.end) {
            return "overlaps " + o.name;
        }
    }
    return "no port patch there: not the 1.09 bytes BBSC expects";
}

void Own(u64 start, u64 size, const char* name) {
    owners.push_back({start, start + size, name});
}

/// The C++ patchers' sites (what they write over, whole instructions, generously).
void PortSites() {
    const char* fr = "frame-rate hooks (bbport_frame_rate)";
    for (const auto& [at, n] : std::initializer_list<std::pair<u64, u64>>{
             {0x203493d, 8}, {0x2034d96, 9}, {0x2018e36, 7}, {0x17f9b71, 7}, {0x1572a7d, 7}, {0x1514c24, 8}}) {
        Own(at, n, fr);
    }
    // The redirected constants: the instruction's address operand (VEX, up to 9 bytes).
    for (const u64 at : {0xab9f0d, 0xdca3ed, 0xee596a, 0xee5a8a, 0xee5baa, 0xef06f1, 0xef6992, 0x1044943, 0x12a4416, 0x15dc1f7,
                         0x173456e, 0x17f9d26, 0x180f647, 0x18bc4f7, 0x18bc50d, 0x1dbc308, 0x1f77d4a, 0x227784b, 0x2277892,
                         0x2313dad, 0x2313db7, 0x2315d71, 0x1515fab, 0x1516030, 0x19084f5, 0x1908522, 0x1908518, 0x190854c,
                         0x143e2b2, 0x143af8c, 0x1a4abca, 0x1514c4d, 0x47c6dc, 0x7e45be, 0x7e4642, 0x7e6239, 0x7f706d,
                         0x7f70ad, 0x7f79e5, 0x7f7be8, 0x7f7dbd, 0x805898, 0x805d18, 0x806269, 0x8062f0, 0x809d62, 0x80a4c9,
                         0x80a876}) {
        Own(at, 9, fr);
    }
    const char* gg = "guest guards (bbport_guest_guards)";
    for (const auto& [at, n] : std::initializer_list<std::pair<u64, u64>>{
             {0xbbc3e0, 6}, {0x2199310, 5}, {0xebbc86, 5}, {0xebbca4, 5}, {0xebbcc2, 5}}) {
        Own(at, n, gg);
    }
    const char* gh = "guest hooks (bbport_guest_hooks)";
    for (const u64 at : {0x26aa255ull, 0x11e6629ull, 0x11e6736ull, 0x11e6846ull, 0x216d5eaull, 0x2171feaull}) {
        Own(at, 2, gh);
    }
    const char* dp = "display hooks (bbport_display, any aspect but 16:9)";
    for (const u64 at : {0x1436c2cull, 0x1436d8aull, 0x1436e68ull, 0x1436eecull, 0x1436f4bull}) Own(at, 5, dp);
    for (const u64 at : {0x1644379ull, 0x1644c45ull, 0x16452b7ull}) Own(at, 9, dp);
    Own(0x1f510, 6, dp);
    Own(0x1bf9e10, 6, dp);
    Own(0x02196A6B - 0x400000, 5, dp);
    Own(0x02196A7A - 0x400000, 5, dp);
}

/// BBPATCH2: magic, image base, count, then {offset, length, bytes}.
int ApplyPatchFile(u8* image, const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    u64 base = 0, count = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&base), 8);
    f.read(reinterpret_cast<char*>(&count), 8);
    if (!f || std::memcmp(magic, "BBPATCH2", 8) != 0) {
        return 0;
    }
    const std::string name = "patch part " + path.filename().string();
    owners.reserve(owners.size() + count);
    static std::vector<std::string> names;
    names.push_back(name);
    int written = 0;
    for (u64 i = 0; i < count; ++i) {
        u64 offset = 0, length = 0;
        f.read(reinterpret_cast<char*>(&offset), 8);
        f.read(reinterpret_cast<char*>(&length), 8);
        std::vector<char> data(length);
        f.read(data.data(), std::streamsize(length));
        std::memcpy(image + offset, data.data(), length);
        owners.push_back({offset, offset + length, names.back()});
        ++written;
    }
    return written;
}
} // namespace

namespace Core {
bool InstallGuestCodeHook(void* address, std::span<const std::uint8_t> expected, std::uint64_t tag, GuestCodeHook hook,
                          const std::uint8_t* skip_flag) {
    const bool ok = RealInstallGuestCodeHook(address, expected, tag, hook, skip_flag);
    hooks.push_back({reinterpret_cast<u64>(address) - ImageBase, expected.size(), tag, skip_flag != nullptr, ok});
    return ok;
}
} // namespace Core

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bbsc_sites ELF [PATCH-PARTS-DIR|-] [--ports]\n");
        return 2;
    }
    std::ifstream elf(argv[1], std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(elf)), std::istreambuf_iterator<char>());
    if (file.size() < 64) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    // Program headers: the PT_LOAD segments, as the runtime lays the image out (vaddr = image offset).
    std::uint64_t phoff;
    std::uint16_t phentsize, phnum;
    std::memcpy(&phoff, &file[32], 8);
    std::memcpy(&phentsize, &file[54], 2);
    std::memcpy(&phnum, &file[56], 2);
    const std::size_t size = 0x5800000;
    auto* image = static_cast<u8*>(mmap(reinterpret_cast<void*>(ImageBase), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    if (image == MAP_FAILED || reinterpret_cast<u64>(image) != ImageBase) {
        std::perror("mmap of the image");
        return 2;
    }
    for (unsigned i = 0; i < phnum; ++i) {
        const char* ph = &file[phoff + std::size_t(i) * phentsize];
        std::uint32_t type;
        std::uint64_t offset, vaddr, filesz;
        std::memcpy(&type, ph, 4);
        std::memcpy(&offset, ph + 8, 8);
        std::memcpy(&vaddr, ph + 16, 8);
        std::memcpy(&filesz, ph + 32, 8);
        if (type == 1 && vaddr + filesz <= size) {
            std::memcpy(image + vaddr, &file[offset], filesz);
        }
    }
    const std::string parts = argc > 2 ? argv[2] : "-";
    bool ports = false;
    for (int i = 2; i < argc; ++i) ports = ports || std::string(argv[i]) == "--ports";
    if (parts != "-" && parts != "--ports") {
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(parts)) {
            if (e.path().extension() == ".bin") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            std::printf("patch part %-36s %4d writes\n", p.filename().c_str(), ApplyPatchFile(image, p));
        }
    }
    if (ports) {
        PortSites();
        // What the patchers write is not the 1.09 code: flip those bytes.
        for (const auto& o : owners) {
            if (o.name.rfind("patch part", 0) != 0) {
                for (u64 at = o.start; at < o.end; ++at) image[at] ^= 0x5a;
            }
        }
    }
    std::vector<u8> before(image, image + size);

    MemoryPatcher::g_eboot_address = ImageBase;
    MemoryPatcher::g_eboot_image_size = size;
    MemoryPatcher::g_game_serial = "CUSA03173";
    Common::Singleton<PSF>::Instance()->app_ver = "01.09";
    Core::SetGuestCodeMismatchExplainer(&Explain);
    Core::KeepGuestCodeWritable();
    Core::Bloodborne::InstallSeamlessCoopPatches();
    Core::Bbsc::InstallPersistentSessionPatches();
    Core::Bloodborne::InstallReverseEngineeringTrace();

    // The bytes BBSC's patch tables wrote (hooks excluded: their jumps are listed by the hooks below).
    std::vector<std::pair<u64, u64>> changed;
    for (u64 at = 0; at < size; ++at) {
        if (image[at] != before[at]) {
            if (!changed.empty() && changed.back().second == at) {
                ++changed.back().second;
            } else {
                changed.emplace_back(at, at + 1);
            }
        }
    }
    std::size_t installed = 0, refused = 0;
    std::printf("\n== BBSC code hooks (image offset, bytes, tag) ==\n");
    for (const auto& h : hooks) {
        (h.installed ? installed : refused)++;
        std::printf("%s %#010llx %2zu bytes tag %llu%s\n", h.installed ? "hook   " : "REFUSED", (unsigned long long)h.offset,
                    h.length, (unsigned long long)h.tag, h.skip ? " (skip flag)" : "");
    }
    auto is_hook = [&](u64 at) {
        for (const auto& h : hooks) {
            if (h.installed && at >= h.offset && at < h.offset + h.length) return true;
        }
        return false;
    };
    std::printf("\n== BBSC byte patches written to the code (image offset, bytes) ==\n");
    std::size_t patches = 0;
    for (const auto& [a, b] : changed) {
        if (is_hook(a)) continue;
        std::printf("patch   %#010llx %2llu bytes\n", (unsigned long long)a, (unsigned long long)(b - a));
        ++patches;
    }
    std::printf("\nsummary: %zu hooks installed, %zu refused, %zu byte-patch runs written (%zu port-patch ranges known)\n",
                installed, refused, patches, owners.size());
    return refused != 0;
}
