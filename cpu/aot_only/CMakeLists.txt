# libbbcpu.so without FEXCore (BB_AOT_ONLY): guest_cpu.h on the ahead-of-time units alone, no fallback CPU (docs/RECOMP_ABI.md, "Strict mode"). Built by
# tools/android/build_bionic.sh with BB_AOT_ONLY=1 (Android arm64); the sources are the FEX build's (cpu/bbcpu.cmake) minus guest_cpu_fex.cpp plus
# guest_cpu_native.cpp. Nothing here links FEX, jemalloc or fmt.
cmake_minimum_required(VERSION 3.20)
project(bbcpu_only C CXX ASM)
set(CPU_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(bbcpu SHARED "${CPU_DIR}/guest_cpu_aot.cpp" "${CPU_DIR}/guest_cpu_native.cpp" "${CPU_DIR}/host_call_arm64.S")
target_compile_definitions(bbcpu PRIVATE BB_AOT_ONLY=1)
target_include_directories(bbcpu PRIVATE "${CPU_DIR}" "${CPU_DIR}/../src")
target_compile_features(bbcpu PRIVATE cxx_std_20)
target_compile_options(bbcpu PRIVATE -O2 -g -Wall -Wextra -Werror -fno-strict-aliasing)
set_target_properties(bbcpu PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)
