#include "bundle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef BBR_BUNDLED
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
LLD_HAS_DRIVER(elf)
#endif

#ifdef BBR_BUNDLED
// LLD's ELF driver calls llvm::InitializeAll{Targets,TargetInfos,TargetMCs,AsmPrinters,AsmParsers}() (for LTO, which the .so link never uses).
// Only the AArch64 and X86 backends are linked into bb-recomp, so the registration functions of every other target are empty here.
#define BBR_NO_TARGET(T) \
  extern "C" void LLVMInitialize##T##Target() {} extern "C" void LLVMInitialize##T##TargetInfo() {} extern "C" void LLVMInitialize##T##TargetMC() {} \
  extern "C" void LLVMInitialize##T##AsmPrinter() {} extern "C" void LLVMInitialize##T##AsmParser() {}
BBR_NO_TARGET(AMDGPU) BBR_NO_TARGET(ARM) BBR_NO_TARGET(AVR) BBR_NO_TARGET(BPF) BBR_NO_TARGET(Hexagon) BBR_NO_TARGET(Lanai) BBR_NO_TARGET(LoongArch)
BBR_NO_TARGET(Mips) BBR_NO_TARGET(MSP430) BBR_NO_TARGET(NVPTX) BBR_NO_TARGET(PowerPC) BBR_NO_TARGET(RISCV) BBR_NO_TARGET(Sparc) BBR_NO_TARGET(SPIRV)
BBR_NO_TARGET(SystemZ) BBR_NO_TARGET(VE) BBR_NO_TARGET(WebAssembly) BBR_NO_TARGET(XCore)
#endif

namespace {
#ifdef BBR_BUNDLED
bool dirExists(const std::string& d) {
  std::ifstream f(d + "/helpers-aarch64.o", std::ios::binary);
  return f.good();
}

std::string exeDir() {
#ifdef _WIN32
  char b[4096]; DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
  std::string p(b, n);
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
#else
  char b[4096]; ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n <= 0) return ".";
  std::string p(b, n);
  size_t s = p.rfind('/');
  return s == std::string::npos ? "." : p.substr(0, s);
#endif
}
#endif

#ifdef BBR_BUNDLED
// LLD 23 writes VER_NDX_LOCAL (0) in .gnu.version for undefined symbols that carry no version (the bbr_* imports of libbbcpu), the NDK's
// LLD 21 writes VER_NDX_GLOBAL (1). Bionic's linker treats both as "unversioned"; the entries are set to 1 so that the loaded part of the
// library is byte-identical to what a developer build (NDK linker) writes.
void normaliseVersym(const std::string& path) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) return;
  struct Eh { unsigned char id[16]; uint16_t type, machine; uint32_t ver; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } eh;
  struct Sh { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t align, entsize; };
  f.read((char*)&eh, sizeof eh);
  if (!f || memcmp(eh.id, "\x7f" "ELF", 4) || eh.shentsize != sizeof(Sh)) return;
  std::vector<Sh> sh(eh.shnum);
  f.seekg((std::streamoff)eh.shoff); f.read((char*)sh.data(), (std::streamsize)(sh.size() * sizeof(Sh)));
  if (!f) return;
  const Sh *vs = nullptr, *ds = nullptr;
  for (auto& x : sh) { if (x.type == 0x6fffffff) vs = &x; else if (x.type == 11) ds = &x; }
  if (!vs || !ds) return;
  const size_t n = (size_t)(vs->size / 2);
  std::vector<uint16_t> ver(n);
  f.seekg((std::streamoff)vs->offset); f.read((char*)ver.data(), (std::streamsize)(n * 2));
  bool changed = false;
  for (size_t i = 1; i < n; ++i) {
    uint16_t shndx = 1;
    f.seekg((std::streamoff)(ds->offset + i * 24 + 6)); f.read((char*)&shndx, 2);
    if (ver[i] == 0 && shndx == 0) { ver[i] = 1; changed = true; }
  }
  if (changed) { f.seekp((std::streamoff)vs->offset); f.write((const char*)ver.data(), (std::streamsize)(n * 2)); }
}
#endif

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out; std::istringstream is(s); std::string t;
  while (is >> t) out.push_back(t);
  return out;
}

bool copyFile(const std::string& from, const std::string& to) {
  std::ifstream in(from, std::ios::binary); if (!in) { fprintf(stderr, "bb-recomp: cannot read %s\n", from.c_str()); return false; }
  std::ofstream out(to, std::ios::binary | std::ios::trunc); if (!out) { fprintf(stderr, "bb-recomp: cannot write %s\n", to.c_str()); return false; }
  out << in.rdbuf();
  return out.good();
}

// The argument after `flag`, or "".
std::string after(const std::vector<std::string>& v, const char* flag) {
  for (size_t i = 0; i + 1 < v.size(); ++i) if (v[i] == flag) return v[i + 1];
  return "";
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
} // namespace

std::string bundleDirDefault() {
#ifdef BBR_BUNDLED
  if (const char* e = getenv("BB_RECOMP_BUNDLE_DIR")) return dirExists(e) ? e : "";
  for (const std::string& d : {exeDir() + "/../share/bb-recomp", exeDir() + "/share/bb-recomp", exeDir() + "/bundle"})
    if (dirExists(d)) return d;
#endif
  return "";
}

int bbrSystem(const std::string& bundleDir, const std::string& cmd) {
  if (!bundleDir.empty() && has(cmd, "aarch64")) {
    std::vector<std::string> t = split(cmd);
    if (has(cmd, "--target=aarch64-linux-android") && has(cmd, "bbr_x87_all.c"))
      return copyFile(bundleDir + "/helpers-aarch64.o", after(t, "-o")) ? 0 : 1;
    if (has(cmd, "--target=aarch64-linux-android") && has(cmd, "-soname,libbbcpu.so"))
      return copyFile(bundleDir + "/libbbcpu-stub.so", after(t, "-o")) ? 0 : 1;
#ifdef BBR_BUNDLED
    if (has(cmd, "ld.lld ") && has(cmd, "aarch64linux")) {
      // nice -n 19 <ndk>/ld.lld <args>: drop the prefix, the NDK's library directory and -lm (the bundle's copy of the NDK's libm.so stands in: the
      // helpers need the DT_NEEDED entry and the LIBC symbol versions), and use the bundled builtins archive.
      std::vector<std::string> args {"ld.lld"};
      bool seen = false;
      for (const std::string& a : t) {
        if (!seen) { seen = a.size() >= 6 && a.compare(a.size() - 6, 6, "ld.lld") == 0; continue; }
        if (a.rfind("-L", 0) == 0) continue;
        if (a == "-lm") { args.push_back(bundleDir + "/libm.so"); continue; }
        if (a.find("libclang_rt.builtins-aarch64-android.a") != std::string::npos) { args.push_back(bundleDir + "/libclang_rt.builtins-aarch64-android.a"); continue; }
        args.push_back(a);
      }
      std::vector<const char*> argv;
      for (auto& a : args) argv.push_back(a.c_str());
      llvm::raw_fd_ostream so(1, false), se(2, false);
      lld::Result r = lld::lldMain(argv, so, se, {{lld::Gnu, &lld::elf::link}});
      if (r.retCode == 0) normaliseVersym(after(t, "-o"));
      return r.retCode;
    }
#endif
  }
  return system(cmd.c_str());
}
