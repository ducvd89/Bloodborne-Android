# Bloodborne-Android

An experimental Android adaptation of [bbport's Linux version](https://github.com/deadinside28/bloodborne_pc), based on commit `daa7165`.
The current target is the **AYN Thor: Snapdragon 8 Gen 2, Adreno 740, Android 13, 16 GB RAM**.

**Status: boots menus; gameplay is unstable and currently crashes. This is not a finished, playable Android port.**
Title-menu navigation has been verified. A recent crash occurred on the `shadPS4:AvVideo` thread with an invalid instruction address; its cause is still under investigation.

The Android build runs the bbport runtime and Vulkan renderer natively on ARM64. FEXCore translates the game's x86-64 code; Linux/glibc Turnip supplies the Vulkan driver. This is not a native recompilation of the game.

## Android features

- A Bloodborne-only Android front end: landscape fullscreen, no bottom navigation, and a drawer that slides in from the left edge.
- Drawer controls for returning to the game, graphics settings, restarting, and quitting. No Steam, Dota 2, or CS launchers.
- The app is named **Bloodborne** and uses `sce_sys/icon0.dds` from the builder's own game dump as its icon.
- Nintendo-position controller mapping for the Thor: **B → Cross**, **A → Circle**, **Y → Square**, **X → Triangle**.
- Optional translucent PlayStation-style touch controller with face buttons, D-pad, sticks, shoulders, triggers, touchpad, and Options. Toggle it in the left-side menu; the choice persists.
- An Android gamepad-event bridge for launching without ADB input permissions, including analog sticks and triggers. Its runtime translation has unit coverage; full gameplay validation remains outstanding.
- Scripts for staging the Linux runtime, fetching generic x86-64 libraries, deploying to the device, and running CPU/Vulkan/controller diagnostics.

The APK includes the ARM64 runtime, FEXCore, Turnip, FSR 3.1, and the tools to prepare the player's game dump on the phone. Release 0.2 fixes a Turnip driver bug that serialized the CPU and GPU (on the Thor, gameplay measured about 19 FPS before and is reported at 30 FPS or more after), fixes flicker with frame generation, and turns diagnostics off. Gameplay remains experimental.

## Install release 0.2

Download `Bloodborne-0.2.apk` from the [0.2 release](https://github.com/ducvd89/Bloodborne-Android/releases/tag/android-0.2). It installs over 0.1 and keeps settings and saves. Install it on an **AYN Thor**, grant storage access, and select your own extracted **CUSA03173 v1.09** folder containing `eboot.bin`. First launch unpacks the Linux runtime and prepares the game image in the app's private storage. Keep at least 1 GB free in addition to the game dump. The APK contains the game icon, but no game executable, modules, assets, or saves.

Open the left-edge menu to show or hide the touch controls. Tap **Edit on-screen controller** to drag each control, then tap **Done**. **Reset controller layout** restores the original positions. The layout and visibility persist across launches.

See [release 0.2 notes](docs/ANDROID_RELEASE_0_2.md) and [setup and build details](docs/ANDROID_RELEASE_0_1.md). The older [Thor development notes](docs/ANDROID_THOR.md) describe the earlier manual prototype.

## Build the release APK

```bash
git clone --recursive https://github.com/ducvd89/Bloodborne-Android.git
cd Bloodborne-Android
# Prepare the ARM64 build, migrated runtime rootfs, Android SDK, pinned bridge APK,
# signing key, and your own game icon as described in docs/ANDROID_RELEASE_0_1.md.
bash tools/android/build_turnip.sh
bash tools/android/build_release_apk.sh
```

The output is `out/release/Bloodborne-<version>.apk` (version from `AndroidManifest.xml`), package `com.ducvd89.bloodborne`. No game files, saves, proprietary upscaler runtimes, signing keys, or downloaded dependencies are committed to this repository. The APK bundles the runtime dependencies and the locally supplied game icon. FSR 4 remains disabled on the Thor; FSR 3.1 is included.

## Known limitations

- Crashes remain unresolved, including during attempts to enter gameplay. Strict FEX TSO settings have not fixed them.
- No sustained 3D gameplay, combat/camera validation, or Android audio validation yet. The 30 FPS menu cap is not a gameplay benchmark.
- Rumble and right-side touchpad gestures are not mapped.
- The release is tested on the AYN Thor; other Android devices are unverified.

The Linux renderer, launcher, optional experimental NVIDIA DLSS integration, and keyboard/mouse work are retained. DLSS is a Linux/NVIDIA feature, not an Android capability. See the [original Linux documentation](README.linux.md), [DLSS notes](docs/DLSS_LOCAL.md), and [keyboard/mouse controls](docs/KEYBOARD_MOUSE.md).

## Credits and license

The original Linux project is [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). Its renderer derives from [shadPS4](https://github.com/shadps4-emu/shadPS4). The Android runtime and native display/input bridges come from [fexdroid](https://github.com/cobrabm12/fexdroid), with CPU translation by [FEX](https://github.com/FEX-Emu/FEX) and Vulkan rendering through [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html).

The upstream [GPL-2.0 license](LICENSE) and third-party notices are retained. The reused native bridges' MIT notice is included in [THIRD_PARTY_LICENSES.txt](tools/android/frontend/THIRD_PARTY_LICENSES.txt). This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD, or NVIDIA.
