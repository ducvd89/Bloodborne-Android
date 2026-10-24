# Device test of 2.0.0-test.0179e37 on the AYN Thor (2026-10-24, partial)

Stopped early on request (the user tests the rest). Device: AYN Thor (Snapdragon 8 Gen 2), app com.ducvd89.bloodborne.
Screenshots and the game log are in `out/device-test-2.0/` (not committed: `out/` is ignored).

## Done

| check | result |
|---|---|
| Backups | saves, `bbport.ini`, `bionic-local.sh`, shared_prefs and state files in `out/thor-backup-2.0/`; the old dev `aot/libbbaot.so` (sha256 b4b9068f...) in `out/thor-backup-2.0/aot-old/` and on the device in `files/aot-old-devbuild/` |
| Tool build (submodule `tools/recomp` at 69ad0cb, dev build per its README) | configure (FEX 2609.1 fetched) 6 s, build 26 s at -j 22 |
| `bb-recomp --game ~/BloodbornePC/game/CUSA03173 --out out/player-2.0` | OK in **2 min 14 s** (compile 2 min 13 s, peak RSS 15.5 GB, 22 jobs): 186,422 units, 0 lift failures, 0 trap units, 146 variants, 219 hook slots; `libbbaot.so` 128.7 MB (sha256 f8f95829...), image hash 3796d60602c196b9 |
| Install over 1.2.0 with `adb install -r` | versionCode 35, versionName 2.0.0-test.0179e37; saves and settings kept |
| No `.so` | full-screen **"Native code needed"** page ("libbbaot.so is missing."), buttons Open the Rebuild Tool page / Choose game folder / Import libbbaot.so…; the game did not start (`01-launch.png`) |
| Import from the game folder | `aot/` pushed next to `eboot.bin` in `/storage/emulated/0/Bloodborne/CUSA03173`; on returning to the app it copied the pair into `files/bbport/aot/` (checksum verified, `libbbaot.verified`) and started the game. The manual Import page was not needed and was not tested |
| Session | title → Continue → ~13 min (17:23-17:36): walking, camera, attacks, rolls, fights with Old Yharnam beasts (echoes rose), inventory and System menus, Quit game → title → Continue (loading screen) → more play. Runtime log: "ahead-of-time units only", 186,422 units, patch variants and 219 hook slots on |
| Misses / STOP | `logs/aot-misses.log` never created; 0 `STOP` lines |
| FPS (`BB_FRAME_STATS`, user settings: preset 4, LSFG x2, limit 60, GSR 2) | 169 samples, mean **29.2 FPS** (game frames; 60 shown with frame generation), min sample 24.2 (incl. title, menus, loading). Earlier AOT/FEX at the save spot: 29.7 / 29.4 |
| Settings | `bbport.ini` and `bionic-local.sh` identical to the backups at the end (preset 4, lsfg_multiplier 2, fps_limit 60) |

## Not done (stopped)

5 short launches, game language, button remap, vibration, CPUID log, 60 s per-thread CPU profile, the phase7-dialog check.

## Issues

- Tool text (submodule, not fixed here): the end of `--game` mode says "...Import libbbaot.so..., pick them, **and switch it on**" and `--help-player` says to import by hand; 2.0.0 has no switch and imports `aot/` by itself. `--game` also writes `libbbaot.so`/`.json` directly into `--out`, not into `--out/aot/` (the player has to make the `aot/` folder).
- The "Native code needed" page has no retry; it re-checked here when the app came back to the foreground (the surface was recreated). Fine, but not obvious to a player.

## Device state left

2.0.0-test.0179e37 installed, the player-built native code installed in `files/bbport/aot/` (also in the game folder's `aot/`), game stopped, settings and saves intact.
