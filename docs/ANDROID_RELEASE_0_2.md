# Android release 0.2

**Target:** AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13, 16 GB RAM).
Gameplay is experimental. Other devices are untested. Install, settings and
build inputs are as in [release 0.1](ANDROID_RELEASE_0_1.md); 0.2 installs over
0.1 and keeps settings, saves and the prepared game image.

## Changes

- **Turnip driver: CPU and GPU no longer take turns.** Mesa 26.2.3's KGSL
  backend passed a zero timeout ("only check") to the kernel's wait ioctl,
  which KGSL treats as "wait forever". Mesa polls timeline semaphores this way
  on every queue submission, so each of the roughly 60 submissions a frame
  waited for the GPU to finish. The Vulkan recording thread sat in
  `adreno_drawctxt_wait`, the GPU command thread spun waiting for it (shown as
  ~98% CPU, ~80% of it kernel time), and the GPU idled at 401 of 680 MHz. In
  gameplay, frames took about 46–52 ms (19–22 FPS). The fix is upstream Mesa
  e984ef29; with it, gameplay is reported at 30 FPS or more.
- **Driver built from source.** `tools/android/build_turnip.sh` builds Turnip
  from Mesa main at a pinned commit with fexdroid's Mesa patches
  ([mesa-patches](../tools/android/mesa-patches/README.md)). That includes the
  direct present path, which the app needs to hide "Starting Bloodborne…" on
  the first frame. The release packager replaces the rootfs's driver, Vulkan
  manifest and drirc files with this build.
- **Frame generation flicker fixed.** The game frame is shown in the video out
  format's channel order, which can differ from the order it was rendered in.
  Generated frames kept the rendered order, so every other frame had red and
  blue swapped. They are now shown in the same order as the game's frames.
- **Diagnostics off.** `run-thor.sh` no longer sets `BB_FRAME_STATS` (per-frame
  timers and stall logs, which grew the log to about 10 MB in a session) or the
  frame-dump trigger. Both can still be set in `thor-local.sh`.

## Build

```bash
bash tools/android/build_turnip.sh
python3 tools/android/build_arm64_bundle.py --device-libs <rootfs library names>
bash tools/android/build_release_apk.sh
```

`build_turnip.sh` fetches its own Debian trixie arm64 sysroot (SHA-256 checked)
and creates a Python virtual environment for meson and mako under
`.local-deps/android`. The APK version comes from `AndroidManifest.xml`; the
output is `out/release/Bloodborne-0.2.apk` with a `.sha256` file.

## Contents

The APK contains the app, FEX/FEXCore, the Linux ARM64 rootfs, Turnip, the
native bbport runtime, FSR 3.1, default settings, the port's compiled patch
files (`bbport/arm64/patch-parts/*.bin`), and Python for game preparation. The
launcher icon comes from the builder's own `sce_sys/icon0.dds`. It contains
**no game executable, PS4 modules, game assets, game-derived images
(`boot-linked.bin`, `content.bin`), or saves**; the packager refuses
game-derived files.

## Verification performed for 0.2

- Turnip deps checked against the rootfs: every needed library is present, glibc
  2.38 symbols at most. The scripted build matches the driver tested on the
  device (same size within 1 KB; 64 bytes of code differ by source path).
- APK signed with the 0.1 key (v3); the runtime digest, driver, GPU library and
  `run-thor.sh` inside it checked; no game files.
- Installed over 0.1 on the Thor: the runtime was replaced, the game started
  with the bundled driver, the start-up text cleared, and the loading screen ran
  at 31 FPS with no stall logging.
- Frame generation without flicker, and gameplay at 30 FPS or more, were
  checked on the device with the same driver and GPU library before packaging.
  Long gameplay stability is not established.
