# Bloodborne-Android

An experimental Android adaptation of [bbport's Linux version](https://github.com/deadinside28/bloodborne_pc), based on commit `daa7165`.
The current target is the **AYN Thor: Snapdragon 8 Gen 2, Adreno 740, Android 13, 16 GB RAM**.

**Status: boots menus; gameplay is unstable and currently crashes. This is not a finished, playable Android port.**
Title-menu navigation has been verified. A recent crash occurred on the `shadPS4:AvVideo` thread with an invalid instruction address; its cause is still under investigation.

The game still uses bbport's x86-64 Linux runtime and Vulkan renderer. FEX translates the CPU code on ARM64, and Linux/glibc Turnip supplies the native Vulkan driver. This is not a native ARM recompilation.

## Android features

- A Bloodborne-only Android front end: landscape fullscreen, no bottom navigation, and a drawer that slides in from the left edge.
- Drawer controls for returning to the game, graphics settings, restarting, and quitting. No Steam, Dota 2, or CS launchers.
- The app is named **Bloodborne**. Local builds use the icon from the user's own game dump when available; no game artwork is included here.
- Nintendo-position controller mapping for the Thor: **B → Cross**, **A → Circle**, **Y → Square**, **X → Triangle**.
- An Android gamepad-event bridge for launching without ADB input permissions, including analog sticks and triggers. Its runtime translation has unit coverage; full gameplay validation remains outstanding.
- Scripts for staging the Linux runtime, fetching generic x86-64 libraries, deploying to the device, and running CPU/Vulkan/controller diagnostics.

The APK front end and game display have been built and installed on the test Thor. Earlier ADB controller tests passed all physical controls, and all four D-pad directions plus confirm/cancel were verified inside the game's menus. The new drawer and direct-launch controller bridge still need broader device testing.

## Build and setup

See [Android build, runtime setup, and limitations](docs/ANDROID_THOR.md).

```bash
git clone --recursive https://github.com/ducvd89/Bloodborne-Android.git
cd Bloodborne-Android
# Install the Linux build dependencies described in README.linux.md first.
bash build.sh
python3 tools/android/fetch_thor_libraries.py
bash tools/android/build_thor_probe.sh
python3 tools/android/build_thor_bundle.py --baseline-root .local-deps/android/baseline

# Requires Android SDK platform 35, build-tools 35.0.0, a JDK, and the pinned
# fexdroid APK described in docs/ANDROID_THOR.md.
bash tools/android/build_frontend.sh
```

The output APK is `out/thor/frontend/Bloodborne-Thor.apk`. It is a small front end, **not a self-contained runtime installer**. It requires the matching FEX/Turnip environment and bbport bundle on the device, plus the user's game dump. The runtime currently uses fexdroid's fixed package/data path; installing this front end over upstream fexdroid requires a verified backup and migration because their signing keys differ.

No game files, saves, proprietary upscaler runtimes, signing keys, or downloaded dependencies are included in this repository. Use your own **Bloodborne CUSA03173 v1.09** dump. The supplied Wine FEXCore WCP and Android/Bionic Turnip ZIP are not compatible replacements for this Linux/glibc runtime.

## Known limitations

- Crashes remain unresolved, including during attempts to enter gameplay. Strict FEX TSO settings have not fixed them.
- No sustained 3D gameplay, combat/camera validation, or Android audio validation yet. The 30 FPS menu cap is not a gameplay benchmark.
- Rumble and right-side touchpad gestures are not mapped.
- The Android installation workflow is still manual and specific to the tested runtime.

The Linux renderer, launcher, optional experimental NVIDIA DLSS integration, and keyboard/mouse work are retained. DLSS is a Linux/NVIDIA feature, not an Android capability. See the [original Linux documentation](README.linux.md), [DLSS notes](docs/DLSS_LOCAL.md), and [keyboard/mouse controls](docs/KEYBOARD_MOUSE.md).

## Credits and license

The original Linux project is [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). Its renderer derives from [shadPS4](https://github.com/shadps4-emu/shadPS4). The Android runtime and native display/input bridges come from [fexdroid](https://github.com/cobrabm12/fexdroid), with CPU translation by [FEX](https://github.com/FEX-Emu/FEX) and Vulkan rendering through [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html).

The upstream [GPL-2.0 license](LICENSE) and third-party notices are retained. The reused native bridges' MIT notice is included in [THIRD_PARTY_LICENSES.txt](tools/android/frontend/THIRD_PARTY_LICENSES.txt). This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD, or NVIDIA.
