# Cross-build for the Thor's native ARM64 Linux userland (Debian trixie arm64) with Clang and lld.
# The sysroot comes from tools/android/fetch_arm64_sysroot.py.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
get_filename_component(BB_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT DEFINED BB_ARM64_SYSROOT)
  set(BB_ARM64_SYSROOT "${BB_REPO}/.local-deps/android/arm64-sysroot")
endif()
set(CMAKE_SYSROOT "${BB_ARM64_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
# Libraries and packages from the sysroot; header-only packages (BB_ARM64_HEADER_PREFIX) may come
# from the host. Programs (code generators, glslang) run on the host.
set(CMAKE_FIND_ROOT_PATH "${BB_ARM64_SYSROOT}" ${BB_ARM64_HEADER_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BB_ARM64_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${BB_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${BB_ARM64_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
