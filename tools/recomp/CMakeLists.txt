# bb-recomp: PC tool that lifts the game's x86-64 code with FEXCore's front end (decoder,
# OpcodeDispatcher, IR passes) and, from step 2 on, lowers FEX IR to LLVM IR for AArch64.
#
# FEX's CMake files expect to be the top-level project (CMAKE_SOURCE_DIR, CMAKE_BINARY_DIR), so this
# is a thin driver: it configures FEX as an external project with only what the front end needs and
# adds the bb-recomp target into FEX's build through CMAKE_PROJECT_FEX_INCLUDE (the same trick as
# cpu/fex_project.cmake). Result: <build>/bb-recomp.
#
#   cmake -S tools/recomp -B out/recomp-build -G Ninja
#   nice -n 19 cmake --build out/recomp-build -j 22
cmake_minimum_required(VERSION 3.20)
project(bb-recomp-driver NONE)
include(ExternalProject)

# FEX source. FEX_GIT_DIR is a git checkout of FEX 2609.1 (default third_party/FEX; in a git worktree that
# submodule is empty, so pass the main checkout's). FEX's own submodules (fmt, unordered_dense, ...) are
# often not initialised either: then fetch-fex.sh builds a prepared tree under out/recomp-fex without
# touching the checkout. Set FEX_SOURCE_DIR to use a ready tree directly.
set(FEX_GIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/FEX" CACHE PATH "git checkout of FEX 2609.1")
set(FEX_SOURCE_DIR "" CACHE PATH "FEX source tree with its submodules (default: FEX_GIT_DIR, or a tree prepared from it)")
# Step 2: the FEX IR -> LLVM lowering needs LLVM (system install with the AArch64 and X86 targets). Also builds the
# ASM test runner bb-recomp-asmtest.
option(BB_RECOMP_LLVM "Link LLVM for the IR lowering and build bb-recomp-asmtest" ON)
get_filename_component(BB_NASM_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../.local-deps/nasm/install/bin/nasm" ABSOLUTE)
set(BB_RECOMP_NASM "${BB_NASM_DEFAULT}" CACHE FILEPATH "nasm for the ASM test runner (built from source into .local-deps/nasm: see README)")
# Player mode (`bb-recomp --game <dump folder>`): compiles tools/android/prepare/*.cpp (bb-prepare) into bb-recomp. Needs zlib.
option(BB_RECOMP_PLAYER "Player mode: bb-prepare's sources and src/player.cpp in bb-recomp (needs BB_RECOMP_LLVM)" ON)
# The self-contained build (package.sh): LLVM and LLD as static libraries from an official release tarball (LLVM_DIR = its
# lib/cmake/llvm), zlib and zstd static from BB_RECOMP_DEPS_PREFIX, the executable fully static; ld.lld runs in the process.
option(BB_RECOMP_BUNDLED "Self-contained bb-recomp: static LLVM + LLD, no NDK or system clang at run time (package.sh)" OFF)
set(BB_RECOMP_LLVM_DIR "" CACHE PATH "LLVM's lib/cmake/llvm (default: the system LLVM)")
set(BB_RECOMP_DEPS_PREFIX "" CACHE PATH "prefix with static zlib and zstd (BB_RECOMP_BUNDLED)")
set(BB_RECOMP_VERSION "0.1.0" CACHE STRING "tool version written to libbbaot.json")
# Windows (experimental, cross-compiled on Linux with llvm-mingw): -DBB_RECOMP_TOOLCHAIN_FILE=tools/recomp/windows/toolchain-mingw-x86_64.cmake
# -DBB_MINGW_ROOT=<llvm-mingw dir>; LLVM_DIR and the deps prefix then name builds for mingw (windows/build.sh).
set(BB_RECOMP_TOOLCHAIN_FILE "" CACHE FILEPATH "CMake toolchain file for a cross build (Windows: windows/toolchain-mingw-x86_64.cmake)")
set(BB_MINGW_ROOT "" CACHE PATH "llvm-mingw directory (cross build for Windows)")
# Linux release build (AppImage): linux-jammy-toolchain.cmake with an Ubuntu 22.04 sysroot and the LLVM tarball (fetch-deps.sh)
set(BB_SYSROOT "" CACHE PATH "sysroot for linux-jammy-toolchain.cmake")
set(BB_LLVM_ROOT "" CACHE PATH "LLVM release tarball directory for linux-jammy-toolchain.cmake")
set(BB_RECOMP_LLD_INCLUDE "" CACHE PATH "include directory of LLD's headers when LLVM is a build tree (Windows build)")
# The window (Dear ImGui + SDL3, src/gui.cpp): BB_RECOMP_SDL_PREFIX holds a static SDL3 (lib/cmake/SDL3), BB_RECOMP_IMGUI_DIR the ImGui sources
# (tools/recomp/fetch-deps.sh fetches both). Without them bb-recomp is the command line tool only.
set(BB_RECOMP_SDL_PREFIX "" CACHE PATH "prefix of a static SDL3 build (enables the GUI)")
set(BB_RECOMP_IMGUI_DIR "" CACHE PATH "Dear ImGui source directory (enables the GUI)")
set(BB_RECOMP_MIMALLOC_DIR "" CACHE PATH "mimalloc source directory: its allocator replaces operator new/delete (Windows build)")
set(BB_RECOMP_JOBS 22 CACHE STRING "Parallel jobs for the inner FEX build (2 of 24 cores stay free)")
set(BB_RECOMP_MARCH x86-64-v3 CACHE STRING "-march for FEX and bb-recomp on x86 hosts (FEX needs AVX)")
set(BB_RECOMP_CC clang CACHE STRING "C compiler for FEX (needs Clang)")
set(BB_RECOMP_CXX clang++ CACHE STRING "C++ compiler for FEX (needs Clang)")

if(NOT FEX_SOURCE_DIR)
  get_filename_component(FEX_GIT_DIR "${FEX_GIT_DIR}" ABSOLUTE)
  if(EXISTS "${FEX_GIT_DIR}/External/fmt/CMakeLists.txt")
    set(FEX_SOURCE_DIR "${FEX_GIT_DIR}")
  else()
    get_filename_component(prepared "${CMAKE_CURRENT_LIST_DIR}/../../out/recomp-fex" ABSOLUTE)
    message(STATUS "FEX submodules not initialised: preparing ${prepared} from ${FEX_GIT_DIR} (needs network once)")
    execute_process(COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/fetch-fex.sh" "${FEX_GIT_DIR}" "${prepared}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "fetch-fex.sh failed: pass -DFEX_GIT_DIR=<FEX git checkout> or -DFEX_SOURCE_DIR=<ready tree>")
    endif()
    set(FEX_SOURCE_DIR "${prepared}")
  endif()
endif()
get_filename_component(FEX_SOURCE_DIR "${FEX_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${FEX_SOURCE_DIR}/FEXCore/CMakeLists.txt")
  message(FATAL_ERROR "FEX_SOURCE_DIR=${FEX_SOURCE_DIR} has no FEXCore")
endif()

set(BB_EXE "")
set(BB_TOOLCHAIN_ARGS -DCMAKE_C_COMPILER=${BB_RECOMP_CC} -DCMAKE_CXX_COMPILER=${BB_RECOMP_CXX})
if(BB_RECOMP_TOOLCHAIN_FILE)
  if(BB_RECOMP_TOOLCHAIN_FILE MATCHES "mingw")
    set(BB_EXE ".exe")
  endif()
  set(BB_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_FILE=${BB_RECOMP_TOOLCHAIN_FILE}" "-DBB_MINGW_ROOT=${BB_MINGW_ROOT}" "-DBB_SYSROOT=${BB_SYSROOT}" "-DBB_LLVM_ROOT=${BB_LLVM_ROOT}")
endif()
set(BB_TARGETS bb-recomp)
set(BB_BUILT_FILES "<BINARY_DIR>/Bin/bb-recomp${BB_EXE}")
if(BB_RECOMP_LLVM AND NOT BB_RECOMP_BUNDLED)
  list(APPEND BB_TARGETS bb-recomp-asmtest bb-recomp-difftest)
  list(APPEND BB_BUILT_FILES "<BINARY_DIR>/Bin/bb-recomp-asmtest" "<BINARY_DIR>/Bin/bb-recomp-difftest")
endif()

# FEX refuses x86-64 hosts unless ENABLE_X86_HOST_DEBUG (its CI configuration). We never run the JIT,
# only the front end, so that is fine. Everything else FEX builds besides FEXCore is switched off;
# bb-recomp is the only target built.
ExternalProject_Add(fex
  SOURCE_DIR "${FEX_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/fex"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    ${BB_TOOLCHAIN_ARGS}
    -DCMAKE_BUILD_TYPE=Release
    -DENABLE_X86_HOST_DEBUG=ON
    -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF
    -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_xxhash=ON -DCMAKE_DISABLE_FIND_PACKAGE_range-v3=ON -DENABLE_LTO=OFF -DENABLE_GDB_SYMBOLS=OFF -DENABLE_CCACHE=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_FEX_ALLOCATOR=OFF -DENABLE_OFFLINE_TELEMETRY=OFF
    -DTUNE_CPU=none -DTUNE_ARCH=${BB_RECOMP_MARCH} -DOVERRIDE_VERSION=FEX-2609.1 -DOVERRIDE_HASH=9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf
    -DCMAKE_PROJECT_FEX_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/fex_project.cmake
    -DBB_RECOMP_DIR=${CMAKE_CURRENT_LIST_DIR}
    -DBB_RECOMP_PLAYER=${BB_RECOMP_PLAYER} -DBB_RECOMP_BUNDLED=${BB_RECOMP_BUNDLED} -DBB_RECOMP_VERSION=${BB_RECOMP_VERSION}
    "-DBB_RECOMP_LLVM_DIR=${BB_RECOMP_LLVM_DIR}" "-DBB_RECOMP_DEPS_PREFIX=${BB_RECOMP_DEPS_PREFIX}" "-DBB_RECOMP_LLD_INCLUDE=${BB_RECOMP_LLD_INCLUDE}" "-DBB_RECOMP_SDL_PREFIX=${BB_RECOMP_SDL_PREFIX}" "-DBB_RECOMP_IMGUI_DIR=${BB_RECOMP_IMGUI_DIR}" "-DBB_RECOMP_MIMALLOC_DIR=${BB_RECOMP_MIMALLOC_DIR}"
    -DBB_RECOMP_LLVM=${BB_RECOMP_LLVM} -DBB_RECOMP_MARCH=${BB_RECOMP_MARCH} -DBB_RECOMP_NASM=${BB_RECOMP_NASM}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ${BB_TARGETS} --parallel ${BB_RECOMP_JOBS}
  INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BB_BUILT_FILES} ${CMAKE_BINARY_DIR}
  BUILD_ALWAYS ON
  USES_TERMINAL_BUILD ON)
