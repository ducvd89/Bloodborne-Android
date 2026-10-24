#include "pkgs.h"

#include "game_check.h"
#include "io.h"
#include "proc.h"
#include "progress.h"
#include "ps4.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace {
constexpr uint64_t MiB = 1u << 20, GiB = 1u << 30;
constexpr const char* kEncrypted = "This PKG is encrypted for a retail PS4 and can't be read; only homebrew (fake) PKGs are supported";

uint64_t be32(const uint8_t* d) { return ((uint64_t)d[0] << 24) | ((uint64_t)d[1] << 16) | ((uint64_t)d[2] << 8) | d[3]; }
uint64_t be64(const uint8_t* d) { return (be32(d) << 32) | be32(d + 4); }

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string utf8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// "01.09" -> 109, for ordering (anything else sorts first).
int versionKey(const std::string& v) {
  int major = 0, minor = 0;
  if (sscanf(v.c_str(), "%d.%d", &major, &minor) == 2) return major * 1000 + minor;
  return -1;
}

std::string gigabytes(uint64_t b) { char s[32]; snprintf(s, sizeof s, "%.1f GB", (double)b / GiB); return s; }

// PkgInstaller.mergeTree: moves the files of `source` over the folder `target` (rename each, replacing; same filesystem). Names match the
// existing ones case-insensitively; eboot.bin and sce_sys go last.
void mergeTree(const fs::path& source, const fs::path& target, const std::function<bool()>& cancel) {
  std::error_code ec;
  if (fs::is_symlink(target, ec) || (fs::exists(target, ec) && !fs::is_directory(target, ec))) fs::remove(target, ec);
  if (!fs::is_directory(target, ec)) fs::create_directory(target, ec);
  if (!fs::is_directory(target, ec)) throw std::runtime_error("Cannot create " + utf8(target));
  std::map<std::string, std::string> existing;
  for (auto& e : fs::directory_iterator(target, ec)) { std::string n = utf8(e.path().filename()); existing.emplace(lower(n), n); }
  std::vector<std::string> names;
  for (auto& e : fs::directory_iterator(source, ec)) names.push_back(utf8(e.path().filename()));
  static const std::set<std::string> last = {"eboot.bin", "sce_sys", "param.sfo"};
  std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
    bool x = last.count(lower(a)), y = last.count(lower(b));
    if (x != y) return y;  // the "last" names after the others
    return a < b;
  });
  for (const std::string& name : names) {
    if (cancel && cancel()) throw std::runtime_error("cancelled");
    fs::path entry = source / fromUtf8(name);
    auto it = existing.find(lower(name));
    fs::path dest = target / fromUtf8(it != existing.end() ? it->second : name);
    if (fs::is_directory(entry, ec) && !fs::is_symlink(entry, ec)) { mergeTree(entry, dest, cancel); continue; }
    if (fs::is_directory(dest, ec) && !fs::is_symlink(dest, ec)) fs::remove_all(dest, ec);
    fs::rename(entry, dest, ec);
    if (ec) throw std::runtime_error("Cannot move " + utf8(entry) + ": " + ec.message());
  }
}

uint64_t treeBytes(const fs::path& dir) {
  uint64_t total = 0; std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  return total;
}

// Runs ps4-pkg-tool PKG STAGING and relays its "Progress-bytes: done total" lines (PkgInstaller.runTool).
bool runTool(const std::string& tool, const PkgInfo& pkg, const fs::path& staging, const std::string& label, std::string& err,
             const std::function<void(const std::string&)>& log, const std::function<bool()>& cancel) {
  static const std::regex progress("Progress-bytes: (\\d+) (\\d+)");
  std::vector<std::string> tail;
  bool spaceChecked = false, tooBig = false;
  std::string runErr;
  int code = runProcess({tool, pkg.path, utf8(staging)}, [&](const std::string& line) {
    std::smatch m;
    if (std::regex_match(line, m, progress)) {
      uint64_t done = std::stoull(m[1]), total = std::stoull(m[2]);
      if (!spaceChecked && total) {
        spaceChecked = true;
        std::error_code ec; auto sp = fs::space(staging, ec);
        if (!ec && sp.available < total + GiB / 2) {
          tooBig = true;
          err = "Not enough free space: " + gigabytes(total + GiB / 2) + " needed (the files take " + gigabytes(total) + "), " + gigabytes(sp.available) + " available";
        }
      }
      bbrProgress(done, total);
      return;
    }
    if (!line.empty()) { if (tail.size() >= 12) tail.erase(tail.begin()); tail.push_back(line); log(line); }
  }, [&] { return tooBig || (cancel && cancel()); }, runErr);
  if (tooBig) return false;
  if (cancel && cancel()) { err = "Cancelled"; return false; }
  if (code == -1) { err = "Can't run the PKG tool: " + runErr; return false; }
  if (code != 0) {
    std::string all; for (auto& l : tail) all += l + "\n";
    if (all.find("file size is different") != std::string::npos || all.find("bigger than pkg size") != std::string::npos)
      err = label + " is damaged or incomplete";
    else err = label + ": " + kEncrypted;
    return false;
  }
  return true;
}
}  // namespace

bool readPkgHeader(const std::string& path, PkgInfo& info, std::string& err) {
  std::ifstream f(fromUtf8(path), std::ios::binary);
  if (!f) { err = "Can't read " + path; return false; }
  f.seekg(0, std::ios::end); uint64_t fileSize = (uint64_t)f.tellg(); f.seekg(0);
  uint8_t head[0x440] = {};
  f.read((char*)head, sizeof head);
  size_t got = (size_t)f.gcount();
  if (got < 4 || head[0] != 0x7f || head[1] != 'C' || head[2] != 'N' || head[3] != 'T') { err = "This is not a PS4 PKG file"; return false; }
  if (got < sizeof head) { err = "This PKG file is incomplete"; return false; }
  info.path = path; info.file = utf8(fromUtf8(path).filename());
  size_t end = 0x40; while (end < 0x64 && head[end]) ++end;
  info.contentId.assign((const char*)head + 0x40, end - 0x40);
  info.size = be64(head + 0x430);
  if (info.size > fileSize) { err = "This PKG file is incomplete (the download or copy was cut short)"; return false; }
  uint64_t count = be32(head + 0x10), table = be32(head + 0x18);
  f.clear(); f.seekg((std::streamoff)table);
  Sfo sfoValues; bool haveSfo = false;
  for (uint64_t i = 0; i < std::min<uint64_t>(count, 4096) && !haveSfo; ++i) {
    uint8_t e[32];
    f.read((char*)e, 32);
    if (f.gcount() < 32) break;
    uint64_t id = be32(e), off = be32(e + 16), size = be32(e + 20);
    if (id == 0x1000 && size > 0 && size <= MiB) {
      auto here = f.tellg();
      f.seekg((std::streamoff)off);
      Bytes data(size);
      f.read((char*)data.data(), (std::streamsize)size);
      data.resize((size_t)f.gcount());
      try { sfoValues = sfo(view(data)); haveSfo = true; } catch (PyError&) {}
      f.clear(); f.seekg(here);
      break;
    }
  }
  auto get = [&](const char* k) { auto it = sfoValues.find(k); return it == sfoValues.end() ? std::string() : it->second.str(); };
  if (!haveSfo) { err = "This PKG is damaged: its param.sfo can't be read"; return false; }
  info.titleId = get("TITLE_ID");
  if (info.titleId.empty() && info.contentId.size() > 7) info.titleId = info.contentId.substr(7, std::min<size_t>(9, info.contentId.size() - 7));
  info.category = get("CATEGORY"); info.appVer = get("APP_VER");
  return true;
}

bool findPkgs(const std::string& dir, PkgSet& set, std::string& err) {
  std::error_code ec;
  fs::path root = fromUtf8(dir);
  if (!fs::is_directory(root, ec)) { err = "The PKG folder " + dir + " does not exist."; return false; }
  std::vector<std::string> files;
  for (auto& e : fs::directory_iterator(root, ec))
    if (e.is_regular_file(ec) && lower(utf8(e.path().extension())) == ".pkg") files.push_back(utf8(e.path()));
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    err = "There are no .pkg files in " + dir + ". Put Bloodborne's base game PKG and its 1.09 update PKG in that folder.";
    return false;
  }
  std::vector<PkgInfo> bases, updates;
  for (const std::string& f : files) {
    PkgInfo info; std::string e;
    std::string name = utf8(fromUtf8(f).filename());
    if (!readPkgHeader(f, info, e)) { err = name + ": " + e; return false; }
    if (!is_supported_title(info.titleId)) { err = name + " is for " + info.titleId + ", which isn't a Bloodborne release bbport supports. The folder must hold only Bloodborne's game PKG and its update."; return false; }
    if (info.category == "gd") bases.push_back(info);
    else if (info.category == "gp") updates.push_back(info);
    else { err = name + " is " + (info.category.empty() ? "unknown" : info.category) + " content; only the game (gd) and its update (gp) can be used. Take other PKGs out of the folder."; return false; }
  }
  if (bases.empty()) { err = "The base game PKG is missing: only update PKGs were found in " + dir + ". Add the Bloodborne game PKG (the big one)."; return false; }
  if (bases.size() > 1) { err = "Two base game PKGs were found (" + bases[0].file + ", " + bases[1].file + "). Keep only one."; return false; }
  std::sort(updates.begin(), updates.end(), [](const PkgInfo& a, const PkgInfo& b) { return versionKey(a.appVer) < versionKey(b.appVer); });
  for (size_t i = 1; i < updates.size(); ++i)
    if (updates[i].appVer == updates[i - 1].appVer) { err = "Two update PKGs have the same version " + updates[i].appVer + " (" + updates[i - 1].file + ", " + updates[i].file + "). Keep only one."; return false; }
  for (auto& u : updates) if (u.titleId != bases[0].titleId) { err = u.file + " is an update for " + u.titleId + ", but the base game is " + bases[0].titleId; return false; }
  int newest = versionKey(updates.empty() ? bases[0].appVer : updates.back().appVer);
  if (newest != versionKey("01.09")) {
    err = std::string("The newest PKG is version ") + (updates.empty() ? bases[0].appVer : updates.back().appVer) +
          "; bbport runs Bloodborne 1.09. Add the 1.09 update PKG (the update PKG for " + bases[0].titleId + ") to the folder.";
    return false;
  }
  set.titleId = bases[0].titleId; set.base = bases[0]; set.updates = updates;
  return true;
}

bool installPkgs(const PkgSet& set, const std::string& tool, const std::string& gameParent, const std::string& work, std::string& gameDir,
                 std::string& err, const std::function<void(const std::string&)>& log, const std::function<bool()>& cancel) {
  std::error_code ec;
  fs::path parent = fromUtf8(gameParent), game = parent / fromUtf8(set.titleId), staging = fromUtf8(work) / "staging";
  if (!fs::is_regular_file(fromUtf8(tool), ec)) { err = "The PKG tool (ps4-pkg-tool) is missing from this installation: " + tool; return false; }
  if (fs::exists(game, ec)) { err = utf8(game) + " already exists. Choose an empty output folder, or delete that folder."; return false; }
  fs::create_directories(parent, ec);
  uint64_t need = set.base.size + GiB;
  { auto sp = fs::space(parent, ec); if (!ec && sp.available < need) { err = "Not enough free space in the output folder: " + gigabytes(need) + " needed, " + gigabytes(sp.available) + " available"; return false; } }
  auto cleanup = [&] { std::error_code e2; fs::remove_all(staging, e2); };
  try {
    std::vector<const PkgInfo*> order = {&set.base};
    for (auto& u : set.updates) order.push_back(&u);
    for (size_t i = 0; i < order.size(); ++i) {
      const PkgInfo& p = *order[i];
      std::string label = p.file;
      cleanup(); fs::create_directories(staging, ec);
      bbrStage("extract", (std::string(i == 0 ? "Extracting the game: " : "Extracting the update: ") + p.file).c_str());
      log("Extracting " + p.file + " (" + (p.category == "gd" ? "base game" : "update") + ", version " + p.appVer + ")");
      if (!runTool(tool, p, staging, label, err, log, cancel)) { cleanup(); std::error_code e2; if (i == 0) fs::remove_all(game, e2); return false; }
      fs::path root;
      int dirs = 0;
      for (auto& e : fs::directory_iterator(staging, ec)) if (e.is_directory(ec) && !e.is_symlink(ec)) { root = e.path(); ++dirs; }
      if (dirs != 1) { err = label + ": " + kEncrypted; cleanup(); return false; }
      if (i == 0) {
        fs::rename(root, game, ec);
        if (ec) { err = "Cannot move the extracted game into place: " + ec.message(); cleanup(); return false; }
      } else {
        bbrStage("merge", ("Merging the update: " + p.file).c_str());
        log("Merging " + p.file + " into the game (" + std::to_string(treeBytes(root) / MiB) + " MB)");
        mergeTree(root, game, cancel);
      }
    }
  } catch (const std::exception& e) { cleanup(); err = std::string(e.what()) == "cancelled" ? "Cancelled" : e.what(); return false; }
  cleanup();
  gameDir = utf8(game);
  return true;
}
