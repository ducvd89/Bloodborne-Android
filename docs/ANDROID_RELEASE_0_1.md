# Android release 0.1

**Target:** AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13, 16 GB RAM).
Gameplay is experimental and can crash. Other devices are untested.

## Install and play

1. Install `Bloodborne-0.1.apk` from the GitHub release. Its package is
   `com.ducvd89.bloodborne`, separate from upstream fexdroid.
2. Extract your own Bloodborne **CUSA03173 v1.09** dump into a folder on the
   phone. Select the folder containing `eboot.bin` when Bloodborne asks.
3. Grant storage access. The first launch expands the bundled Linux runtime
   into the app's private storage and prepares the executable image from your
   game files. Allow a few minutes and about 1 GB free beyond the game dump.
4. Open the left-edge menu and tap **On-screen controller** to turn the touch
   controls on or off. The setting persists across app launches. The overlay
   provides △ ○ × □, D-pad, left and right sticks, L1/L2/R1/R2, L3/R3,
   touchpad, and Options. It uses the same game mapping as the physical pad.
5. Tap **Edit on-screen controller** in the same menu to drag each button or
   stick to a preferred position, then tap **Done** at the top of the screen.
   The positions persist across launches. **Reset controller layout** in the
   menu restores every control to its original position.

The APK includes the app, FEX/FEXCore, Linux ARM64 rootfs, Turnip Vulkan driver,
native bbport runtime, FSR 3.1, default settings, patch data, and Python needed
for game preparation. The launcher icon comes from the builder's own
`sce_sys/icon0.dds`. The APK contains **no game executable, PS4 modules, game
assets, or saves**. FSR 4 is disabled on the Thor and is not included.

Settings and saves stay in the app's private data on update. Do not uninstall
an app with saves without backing them up. The 0.1 APK uses a development
signing key and has `android:debuggable=true` because its Linux runtime runs
extracted executables from app data.

## Build inputs

`tools/android/build_release_apk.sh` expects:

- Android SDK platform 35 and build-tools 35.0.0, a JDK, and Python with Pillow.
- The pinned fexdroid legacy APK described in [ANDROID_THOR.md](ANDROID_THOR.md)
  for its two MIT-licensed Android display/input bridges.
- The ARM64 native bundle at `out/arm64/bundle` (`bb-probe`, `libbbcpu.so`,
  `libbbgpu.so`, dependencies, and compiled patch data).
- A migrated Debian/Turnip/FEX rootfs at
  `.local-deps/android/migration/x/rootfs`, with runtime paths rewritten for
  `com.ducvd89.bloodborne`. This is a local build input; a clean clone alone
  does not recreate it yet.
- The builder's own `game/CUSA03173/sce_sys/icon0.dds` for the app icon.

The release builder fetches three Debian trixie ARM64 Python packages over
HTTPS, checks their SHA-256 values against Debian's package index, and patches
Python's ELF interpreter for the bundled rootfs. It excludes runtime state,
private account files, game data, and saves from the archive. The signed APK
is `out/release/Bloodborne-0.1.apk`; `runtime.sha256` verifies the embedded
runtime before installation. The signing key remains outside the repository.

## Verification performed for 0.1

- Android package signature, embedded runtime digest, and required payload
  entries checked.
- Installed over the test Thor app; first-run extraction completed.
- Python prepared the selected CUSA03173 dump on the phone. Its resulting
  `boot-linked.bin` SHA-256 matched the existing known-good image.
- The native FEXCore guest CPU and Vulkan renderer started with the bundled
  runtime. Long gameplay stability is not established.
- The touch controller toggle and on-screen rendering were checked over the
  running game. Face button, D-pad, analog stick, and trigger events reached
  the gamepad bridge and cleared on release. Moving Cross changed its active
  touch location and persisted after app restart; Reset restored the default
  and diagonal D-pad input.

The older [Thor notes](ANDROID_THOR.md) describe the manual fexdroid-based
prototype and include historical commands that are not the 0.1 install path.
