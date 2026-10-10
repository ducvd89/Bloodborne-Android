# Android release 0.5.0

**Target:** AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13). Gameplay is experimental. 0.5.0
installs over 0.4.0 and keeps settings, saves and the prepared game image. Tested on the Thor; the
Oppo Find N5 (Adreno 830) was not tested with 0.5.0, and Lossless Scaling frame generation still
shows doubled frames there (an open issue).

## Changes

- **Upstream bloodborne_pc 0.5 merged.** Save files are written to a temporary copy, synced and
  renamed (no damaged saves when Android kills the app), GPU crash and stall fixes, the per-frame
  passes' pipelines compiled on worker threads, DLC folders, the damaged-extraction check.
- **Cheats** (menu → Cheats): infinite health (HP refilled to full every frame) and infinite blood
  vials and items (using an item leaves its count as it was). They switch at once while you play:
  code hooks for 1.09, always in place, test two flag bytes the loader keeps as the app's
  `cheats.state` says (`src/probe.c`, `tools/android/build_thor_patch_parts.py`). The hooks are at
  the same places as Shiningami's GoldHEN cheats for CUSA03173 1.09; their code is in the unused
  ends of the game's own code and data pages, and the build refuses another eboot.
- **Game settings** (menu → Game settings): skip the online/offline screen (upstream's new patch, on
  by default), skip the startup intros, the free camera and the game's debug menu.
- **Restart errors fixed:** applying settings restarted the game but showed the old process's read
  error over the new one ("InterruptedIOException"), keeping the start screen up.

## Upstream features not used on Android

- The settings pages inside the game's System menu and the native detours hook the game's code with
  jumps to x86-64 code beside it: under FEX the game's code cannot run there (it crashed at the
  title). The pages are off on AArch64 (`gpu/shim/bbport_game_menu.cpp`); the app's menu has the
  settings that apply here, the hooks stay traps (`gpu/shim/bbport_guest_hooks.cpp`).
- The GPU check of indirect dispatch counts (`BB_INDIRECT_GUARD`) is off: on the Thor the game hung
  in an indirect dispatch with it after a few minutes of play, not without it nor in 0.4.
- The new memory model (AMD and NVIDIA desktop GPUs), DLSS, MangoHud, monitor and mouse settings.

## Build

As 0.4.0 ([ANDROID_RELEASE_0_4.md](ANDROID_RELEASE_0_4.md)). The patch parts now include
`cheats.bin` and `skip_network_choice=1.bin`; the loader allows patch writes in a segment's last
page past its data (mapped with it), where the cheats' code and flags go.
