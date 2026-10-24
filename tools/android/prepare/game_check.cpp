#include "game_check.h"

#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>

#include <cstdlib>

#include "io.h"
#include "ps4.h"

namespace {
const char* const SUPPORTED_TITLES[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                        "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};
const char* const SUPPORTED_VERSION = "01.09";
const char* const SUPPORTED_IMAGE = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
const char* const LANCE_60FPS_IMAGE = "bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f";
const char* const CHECKED_FOLDER = "dvdroot_ps4/shader";

bool is_oserror(const std::string& k) {
    return k == "OSError" || k == "FileNotFoundError" || k == "FileExistsError" || k == "PermissionError" ||
           k == "IsADirectoryError" || k == "NotADirectoryError";
}
bool is_value_error(const std::string& k) { return k == "ValueError" || k == "UnicodeDecodeError"; }

// True when a DFLT .dcx file does not unpack to its stated size or fails zlib's checksum.
bool dcx_broken(const std::string& path) {
    MappedFile f = read_file(path);
    View d = f.view();
    if (d.n < 0x2c || std::memcmp(d.p, "DCX\0", 4) != 0 || std::memcmp(d.p + 0x18, "DCS\0", 4) != 0 ||
        std::memcmp(d.p + 0x28, "DFLT", 4) != 0)
        return false;
    u32 size = (u32)d.p[0x1c] << 24 | (u32)d.p[0x1d] << 16 | (u32)d.p[0x1e] << 8 | d.p[0x1f];
    u32 packed = (u32)d.p[0x20] << 24 | (u32)d.p[0x21] << 16 | (u32)d.p[0x22] << 8 | d.p[0x23];
    // zlib.decompress(data[0x4C:0x4C+packed]): the stream must end inside the slice.
    size_t from = 0x4c < d.n ? 0x4c : d.n;
    size_t to = (u64)0x4c + packed < d.n ? (size_t)(0x4c + (u64)packed) : d.n;
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return true;
    z.next_in = const_cast<Bytef*>(d.p + from);
    z.avail_in = (uInt)(to - from);
    static thread_local u8 sink[1 << 16];
    int rc;
    do {
        z.next_out = sink;
        z.avail_out = sizeof sink;
        rc = inflate(&z, Z_NO_FLUSH);
    } while (rc == Z_OK || (rc == Z_BUF_ERROR && z.avail_out == 0));
    u64 total = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END) return true;  // zlib.error, including a truncated stream
    return total != size;
}

// Any .dcx under root; sets *unreadable on an I/O error (Python's OSError out of rglob + read_bytes).
bool any_broken(const std::string& dir, bool* unreadable) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string path = dir + "/" + n;
        struct stat st;
        bool is_dir = lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        if (n.size() >= 4 && n.compare(n.size() - 4, 4, ".dcx") == 0) {
            try {
                if (dcx_broken(path)) found = true;
            } catch (PyError& err) {
                if (!is_oserror(err.kind)) throw;
                *unreadable = true;
            }
        }
        if (is_dir && !found && !*unreadable) found = any_broken(path, unreadable);
        if (found || *unreadable) break;
    }
    closedir(d);
    return found;
}
}  // namespace

bool is_supported_title(const std::string& title_id) {
    for (auto t : SUPPORTED_TITLES)
        if (title_id == t) return true;
    return false;
}

bool game_check_skipped() {
    const char* v = std::getenv("BB_SKIP_GAME_CHECK");
    return v && std::string(v) == "1";
}

std::optional<Problem> game_problem(const std::string& game, const std::string& image_hash) {
    if (game_check_skipped()) return std::nullopt;
    Sfo info;
    try {
        MappedFile f = read_file(path_join(game, "sce_sys/param.sfo"));
        info = sfo(f.view());
    } catch (PyError& e) {
        if (!is_oserror(e.kind) && !is_value_error(e.kind) && e.kind != "IndexError") throw;
        info.clear();
    }
    auto get = [&](const char* k) {
        auto it = info.find(k);
        return it == info.end() ? std::string("?") : it->second.str();
    };
    std::string title = get("TITLE_ID"), version = get("APP_VER");
    // A TITLE_ID/APP_VER stored as an integer never equals the strings below, as in Python.
    auto is_int = [&](const char* k) { auto it = info.find(k); return it != info.end() && it->second.is_int; };
    if (image_hash == SUPPORTED_IMAGE) {
        bool unreadable = false;
        bool broken = any_broken(path_join(game, CHECKED_FOLDER), &unreadable);
        if (unreadable) return Problem{"unreadable", title, version};
        return broken ? std::optional<Problem>(Problem{"damaged_files", title, version}) : std::nullopt;
    }
    if (image_hash == LANCE_60FPS_IMAGE) return Problem{"patched_eboot", title, version};
    bool title_ok = false;
    for (auto t : SUPPORTED_TITLES) title_ok |= !is_int("TITLE_ID") && title == t;
    if (!title_ok) return Problem{"other_title", title, version};
    if (is_int("APP_VER") || version != SUPPORTED_VERSION) return Problem{"missing_update", title, version};
    return Problem{"wrong_eboot", title, version};
}

std::string game_explain(const Problem& p) {
    std::string found = "Found " + p.title + " version " + p.version + " (sce_sys/param.sfo).";
    const std::string& k = p.kind;
    if (k == "missing_update")
        return found + " bbport needs the 1.09 update merged into the game folder: copy everything from the dumped 1.09 "
                       "update (e.g. CUSA03173-patch) into the game folder, replacing files (eboot.bin and sce_sys too).";
    if (k == "wrong_eboot")
        return found + " param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from the dumped 1.09 update into "
                       "the game folder, replacing the old one.";
    if (k == "patched_eboot")
        return found + " This eboot.bin has the 60 fps patch by Lance McDonald applied to it, which crashes the game "
                       "when the gestures menu opens (bbport has its own 60 fps): copy eboot.bin from a clean dump of "
                       "the 1.09 update into the game folder, replacing this one.";
    if (k == "other_title")
        return found + " This is not a retail release of Bloodborne: bbport runs Bloodborne (any region, The Old Hunters "
                       "editions too) with update 1.09.";
    if (k == "unreadable")
        return found + " eboot.bin could not be read as a decrypted PS4 executable: dump the game and the 1.09 update again.";
    return found + std::string(" The game files are damaged: the shaders in ") + CHECKED_FOLDER +
           " do not unpack, and the game would hang while loading them. The extraction tool left parts of compressed "
           "sectors stale (LibOrbisPkg PkgTool on .NET 6 or newer, issue #81): extract the game and the 1.09 update "
           "again with a fixed tool. scripts/game_check.py GAME_DIR checks every file.";
}
