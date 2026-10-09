# Bloodborne-Android

Bloodborne on Android, built from the [bbport Linux project](https://github.com/deadinside28/bloodborne_pc). The 0.1 release targets the **AYN Thor with Snapdragon 8 Gen 2, Adreno 740, Android 13, and 16 GB RAM**. The game is running on that device, with **20–35 FPS reported during gameplay**. Performance depends on the scene and settings; other phones have not been tested.

The Android app runs bbport's renderer and supporting runtime natively on ARM64. FEXCore translates the game's x86-64 code, and Mesa Turnip supplies Vulkan. The APK includes these components, FSR 3.1, and the tools to prepare a game dump on the phone. It does **not** include the game executable, PS4 modules, assets, or saves.

## Install 0.1

1. Download [Bloodborne-0.1.apk](https://github.com/ducvd89/Bloodborne-Android/releases/tag/android-0.1) and install it on the AYN Thor. The Android package is `com.ducvd89.bloodborne`.
2. Extract your own **Bloodborne CUSA03173 v1.09** dump to a folder on the phone. Select the folder containing `eboot.bin` when the app asks.
3. Grant storage access. On first launch, the app unpacks its Linux runtime and prepares the executable image from your game files. Allow a few minutes and about 1 GB of free space beyond the game dump.

The app icon is generated from `sce_sys/icon0.dds` in the builder's game dump. The release APK includes that icon but no game executable, modules, assets, or saves. App settings and saves remain in private app data when installing an update; back them up before uninstalling.

## Controls and settings

- The Thor's physical controller maps its Nintendo-position buttons to PlayStation inputs: **B → Cross**, **A → Circle**, **Y → Square**, **X → Triangle**. Sticks, D-pad, shoulders, and triggers are bridged to the game.
- Swipe from the left edge or tap the left-edge handle to open the game menu. Use it for graphics settings, the game folder, restart, and quit.
- Turn **On-screen controller** on or off in that menu. The PlayStation-style overlay includes face buttons, D-pad, sticks, L1/L2/R1/R2, L3/R3, touchpad, and Options.
- Tap **Edit on-screen controller** to drag individual controls, then tap **Done**. **Reset controller layout** restores the original positions. Visibility and positions persist across app launches.

FSR 3.1 is bundled. FSR 4 is disabled on the Thor. The FEX configuration uses the fast profile with TSO emulation off.

## Build the APK

```bash
git clone --recursive https://github.com/ducvd89/Bloodborne-Android.git
cd Bloodborne-Android
bash tools/android/build_release_apk.sh
```

The build also needs an Android SDK, JDK, ARM64 native bundle, migrated FEX/Turnip rootfs, pinned display/input bridge APK, signing key, and the builder's own `game/CUSA03173/sce_sys/icon0.dds`. A clean clone does not recreate the migrated rootfs yet. See [release build inputs and verification](docs/ANDROID_RELEASE_0_1.md) for details. The output is `out/release/Bloodborne-0.1.apk`.

## Scope and credits

Release 0.1 has been tested on the AYN Thor. Performance and stability on other Snapdragon devices are unknown. Rumble and right-side touchpad gestures are not mapped. The Linux launcher, keyboard/mouse support, and optional NVIDIA DLSS work remain in the source tree; DLSS is a Linux/NVIDIA feature and is not part of the Android APK.

The original Linux project is [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). The ARM64/FEX port builds on [MaSieS4Fun/ARM_bloodborne_pc](https://github.com/MaSieS4Fun/ARM_bloodborne_pc). Its renderer derives from [shadPS4](https://github.com/shadps4-emu/shadPS4). The Android display/input bridges come from [fexdroid](https://github.com/cobrabm12/fexdroid), CPU translation from [FEX](https://github.com/FEX-Emu/FEX), and Vulkan rendering from [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html). The [GPL-2.0 license](LICENSE) and [third-party notices](tools/android/frontend/THIRD_PARTY_LICENSES.txt) are retained. This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD, or NVIDIA.
