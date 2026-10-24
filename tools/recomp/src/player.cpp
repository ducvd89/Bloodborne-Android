// SPDX-License-Identifier: GPL-3.0-or-later
// Player mode of bb-recomp: `bb-recomp --game <dump folder> [--out <dir>]`.
//
// One command, no other inputs. The player's own PS4 dump of Bloodborne (eboot.bin as a decrypted SELF, sce_module/*.prx,
// sce_sys/param.sfo) goes in; libbbaot.so and libbbaot.json come out, to be copied to the phone (Native CPU in the app).
// Translated game code is derived from the game, so it is made here, on the player's machine, and never distributed.
//
// Steps: (1) check the folder, (2) build the same boot image the device's bb-prepare builds (tools/android/prepare/*.cpp are
// compiled into this tool: bb-prepare's own main() is called, so the two cannot drift), (3) lift and compile everything with the
// memory-adaptive plan (recompMain() with the options of a developer run, plus the variant catalog when this build has one),
// (4) verify the .so, (5) write libbbaot.so and libbbaot.json.
#include "bundle.h"
#include "game_check.h"
#include "hash.h"
#include "io.h"
#include "pkgs.h"
#include "proc.h"
#include "progress.h"
#include "ps4.h"

#include <llvm/Config/llvm-config.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#if __has_include("variants.h")
#define BBR_HAVE_VARIANTS 1 // the recomp-hooks build: recompMain() knows --variants
#endif

#ifndef BBR_TOOL_VERSION
#define BBR_TOOL_VERSION "0.1.0"
#endif

int recompMain(int argc, char** argv);
int bb_prepare_main(int argc, char** argv); // tools/android/prepare/bb_prepare.cpp, main() renamed at build time

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace {
// The ABI version this tool writes (bbr_abi_version in compile.cpp) and the one the runtime (libbbcpu) accepts.
constexpr uint32_t kAbiVersion = 1;

double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

std::string fmtTime(double s) {
  char b[64];
  if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
  else snprintf(b, sizeof b, "%d min %02d s", (int)(s / 60), (int)s % 60);
  return b;
}

bool g_machine = false;           // --machine: "@@..." lines for the GUI (STAGE id|text, PROGRESS done total, ERROR text, DONE folder)
std::atomic<bool> g_stop {false}; // SIGTERM / SIGINT: stop the PKG step; later stages end the process at once
std::atomic<bool> g_stoppable {false};
int g_step = 0, g_steps = 5;
uint64_t g_lastPct = 101;

void onStopSignal(int) { if (g_stoppable) g_stop = true; else _exit(143); }

void step(const char* id, const char* title) {
  ++g_step; g_lastPct = 101;
  if (g_machine) printf("@@STAGE %s|%s\n", id, title);
  printf("\n[%d/%d] %s\n", g_step, g_steps, title);
  fflush(stdout);
}
void hookStage(const char* id, const char* text) {
  g_lastPct = 101;
  if (g_machine) printf("@@STAGE %s|%s\n", id, text); else printf("  %s\n", text);
  fflush(stdout);
}
void hookProgress(uint64_t done, uint64_t total) {
  if (!total) return;
  if (g_machine) { printf("@@PROGRESS %" PRIu64 " %" PRIu64 "\n", done, total); fflush(stdout); return; }
  uint64_t pct = done * 100 / total;
  if (pct / 10 != g_lastPct / 10 && pct % 10 < 10) { g_lastPct = pct; printf("    %" PRIu64 " %%\n", pct / 10 * 10); fflush(stdout); }
}

[[noreturn]] void die(const std::string& msg) {
  fflush(stdout);
  if (g_machine) printf("@@ERROR %s\n", msg.c_str());
  fprintf(stderr, "\nbb-recomp: %s\n", msg.c_str());
  fflush(stdout);
  exit(1);
}

void playerUsage() {
  fputs("bb-recomp: builds everything the phone needs for the native CPU from your own copy of Bloodborne.\n"
        "\n"
        "  bb-recomp --pkgs <folder with the game's PKG files> --out <output folder>\n"
        "  bb-recomp --game <extracted game folder> [--out <dir>]     (an already extracted dump: only libbbaot.so and .json are written)\n"
        "\n"
        "  --pkgs <dir>       a folder holding Bloodborne's base game PKG and its update PKG(s) (1.09). They are unpacked, merged and\n"
        "                     compiled. The output folder then holds the game folder (CUSA03173, as the app expects it) with aot/ inside\n"
        "                     and a README.txt: copy it to the phone and choose it as the game folder in the app\n"
        "  --game <dir>       the PS4 dump of Bloodborne with update 1.09 merged in: the folder that holds eboot.bin, sce_module/\n"
        "                     and sce_sys/ (e.g. CUSA03173). Nothing in it is changed\n"
        "  --out <dir>        the output folder (default: ./bb-recomp-out)\n"
        "  --pkg-tool <file>  ps4-pkg-tool (default: next to this program)\n"
        "  --machine          progress lines for the GUI\n"
        "  --jobs <n>         worker threads (default: the CPU cores minus two, at most 22; fewer when the memory needs it)\n"
        "  --mem-fraction <f> share of the free memory the compile may use (default 0.6)\n"
        "  --variants <dir>   a variant catalog to use instead of the one next to the tool (--no-variants: none)\n"
        "  --keep-work        keep the work folder (<out>/work: the game image and the object files)\n"
        "\n"
        "It takes a few minutes and needs about 3 GB of free disk space while it works. Then copy libbbaot.so and libbbaot.json to\n"
        "the phone and import them in the app: Menu -> Native CPU (recompiled) -> Import. See docs/NATIVE_CPU.md.\n", stdout);
}

// ---- a small ELF reader for the verification of the .so (an AArch64 shared object: it is never loaded here) ----
struct Ehdr64 { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr64 { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Shdr64 { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };
struct Sym64 { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };

struct SoInfo {
  uint64_t imageHash = 0, unitCount = 0, funcCount = 0, abiFlags = 0, textEnd = 0, unitsSize = 0;
  uint32_t abiVersion = 0;
  std::vector<std::string> needed;
  uint64_t size = 0;
  std::string sha256;
};

// Reads the .so; returns an empty string when it is a well-formed bb-recomp library, else what is wrong with it.
std::string readSo(const std::string& path, uint64_t imageSize, SoInfo& out) {
  std::vector<uint8_t> f;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "cannot read " + path;
    f.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  out.size = f.size();
  if (f.size() < sizeof(Ehdr64) || memcmp(f.data(), "\x7f" "ELF", 4)) return "not an ELF file";
  Ehdr64 eh; memcpy(&eh, f.data(), sizeof eh);
  if (eh.ident[4] != 2 || eh.ident[5] != 1) return "not a 64-bit little-endian ELF";
  if (eh.machine != 183) return "not an AArch64 library (machine " + std::to_string(eh.machine) + ")";
  if (eh.type != 3) return "not a shared object";
  if (eh.shoff == 0 || eh.shentsize != sizeof(Shdr64) || eh.shoff + (uint64_t)eh.shnum * sizeof(Shdr64) > f.size()) return "no section headers";
  std::vector<Shdr64> sh(eh.shnum);
  memcpy(sh.data(), f.data() + eh.shoff, sh.size() * sizeof(Shdr64));
  std::vector<Phdr64> ph;
  for (unsigned i = 0; i < eh.phnum && eh.phoff + (uint64_t)(i + 1) * eh.phentsize <= f.size(); ++i) { Phdr64 p; memcpy(&p, f.data() + eh.phoff + (uint64_t)i * eh.phentsize, sizeof p); ph.push_back(p); }
  auto fileOff = [&](uint64_t v, uint64_t len, uint64_t& off) {
    for (auto& p : ph) if (p.type == 1 && v >= p.vaddr && v + len <= p.vaddr + p.filesz) { off = p.offset + (v - p.vaddr); return off + len <= f.size(); }
    return false;
  };
  auto str = [&](const Shdr64& strtab, uint32_t at) { return strtab.offset + at < f.size() ? std::string((const char*)f.data() + strtab.offset + at) : std::string(); };
  const Shdr64* dynsym = nullptr; const Shdr64* dynamic = nullptr;
  for (auto& s : sh) { if (s.type == 11) dynsym = &s; else if (s.type == 6) dynamic = &s; }
  if (!dynsym || dynsym->link >= sh.size() || dynsym->offset + dynsym->size > f.size()) return "no dynamic symbol table";
  const Shdr64& strtab = sh[dynsym->link];
  struct Found { uint64_t value = 0, size = 0; bool ok = false; };
  std::vector<std::pair<const char*, Found>> want = {{"bbr_units", {}}, {"bbr_unit_count", {}}, {"bbr_func_count", {}}, {"bbr_abi_flags", {}}, {"bbr_image_hash", {}}, {"bbr_abi_version", {}}, {"bbr_text_end", {}}, {"bbr_image_base", {}}, {"bbr_dirty", {}}, {"bbr_unit_end", {}}};
  for (uint64_t o = 0; o + sizeof(Sym64) <= dynsym->size; o += sizeof(Sym64)) {
    Sym64 s; memcpy(&s, f.data() + dynsym->offset + o, sizeof s);
    if (s.shndx == 0) continue;
    const std::string n = str(strtab, s.name);
    for (auto& w : want) if (n == w.first) w.second = {s.value, s.size, true};
  }
  auto get = [&](const char* name) -> const Found* { for (auto& w : want) if (!strcmp(w.first, name)) return w.second.ok ? &w.second : nullptr; return nullptr; };
  auto rd = [&](const char* name, unsigned bytes, uint64_t& v) {
    const Found* s = get(name); uint64_t off;
    if (!s || !fileOff(s->value, bytes, off)) return false;
    v = 0; memcpy(&v, f.data() + off, bytes); return true;
  };
  for (const char* req : {"bbr_units", "bbr_unit_count", "bbr_abi_version", "bbr_image_hash", "bbr_abi_flags", "bbr_func_count", "bbr_image_base"})
    if (!get(req)) return std::string("symbol ") + req + " is missing";
  uint64_t v = 0;
  if (!rd("bbr_abi_version", 4, v)) return "bbr_abi_version is not readable";
  out.abiVersion = (uint32_t)v;
  if (!rd("bbr_image_hash", 8, out.imageHash) || !rd("bbr_unit_count", 8, out.unitCount) || !rd("bbr_func_count", 8, out.funcCount) || !rd("bbr_abi_flags", 8, out.abiFlags)) return "a table header is not readable";
  rd("bbr_text_end", 8, out.textEnd);
  // The unit table: {vaddr, function pointer} pairs sorted by vaddr (the pointers are relocations: only the addresses are read).
  const Found* tab = get("bbr_units");
  uint64_t off;
  if (tab->size != out.unitCount * 16) return "bbr_units has " + std::to_string(tab->size / 16) + " entries, bbr_unit_count says " + std::to_string(out.unitCount);
  if (!out.unitCount || !fileOff(tab->value, tab->size, off)) return "the unit table is empty or not mapped";
  uint64_t prev = 0;
  for (uint64_t i = 0; i < out.unitCount; ++i) {
    uint64_t a; memcpy(&a, f.data() + off + i * 16, 8);
    if (i && a <= prev) return "the unit table is not sorted at entry " + std::to_string(i);
    if (a >= imageSize) return "unit " + std::to_string(i) + " is outside the image";
    prev = a;
  }
  if (const Found* d = get("bbr_dirty")) if (d->size != out.unitCount) return "bbr_dirty does not have one byte per unit";
  if (dynamic && dynamic->link < sh.size()) {
    const Shdr64& ds = sh[dynamic->link];
    for (uint64_t o = 0; o + 16 <= dynamic->size && dynamic->offset + o + 16 <= f.size(); o += 16) {
      int64_t tag; uint64_t val; memcpy(&tag, f.data() + dynamic->offset + o, 8); memcpy(&val, f.data() + dynamic->offset + o + 8, 8);
      if (tag == 1) out.needed.push_back(str(ds, (uint32_t)val));
      if (tag == 0) break;
    }
  }
  if (std::find(out.needed.begin(), out.needed.end(), "libbbcpu.so") == out.needed.end()) return "libbbcpu.so is not among the needed libraries";
  out.sha256 = sha256_hex(View{f.data(), f.size()});
  return "";
}

// Where libbbaot.so and libbbaot.json go: aot/ inside the game folder made from PKGs (the app imports from there), else the output folder.
fs::path fromPkgsDir(bool fromPkgs, const fs::path& game, const fs::path& out) { return fromPkgs ? game / "aot" : out; }

std::string exeDirectory() {
  std::string d = bundleExeDir();
  return d;
}

std::string readText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss; ss << in.rdbuf();
  return ss.str();
}

// The number after `key` in the compile summary (stats file), or -1.
long long statNumber(const std::string& text, const char* key) {
  size_t at = text.find(key);
  if (at == std::string::npos) return -1;
  at += strlen(key);
  while (at < text.size() && !isdigit((unsigned char)text[at])) ++at;
  return at < text.size() ? atoll(text.c_str() + at) : -1;
}

std::string isoNow() {
  std::time_t t = std::time(nullptr); std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char b[32]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}

std::string jsonStr(const std::string& s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    if (c == '"') r += "\\\""; else if (c == '\\') r += "\\\\"; else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; } else r += (char)c;
  }
  return r + "\"";
}

std::string hex16(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%016" PRIx64, v); return b; }

// FNV-1a over the flat image of a boot-linked file: bbr_image_hash (compile.cpp fnv1a(img.file)). The boot file is
// "BBPROBE5", 12 header words, the module table, bindings, segments, 128 bytes per import and 32 per relocation, then the image.
bool imageHashOfBoot(const std::string& boot, uint64_t& hash, uint64_t& imageSize) {
  std::ifstream in(boot, std::ios::binary);
  if (!in) return false;
  uint64_t w[14];
  char magic[8]; in.read(magic, 8);
  if (memcmp(magic, "BBPROBE5", 8)) return false;
  in.read((char*)w, 12 * 8); // size entry ns nr ni caps procparam tls[4] mc
  const uint64_t size = w[0], ns = w[2], nr = w[3], ni = w[4], mc = w[11];
  in.seekg((std::streamoff)(8 + 12 * 8 + mc * 7 * 8));
  uint64_t nb; in.read((char*)&nb, 8);
  const uint64_t skip = nb * 24 + ns * 24 + ni * 128 + nr * 32;
  in.seekg((std::streamoff)skip, std::ios::cur);
  std::vector<char> buf(1 << 20);
  uint64_t h = 1469598103934665603ull, left = size;
  while (left && in) {
    in.read(buf.data(), (std::streamsize)std::min<uint64_t>(left, buf.size()));
    const size_t n = (size_t)in.gcount();
    if (!n) break;
    for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)buf[i]; h *= 1099511628211ull; }
    left -= n;
  }
  if (left) return false;
  hash = h; imageSize = size;
  return true;
}

// Runs bb-prepare's main() with stderr captured (its errors are reworded here).
int runPrepare(const std::string& game, const std::string& data, std::string& err, const fs::path& errFile) {
  std::string a0 = "bb-prepare", a1 = game, a2 = data;
  char* argv[] = {a0.data(), a1.data(), a2.data(), nullptr};
  fflush(stdout); fflush(stderr);
#ifdef _WIN32
  int saved = _dup(2); int fd = _open(errFile.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, 0600); _dup2(fd, 2); _close(fd);
#else
  int saved = dup(2); int fd = open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(fd, 2); close(fd);
#endif
  int rc;
  try { rc = bb_prepare_main(3, argv); }
  catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); rc = 1; }
  catch (...) { fprintf(stderr, "unknown error\n"); rc = 1; }
  fflush(stdout); fflush(stderr);
#ifdef _WIN32
  _dup2(saved, 2); _close(saved);
#else
  dup2(saved, 2); close(saved);
#endif
  err = readText(errFile.string());
  while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
  return rc;
}
} // namespace

int playerMain(int argc, char** argv) {
  std::string gameArg, pkgsArg, outArg = "bb-recomp-out", variantsArg, jobs, memFraction, toolArg;
  bool keepWork = false, noVariants = false;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* s) { return !strcmp(argv[i], s); };
    auto arg = [&]() -> std::string { if (i + 1 >= argc) { playerUsage(); exit(2); } return argv[++i]; };
    if (is("--game")) gameArg = arg();
    else if (is("--pkgs")) pkgsArg = arg();
    else if (is("--out")) outArg = arg();
    else if (is("--jobs")) jobs = arg();
    else if (is("--mem-fraction")) memFraction = arg();
    else if (is("--variants")) variantsArg = arg();
    else if (is("--no-variants")) noVariants = true;
    else if (is("--keep-work")) keepWork = true;
    else if (is("--pkg-tool")) toolArg = arg();
    else if (is("--machine")) g_machine = true;
    else if (is("--help-player") || is("--help") || is("-h")) { playerUsage(); return 0; }
    else { fprintf(stderr, "bb-recomp: unknown option %s (bb-recomp --help-player)\n", argv[i]); return 2; }
  }
  if (gameArg.empty() == pkgsArg.empty()) { playerUsage(); return 2; }
  const bool fromPkgs = !pkgsArg.empty();
  g_steps = fromPkgs ? 6 : 5;
  signal(SIGINT, onStopSignal); signal(SIGTERM, onStopSignal);
  g_progress.stage = hookStage; g_progress.progress = hookProgress;
  setvbuf(stdout, nullptr, _IOLBF, 0);

  const auto t0 = clk::now();
  printf("bb-recomp %s (LLVM %s): builds what the phone needs for the native CPU, from your own copy of Bloodborne.\n", BBR_TOOL_VERSION, LLVM_VERSION_STRING);
  printf("The result is derived from your game: keep it for yourself and never share it.\n");

  std::error_code ec;
  fs::path out = fs::absolute(outArg, ec);
  fs::create_directories(out, ec);
  if (!fs::is_directory(out)) die("cannot create the output folder " + out.string());
  fs::path work = out / ".bbwork";
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  if (!fs::is_directory(work)) die("cannot create the work folder " + work.string());
  auto cleanup = [&] { if (!keepWork) { std::error_code e2; fs::current_path(out, e2); fs::remove_all(work, e2); } };
  std::string err;

  fs::path game;
  if (fromPkgs) {
    // ---- the PKG files: find, identify, unpack, merge ----
    step("check", "Checking the PKG files");
    PkgSet set;
    if (!findPkgs(fs::absolute(pkgsArg, ec).string(), set, err)) { cleanup(); die(err); }
    printf("  base game: %s (%s, version %s, %.1f GB)\n", set.base.file.c_str(), set.titleId.c_str(), set.base.appVer.c_str(), set.base.size / 1073741824.0);
    for (auto& u : set.updates) printf("  update:    %s (version %s, %.1f GB)\n", u.file.c_str(), u.appVer.c_str(), u.size / 1073741824.0);
    step("extract", "Unpacking the game (this takes a while: the game is tens of GB)");
    std::string tool = toolArg;
    if (tool.empty()) {
      const std::string b = bundleDirDefault();
      for (const std::string& c : {exeDirectory() + "/ps4-pkg-tool", exeDirectory() + "/ps4-pkg-tool.exe", b.empty() ? std::string() : b + "/../../bin/ps4-pkg-tool"})
        if (!c.empty() && fs::is_regular_file(c, ec)) { tool = c; break; }
    }
    if (const char* e = getenv("BB_PKG_TOOL")) if (toolArg.empty() && *e) tool = e;
    g_stoppable = true;
    std::string gameDir;
    const bool ok = installPkgs(set, tool, out.string(), work.string(), gameDir, err, [](const std::string& l) { printf("  %s\n", l.c_str()); fflush(stdout); }, [] { return g_stop.load(); });
    g_stoppable = false;
    if (!ok) { cleanup(); die(err); }
    game = gameDir;
  } else {
    step("check", "Checking the game folder");
    game = fs::absolute(gameArg, ec);
    if (!fs::is_directory(game)) { cleanup(); die("the game folder " + gameArg + " does not exist."); }
  }
  const char* needed[] = {"eboot.bin", "sce_module/libc.prx", "sce_module/libSceFios2.prx", "sce_sys/param.sfo"};
  std::string missing;
  for (const char* n : needed) if (!fs::is_regular_file(game / n)) missing += std::string("  ") + n + "\n";
  if (!missing.empty()) {
    std::string hint;
    for (auto& e : fs::directory_iterator(game, ec))
      if (e.is_directory() && fs::is_regular_file(e.path() / "eboot.bin")) hint += "  " + e.path().string() + "\n";
    cleanup();
    die("this does not look like a Bloodborne game folder. Missing in " + game.string() + ":\n" + missing +
        (hint.empty() ? std::string("Pass the folder that contains eboot.bin, sce_module and sce_sys (for example CUSA03173), with the 1.09 update merged in.")
                      : "Did you mean one of these?\n" + hint));
  }
  std::string titleId = "?", version = "?", title = "?", contentId = "?";
  try {
    MappedFile f = read_file((game / "sce_sys/param.sfo").generic_string());
    Sfo info = sfo(f.view());
    auto get = [&](const char* k, std::string& dst) { auto it = info.find(k); if (it != info.end()) dst = it->second.str(); };
    get("TITLE_ID", titleId); get("APP_VER", version); get("TITLE", title); get("CONTENT_ID", contentId);
  } catch (PyError& e) {
    cleanup();
    die("sce_sys/param.sfo cannot be read (" + e.kind + (e.msg.empty() ? "" : ": " + e.msg) + "): the game files are damaged.");
  }
  printf("  %s (%s), version %s, %s\n", title.c_str(), titleId.c_str(), version.c_str(), game.generic_string().c_str());

  // ---- the game image: bb-prepare's code ----
  step("prepare", "Building the game image (the same step the app runs on the phone)");
  auto tp = clk::now();
  fs::path data = work / "data";
  if (runPrepare(game.generic_string(), data.generic_string(), err, work / "prepare.err") != 0) {
    cleanup();
    std::string text = err;
    const std::string kCheck = "prepare.GameCheckError: ";
    if (text.rfind(kCheck, 0) == 0)
      die("this game is not supported.\n  " + text.substr(kCheck.size()) + "\nbb-recomp compiles Bloodborne with the 1.09 update merged in (the same check the app makes).");
    die("the game files could not be read:\n  " + text + "\nThe PKGs or the dump may be damaged: get them again.");
  }
  const fs::path boot = data / "boot-linked.bin";
  uint64_t imageHash = 0, imageSize = 0;
  if (!imageHashOfBoot(boot.string(), imageHash, imageSize)) { cleanup(); die("the game image (boot-linked.bin) is not readable."); }
  // The eboot's own ELF (what scripts/prepare.py writes as eboot.elf): the lifter reads its program headers from it.
  fs::path elf = work / "eboot.elf";
  try {
    MappedFile f = read_file((game / "eboot.bin").generic_string());
    SelfInfo s = parse_self(f.view());
    std::ofstream o(elf, std::ios::binary); o.write((const char*)s.elf.data(), (std::streamsize)s.elf.size());
    if (!o) throw PyError{"OSError", "cannot write " + elf.string()};
  } catch (PyError& e) { cleanup(); die("eboot.bin cannot be read: " + e.kind + ": " + e.msg); }
  printf("  image ready: %.1f MB, hash %s, %s\n", imageSize / 1048576.0, hex16(imageHash).c_str(), fmtTime(since(tp)).c_str());

  // ---- lift and compile ----
  step("analyse", "Analysing and recompiling the game's code (a few minutes; the memory plan below fits your RAM)");
  const auto tc = clk::now();
  std::vector<std::string> args = {"bb-recomp", "--boot", boot.string(), "--elf", elf.string(), "--prx-dir", (game / "sce_module").string(),
                                   "--so", "libbbaot.so", "--stats", (work / "compile.txt").string(), "-O1"};
  if (!jobs.empty()) { args.push_back("--jobs"); args.push_back(jobs); }
  if (!memFraction.empty()) { args.push_back("--mem-fraction"); args.push_back(memFraction); }
  std::string variantNote = "none (this build has no variant support)";
#ifdef BBR_HAVE_VARIANTS
  {
    std::string vdir = variantsArg;
    if (noVariants) variantNote = "off (--no-variants)";
    else {
      if (vdir.empty()) {
        // next to the tool (the package: <exe>/../share/bb-recomp/variants), or the source tree's tools/recomp/variants
        const std::string b = bundleDirDefault();
        if (!b.empty() && fs::is_directory(b + "/variants")) vdir = b + "/variants";
        else if (fs::is_directory(std::string(BBR_HELPERS_DIR) + "/../variants")) vdir = std::string(BBR_HELPERS_DIR) + "/../variants";
      }
      if (!vdir.empty() && fs::is_directory(vdir)) { args.push_back("--variants"); args.push_back(fs::absolute(vdir).string()); variantNote = vdir; }
      else variantNote = "none found";
    }
  }
#else
  if (!variantsArg.empty() || noVariants) printf("  note: this build has no variant catalog support; --variants/--no-variants ignored\n");
  (void)variantsArg; (void)noVariants;
#endif
  printf("  variant catalog: %s\n", variantNote.c_str());
  fflush(stdout);
  // The compile runs with the work folder as the working directory and relative names for its objects and the .so (its
  // external commands are plain strings: no spaces in them).
  fs::current_path(work, ec);
  std::vector<char*> av;
  for (auto& a : args) av.push_back(a.data());
  av.push_back(nullptr);
  const int rc = recompMain((int)args.size(), av.data());
  fflush(stdout);
  if (rc != 0) { cleanup(); die("the compile failed (code " + std::to_string(rc) + "); see the messages above."); }
  const double compileSecs = since(tc);

  // ---- verify ----
  step("verify", "Checking the result");
  SoInfo so;
  const std::string bad = readSo((work / "libbbaot.so").string(), imageSize, so);
  if (!bad.empty()) { cleanup(); die("the library failed its check: " + bad); }
  if (so.abiVersion != kAbiVersion) { cleanup(); die("the library has ABI version " + std::to_string(so.abiVersion) + ", expected " + std::to_string(kAbiVersion)); }
  if (so.imageHash != imageHash) { cleanup(); die("the library's image hash " + hex16(so.imageHash) + " is not the game image's " + hex16(imageHash)); }
  const std::string stats = readText((work / "compile.txt").string());
  const long long liftFail = statNumber(stats, "lift failures"), trapUnits = statNumber(stats, "units with bbr_trap (unsupported ops):");
  printf("  ELF AArch64 shared object, %.1f MB, needs %s\n", so.size / 1048576.0, [&] { std::string s; for (auto& n : so.needed) s += (s.empty() ? "" : ", ") + n; return s; }().c_str());
  printf("  ABI version %u, flags 0x%" PRIx64 ", %" PRIu64 " units in %" PRIu64 " functions, sorted and inside the image\n", so.abiVersion, so.abiFlags, so.unitCount, so.funcCount);
  printf("  image hash %s matches the game image\n", hex16(so.imageHash).c_str());
  if (liftFail > 0) printf("  note: %lld functions could not be lifted; the runtime runs them on its own CPU emulation\n", liftFail);
  if (trapUnits > 0) printf("  note: %lld units use instructions this tool cannot translate yet; the runtime runs those on its own CPU emulation\n", trapUnits);

  // ---- write ----
  step("finish", "Writing the result");
  const fs::path aotDir = fromPkgsDir(fromPkgs, game, out);
  fs::create_directories(aotDir, ec);
  const fs::path soOut = aotDir / "libbbaot.so", jsonOut = aotDir / "libbbaot.json";
  fs::remove(soOut, ec);
  fs::rename(work / "libbbaot.so", soOut, ec);
  if (ec) { ec.clear(); fs::copy_file(work / "libbbaot.so", soOut, fs::copy_options::overwrite_existing, ec); }
  if (ec) { cleanup(); die("cannot write " + soOut.string() + ": " + ec.message()); }
  {
    std::ostringstream j;
    j << "{\n"
      << "  \"format\": 1,\n"
      << "  \"kind\": \"bb-recomp-aot\",\n"
      << "  \"game_id\": " << jsonStr(titleId) << ",\n"
      << "  \"game_version\": " << jsonStr(version) << ",\n"
      << "  \"content_id\": " << jsonStr(contentId) << ",\n"
      << "  \"game_title\": " << jsonStr(title) << ",\n"
      << "  \"image_hash\": " << jsonStr(hex16(so.imageHash)) << ",\n"
      << "  \"image_size\": " << imageSize << ",\n"
      << "  \"abi_version\": " << so.abiVersion << ",\n"
      << "  \"abi_flags\": " << so.abiFlags << ",\n"
      << "  \"unit_count\": " << so.unitCount << ",\n"
      << "  \"function_count\": " << so.funcCount << ",\n"
      << "  \"tool\": " << jsonStr(std::string("bb-recomp ") + BBR_TOOL_VERSION) << ",\n"
      << "  \"llvm\": " << jsonStr(LLVM_VERSION_STRING) << ",\n"
      << "  \"created\": " << jsonStr(isoNow()) << ",\n"
      << "  \"so_size\": " << so.size << ",\n"
      << "  \"so_sha256\": " << jsonStr(so.sha256) << "\n"
      << "}\n";
    std::ofstream o(jsonOut, std::ios::binary);
    o << j.str();
    if (!o) { cleanup(); die("cannot write " + jsonOut.string()); }
  }
  {
    std::ofstream o(out / "bb-recomp.log", std::ios::binary);
    o << "bb-recomp " << BBR_TOOL_VERSION << " (LLVM " << LLVM_VERSION_STRING << "), " << isoNow() << "\n" << stats;
  }
  if (fromPkgs) {
    std::ofstream o(out / "README.txt", std::ios::binary);
    o << "Bloodborne for Android: the game folder, ready for the app\n"
         "==========================================================\n\n"
         "What is in here\n"
         "  " << game.filename().string() << "/    Bloodborne " << version << " (" << title << "), unpacked from your PKG files, in the layout the app expects\n"
         "  " << game.filename().string() << "/aot/libbbaot.so and libbbaot.json   the game's code compiled for your phone (the native CPU)\n\n"
         "What to do\n"
         "  1. Copy the folder \"" << game.filename().string() << "\" to your phone's storage (internal storage or an SD card), for example into Download.\n"
         "     (Copying this whole folder, with this README, works too: the app finds the game folder inside.)\n"
         "  2. In the Bloodborne app, choose that folder as the game folder (Menu > Game folder).\n"
         "  3. The app copies aot/libbbaot.so into its own files and turns the native CPU on by itself (Menu > Native CPU shows it).\n"
         "     It only copies again when the files change. Restart the game after the first time.\n\n"
         "These files are made from your own game and are for you only: do not share or upload them.\n"
         "Made by bb-recomp " << BBR_TOOL_VERSION << " on " << isoNow() << ".\n";
  }
  cleanup();
  printf("\nDone in %s (compile %s).\n\n", fmtTime(since(t0)).c_str(), fmtTime(compileSecs).c_str());
  if (fromPkgs) {
    printf("  %s\n  %s  (%.1f MB)\n\n", game.generic_string().c_str(), soOut.generic_string().c_str(), so.size / 1048576.0);
    printf("Copy the folder \"%s\" to your phone's storage and choose it as the game folder in the app (Menu > Game folder).\n"
           "The app copies the native CPU files in by itself and switches it on; restart the game after the first time.\n"
           "These files are derived from your game: do not share them.\n", game.filename().string().c_str());
    if (g_machine) printf("@@DONE %s\n", game.generic_string().c_str());
  } else {
    printf("  %s  (%.1f MB)\n  %s\n\n", soOut.generic_string().c_str(), so.size / 1048576.0, jsonOut.generic_string().c_str());
    printf("Copy BOTH files to your phone, then in the app: Menu -> Native CPU (recompiled) -> Import libbbaot.so..., pick them, and switch it on.\n"
           "Or put the folder aot/ with both files inside your game folder: the app imports it by itself.\n"
           "Restart the game to apply. These files are derived from your game: do not share them.\n");
    if (g_machine) printf("@@DONE %s\n", out.generic_string().c_str());
  }
  return 0;
}
