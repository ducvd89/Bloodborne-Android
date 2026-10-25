# Android 2.1.0 Beta 1 (versionCode 37): the app builds the native code on the phone

2.1.0 removes the PC from the setup: when the game's native code is missing or out of date, the app builds it itself from the game files it already has, with the Rebuild Tool's command line build (1.2.0, shipped inside the APK).
Building on a PC with the tool still works and is offered next to it.

## Build on the phone

- **The rebuild dialog** ("Native code needed", `RebuildDialog`) replaces the old dialog and the tool-download links. It opens at the game's start when no native code fits (once per game folder; the full-screen message that stays behind it has a **Rebuild native code** button).
  Its first line is the reason: no native code, built for another app version (ABI), built for another game build, or damaged. Then:
  1. **Build on this phone** (or **Continue the build (N % done)** when an unfinished build of the same tool and game exists), with the estimated time and what it needs; shown disabled with the reason when the phone cannot do it
     (not arm64, under about 1.5 GB of free memory, under 1.3 GB of free storage in the app's data). **Build on a PC** opens the tool's GitHub page in the default browser and offers **Check again** (the same check and import as at the start).
     **Quit** closes the app.
  2. **Before you build**: the phone will get hot (CPU up to about 95 C), a live charger and battery line, do not close the game from recents. Two checkboxes: "I understand the phone will get hot" and, when not charging, "Build without the charger anyway". **Start build** needs the ticked boxes.
  3. **Building native code**: stage, bar with percent, time left, elapsed, CPU temperature (hidden when the phone does not let the app read it), charger, the tool's memory in use. **Run in background** (a notification with the progress and Cancel) and **Cancel** (finished parts are kept).
  4. **Native code ready**: "Built in M min S s and checked", **Start game**. A failure shows the tool's message with **Retry** and **Report a problem** (the report holds the build's log and compile.txt).
  All controls work with the gamepad (D-pad, A, B) and touch.
- **The build** (`AotBuildService`, a foreground service with a partial wake lock) runs `libbbrecomp.so` from the APK's native library folder:
  `--boot bbport/data/boot-linked.bin --game-dir <work>/game --bundle-dir/--variants (copied from the APK assets to bbport/recomp/<tool version>/) --so <work>/libbbaot.so --stats <work>/compile.txt --jobs <n> --shards 64 -O1 --no-nice --progress --resume --clean-work`.
  `<work>` is `bbport/aot-build/<tool version>-<image hash>/`; `bb-prepare` makes the game image first when needed; the four game files the tool reads are copied into `<work>/game` first. Jobs: `min(4, non-little cores, (free memory - 1 GB) / 0.8 GB)`, at least 1.
  The bar uses the tool's `BBR-PROGRESS` lines (scan 1, discover 2, lift 2, compile 93, variants 1, link 1 %); only `BBR-DONE 0` counts as success. A stopped, cancelled or killed build resumes from its finished shards.
- **On success** the pair goes through the same checks as a pair from a PC (`NativeCpu.install`: ABI, game image, checksum) into `bbport/aot`, a copy goes into the game folder's `aot/` on storage (so the folder never brings back an old library), and the work folder is deleted.
- **Menu → Native CPU → Rebuild on this phone** starts the same flow from the confirmations (the running game is closed when the build starts).

## ABI 2

- The runtime and the app now take ABI version **2** (`BbrAbiVersion`, `NativeCpu.ABI_VERSION`); Rebuild Tool 1.2.0 writes it. Every library built by tool 1.1.0 or older counts as "built for another app version": everyone rebuilds once.
  That also brings the 1.2.0 fix for the game stopping with `guest trap (0x800d0b, 0x287188a)` (jump table scan, granular probe and rdtsc fixes).
- `tools/check_recomp_abi.sh` reads the tool's `kBbrAbiVersion` (`src/version.h`).

## Trigger rule at the game's start

1. The installed native code (`bbport/aot`) is valid, has ABI 2 and fits the game: start.
2. Else the game folder's `aot/` pair is valid with ABI 2 and fits: import it, start.
3. Else (nothing anywhere, another ABI, another game build, damaged): the rebuild dialog. `NativeCpu.needed` decides and words the reason; tests: `tests/android-native-cpu`.

## Other

- Rebuild Tool 1.2.0 (`tools/recomp` at 00ccaf7): `libbbrecomp.so`, `bundle/`, `variants/` and the licenses come from the tool's Android package (`tools/android/fetch_recomp_android.sh`, pinned by sha256; `BB_RECOMP_ANDROID_TARBALL=<file>` takes a local tarball), the tool's notices are appended to the app's.
- Permissions added: `FOREGROUND_SERVICE`, `WAKE_LOCK`. The app targets API 28, so there is no notification permission request of its own: on Android 13 and newer the system asks when the notification channel is created; if refused, the build runs and only its dialog shows the progress.
- Test builds: `BB_TEST_BUILD=1 bash tools/android/build_release_apk.sh` makes `Bloodborne-2.1.0-beta1-test.<commit>.apk`.
