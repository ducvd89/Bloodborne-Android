// SPDX-License-Identifier: GPL-2.0-or-later
// bb-texpack GAME_DIR [CACHE]: whether the game folder holds a texture pack (upscaled map textures), for
// run-bionic.sh. A port of scripts/game_check.py texture_pack (upstream #240): a map texture with a side above
// the retail game's 4096 pixels fills the game's graphics heaps, so such a folder gets the larger heaps.
// Prints "1 <file> <side>" or "0". CACHE keeps the answer until a texture file's size or time changes (the
// first look reads ~8600 archive entries). The folder is the one the game runs from (the mods overlay):
// directory links are followed.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t RETAIL_TEXTURE_SIDE = 4096;
// Only archive entries this large are opened: a texture with a side above 4096 packs to more.
constexpr int64_t TEXTURE_PACK_MIN_ENTRY = 256 << 10;

struct TexFile {
    std::string path, rel;
    int64_t size;
    int64_t mtime_ns;
};

bool ends_with_nocase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
    return true;
}

void walk(const std::string& dir, const std::string& root, int depth, std::vector<TexFile>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d || depth > 12) {
        if (d) closedir(d);
        return;
    }
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = dir + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(path, root, depth + 1, out);
        } else if (S_ISREG(st.st_mode) && (ends_with_nocase(name, ".tpfbhd") || ends_with_nocase(name, ".tpf.dcx"))) {
            out.push_back({path, path.substr(root.size() + 1), (int64_t)st.st_size,
                           (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec});
        }
    }
    closedir(d);
}

// The larger side of the first texture in a DFLT .dcx PS4 TPF (at most 0x4C + 512 bytes read), 0 for anything else.
uint32_t tpf_side(const std::vector<uint8_t>& blob) {
    if (blob.size() < 0x4D || std::memcmp(blob.data(), "DCX\0", 4) != 0 || std::memcmp(blob.data() + 0x28, "DFLT", 4) != 0)
        return 0;
    uint8_t tpf[32];
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return 0;
    z.next_in = const_cast<Bytef*>(blob.data() + 0x4C);
    z.avail_in = (uInt)(blob.size() - 0x4C);
    z.next_out = tpf;
    z.avail_out = sizeof tpf;
    const int rc = inflate(&z, Z_SYNC_FLUSH);
    const size_t got = sizeof tpf - z.avail_out;
    inflateEnd(&z);
    if ((rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) || got < 32) return 0;
    // TPF header (16 bytes, platform 4: PS4), then the first texture: offset, size, format, type, mipmaps, flags, width, height.
    if (std::memcmp(tpf, "TPF\0", 4) != 0 || tpf[12] != 4) return 0;
    const uint32_t w = tpf[28] | tpf[29] << 8, h = tpf[30] | tpf[31] << 8;
    return std::max(w, h);
}

bool read_all(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
int64_t rd64(const uint8_t* p) { return (int64_t)rd32(p) | (int64_t)rd32(p + 4) << 32; }

// The larger side of the first oversized texture in a .tpfbhd/.tpfbdt pair, else 0.
uint32_t archive_side(const std::string& bhd_path) {
    const size_t slash = bhd_path.rfind('/');
    const std::string dir = bhd_path.substr(0, slash), name = bhd_path.substr(slash + 1);
    std::string want = name.substr(0, name.size() - 3) + "bdt";
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string bdt_path;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
            if (n == want) {
                bdt_path = dir + "/" + e->d_name;
                break;
            }
        }
        closedir(d);
    }
    std::vector<uint8_t> bhd;
    if (bdt_path.empty() || !read_all(bhd_path, bhd) || bhd.size() < 0x40 || std::memcmp(bhd.data(), "BHF4", 4) != 0 ||
        rd64(bhd.data() + 0x20) != 36)
        return 0;
    const int32_t count = (int32_t)rd32(bhd.data() + 0x0C);
    FILE* bdt = std::fopen(bdt_path.c_str(), "rb");
    if (!bdt) return 0;
    uint32_t found = 0;
    for (int32_t i = 0; i < count && found == 0; ++i) {
        const size_t entry = 0x40 + 36 * (size_t)i;
        if (entry + 36 > bhd.size()) break;
        const int64_t packed = rd64(bhd.data() + entry + 8);
        const uint32_t offset = rd32(bhd.data() + entry + 24);
        if (packed < TEXTURE_PACK_MIN_ENTRY) continue;
        std::vector<uint8_t> blob(0x4C + 512);
        if (std::fseek(bdt, (long)offset, SEEK_SET) != 0) continue;
        blob.resize(std::fread(blob.data(), 1, blob.size(), bdt));
        if (const uint32_t side = tpf_side(blob); side > RETAIL_TEXTURE_SIDE) found = side;
    }
    std::fclose(bdt);
    return found;
}

uint64_t key_of(const std::vector<TexFile>& files) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
    };
    for (const auto& f : files) {
        mix(f.rel.data(), f.rel.size());
        mix(&f.size, sizeof f.size);
        mix(&f.mtime_ns, sizeof f.mtime_ns);
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fputs("usage: bb-texpack GAME_DIR [CACHE]\n", stderr);
        return 2;
    }
    const std::string root = argv[1];
    std::vector<TexFile> files;
    walk(root + "/dvdroot_ps4/map", root, 0, files);
    std::sort(files.begin(), files.end(), [](const TexFile& a, const TexFile& b) { return a.rel < b.rel; });
    char key[32];
    std::snprintf(key, sizeof key, "%016llx", (unsigned long long)key_of(files));
    if (argc == 3) {
        std::ifstream c(argv[2]);
        std::string saved_key, line;
        if (c && std::getline(c, saved_key) && saved_key == key && std::getline(c, line)) {
            std::puts(line.c_str());
            return 0;
        }
        std::fprintf(stderr, "Texture check: looking for a texture pack in the map textures (once, kept in %s)\n", argv[2]);
    }
    std::string answer = "0";
    for (const auto& f : files) {
        uint32_t side = 0;
        if (ends_with_nocase(f.path, ".tpfbhd")) {
            side = archive_side(f.path);
        } else if (f.size >= TEXTURE_PACK_MIN_ENTRY) {
            std::vector<uint8_t> blob(0x4C + 512);
            if (FILE* fp = std::fopen(f.path.c_str(), "rb")) {
                blob.resize(std::fread(blob.data(), 1, blob.size(), fp));
                std::fclose(fp);
                side = tpf_side(blob);
            }
        }
        if (side > RETAIL_TEXTURE_SIDE) {
            answer = "1 " + f.rel + " " + std::to_string(side);
            break;
        }
    }
    if (argc == 3) {
        std::ofstream c(argv[2], std::ios::trunc);
        c << key << "\n" << answer << "\n";
    }
    std::puts(answer.c_str());
    return 0;
}
