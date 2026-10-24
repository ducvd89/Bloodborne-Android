# Android 2.0.0 (versionCode 35): native ARM64 code, FEXCore removed

2.0.0 changes how the game runs. This is a major version because it needs one extra step before the first start.

## Native ARM64 code (required)

- The game's x86-64 code now runs **only as ahead-of-time compiled ARM64 code** (`libbbaot.so`). Nothing is translated while you play: no translation stutter in new areas, less CPU work.
- You build that code **once, on a PC, from your own game files**, with the free [Bloodborne Android Rebuild Tool](https://github.com/ducvd89/Bloodborne-android-rebuild-tool)
  (Windows or Linux; 1.1.0 or newer, also attached to the 2.0.0 Beta 1 release). It takes either source:
  - **the PKG files** (base game + 1.09 update): it writes one folder with the game and the native code (`aot/`); copy it to the phone and choose it as the game folder;
  - **an extracted game** (the folder with `eboot.bin`, or the `eboot.bin` itself; new in tool 1.1.0): it writes only `aot/`; copy it next to `eboot.bin` in the game folder on the phone.
  The native code is made from your game, so it cannot be downloaded or shared.
- **Without a `libbbaot.so` that fits the game, the app does not start it.** A full-screen message explains the steps and has a button to the tool's page, **Choose game folder** and **Import libbbaot.so…**.
  It appears when the library is missing, was built from another game build, has the wrong ABI version, or is damaged (checksum). Menu → **Native CPU** is now a status and import page (the On/Off switch is gone);
  its drawer row says what is wrong.
- **Phone-only installs are over.** Installing PKGs on the phone (**Install from PKG**) still extracts the game, then tells you the native code from the PC tool is still needed: build it from the
  same PKGs or the extracted folder and copy `aot/` next to `eboot.bin`.
- If the native code is missing an address (a gap in the library), the game stops with `STOP: AOT: native code is missing an entry at +0x...` and writes `logs/aot-misses.log`. Send it with Menu → **Report a problem**:
  the tool can close the gap from that log (`--extra-entries`), you rebuild once.
- Verified on the AYN Thor before this change: no misses in about 20 minutes of play and three clean launches of this exact runtime.

## FEXCore removed

- The runtime has no FEXCore any more: `libbbcpu.so` is the native engine alone (123 KB stripped in the APK, it was 3.9 MB with FEX), the APK is smaller, and there is no JIT, interpreter or fallback of any kind.
  Removed with it: `fex-compatible.json`, the FEX submodule and build, the fallback trampolines (`bb_fb_*`), FEX's environment variables and config files, `native_cpu` in `bbport.ini`.
- Hooks are native slots only: the game's code is never rewritten. Diagnostics that wrote x86 stubs into it (the Gnm census, the observe mode) are off on Android with a log line.
- The CPUID model the game sees is unchanged (ported from FEX-Emu, MIT; the notice is in the app's licenses).
- The Rebuild Tool is now its own repository and the `tools/recomp` submodule of this one. `tools/android/prepare` exists in both and must stay the same (`tools/android/check_prepare_sync.sh`);
  `tools/check_recomp_abi.sh` compares the ABI version of the library between the runtime, the app and the tool.

## Rebuild Tool

- **1.0.0**, released with this beta: the window and command line, PKG damage check, extract and merge, recompile, the all-in-one game folder.
- **1.1.0**: also builds from an already extracted game. In the window, choose **From an extracted game** and the folder with `eboot.bin` (or **eboot.bin…** for the file); the PKG row is grayed out
  while it is chosen, and the other way round. On the command line, `bb-recomp --game <folder or eboot.bin> --out <dir>`. Only `aot/` is written; the game folder is never changed. The native code is the
  same as 1.0.0's (ABI version 1).

## Also in this release

- **Button remapping** for gamepads: Menu → Remap buttons.
- **Controller vibration** from the game (on/off in the Controls menu).
- **Game language** in Game settings (the game's own language choice, `BB_LANGUAGE`).

## Updating

Saves, settings, mods and the game folder are kept. After updating from 1.2.0 or earlier you need the native code before the game starts (above). Your game is already on the phone, so build
from the game files you have on the PC (the extracted folder or its `eboot.bin` with **From an extracted game**, or the PKGs) and copy only the `aot/` folder next to `eboot.bin` on the phone:
nothing has to be extracted or copied again.

## For developers

- Test builds: `BB_TEST_BUILD=1 bash tools/android/build_release_apk.sh` makes `Bloodborne-2.0.0-test.<commit>.apk` (versionName `2.0.0-test.<commit>`, versionCode 35); the update checker reads it as 2.0.0.
- `tools/android/check_bionic_symbols.sh` fails on any FEX or jemalloc symbol or string in the runtime (the two CPUID constants `FEXIFEXIEMU` and `FEX-2609.1` the model presents to the game are the only allowed names).
- Phase 7 notes: docs/STATIC_RECOMPILE_PLAN.md.
