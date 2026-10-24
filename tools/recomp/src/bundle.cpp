#include "bundle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
      // nice -n 19 <ndk>/ld.lld <args>: drop the prefix, the NDK's library directory and -lm (the stub stands in for libm.so: the
      // helpers only need the DT_NEEDED entry), and use the bundled builtins archive.
      std::vector<std::string> args {"ld.lld"};
      bool seen = false;
      for (const std::string& a : t) {
        if (!seen) { seen = a.size() >= 6 && a.compare(a.size() - 6, 6, "ld.lld") == 0; continue; }
        if (a.rfind("-L", 0) == 0) continue;
        if (a == "-lm") { args.push_back(bundleDir + "/libm-stub.so"); continue; }
        if (a.find("libclang_rt.builtins-aarch64-android.a") != std::string::npos) { args.push_back(bundleDir + "/libclang_rt.builtins-aarch64-android.a"); continue; }
        args.push_back(a);
      }
      std::vector<const char*> argv;
      for (auto& a : args) argv.push_back(a.c_str());
      llvm::raw_fd_ostream so(1, false), se(2, false);
      lld::Result r = lld::lldMain(argv, so, se, {{lld::Gnu, &lld::elf::link}});
      return r.retCode;
    }
#endif
  }
  return system(cmd.c_str());
}
