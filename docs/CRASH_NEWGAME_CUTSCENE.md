# Crash at the New Game opening movie (2026-10-24)

Status: two separate crashes at the same movie, both fixed on `phase7` (device: AYN Thor, app 2.0.0-test):

1. **Mid-movie, only with frame generation on** (Lossless Scaling / Bloodframe): the game's main thread and its render
   thread write one GX command buffer at once. Fix: frame generation pauses while a movie plays (aaab4e2, "Fix 1" below).
2. **At the end of the movie, with or without frame generation** (found by the user after the first one): our AvPlayer
   HLE said the movie was inactive for a moment before its last frames were decoded, and the game's teardown destroyed
   mutexes its movie audio thread still used. Fix: `AvPlayerSource::IsActive` counts the packet a decoder holds (314ed21).

Earlier device: app 2.0.0-test.4c99d50.
Logs of every run below: `out/thor-backup-crash/logs/` (not committed). Saves and settings backed up in
`out/thor-backup-crash/device-saves.tar` before the first run; the save files were checked unchanged after the runs
(the crash comes before character creation writes anything).

## Symptoms (all at the opening movie, `/app0/dvdroot_ps4/movie/sprj_opening.mp4`, 2-18 s after New Game)

Different each run, all from the same corruption:

| run | build | symptom |
|---|---|---|
| user | 2.0.0 AOT | `Bad PM4 packet ... Unimplemented PM4 type 0` (7 zero dwords in a DCB) |
| r2, r6-r8, r11, r13, r14, r26, r28 | 2.0.0 AOT | guest fault at +0x26bc320 (`cmp byte [r9+0xd2]`, r9 = a null shader pointer read from a GX draw state) |
| r3 | 2.0.0 AOT | `[Dantelion2 Panic] DLRegularHeap.cpp(710) given memory block seems to be improper or freed already` |
| r5 | 2.0.0 AOT, UMA off | host fault in libvulkan_freedreno.so (address 0x150) |
| r10, r20 | 2.0.0 AOT | `SignalFence: Unreachable code`, `ExecuteCopyShaderHLE` assertion |
| r4 | **1.2.0 FEX** (stock release) | bad PM4 packet (zeros) |
| r15 | 1.2.0 FEX with `TSOEnabled=1` | bad PM4 packet |
| r16 | 1.2.0 FEX, frame-rate hooks off | bad PM4 packet |
| r9 | **0.11.1 FEX** | bad PM4 packet: a `SET_SH_REG COMPUTE_PGM_LO` header whose two values are missing, a draw packet in their place |
| r17 | **0.9.2 FEX** | guest fault, address 0xd2 (the null shader pointer) |

So it is **not an AOT / lowering bug**: the stock FEX runtimes 0.9.2, 0.11.1 and 1.2.0 crash the same way, and FEX with
TSO emulation on crashes too (hypotheses 1, ordering, and 4, a lowering bug, ruled out by these runs). Not the movie
decoder either: the AvPlayer HLE's allocations, frames and buffers are consistent (BB_AVPLAYER_LOG: two 1920x1088 NV12
texture buffers of 0x2fd000 bytes, four 32 KiB audio buffers, frames alternating between them).

Settings ruled out (each one changed alone, still crashed): skip_intro, cheats, UMA (unified memory) off, the GX command
heap patch, upscaler preset 0, the frame-rate hooks (BB_FRAME_HOOKS=0).

**What makes it go away: frame generation (Lossless Scaling / Bloodframe) off.** With `lsfg_multiplier=0` the movie plays
through to character creation (r21 vanilla timing, r22 frame-rate hooks on; and r39 on the fixed build). With the user's
x2 it crashes every time (20+ runs).

## Evidence: two threads write the same command buffer at once

The diagnostic build records every driver write into a guest command buffer (`BbGnmHooks::DriverWrite`: address, size,
first dword written, thread, and the guest frame-pointer backtrace) and prints the ones that landed in a bad buffer.
Run r19 (one example; r12, r14, r18, r23, r27 show the same):

```
Bad PM4 packet: guest buffer 0x1039780090 (packet at 0x1039780288), submitted by tid 12884 (GXRenderThread)
Driver write: 0x1039780698 (+0x608) 29 dwords, tid 12669 (bb-probe), 0.001 s ago; callers +0x10754ce +0x26c025d +0x2169730 +0x28ff215 +0x1fce437 ...
Driver write: 0x103978048c (+0x3fc) 40 dwords, tid 12884 (GXRenderThread), 0.001 s ago; callers +0x1075405 +0x26c031b +0x2169730 +0x2190cff ...
Driver write: 0x1039780358 (+0x2c8) 25 dwords, tid 12669 (bb-probe), 0.001 s ago; callers +0x1075885 +0x26cd5b1 +0x26b26be +0x28feb40 +0x1fce437 ...
Driver write: 0x10397802fc (+0x26c) 29 dwords, first c0001000, tid 12884 (GXRenderThread), 0.001 s ago; ...
Driver write: 0x10397800c4 (+0x34) 25 dwords, tid 12669 (bb-probe), 0.001 s ago; ...
```

Within one millisecond, in one 0x7ac-byte command buffer, the main thread (`bb-probe`, the game's main thread) and
GXRenderThread write alternately. GXRenderThread's `setVsShader` at +0x26c already had its first dword overwritten
(`first c0001000`, a NOP) when the driver call returned. r12 shows two `setCsShader` outputs overlapping, r9 (0.11.1) a
packet header without its body.

Who they are (static analysis of the 1.09 eboot):

- Main thread: a state-machine step (`+0x1fd09b0`, "State Finished.(No StepMethod is Executing") runs the movie step
  `+0x1fce3b0`, which does `sceKernelUsleep(20000)` (+0x1fce3fa), the movie object's update, then its `Draw`
  (vtable slot `+0x52e3870` = `+0x28fe7c0`): `sceAvPlayerGetVideoDataEx`, then an immediate GX draw (`+0x21696c0`) into
  the GX device's context `[[[+0x55406c8]+0x10]+8]`.
- GXRenderThread: its loop `+0x21b6910` waits for work, then runs the frame (`+0x1f62640`), which records into the same
  context `[[[+0x55406c8]+0x10]+8]`.

The game keeps them apart only by time: the movie step sleeps 20 ms, by then the render thread's frame (on the PS4 a few
ms of CPU work right after the main thread hands it over) is done. Nothing else orders them (no lock on the context:
`+0x21696c0` takes none).

## Why frame generation breaks it: the submission lock

GXRenderThread is held for a whole display interval in the middle of its frame:

```
GPU idle wait: 33.1 ms in tid 22672 (GXRenderThread), callers +0x26b0c28 +0x26afb01 +0x216a9f8 +0x21b4d9c ...
```

`+0x26b0c23` calls the eboot's `sceGnmSubmitCommandBuffers` wrapper (`+0x1089a10`) part way through the frame. The wait is
the upstream shadPS4 submission lock:

- `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2676-2678` (`sceGnmSubmitDone`): if the GPU is not idle,
  `submission_lock = true`;
- `gnmdriver.cpp:2589` / `2640` (`sceGnmSubmitAndFlipCommandBuffersForWorkload`,
  `sceGnmSubmitCommandBuffersForWorkload`) and `547` (`sceGnmDingDong`): `if (!SubmitLockOnDecode()) WaitGpuIdle();`
  before the submission;
- `gnmdriver.cpp:299` `WaitGpuIdle`: waits until the GPU-idle interrupt (`ResetSubmissionLock`) clears the lock;
  `SubmitLockOnDecode()` is false here because honest labels are off in UMA copy mode (`BB_GUEST_IN_PLACE=0`), so the
  decode-time lock (`WaitPreviousFrame`, `gnmdriver.cpp:268`) is not the one used.

"guest waited for the previous frame" in the frame report: ~31-32 ms per frame with x2 frame generation, 0.5 ms with it
off. With frame generation the GPU becomes idle only once the frame has gone through presentation (frame generation work
and the 60 Hz FIFO pacing), so the render thread's first submission of each frame waits about one display interval.
The render thread then runs behind; frames handed over meanwhile queue up, and it starts the next one right after
`sceGnmSubmitDone`. The main thread's 20 ms sleep ends inside that, and the two record into the context together.

Trace of one frame (BB_MOVIE_TRACE, run r30, ms):

```
60016.04 submitdone-exit GXRenderThread
60016.07 init            GXRenderThread   (next command buffer's default state)
60039.54 barrier         bb-probe         (movie step after its 20 ms sleep; render thread held in a submission wait)
60048.16 submit x4       GXRenderThread   (released ~32 ms after its previous frame ended)
60051.27 submitdone      GXRenderThread
```

## Fix attempts for crash 1

### 1. Movie-step barrier (call-site specific; dropped)

`sceKernelUsleep(20000)` from the movie step (return address +0x1fce3ff, code bytes checked) waited until the render
thread was quiet (held in a submission wait, or 2 ms past `sceGnmSubmitDone`), then held the render thread out of its
submissions for an 8 ms draw window. Crash-free (r38-1, r38-2, r39), but the user saw the movie at about 4 fps on the
device, and review (Fable) rejected it as fragile (one call site, per-submission locking for the whole game). Kept as
`out/thor-backup-crash/diag-src/barrier-version.diff`.

### 2. Skip the mid-frame submission wait while a movie plays (Fable's address-free fix; crashes)

`Libraries::AvPlayer::PlayingCount()` (sceAvPlayerStart .. Stop/Close); while it is non-zero, `WaitGpuIdle` before a
submission and `WaitPreviousFrameAtSubmit` return at once (the wait in `sceGnmSubmitDone` stays).

| variant | result |
|---|---|
| no submission waits for any thread (g1-1, g1-2) | guest fault at **+0x263b8b0** (the `mov rcx,[r14]` at +0x263b8e7), address **0x5300000000**, 6-9 s into the movie |
| the same, `sceGnmSubmitDone` waits for its own frame instead (g2-1, g2-2) | same fault, 3-6 s |
| only the `sceGnmSubmitDone` thread skips, others wait (g3-1, g3-2) | same fault, 9 s |

+0x263b8e7 is the free-list read that the submission lock was made for (gnmdriver.cpp comment on `SignalGpuIdle`:
"late fences in its heap (guest fault 0x263b8e7)"): a fence value (0x53 in the upper half) written into a block the
game had already freed. So the wait is what keeps the game from freeing a frame's fence memory while the GPU still
writes it; skipping it during the movie turns the command-buffer race into a heap race. The 7-dword hole was not an
early return of `sceGnmDrawIndexAuto` / `sceGnmInsertWaitFlipDone`: both now log a rejected call, and no run logged one.

### Discriminating tests (Fable's list)

| test | result |
|---|---|
| a. stock 2.0.0 runtime, frame generation x2, `BB_HONEST_LABELS=1 BB_SUBMIT_AHEAD=16` (decode-time lock, no mid-frame waits; waits only in `sceGnmSubmitDone`, 4.7 ms/frame) | crash, 3 s (t1) |
| b. stock 2.0.0 runtime, frame generation off | movie plays to character creation (t2; also r21, r22, r39, v0); "guest waited for the previous frame" 0.5-1.2 ms/frame, 2.0 submissions/frame during the movie |
| movie pacing, stock waits, frame generation off (v0) | `AvPlayer: 1215 video frames in 40.6 s (29.9 per second)` |
| c. `copy->tid` vs the writers | the submitting thread is GXRenderThread; the overlapping driver writes come from GXRenderThread and the main thread (above) |

Submissions per frame stay 2.0 during the movie (the movie does not add a submission, so the mid-frame wait is not
new with the movie; what is new is the main thread drawing into the render thread's context).

### 3. Decouple the guest's frame from frame generation's presentation (Bloodframe side; dropped)

Present a copy of the game's frame and complete the guest's flip only after presentation (a flip-done thread, a present
cap), so the submission lock no longer waited for the paced presents. It did not stop the crash on the device and
changed presentation for every frame; dropped on the user's decision. Saved as
`out/thor-backup-crash/diag-src/bloodframe-decouple.diff` (with the touched files beside it).

## Root cause 1 (mid-movie, frame generation on)

The race is the game's: the movie step's 20 ms sleep is its only ordering against GXRenderThread. Frame generation
exposes it: the GPU finishes a frame only once presentation lets the command processor through the flip (the present
pool is drained at 60 Hz, two presents per game frame), so the render thread's first submission of a frame waits
~32 ms (the submission lock) in the middle of its frame, and the main thread's movie draw lands in the same command
buffer. The wait cannot simply be skipped (attempt 2: fence memory freed early).

## Fix 1: frame generation pauses while a movie plays

`Libraries::AvPlayer::PlayingCount()` counts the players started and not stopped, closed or at the end of their stream
(it asks each listed player `IsActive`, so it never sticks above 0). While it is non-zero, `Presenter::PollLsfgState`
pauses frame generation: frames are presented as with frame generation off (no generated frames, `frames_cap` 0); the
LSFG context, its buffers and the swapchain stay as they are. It resumes 1.5 s after the last player ends (the game
tears the movie down meanwhile, also timed by sleeps). The log says `LSFG: paused while a movie plays` and
`LSFG: resumed (the movie ended)` once each. A 30 fps video gains nothing from generated frames. The in-game overlay
does not show a frame generation status (its FPS counter shows the presented rate: 30 during the movie, 60 again
after), so there is nothing that claims frame generation while paused. The first generated frame after resuming
interpolates from the last frame before the movie (one frame, at a scene cut).

## Root cause 2 (end of the movie, any setting)

The user's log (frame generation off, movie played to the end):

```
Runtime: lock of a destroyed mutex 0x101399cf20 in 'Mv_AudioPlayThread' (guest return 0x802188125)
Runtime:   destroyed by 'main' (guest return 0x8021880b9)
[Dantelion2 Panic] ...PthreadMutex.cpp(130) Logical Error. Invalid mutex.(EINVAL)
```

The game's movie class (vtable `+0x52e3800`; static analysis of the 1.09 eboot):

- `IsFinished` (`+0x28fe780`): not playing (no `StatePlay` event, or a `StateStop` event) or `!sceAvPlayerIsActive`.
- The movie step (`+0x1fce3b0`) moves on when `IsFinished`; the movie's teardown (`+0x1fce650`) then calls
  `UpdateEvents`, `Close` (`+0x28fe2e0`), `Shutdown` and deletes the object, **ignoring `Close`'s result**.
- `Close` does its work only `if (IsFinished())`: stop the audio component (`+0x29002d0`: state 1 -> 2, then yield
  until the audio thread writes 3 on leaving its loop), delete the thread, `sceAvPlayerClose`.
- The audio thread (`+0x29003f0`, Mv_AudioPlayThread): while its state is 1, `sceAvPlayerGetAudioData` and a blocking
  audio output (silence when there is no data, ~21 ms).
- The destructor (`+0x29001c0` for the audio component) destroys the state mutexes: the four mutexes destroyed 0.3 ms
  after the step ended, 18 ms before the audio thread came back from its output and locked one.

So `IsFinished` was true in the step and false again in `Close`: `sceAvPlayerIsActive` went false, then true.
`AvPlayerSource::IsActive` (same as shadPS4 upstream) was `!eof || packet queues || frame queues`; a packet the
decoder thread had taken and was still decoding was in none of them. At the end of a stream the decoder holds the last
packets while it waits for the buffer of the frame the game just took, so the player looked inactive for a moment.
Skipping the movie works because the game then calls `sceAvPlayerStop` first. Our mutex HLE matches the PS4 here
(FreeBSD libthr: a lock after destroy returns EINVAL); the bug is the inactive gap.

## Fix 2: a movie is active until its last frame is decoded

The decoder threads flag the packet they hold (set before taking it from the queue, cleared after its frames are
queued); `IsActive` reads packets, decoders, frames in that order, so one of the three always shows a packet in flight.
`BB_AVPLAYER_LOG=1` also logs `IsActive(...) = false`.

## Test results (Thor, 2.0.0-test, native code rebuilt with tool ab25461, user settings: preset 4, fps_limit 30, GSR 2)

| build / setting | run | result |
|---|---|---|
| 314ed21 (fix 2 only), frame generation off | New Game movie to the end, 4 runs | all reach character creation; `IsActive = false` once, then `Close`; `AvPlayer: 1215 video frames in 43.0 s (28.2 per second)` |
| 314ed21, frame generation off | skipped after 8 s (Options) | `Stop`, `Close`, `307 video frames in 10.4 s`; character creation |
| 314ed21, frame generation x2 (no pause) | New Game movie, 3 runs | **3 of 3 crashed**: guest fault +0x26bc320 (address 0xd2) twice, Bad PM4 (`PM4 type 0`, a GXRenderThread buffer) once ("guest waited for the previous frame" 31.7 ms/frame) |
| aaab4e2 (both fixes), x2 | Continue on the user's save, ~3 min in game | frame generation active (60 shown, ~28.5 game FPS), no pause logged, no errors |
| aaab4e2, x2 | New Game movie with the pause, title movie | pending: the user runs these on the device |

Logs: `out/thor-backup-lsfgpause/logs/` (not committed).
