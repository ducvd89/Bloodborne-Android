// The self-contained build of bb-recomp (tools/recomp/package.sh): everything the compile step used to ask the NDK and the
// system's clang for comes from the tool itself or from its bundle directory (<exe>/../share/bb-recomp):
//   - ld.lld runs in the process (LLD as a library, BBR_BUNDLED),
//   - the x87 helpers are an AArch64 object compiled when the package was made (helpers-aarch64.o),
//   - libbbcpu.so is an empty soname stub and libm.so the NDK's own link stub (53 KB: no code, only the symbol list with its LIBC versions),
//   - libclang_rt.builtins-aarch64-android.a (__divti3 and friends) is the NDK's own file.
// compile.cpp runs its external commands through bbrSystem(): without a bundle directory that is system(); with one, the
// three aarch64 commands (compile the helpers, make the stub, link) are replaced as above and every other command is run.
#pragma once
#include <string>

// The bundle directory of this executable, or "" when the build is not a bundled one or the directory does not exist.
// BB_RECOMP_BUNDLE_DIR overrides the place.
std::string bundleDirDefault();

// Runs `cmd` the way system() would, except for the bundled replacements above.
int bbrSystem(const std::string& bundleDir, const std::string& cmd);

// The directory of this executable (".": unknown).
std::string bundleExeDir();
