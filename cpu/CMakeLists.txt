# libbbcpu.so: the guest CPU of the runtime (src/guest_cpu.h) as ahead-of-time recompiled units only: no interpreter, JIT or fallback of any kind
# (docs/RECOMP_ABI.md, "Strict mode"). Sources: the unit engine (guest_cpu_aot.cpp) and its AArch64 platform services (guest_cpu_native.cpp,
# host_call_arm64.S). Built by tools/android/build_bionic.sh (Android arm64) and by build.sh on AArch64 hosts. Links nothing but libc/libdl.
cmake_minimum_required(VERSION 3.20)
project(bbcpu C CXX ASM)
set(CPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(bbcpu SHARED "${CPU_DIR}/guest_cpu_aot.cpp" "${CPU_DIR}/guest_cpu_native.cpp" "${CPU_DIR}/host_call_arm64.S")
target_include_directories(bbcpu PRIVATE "${CPU_DIR}" "${CPU_DIR}/../src")
target_compile_features(bbcpu PRIVATE cxx_std_20)
target_compile_options(bbcpu PRIVATE -O2 -g -Wall -Wextra -Werror -fno-strict-aliasing)
set_target_properties(bbcpu PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)
