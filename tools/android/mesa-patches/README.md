# Turnip patches

`build_turnip.sh` applies these to Mesa (pinned commit in the script) in name order.

They come unchanged from fexdroid (https://github.com/cobrabm12/fexdroid, `patches/mesa`, MIT),
whose Mesa 26.2.3 build the 0.1 release shipped:

- `0001`: KGSL patch-id wildcard for the Adreno 840 (not used on the Thor's 740).
- `0005`: the X11 software present path waits for the GPU on its present thread.
- `0006`: KGSL GPU power constraint from the environment.
- `0007`: frames go to the app directly (`FEXDROID_PRESENT`) instead of through Xvfb. The
  frontend hides "Starting Bloodborne…" on the first direct frame.

fexdroid's `0002`–`0004` are already in Mesa upstream. Mesa 26.2.3 lacked e984ef29 ("tu/kgsl:
Prevent indefinite wait times for 0 timeout waits", 2026-10-08): KGSL turned zero-timeout polls
into infinite waits, so each queue submission waited for the GPU (about 19 FPS in gameplay).
The pinned commit includes it.

## Adreno 8xx variant (`build_turnip.sh gen8`)

Built from whitebelyash's `turnip/gen8` branch (https://github.com/whitebelyash/mesa-unified,
pinned in the script): Mesa main of 2026-09-18 plus Adreno 8xx work (A8xx family configs, FDM on
825/829/830, concurrent binning off). It predates the KGSL fixes, so `gen8/` adds four upstream
Mesa commits first: b889ab52, b7ad24ad, e984ef29 (the zero-timeout fix) and 6cb65e61. Then the
fexdroid patches above. `run-thor.sh` loads it when `kgsl-chip-id` reports an Adreno 8xx
(`0x44…`, e.g. the Snapdragon 8 Elite's 830); `BB_TURNIP=main|gen8` overrides that.
