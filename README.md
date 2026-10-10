<h1 align="center">Bloodborne-Android</h1>

<p align="center">
  <b>Bloodborne on Android.</b> Yharnam on Snapdragon phones and handhelds.
</p>

<p align="center">
  <a href="https://github.com/ducvd89/Bloodborne-Android/releases/latest"><img src="https://img.shields.io/github/v/release/ducvd89/Bloodborne-Android?label=download&color=8b1e1e" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/Android-9%2B-3ddc84?logo=android&logoColor=white" alt="Android 9+">
  <img src="https://img.shields.io/badge/GPU-Adreno%206xx%20%7C%207xx%20%7C%208xx-d6a64a" alt="Adreno">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0-blue" alt="GPL-2.0"></a>
</p>

<table align="center">
  <tr>
    <td align="center" width="40%"><img src="docs/images/ayn-thor.jpg" alt="Bloodborne running on the AYN Thor"><br><sub>AYN Thor · Snapdragon 8 Gen 2</sub></td>
    <td align="center" width="60%"><img src="docs/images/find-n5.jpg" alt="Bloodborne with the on-screen controller on the Oppo Find N5"><br><sub>Oppo Find N5 · Snapdragon 8 Elite · on-screen controller</sub></td>
  </tr>
</table>

Port of Bloodborne (PS4, **CUSA03173 v1.09**). The runtime and renderer run natively on ARM64, FEXCore translates the game's x86-64 code, and Mesa Turnip drives the GPU. Experimental.

## Features

- **Lossless Scaling frame generation** (2×, 3×, 4×) — new in 0.4.0. Sign in with Steam in the app and it downloads `Lossless.dll` from your own copy of Lossless Scaling
- **FSR 3.1** upscaling and **FSR 3.1 frame generation**
- **On-screen PlayStation controller** with an editable layout; physical controllers work too
- **Two Vulkan drivers in one APK**, picked automatically for your GPU
- 16:9 picture on any screen, including foldables

## Devices

| Tested | SoC | GPU | Driver |
|---|---|---|---|
| AYN Thor | Snapdragon 8 Gen 2 | Adreno 740 | Mesa main |
| Oppo Find N5 | Snapdragon 8 Elite | Adreno 830 | Mesa turnip/gen8 |

Other Adreno 6xx/7xx/8xx devices are untested. Requires Android 9+ and 4 KB memory pages; tested with 16 GB RAM.

## Install

1. Download the APK from [Releases](https://github.com/ducvd89/Bloodborne-Android/releases/latest) and install it.
2. Copy your own extracted **CUSA03173 v1.09** dump to the phone.
3. Open the app, select the folder containing `eboot.bin`, and allow storage access. The first launch takes a few minutes.

The APK contains no game files. Updates keep settings and saves.

On ColorOS / OxygenOS (Oppo, OnePlus) a "Security warning" about a malicious app may appear at launch. Tap **Got it**; the game keeps running. ColorOS shows it for any app that runs Linux programs, such as Termux and Winlator.

## The menu

Swipe in from the left edge (or press Back) to open the menu. Its sections:

- **Game:** resume or restart the game.
- **Graphics:** graphics settings (upscaler, render size, FPS limit, effects) and Lossless Scaling.
- **Controls:** turn the on-screen controller on or off, edit its layout, reset it.
- **Storage:** choose the game folder, quit.

Each entry shows its current setting underneath.

## Lossless Scaling frame generation

[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) generates extra frames between the game's own, so the picture moves more smoothly. The app runs it through [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) 1.0, adapted for the Turnip driver. Lossless Scaling itself is not included: it needs your own copy, bought on Steam.

1. Open the menu → **Get Lossless.dll from Steam**.
2. Scan the QR code with the Steam app on your phone (Steam Guard → scan QR code), or choose **Sign in with password instead** and enter your Steam Guard code.
3. The app checks that your account owns Lossless Scaling and downloads only `Lossless.dll` (about 7 MB).
4. Open **Lossless Scaling** in the menu and pick 2×, 3× or 4×. Lower flow scale and performance mode are faster. Changes apply while you play.

The sign-in is used once, for that download. The app keeps no password and no sign-in token; the file is saved in the app's own storage. You can also copy `Lossless.dll` from a PC (`steamapps/common/Lossless Scaling/Lossless.dll`) next to your game folder, e.g. `/sdcard/Bloodborne/Lossless.dll` beside `/sdcard/Bloodborne/CUSA03173`.

Tips:

- Frames are smoothest when the screen refresh rate is the FPS limit × the multiplier. The app sets the screen to that rate when it can: 30 FPS × 2 → 60 Hz, 30 × 4, 40 × 3 or 60 × 2 → 120 Hz.
- It works with any upscaler and replaces FSR 3.1 frame generation. It sees only finished frames, without the game's motion vectors, so fast motion and the HUD show more artifacts than with FSR 3.1 frame generation.
- Technical details: [docs/ANDROID_LSFG.md](docs/ANDROID_LSFG.md).

## Build

```bash
git clone --recursive https://github.com/ducvd89/Bloodborne-Android.git
cd Bloodborne-Android
bash tools/android/build_turnip.sh main
bash tools/android/build_turnip.sh gen8
bash tools/android/build_arm64.sh
bash tools/android/build_lsfg.sh
bash tools/android/fetch_steam_libs.sh
python3 tools/android/build_arm64_bundle.py --device-libs <rootfs library list>
bash tools/android/build_release_apk.sh
```

Build inputs (SDK, rootfs, signing key, game icon) are listed in [docs/ANDROID_RELEASE_0_1.md](docs/ANDROID_RELEASE_0_1.md). The Linux PC build is documented in [README.linux.md](README.linux.md).

## Credits

[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) (Linux port), [shadPS4](https://github.com/shadps4-emu/shadPS4), [ARM_bloodborne_pc](https://github.com/MaSieS4Fun/ARM_bloodborne_pc), [FEX](https://github.com/FEX-Emu/FEX), [fexdroid](https://github.com/cobrabm12/fexdroid), [Mesa](https://gitlab.freedesktop.org/mesa/mesa), [mesa-unified turnip/gen8](https://github.com/whitebelyash/mesa-unified), [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), [JavaSteam](https://github.com/Longi94/JavaSteam), and [GameNative](https://github.com/utkarshdalal/GameNative) for showing LSFG on Android.

Licensed under [GPL-2.0](LICENSE); third-party notices in [THIRD_PARTY_LICENSES.txt](tools/android/frontend/THIRD_PARTY_LICENSES.txt). Not affiliated with Sony Interactive Entertainment, FromSoftware, Valve or the developers of Lossless Scaling.
