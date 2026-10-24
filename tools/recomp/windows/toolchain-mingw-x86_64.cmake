# Cross-compiling bb-recomp for Windows x86-64 with llvm-mingw (https://github.com/mstorsjo/llvm-mingw, UCRT build), run on Linux.
# Experimental, see docs/NATIVE_CPU.md ("Native Windows"): this is how far it gets.
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/recomp/windows/toolchain-mingw-x86_64.cmake -DBB_MINGW_ROOT=<llvm-mingw dir> ...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT BB_MINGW_ROOT)
  set(BB_MINGW_ROOT "$ENV{BB_MINGW_ROOT}")
endif()
set(CMAKE_C_COMPILER "${BB_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${BB_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${BB_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH "${BB_MINGW_ROOT}/x86_64-w64-mingw32")
if(BB_RECOMP_DEPS_PREFIX)
  list(APPEND CMAKE_FIND_ROOT_PATH "${BB_RECOMP_DEPS_PREFIX}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
