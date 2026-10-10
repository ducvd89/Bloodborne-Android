# Lossless Scaling frame generation on Android (experimental)

The app includes [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) 1.0.0 (MIT) with patches for Turnip, which runs Lossless Scaling's frame generation as a Vulkan layer. Lossless Scaling itself is not included: it needs your own copy (Steam, app 993090).

1. Install Lossless Scaling on a PC through Steam and copy `Lossless.dll` from its folder (`steamapps/common/Lossless Scaling/Lossless.dll`).
2. Put it next to your game folder on the phone, e.g. `/sdcard/Bloodborne/Lossless.dll` beside `/sdcard/Bloodborne/CUSA03173`.
3. In the menu open **Lossless Scaling frame generation** and pick 2×, 3× or 4×. Lower flow scale and performance mode are faster. Changes apply while you play.

It works with any upscaler and replaces FSR 3.1 frame generation. It sees only finished frames, without the game's motion vectors, so expect more artifacts around fast motion and the HUD than with FSR 3.1 frame generation. Its messages (`lsfg-vk: ...`) are in `files/bbport/logs/android-game.log`; `export LSFG_STATS=1` in `bbport/thor-local.sh` logs real and generated frames a second.

## Pacing

lsfg-vk presents the generated frames right after the real one and relies on FIFO presentation to space them, which is even only when the display runs at FPS limit × frames. While frame generation is on, the app picks that display mode when the panel has it (the Thor: 30 FPS × 2 → 60 Hz; 30 × 4, 40 × 3 or 60 × 2 → 120 Hz), else the fastest one. At the system's default 60 Hz, 45 real + 45 generated frames a second showed only 60 on screen.

Measured on the AYN Thor (Adreno 740), title screen, 2×, flow scale 0.75, performance mode: 45 real + 45 generated frames a second with the GPU 23% busy at 401 MHz; with a 30 FPS limit at 60 Hz nearly every frame was shown 16.8 ms apart.

## Why 1.0 with patches

- lsfg-vk runs the frame generation on its own Vulkan device and shares images and semaphores with the game's. Upstream shares semaphores as OPAQUE_FD, which Turnip's KGSL backend cannot export (only sync files). lsfg-vk 2.0 additionally shares timeline semaphores between the devices, which KGSL cannot do at all, and its CC BY-NC-ND 4.0 licence allows no modified builds.
- GameNative's 1.0 fork (Ragnarok93/lsfg-vk-android) solves this with AHardwareBuffers, but that path is bionic-only: this game runs in a glibc process with a glibc Turnip, which has neither.
- `tools/android/lsfg-patches/0001-turnip-kgsl-sync-fd.patch`: the game side exports the frame-ready semaphore as a sync file after its copy is submitted, the framegen side exports each generated frame's semaphore the same way, and each side imports the other's temporarily. The shared images are created with the same usage on both devices, since Turnip derives tiling and UBWC from it. The layer disables itself instead of exiting the game when it cannot write its `/tmp` file (Android has none; it uses `TMPDIR`) or read the DLL, and `LSFG_STATS=1` logs frame rates.

Building: `tools/android/build_lsfg.sh` compiles lsfg-vk v1.0.0 (pinned commit) with the patches in `tools/android/lsfg-patches/`.
