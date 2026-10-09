# AYN Thor Linux boot experiment

Target: AYN Thor, Snapdragon 8 Gen 2 / Adreno 740, Android 13, 16 GB RAM,
4 KiB pages. This uses the existing x86-64 Linux bbport under FEX; it is not
a native ARM port. A small Bloodborne-only APK front end now starts the installed
Linux runtime; it is not a self-contained runtime installer.

## Runtime and verified checks

The runtime was initially installed using `ro.cobrabm.fexdroid` version
`0.3.112-alpha-legacy` from
[fexdroid's official apk-latest release](https://github.com/cobrabm12/fexdroid/releases/tag/apk-latest).
Source inspected at `2b03ec73c3507b640fba1942fb8d99b43b88945a`, payload
`d9ac9cb73cf20f96`. The runtime includes Linux/glibc FEX and Turnip 26.2.3.

Checks on the test Thor:

* x86-64 dynamic hello under FEX: passed.
* bbport mapped x86 code plus an imported-call fixture: passed, expected exit 20.
* bbport Vulkan command submission and 4096-byte GPU readback: passed.
* bbport's complete headless renderer device initialization: passed on Turnip 26.2.3.
* SDL window, Vulkan presenter, and mapped CPU fixture together: passed, expected exit 20.
* Bloodborne v1.09 title menu (Play Online / Play Offline): rendered on Adreno 740
  and displayed on the Thor after enabling direct frame delivery.

Logs: `out/thor-cpu-fixture.log`, `out/thor-bbport-vulkan.log`,
`out/thor-renderer.log`, `out/thor-window.log`, `out/thor-vulkan-summary.txt`.
The title menu boots. No 3D gameplay or audio has been verified.
Menu frame statistics reached the configured 30 FPS cap; this is not a gameplay benchmark.
Evidence: `out/thor-game-direct.log`, `out/thor/game-direct.png`, and the GPU capture
`out/thor/frames/f001_present_1280x720_rgba.png`.
All 28,850 game files (31,438,466,019 bytes) match the source file sizes;
`eboot.bin` and `boot-linked.bin` additionally match SHA-256 hashes.

The supplied `FEXCore-2610-14c92681f.wcp` contains the Wine PE DLLs
`libarm64ecfex.dll` and `libwow64fex.dll`. The supplied Turnip 26.3.0-R6 ZIP
contains an ARM64 Android/Bionic driver, depending on Android `libc.so`,
`libhardware.so`, and `libnativewindow.so`. Neither is a replacement for
the Linux/glibc components used here. The user chose to continue the Linux
prototype. Both originals are saved on the Thor under
`/sdcard/Download/BloodbornePC-components/`; neither is active in this runtime.
See the [FEX ARM64EC documentation](https://wiki.fex-emu.com/index.php/Development:ARM64EC)
for the separate Wine integration.

## Building the experiment

First install the normal Linux build dependencies from `README.linux.md`,
then build with `bash build.sh`. The original build machine's
CachyOS system libraries and CRT startup objects require x86-64-v4 (AVX-512),
which caused SIGILL under FEX. Fetch generic Arch x86-64 libraries into an
isolated directory and link the loader with generic startup objects:

```bash
python3 tools/android/fetch_thor_libraries.py
bash tools/android/build_thor_probe.sh
python3 tools/android/build_thor_bundle.py --baseline-root .local-deps/android/baseline
bash tools/android/deploy_thor.sh $THOR_SERIAL
```

Downloads come from Arch's HTTPS mirror and their SHA-256 hashes are verified
against the repository index. Package/version/hash records are saved in
`.local-deps/android/baseline/packages.json`. The host system is not modified.
The package list describes this machine's build; refresh it when dependencies
change. The GPU library and ATRAC decoder still come from the normal build;
compile those with generic x86-64 flags, never `-march=native` or v4.

Deployment replaces guest `libvulkan.so.1` with fexdroid's matching guest thunk
so graphics execute in native ARM Turnip. The staging manifest describes the
host bundle before this deliberate runtime replacement.

Game files from the user's extracted `game/CUSA03173` belong in
`/data/data/ro.cobrabm.fexdroid/files/bbport/game`, copied under `run-as`:

```bash
adb -s $THOR_SERIAL shell run-as ro.cobrabm.fexdroid mkdir -p files/bbport/game
tar -C game/CUSA03173 -cf - . | adb -s $THOR_SERIAL shell -T run-as ro.cobrabm.fexdroid tar -xf - -C files/bbport/game
```

## Running

Set `THOR_SERIAL` to your device ID from `adb devices` before these commands.

```bash
bash tools/android/launch_thor.sh $THOR_SERIAL cpu       # expected exit 20
bash tools/android/launch_thor.sh $THOR_SERIAL vulkan
bash tools/android/launch_thor.sh $THOR_SERIAL renderer
bash tools/android/launch_thor.sh $THOR_SERIAL window    # expected exit 20
bash tools/android/launch_thor.sh $THOR_SERIAL game
```

The Bloodborne front end enables direct frame delivery automatically. On the
original fexdroid app, this is the **Frames straight from the game (experimental)**
setting. The ordinary Xvfb presentation stayed black despite a correct GPU image;
`FEXDROID_PRESENT` fixed it.

The launcher modes above remain diagnostic tools. Game mode runs the loader in
the foreground over ADB, without a watchdog by default; `BB_TIMEOUT` sets a
bounded run inside the device shell. For normal direct launch, open **Bloodborne**
on the Thor. It starts Xvfb, attaches the native display bridge, and runs bbport.

The isolated `thor.ini` uses 640×360 rendering, 1280×720 output, FSR3 and a
30 FPS cap; these are test settings, not measured performance. Retail direct
memory is 5056 MiB. Audio remains unverified.
PC settings and saves are separate from the Thor's configuration and user directory.

## Thor controller (Nintendo printed labels)

The built-in controller reports vendor/product `2020:0112`, GUID
`030018dc202000001201000000000000`, initially named Xbox Wireless Controller.
That firmware name does not describe the printed Nintendo face-button labels.
The user's physical A/B/X/Y sequence confirmed that printed A reports SDL East,
B South, X North, and Y West. The mapping preserves PlayStation button positions:

| Thor control | PS4 input / action |
| --- | --- |
| B (bottom) | Cross: confirm / interact |
| A (right) | Circle: cancel / dodge / sprint |
| Y (left) | Square: use item |
| X (top) | Triangle: heal |
| L / R bumpers | L1 / R1: transform / attack |
| L2 / R2 triggers | L2 / R2: left weapon / heavy attack |
| Left stick / click | Move / L3 |
| Right stick / click | Camera / R3 lock-on |
| D-pad | PS4 D-pad |
| Start | Options |
| Select | Left touchpad click |

SDL's default mapping incorrectly uses advertised D-pad buttons; the Thor
actually emits HAT0. `tools/android/thor-gamecontrollerdb.txt` maps all four
directions to that hat. The launcher explicitly exposes the input node to
SDL's Linux evdev driver and selects this GUID. The node is discovered by
vendor/product, since its current `/dev/input/event9` number can change.

Actual hardware verification: all face buttons, both bumpers, both analog
triggers, both analog sticks, stick clicks, Start, Select, and all four D-pad
directions registered. Logs: `out/thor-controller-probe.log` and
`out/thor-controller-mapped2.log`. The game's log confirms
`gamepad connected: AYN Thor (the chosen one)`.

Xvfb initially left the game window without input focus (`None`), causing the
runtime to suppress pad input. The Thor's `BB_WINDOW_SIZE=1280x720` startup path
now raises the SDL window. The rebuilt library is installed on the device;
`XGetInputFocus` confirmed the game's window (`0x40000f`) has focus.

Remote in-game verification passed on 2026-10-09. ADB `sendevent` wrote events
to the actual controller's `/dev/input/event9`, exercising its SDL mapping and
the game's pad runtime. Each press lasted 150 ms and was released with a
`SYN_REPORT` after each transition:

- `EV_ABS ABS_HAT0Y` (`3 17`, values +1 / -1 / 0) moved the title selection
  Down to Play Offline and Up to Play Online.
- Printed B's `EV_KEY BTN_SOUTH` (`1 304`, values 1 / 0) selected Play Offline,
  opened System, and entered Controls.
- `EV_ABS ABS_HAT0X` (`3 16`, values +1 / -1 / 0) changed Camera X-Axis from
  Normal to Reversed with Right and restored Normal with Left.
- Printed A's `EV_KEY BTN_EAST` (`1 305`, values 1 / 0) returned from Controls
  to System and from System to the offline title menu.

Screenshots are retained as `out/thor/auto-controller-{initial,down,up,confirm,
system,controls,right,left,cancel,final}.png`. The run is logged in
`out/thor-game-automated-controller.log`. No host fault occurred during these
menu checks. These tests establish menu navigation, confirm, and cancel;
combat actions and camera movement in 3D gameplay remain unverified.

The Linux prototype has also hit host-memory faults in some runs, including
during a menu-navigation test. `fex-compatible.json` now selects strict scalar,
vector, and memcpy TSO memory ordering and full x87 precision. This is a
compatibility measure; a later run still crashed, so it does not resolve the
instability.
Logs are retained in `out/thor-game-controller.log` and
`out/thor-game-controller-compatible.log`.

The evdev diagnostics above rely on ADB `run-as`, which inherits the shell's
input group. The new APK instead handles Android key/joystick events and writes
an atomic `android-pad.state` snapshot through bbport's `BB_PAD_FILE` interface.
`BB_ANDROID_INPUT=1` enables this path. It includes partial analog triggers
(`l2=` / `r2=`), resets on focus loss, and releases controls when the drawer opens.
The pad runtime unit test covers analog values, clamping, digital overrides, and
release. Device gameplay validation of this new frontend bridge remains pending.
Rumble and right-side touchpad gestures are not mapped.

## Bloodborne-only fullscreen front end

Sources are in `tools/android/frontend/`. The app is named **Bloodborne**,
uses a fullscreen landscape surface, and has a left-edge drawer with Resume,
Graphics settings, Restart, and Quit. It contains no Steam, Dota 2, or CS UI.
The local build converts the user's `sce_sys/icon0.dds` to the launcher icon;
game artwork is not committed. A simple fallback icon is provided in source.

The installed front end has displayed the game fullscreen on the Thor. The
swipe drawer is implemented but needs further on-device validation. The game
also crashed during this session: the original fault reported an unmapped
instruction address on `shadPS4:AvVideo`; recursive unwind faults followed.
The underlying cause is unknown. Local evidence is retained in
`out/thor-game-android-crash.log` and `out/thor/fullscreen-game.png`.
Do not treat the fullscreen app as a gameplay stability fix.

### Build the APK

Requirements: a JDK with `javac`, Android SDK platform 35 and build-tools 35.0.0,
Python with Pillow, and the exact original fexdroid legacy APK. No Gradle or NDK
is needed: only the existing native display/input bridge binaries are reused.
Their MIT notice is included in the APK. The native Linux FEX/Turnip environment
is installed separately.

```bash
sdkmanager --sdk_root="$ANDROID_SDK_ROOT" 'platforms;android-35' 'build-tools;35.0.0'
export BB_ANDROID_SDK="$ANDROID_SDK_ROOT"
export BB_FEXDROID_APK=/path/to/fexdroid-legacy.apk
bash tools/android/build_frontend.sh
```

The original APK was obtained from the official release linked above. Its
SHA-256 is `2074fcee551dab0c6b0719c3f90f6389e517f194aaabacb5b0080bb2977ed847`.
The build script verifies that hash before extracting only `libfxdisplay.so`
and `libfxio.so`. The upstream `apk-latest` tag can change; a newer APK is not
silently accepted. The build uses an existing local JDK if available, otherwise
`javac` on PATH. A local development signing key is generated under `.local-deps/`.
Do not commit or distribute that key.

Output: `out/thor/frontend/Bloodborne-Thor.apk`. It contains the Java front end,
two native bridges, and the local launcher icon. It does **not** contain the
Linux rootfs, FEX, Turnip, the bbport bundle, or game files. A fresh install alone
cannot run the game.

### Installation and migration

The fixed data path `/data/data/ro.cobrabm.fexdroid/files/rootfs` is compiled
into the runtime. The frontend therefore retains that package ID, although its
visible name is Bloodborne. It cannot coexist with upstream fexdroid under a
different package ID without rebuilding the entire runtime for the new path.
The local signing key differs from the published fexdroid key, so Android will
reject a direct update over upstream fexdroid.

On the development device, all app data and saves were backed up before replacing
upstream fexdroid. The runtime and game were restored afterward. All 136 saved
save/config files matched their pre-migration SHA-256 hashes. The three skipped
backup entries were stale Unix sockets, not regular files. A complete backup
remains in `/sdcard/Download/BloodbornePC-before-fullscreen.tar`; a separate host
runtime/save backup also exists under `.local-deps/android/`. These are private
local artifacts and are not part of the repository.

For another device, first install the matching original runtime, initialize its
payload, and deploy the bbport bundle and user-owned game. Back up its complete
app data, verify the backup and keep a rollback copy of the original APK before
any signing-key migration. Restore the runtime and saves into the replacement
app's sandbox with `run-as`. Do not uninstall a populated runtime without a
verified backup.

After migration, source-only frontend updates signed with the same local key
can use:

```bash
adb -s "$THOR_SERIAL" install -r out/thor/frontend/Bloodborne-Thor.apk
adb -s "$THOR_SERIAL" shell am start -n ro.cobrabm.fexdroid/.MainActivity
```

Stop the running game before redeploying the Linux bundle: overwriting a mapped
shared library in place can corrupt the running process. The build/deploy scripts
do not automate the complete migration or enforce this prerequisite yet.
