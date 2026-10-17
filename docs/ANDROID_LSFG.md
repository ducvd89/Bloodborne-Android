# Lossless Scaling frame generation on Android

The game runs as a native Android (Bionic) process, so Lossless Scaling's frame generation is no longer a Vulkan layer: this port's engine is **Bloodframe**, `libbloodframe.so`. Bloodframe is based on lsfg-vk 1.0 by PancakeTAS (MIT) via [GameNative's Android fork of it](https://github.com/Ragnarok93/lsfg-vk-android) (MIT). It lives in its own repository, [ducvd89/bloodframe](https://github.com/ducvd89/bloodframe) (private for now), which continues the fork's history; the game build fetches it at a pinned commit (`tools/android/build_bloodframe.sh`; the fork's licence ships with the app as `bloodframe-lsfg-vk.LICENSE.md`). Its small C API is `tools/android/lsfg/bb_lsfg.h` (the game's copy, checked against the pinned commit's). libbbgpu loads it with `dlopen` only when the multiplier is 2 or more. Lossless Scaling itself is not included: it needs your own copy (Steam, app 993090).

## Using it

1. Menu → **Frame generation** (right under Graphics settings): 2×, 3× or 4×, flow scale (0.25–1.0: lower is faster and the motion coarser), performance mode (the light model) and **Half precision (FP16, faster)** (on by default, see below).
2. **Sign in with Steam** downloads just `Lossless.dll` from your own copy (QR code or name/password; nothing but the file is kept), or **Choose Lossless.dll…** copies one from your storage (a PC copy is `steamapps/common/Lossless Scaling/Lossless.dll`). The file lives in `files/bbport/Lossless.dll`; `/sdcard/Bloodborne/Lossless.dll` is used when there is no such copy.
3. Apply: it takes effect at once, while you play (no restart). Without a Lossless.dll the game logs one line (`Lossless Scaling frame generation: no Lossless.dll`) and runs without it.

It replaces FSR 3.1 frame generation (FSR 3.1 frame generation stays in Graphics settings, which needs a restart; the two menus lock each other while one is on). It sees only finished frames, without the game's motion vectors, so expect more artifacts around fast motion and the HUD than with FSR 3.1 frame generation. Messages are in `files/bbport/logs/android-game.log` (`LSFG: ...` for the state, `Bloodframe: ...` for the engine); `BB_LSFG_STATS=1` in `bbport/bionic-local.sh` logs real and generated frames a second, with the shader set in use (`FP16`, `FP32` or `FP32-DXBC`).

## Half precision (FP16) and the shader sets

A recent Lossless.dll carries every frame generation shader three times, as resources of its RCDATA: the DXBC set (resource id N), a half-precision SPIR-V set (N + 49) and a full-precision SPIR-V set (N + 98). Bloodframe runs the dll's own SPIR-V whenever it can and translates the DXBC set (the fork's DXBC to SPIR-V translation) only as the last resort. The three sets, in the order they are tried:

1. **FP16 SPIR-V** (N + 49): the setting is on, the dll has the set and the GPU driver has `shaderFloat16` (the framegen device then enables it).
2. **FP32 SPIR-V** (N + 98): the setting is off, or anything above fails.
3. **DXBC (translated)**: an old Lossless.dll without the SPIR-V sets, a module the build was not checked against, or a driver that rejects the SPIR-V sets.

Both SPIR-V sets are used as the dll has them; only the bindings are renumbered, from the dll's per-class numbers 0, 16, 32, 48 to consecutive ones, the order the framegen's descriptor layouts use. The modules may use only these capabilities beyond what the framegen device already requires: `Float16` for the FP16 set alone (the FP32 set must not need it), no 16-bit storage, no `Int16`, no extensions; a module that needs more, or a missing one, drops that set. Checked on the PC against a real dll: all 52 modules of each SPIR-V set (26 shaders in each of the two models) are there, pass `spirv-val`, match the framegen's descriptor layouts exactly after the renumbering and build as compute pipelines (RADV, NVIDIA). The FP32 set uses `Shader`, `StorageImageExtendedFormats`, `ImageQuery` and `StorageImageWriteWithoutFormat`; the FP16 set the same plus `Float16`.

A set is dropped for the whole run of the game, with a log line `Bloodframe: shaders FP16 SPIR-V | FP32 SPIR-V | DXBC (translated) (reason)` whenever the set in use changes (the engine's own line goes to logcat as `Bloodframe: shaders ...`). The reasons:

- FP16 is off in the setting (the FP32 SPIR-V runs);
- the Lossless.dll has no such set (an old version), or a module the build was not checked against;
- the GPU driver has no `shaderFloat16` (for FP16);
- the driver rejects a shader or pipeline of the set when the context is built (the first frames): the same call builds everything again with the next set.

The setting is live like the others: a change starts frame generation again (a moment of the game paused, as for the flow scale). The state file has a fourth line, `fp16=1|0`. The stats line prints `FP16`, `FP32` (the FP32 SPIR-V) or `FP32-DXBC`.

## Adreno 8xx ghosting: fixed in 0.10.0

Up to 0.9.5, on Adreno 8xx (Oppo Find N5, Adreno 830) about 10-40% of the generated frames showed the picture doubled and shifted by ~50 px, as if a bogus smooth motion field warped the whole frame. Real frames, frame generation off, and the AYN Thor's Adreno 740 were clean. A host harness that ran the framegen on one static frame reproduced it (151 of 198 frames wrong on the N5), and the investigation (paused on 2026-10-10) ruled out: live config reloads, flow scale, performance mode, UBWC, intra-device sync (`TU_DEBUG=syncdraw`, `flushall`), ir3 optimisations, the gen8 vs main Turnip, early sync-file exports, the NaN/inf behaviour of the translated min/max/log2/exp2/fma, and the display scaling.

With Bloodframe in 0.10.0 the ghosting is gone on the Find N5 (user report, 2026-10-17, with the defaults: 2x, performance mode, FP16 on). Turning FP16 **off** there (user report, 2026-10-17) brought the ghosting back: with FP16 on the shaders are Lossless Scaling's own SPIR-V from the dll, with it off 0.10.0 still ran our DXBC to SPIR-V translation, which the earlier passes (mipmaps, beta[4]: 1024-thread workgroups with shared memory) went through. So the translation was the cause, not the 0.10.0 presentation changes.

The next version therefore runs the dll's own full-precision SPIR-V (N + 98) for FP32 too, and translates DXBC only as the last resort (section above). **Not yet tested on the Find N5**: whether FP32 with the dll's SPIR-V is clean there still needs a run with FP16 off, 2x, both models if possible.

## How a frame goes (gpu/shadps4/video_core/renderer_vulkan/vk_lsfg.cpp, vk_presenter.cpp)

For every real game frame, on the presenter's thread:

1. The final picture (before the overlay) is blitted into one of two input AHardwareBuffers (alternating) in a submission of its own; its semaphore is exported as a sync file.
2. `bb_lsfg_present` gives that sync file to the framegen, which runs on its own `VkDevice` of the same driver and physical device (made through the game's own `vkGetInstanceProcAddr`, so on the same Turnip), and makes the generated frames into output buffers. The call returns a sync file per generated frame and one for "done with the input buffers", which the next frame's copy waits for.
3. Each generated frame and then the real frame are blitted into swapchain buffers (waiting on the sync files on the GPU) and queued at once; only the hand-over to the app is spaced evenly over the real frame interval (`BB_LSFG_PACE=0` hands them over back to back).

Buffers change devices with queue-family ownership transfers to `VK_QUEUE_FAMILY_EXTERNAL`, as the fork's layer does. If a sync file cannot be exported or imported the call waits on the CPU (`BB_LSFG_SYNC=host` forces that); any other failure turns frame generation off and the game goes on with its own frames. The first four frames only fill the framegen's history.

## Latency

Nothing on the present thread waits for the GPU (`BB_LSFG_CHAIN=1`, the default): the copy of frame N waits on the GPU for the framegen's "done with the inputs" of frame N − 1, and its own signal, the sync file the framegen waits on before batch N, also covers (by submission order on the game's one queue) the presents of cycle N − 1 that read the output buffers batch N writes again. Each kind of semaphore is a ring of four, reused only once the submission that used it completed (a tick of the present scheduler; it never waits in practice). Until 0.9.5 the present thread waited at every frame for the last cycle's presents and copy: once the GPU was behind (a heavy scene, a large flow scale or the quality model) that wait plus the pacing sleep took the whole frame interval, the present thread became the bottleneck and up to 7 finished frames queued for it (the presenter's pool), ~200 ms late on screen. `BB_LSFG_CHAIN=0` brings those waits back.

While frame generation runs, the GPU command thread also waits while a game frame is out (`BB_LSFG_FRAMES`, 1 by default; 0: only the presenter's pool of 9 bounds them): the next frame's display buffer is made only once the last one was handed to the app, so the game is held back instead of queueing finished frames. It matters where the FPS limit holds the game back (the title screen at 60: the real frame complete 28 ms after its flip with 1, 44 ms with 2, 34 ms with frame generation off, where the present-side limiter keeps two frames waiting); where the GPU is the limit it changes nothing (27.4–30.2 FPS and the same latency with 1 or 2).

`BB_LSFG_LATENCY=1` (in `bionic-local.sh`) logs every 5 s, per mode, the mean and 95th percentile of each step of a game frame in ms after its flip reached the video out driver: the game's submission (negative), the present thread's start, the GPU done with the frame, the copy into the framegen's input (recorded, done), the framegen done, the first generated frame handed to the app and complete, the real frame handed to the app and complete, and how many frames waited for the present thread (`vk_frame_latency.h`). A file `lsfg.state.tune` next to the state file (`chain=0|1 pace=0|1 frames=N`) overrides those switches for every new frame generation (every change of the state file makes one), for comparisons in one place of the game.

Measured on the AYN Thor (Adreno 740, Turnip 26.3), standing still in Cathedral Ward, x2, ms after the flip (mean):

| | real FPS | present thread queue | GPU done | real frame complete |
|---|---|---|---|---|
| off | 34.6 | 0 | 21.7 | 23.0 |
| performance, flow 0.75, FP16, before | 30.4 | 0 | 22.5 | 30.6 |
| performance, flow 0.75, FP16, now | 30.2 | 0 | 22.5 | 30.8 |
| quality, flow 1.0, FP32, before | 28.0 | 5–7 frames | 183–222 | 235–274 |
| quality, flow 1.0, FP32, now | 27.8 | 0 | 45 | 61 |

What frame generation still adds is its own: the copy (~1.5 ms), the framegen on the GPU (5 ms in performance mode at flow 0.75 with FP16, 12 ms in the quality model at 1.0 with FP32), and the real frame shown after the generated one (at least one refresh); its GPU time also lowers the real frame rate (34.6 → 30 FPS there), and the framegen of frame N − 1 runs on the GPU before frame N.

## Pacing

Generated frames only look right when each is shown for one refresh. With frame generation on, libbbgpu asks the app for FIFO presentation whatever the VSync setting says, and the app picks the display mode of FPS limit × multiplier when the panel has it (30 × 2: 60 Hz; 30 × 4, 40 × 3 or 60 × 2: 120 Hz), else the fastest one. The presentation needs the multiplier plus three swapchain buffers.

### Live changes

The Frame generation menu writes `bbport/lsfg.state` (`multiplier=`, `flow=`, `performance=`, `fp16=`; whole file, then a rename) next to `bbport.ini`; `run-bionic.sh` writes the same file at start from `bbport.ini` and names it in `BB_LSFG_STATE_FILE`. It is the one source of truth (`BB_LSFG_MULTIPLIER` and friends (`BB_LSFG_FP16` too) are only read when there is no state file, for harnesses). The presenter looks at it at most every 250 ms, between two frames, and applies a change there and then:

- The framegen fixes its flow scale, model, precision and generated-frame count when it starts, so every change of an on state is a new `Lsfg`: the old one waits for its last batch and the game device, destroys its context, buffers and runtime (`libbloodframe.so` stays loaded), the new one starts and makes its context with the next real frame (the first four frames only fill the history). Starting takes about as long as at launch (reading the shaders from Lossless.dll): the game pauses for that moment.
- Off: the same teardown, then normal presents.
- The swapchain buffers follow what runs (multiplier + 3, else 3) with a RESET and new BUFFERs, as for a resize.
- The presentation mode is FIFO while frame generation runs or VSync is on. The game tells the app with a `BB_PRESENT_MODE` message (protocol version 3); the app, going FIFO → newest-wins, shows the newest queued frame and releases the older ones unseen with their acquire fences.
- One log line per change: `LSFG: now x3, flow 0.75, performance 1` or `LSFG: off` (the reason follows from `LSFG: off (...)` when it cannot start: no Lossless.dll, FSR 3.1 frame generation on, ...).

The game's frame rate no longer depends on frame generation: hooks in the game's code (installed when it loads) give it each frame's real time at every FPS limit, 30 included, and the present thread limits the rate, so Lossless Scaling can be turned on and off live at any limit. With `BB_FRAME_HOOKS=0` (in `bionic-local.sh`) the old behaviour returns: at the 30 limit the delta-time patch loads only when frame generation is on at start.

## What Bloodframe changes from the fork (the bloodframe repository's commits after the fork's)

- the framegen's Vulkan instance comes from the entry point the game process gives (`volkInitializeCustom`), not volk's own loader (the process loads its driver directly: no Android loader, no layers).
- the shaders are read from the one Lossless.dll the host names, not from a search of Steam folders (that removes the fork's config-file dependency).
- the extractor hands out the dll's resources and the shader names (for the half-precision set, `android/bb_half.cpp` in the bloodframe repository).
- the framegen takes SPIR-V 1.0 modules unchanged (the half-precision modules are 1.0; the fork's compatibility check wanted 1.4 and up).
- the framegen device enables `shaderFloat16` only when the host asks and the GPU has it.

The fork's layer, adaptive scheduler and config parser are not built. `tools/android/build_lsfg.sh` and `tools/android/lsfg-patches` are the old glibc layer build, kept for reference.

## Not verified on a device

Verified on devices since: half precision runs on Turnip on the AYN Thor (Adreno 740) and the Oppo Find N5 (Adreno 830), and fixed the Find N5's ghosting (above); the 0.10.0 latency changes were measured on the Thor (section Latency). Still unverified: Adreno 6xx and Mali, the picture with FP16 on vs off side by side, and the FP32 SPIR-V set on a device at all (the translated DXBC FP32 path ghosted on Adreno 8xx; the new one runs only on the PC so far, where it builds and validates).
