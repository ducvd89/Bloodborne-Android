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
#include "ps4.h"

#include <llvm/Config/llvm-config.h>

#include <algorithm>
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
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
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

void step(int n, const char* title) { printf("\n[%d/5] %s\n", n, title); fflush(stdout); }

[[noreturn]] void die(const std::string& msg) {
  fflush(stdout);
  fprintf(stderr, "\nbb-recomp: %s\n", msg.c_str());
  exit(1);
}

void playerUsage() {
  fputs("bb-recomp player mode: compile the game's code for your phone, from your own game dump.\n"
        "\n"
        "  bb-recomp --game <game dump folder> [--out <dir>]\n"
        "\n"
        "  --game <dir>       the PS4 dump of Bloodborne with update 1.09 merged in: the folder that holds eboot.bin, sce_module/\n"
        "                     and sce_sys/ (e.g. CUSA03173). Nothing in it is changed\n"
        "  --out <dir>        where libbbaot.so and libbbaot.json go (default: ./bb-recomp-out)\n"
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
  std::string gameArg, outArg = "bb-recomp-out", variantsArg, jobs, memFraction;
  bool keepWork = false, noVariants = false;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* s) { return !strcmp(argv[i], s); };
    auto arg = [&]() -> std::string { if (i + 1 >= argc) { playerUsage(); exit(2); } return argv[++i]; };
    if (is("--game")) gameArg = arg();
    else if (is("--out")) outArg = arg();
    else if (is("--jobs")) jobs = arg();
    else if (is("--mem-fraction")) memFraction = arg();
    else if (is("--variants")) variantsArg = arg();
    else if (is("--no-variants")) noVariants = true;
    else if (is("--keep-work")) keepWork = true;
    else if (is("--help-player") || is("--help") || is("-h")) { playerUsage(); return 0; }
    else { fprintf(stderr, "bb-recomp: unknown option %s in player mode (bb-recomp --help-player)\n", argv[i]); return 2; }
  }
  if (gameArg.empty()) { playerUsage(); return 2; }

  const auto t0 = clk::now();
  printf("bb-recomp %s (LLVM %s): compiles Bloodborne's code for your phone, from your own game dump.\n", BBR_TOOL_VERSION, LLVM_VERSION_STRING);
  printf("The result is derived from your game: keep it for yourself and never share it.\n");

  // ---- [1/5] the game folder ----
  step(1, "Checking the game folder");
  std::error_code ec;
  fs::path game = fs::absolute(gameArg, ec);
  if (!fs::is_directory(game)) die("the game folder " + gameArg + " does not exist.");
  const char* needed[] = {"eboot.bin", "sce_module/libc.prx", "sce_module/libSceFios2.prx", "sce_sys/param.sfo"};
  std::string missing;
  for (const char* n : needed) if (!fs::is_regular_file(game / n)) missing += std::string("  ") + n + "\n";
  if (!missing.empty()) {
    std::string hint;
    for (auto& e : fs::directory_iterator(game, ec))
      if (e.is_directory() && fs::is_regular_file(e.path() / "eboot.bin")) hint += "  " + e.path().string() + "\n";
    die("this does not look like a Bloodborne dump. Missing in " + game.string() + ":\n" + missing +
        (hint.empty() ? std::string("Pass the folder that contains eboot.bin, sce_module and sce_sys (for example CUSA03173), with the 1.09 update merged in.")
                      : "Did you mean one of these?\n" + hint));
  }
  std::string titleId = "?", version = "?", title = "?", contentId = "?";
  try {
    MappedFile f = read_file((game / "sce_sys/param.sfo").string());
    Sfo info = sfo(f.view());
    auto get = [&](const char* k, std::string& dst) { auto it = info.find(k); if (it != info.end()) dst = it->second.str(); };
    get("TITLE_ID", titleId); get("APP_VER", version); get("TITLE", title); get("CONTENT_ID", contentId);
  } catch (PyError& e) {
    die("sce_sys/param.sfo cannot be read (" + e.kind + (e.msg.empty() ? "" : ": " + e.msg) + "): dump the game again.");
  }
  printf("  %s (%s), version %s, %s\n", title.c_str(), titleId.c_str(), version.c_str(), game.string().c_str());

  fs::path out = fs::absolute(outArg, ec);
  fs::create_directories(out, ec);
  if (!fs::is_directory(out)) die("cannot create the output folder " + out.string());
  fs::path work = out / "work";
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  if (!fs::is_directory(work)) die("cannot create the work folder " + work.string());
  auto cleanup = [&] { if (!keepWork) { std::error_code e2; fs::current_path(out, e2); fs::remove_all(work, e2); } };

  // ---- [2/5] the game image: bb-prepare's code ----
  step(2, "Building the game image (the same step the app runs on the phone)");
  auto tp = clk::now();
  std::string err;
  fs::path data = work / "data";
  if (runPrepare(game.string(), data.string(), err, work / "prepare.err") != 0) {
    cleanup();
    std::string text = err;
    const std::string kCheck = "prepare.GameCheckError: ";
    if (text.rfind(kCheck, 0) == 0) {
      die("this game folder is not supported.\n  " + text.substr(kCheck.size()) +
          "\nbb-recomp compiles Bloodborne with the 1.09 update merged in (the same check the app makes). Nothing was written.");
    }
    die("the game files could not be read:\n  " + text + "\nThe dump may be encrypted or damaged: dump the game and the 1.09 update again.");
  }
  const fs::path boot = data / "boot-linked.bin";
  uint64_t imageHash = 0, imageSize = 0;
  if (!imageHashOfBoot(boot.string(), imageHash, imageSize)) { cleanup(); die("the game image (boot-linked.bin) is not readable."); }
  // The eboot's own ELF (what scripts/prepare.py writes as eboot.elf): the lifter reads its program headers from it.
  fs::path elf = work / "eboot.elf";
  try {
    MappedFile f = read_file((game / "eboot.bin").string());
    SelfInfo s = parse_self(f.view());
    std::ofstream o(elf, std::ios::binary); o.write((const char*)s.elf.data(), (std::streamsize)s.elf.size());
    if (!o) throw PyError{"OSError", "cannot write " + elf.string()};
  } catch (PyError& e) { cleanup(); die("eboot.bin cannot be read: " + e.kind + ": " + e.msg); }
  printf("  image ready: %.1f MB, hash %s, %s\n", imageSize / 1048576.0, hex16(imageHash).c_str(), fmtTime(since(tp)).c_str());

  // ---- [3/5] lift and compile ----
  step(3, "Lifting and compiling the game's code (a few minutes; the memory plan below fits your RAM)");
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

  // ---- [4/5] verify ----
  step(4, "Checking the result");
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

  // ---- [5/5] write ----
  step(5, "Writing the result");
  const fs::path soOut = out / "libbbaot.so", jsonOut = out / "libbbaot.json";
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
  cleanup();
  printf("\nDone in %s (compile %s).\n\n", fmtTime(since(t0)).c_str(), fmtTime(compileSecs).c_str());
  printf("  %s  (%.1f MB)\n  %s\n\n", soOut.string().c_str(), so.size / 1048576.0, jsonOut.string().c_str());
  printf("Copy BOTH files to your phone (any folder, for example Downloads), then in the app:\n"
         "  Menu -> Native CPU (recompiled) -> Import libbbaot.so..., pick libbbaot.so (and libbbaot.json if asked), and switch it on.\n"
         "Restart the game to apply. These files are derived from your game: do not share them.\n");
  return 0;
}
