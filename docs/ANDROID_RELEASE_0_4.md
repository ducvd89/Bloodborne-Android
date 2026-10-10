# Android release 0.4.0

**Target:** AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) and Oppo Find N5 (Snapdragon 8
Elite, Adreno 830). Gameplay is experimental. 0.4.0 installs over 0.3.x and keeps settings, saves
and the prepared game image.

## Changes

- **Lossless Scaling frame generation.** 2×, 3× or 4× frames from Lossless Scaling's frame
  generation, through lsfg-vk 1.0.0 (MIT) with this port's changes for Turnip
  ([ANDROID_LSFG.md](ANDROID_LSFG.md)). It needs `Lossless.dll` from the player's own Lossless
  Scaling; the APK does not contain it.
- **Steam sign-in for Lossless.dll.** Menu → *Get Lossless.dll from Steam*: sign in with a QR code
  scanned in the Steam mobile app, or with name, password and Steam Guard code. The app checks that
  the account owns Lossless Scaling and downloads only `Lossless.dll` from its Windows depot,
  checked against the manifest's SHA-1. No password or sign-in token is kept. Copying the file
  next to the game folder still works.
- **Display refresh rate follows frame generation.** With frame generation on, the app asks for the
  display mode equal to FPS limit × frames when the screen has it (30 × 2 → 60 Hz; 30 × 4, 40 × 3,
  60 × 2 → 120 Hz), else the fastest one. At the system's default 60 Hz a third of the generated
  frames were dropped.
- **New menu.** The left-edge menu is grouped into Game, Graphics, Controls and Storage, with an
  icon for each entry and its current setting underneath; the gamepad focus is highlighted.
- **FSR 4 is not offered.** On the Adreno 740 it ran at 2.4 FPS (about 470 ms of GPU time a frame)
  with a black scene, and with Lossless Scaling it crashed in the driver. Its model files are no
  longer in the APK.
- **Lossless Scaling failures restart the game without it** instead of stopping it.

## Measured on the AYN Thor

- Title screen, 2×, flow scale 0.75, performance mode: 45 real + 45 generated frames a second, GPU
  23% busy at 401 MHz.
- 30 FPS limit × 2 at 60 Hz: nearly every frame shown, 16.8 ms apart.
- 60 FPS limit × 2 at 120 Hz in the world: 31–60 real frames a second, depending on the scene.

## Build

```bash
bash tools/android/build_lsfg.sh
bash tools/android/fetch_steam_libs.sh
bash tools/android/build_arm64.sh
python3 tools/android/build_arm64_bundle.py --device-libs <rootfs library names>
bash tools/android/build_release_apk.sh
```

`fetch_steam_libs.sh` resolves JavaSteam and its dependencies with a pinned, checksum-verified
Apache Maven into `.local-deps/android/steam-libs`; the APK grows from about 0.4 MB of app code to
about 9 MB.
