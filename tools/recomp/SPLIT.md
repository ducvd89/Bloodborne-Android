<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Splitting tools/recomp into its own repository

`tools/recomp` is meant to become `ducvd89/Bloodborne-android-rebuild-tool` (GPL-3.0-or-later). It builds with the files below, which live
outside this folder in the Bloodborne-Android repository. Copy them into the new repository (suggested place in brackets) and adjust the
three path expressions marked with `(*)`.

## Sources compiled into bb-recomp

| Path | Why | Notes |
|---|---|---|
| `tools/android/prepare/*.cpp`, `*.h` [`prepare/`] | the game image (`bb-prepare`): `bb_prepare.cpp` (its `main` is renamed to `bb_prepare_main`), `ps4.cpp`, `link.cpp`, `hash.cpp`, `pyish.cpp`, `game_check.cpp`, `io.cpp` (`windows/io_win.cpp` replaces it on Windows) | `(*)` `bbrecomp.cmake`: `BBR_PREPARE`. The phone's app builds the same files, so they should stay one source (a git subtree or a submodule shared by both repositories). Two small edits were made for the tool: `is_supported_title()` in `game_check.cpp/.h`, and a Windows `#ifdef` for `st_mtime` in `bb_prepare.cpp`. Licensed as the phone app is (GPL-2.0-or-later), which a GPL-3.0-or-later program may include. |
| `third_party/FEX` (FEX-Emu 2609.1, MIT) [`third_party/FEX`] | FEXCore's x86-64 front end (decoder, OpcodeDispatcher, IR passes) | `(*)` `CMakeLists.txt`: `FEX_GIT_DIR`. `fetch-fex.sh` prepares a tree with its submodules; `FEX_SOURCE_DIR` points at a ready one. No FEX patches are needed. |

## The PKG tool

| Path | Why |
|---|---|
| `tools/android/build_pkg_tool.sh` [`pkg-tool/build_pkg_tool.sh`] | builds `ps4-pkg-tool` for the host, Linux (glibc 2.35 sysroot) and Windows (llvm-mingw); `package.sh` and `package-windows.sh` call it. `(*)` the two calls and the `root`-relative paths in the script |
| `tools/android/pkg-tool-patches/*.patch` [`pkg-tool/patches/`] | byte progress, 64-bit offsets, unsafe-name refusal |
| `tools/android/ps4-pkg-tool.NOTICE` [`pkg-tool/`] | its notice (GPL-2.0-or-later, shadPS4) |
| `.local-deps/src/ps4-pkg-tools` (not in git) | the source of ps4-pkg-tools at 45baeda (`https://github.com/xXJSONDeruloXx/ps4-pkg-tools`); the script finds it in `.local-deps` or `BB_PKG_TOOL_SRC` |

## Documents and tests

| Path | Why |
|---|---|
| `docs/NATIVE_CPU.md` | the players' guide; `package.sh` puts a copy in the AppImage and in the zip |
| `docs/RECOMP_ABI.md`, `docs/STATIC_RECOMPILE_PLAN.md`, `docs/AOT_DEVICE_STATUS.md`, `docs/RECOMP_HOOKS.md` (on recomp-hooks) | design and ABI of the library the tool writes; they describe the runtime side and could move with the tool |
| `tests/recomp-pkgs/` | synthetic PKGs and a stand-in PKG tool for the PKG step |

## Needed at package time, not in git

- The Android NDK r29 (`BB_NDK`, default `.local-deps/android/sdk/ndk/29.0.14206865` here or in the main checkout): the AArch64 x87 helper object, the
  `libbbcpu.so` stub, the `libm.so` link stub and the compiler-rt builtins archive (`package.sh`, `package-windows.sh`).
- Downloads (`fetch-deps.sh`, `fetch-sysroot.py`, `package-windows.sh`): the LLVM 23.1.3 release tarball and sources, an Ubuntu 22.04 sysroot, zlib, zstd, SDL3,
  Dear ImGui, llvm-mingw, appimagetool.

## What the app needs from the tool (the other direction)

The app reads `libbbaot.json` (`NativeCpu.java`) and the `aot/` folder of the game folder. The format is in `src/player.cpp` (`"format": 1`); the library's
ABI is `docs/RECOMP_ABI.md` (`bbr_abi_version`, checked by the tool and by the app).
