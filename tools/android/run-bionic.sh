#!/system/bin/sh
# Runs inside the app's sandbox (started by MainActivity.java with /system/bin/sh): the game process,
# bb-probe with libbbgpu/libbbcpu (an Android NDK build), from the app's data directory. It uses the
# phone's own Vulkan driver, or an adrenotools-style one (files/drivers/<name>/: the app's bundled driver for
# the GPU, picked automatically, or one the player installed).
# The game files are prepared by bb-prepare, the mods overlay is built by the app (BB_APP0_DIR): no
# Python, no rootfs, no X server. libbbgpu presents through the app (BB_PRESENT_SOCKET).
# Only Android's own tools run here (toybox: sed, tail, cat, cp, mkdir, printf, rm, taskset).
# usage: run-bionic.sh [game|vulkan]   env from the app: BB_SCREEN_W/BB_SCREEN_H (the surface's size),
#        BB_GAME_DIR (the chosen game folder), BB_APP0_DIR (the folder to run from: the game with mods),
#        BB_ANDROID_INPUT=1
set -eu
files=${FXD_FILES:-/data/data/com.ducvd89.bloodborne/files}
base="$files/bbport"
bionic="$base/bionic"
export FXD_FILES="$files"
export PATH=/system/bin
export HOME="$files/home/bbport"
export TMPDIR="$files/tmp" XDG_RUNTIME_DIR="$files/tmp"
# FEXCore (libbbcpu) reads its Config.json from here (it runs only the guest's code: no x86 rootfs,
# loader or syscalls of its own).
export FEX_APP_CONFIG_LOCATION="$HOME/.fex-emu/"
export FEX_APP_DATA_LOCATION="$HOME/.fex-emu/"
export FEX_APP_CACHE_LOCATION="$HOME/.cache/fex-emu/"
mkdir -p "$HOME/.fex-emu" "$HOME/.cache/fex-emu" "$TMPDIR" "$base/user" "$base/logs" "$base/logs/failed-shaders"

# ---- Settings: bbport.ini (the app's settings screen writes it) turned into BB_* variables ----
export BB_CONFIG="$base/bbport.ini"
[ -f "$BB_CONFIG" ] || cp "$base/bbport.default.ini" "$BB_CONFIG"
setting() { sed -n "s/^$1=//p" "$BB_CONFIG" 2>/dev/null | tail -n 1; }
# The app's pad state file (PadBridge.java) and text requests (the character name: its touch keyboard).
export BB_ANDROID_INPUT=1
export BB_PAD_FILE="$base/android-pad.state" BB_PAD_QUIET=1
export BB_IME_FILE="$base/ime.request"
# ---- Aspect ratio, output and scene size ----
# aspect_ratio (auto, 4:3, 16:10, 16:9, 21:9, 32:9; auto when missing, 16:9 when unknown) is the display's: the app
# sizes the game's surface to it and passes the surface's pixel size as BB_SCREEN_W/BB_SCREEN_H; auto is that
# surface's own ratio (any, e.g. 20:9; else display_size, else 16:9). output_res (WxH, any even size; native:
# the surface's size, else display_size fitted to the aspect ratio; 1080p when missing) is the size of the final
# frame and the UI; when its ratio is not the aspect ratio's, the height is kept and the width follows it.
# The preset sets the scene size relative to the output, FSR 3.1 upscales it to the output.
# Another ratio than 16:9 is shown by hooks in the game's code (libbbgpu: bbport_display.cpp, from the output's
# ratio in BB_OUTPUT_RES): the camera, the UI's viewport, the HUD's markers and groups; and the scene is
# drawn at the output's ratio by the resolution patch (any size: its immediates are written by the runtime).
upscaler=$(setting upscaler); preset=$(setting preset); output_setting=$(setting output_res)
aspect_setting=$(setting aspect_ratio); display_setting=$(setting display_size)
# Defaults when missing: aspect ratio auto, output 1080p (the height kept, the width follows the aspect ratio below).
[ -n "$aspect_setting" ] || aspect_setting=auto
[ -n "$output_setting" ] || output_setting=1920x1080
# size_ok WxH: size_w, size_h (positive, rounded down to even, at most 16384) or fails.
size_ok() {
    case $1 in *x*) ;; *) return 1 ;; esac
    size_w=${1%%x*}; size_h=${1#*x}
    case $size_w in ''|*[!0-9]*) return 1 ;; esac
    case $size_h in ''|*[!0-9]*) return 1 ;; esac
    [ ${#size_w} -le 5 ] && [ ${#size_h} -le 5 ] || return 1
    size_w=$((10#$size_w / 2 * 2)); size_h=$((10#$size_h / 2 * 2))
    [ "$size_w" -ge 2 ] && [ "$size_h" -ge 2 ] && [ "$size_w" -le 16384 ] && [ "$size_h" -le 16384 ]
}
screen_w=0; screen_h=0; display_w=0; display_h=0
if size_ok "${BB_SCREEN_W:-}x${BB_SCREEN_H:-}"; then screen_w=$size_w; screen_h=$size_h; fi
if size_ok "$display_setting"; then display_w=$size_w; display_h=$size_h; fi
case $aspect_setting in
    4:3) an=4; ad=3 ;; 16:10) an=16; ad=10 ;; 21:9) an=21; ad=9 ;; 32:9) an=32; ad=9 ;;
    auto)
        if [ "$screen_w" != 0 ]; then an=$screen_w; ad=$screen_h
        elif [ "$display_w" != 0 ]; then an=$display_w; ad=$display_h
        else an=16; ad=9; fi ;;
    *) aspect_setting=16:9; an=16; ad=9 ;;
esac
output_w=0
case $output_setting in
    native)
        if [ "$screen_w" != 0 ]; then output_w=$screen_w; output_h=$screen_h
        elif [ "$display_w" != 0 ]; then
            # The display fitted to the aspect ratio (the largest size of that ratio inside it).
            if [ $((display_w * ad)) -ge $((display_h * an)) ]; then
                output_h=$display_h; output_w=$((display_h * an / ad / 2 * 2))
            else
                output_w=$display_w; output_h=$((display_w * ad / an / 2 * 2))
            fi
        fi ;;
    *) if size_ok "$output_setting"; then output_w=$size_w; output_h=$size_h; fi ;;
esac
if [ "$output_w" = 0 ] || [ "$output_h" = 0 ]; then output_w=1280; output_h=720; fi
# Not the aspect ratio's ratio (more than 1% off): the height is kept.
if [ $(( (output_w * ad - output_h * an) * 100 )) -gt $((output_h * an)) ] ||
   [ $(( (output_h * an - output_w * ad) * 100 )) -gt $((output_h * an)) ]; then
    output_w=$(( (output_h * an + ad) / (2 * ad) * 2 ))
fi
output=${output_w}x${output_h}
echo "Display: aspect ratio $aspect_setting ($an:$ad), output $output (surface ${BB_SCREEN_W:-?}x${BB_SCREEN_H:-?}, display $display_setting)"
# FSR 4 (INT8 models) is not offered: on the Adreno 740 it took ~3 min to compile in Turnip, then ran
# at 2.4 FPS with a black scene. off, fsr3 and gsr2 as they are; none means GSR 2 (the default since
# 0.11.0); any other value means FSR 3.1.
case $upscaler in off|gsr2|fsr3) ;; '') upscaler=gsr2 ;; *) upscaler=fsr3 ;; esac
echo "Upscaler setting: $upscaler"
case $preset in 0|1|2|3|4) ;; *) preset=3 ;; esac
# The scene size: the output / the preset's scale (1, 1.5, 1.7, 2, 3), each side rounded to even, as
# patches.py (scaled_sizes) does; any size, any ratio.
scale10=10
if [ "$upscaler" != off ]; then
    case $preset in 1) scale10=15 ;; 2) scale10=17 ;; 3) scale10=20 ;; 4) scale10=30 ;; esac
fi
scene_w=$(( (output_w * 10 + scale10) / (2 * scale10) * 2 )); scene_h=$(( (output_h * 10 + scale10) / (2 * scale10) * 2 ))
[ "$scene_w" -ge 2 ] || scene_w=2
[ "$scene_h" -ge 2 ] || scene_h=2
render=${scene_w}x${scene_h}
# A scene of the game's own 1920x1080 (1080p Native, or 1080p without the upscaler) needs no resolution
# patch and no BB_RENDER_RES: the renderer then takes the game's targets as they are (with
# BB_RENDER_RES it tells the scene from the 1920x1080 UI by its size).
if [ "$render" = 1920x1080 ]; then render=; preset=0; fi
# A scene larger than 1080p (pixels) needs the larger graphics heaps (the patch's own 'above 1080p') and
# direct memory (the PC build uses 9152 MB; 5056 is enough up to 1080p): that is most of the phone's RAM.
dmem_mb=5056; big_scene=0
if [ $((scene_w * scene_h)) -gt $((1920 * 1080)) ]; then big_scene=1; dmem_mb=${BB_DMEM_MB_HIGH:-9152}; fi
echo "Scene ${render:-1920x1080} for $output at preset $preset$([ "$big_scene" = 1 ] && echo "; larger than 1080p: $dmem_mb MB direct memory")"
export BB_FULLSCREEN=1 BB_UPSCALER=$upscaler BB_UPSCALE_PRESET=$preset
# FSR 3.1 frame generation (frame_generation=1): a frame between each two, with FSR 3.1 only.
[ "$(setting frame_generation)" = 1 ] && [ "$upscaler" = fsr3 ] && export BB_FRAME_GEN=1
# Lossless Scaling frame generation (lsfg_multiplier 2-4): libbbgpu loads libbloodframe.so (tools/android/lsfg:
# Bloodframe, the framegen of GameNative's lsfg-vk fork, on the game's own Vulkan driver) which reads its shaders from
# the player's own Lossless.dll: bbport/Lossless.dll (the app's Steam sign-in or file picker), else the
# Bloodborne folder of shared storage. It replaces FSR 3.1 frame generation. Generated frames only look
# right when each is shown for one refresh: while it runs libbbgpu asks the app for its FIFO (VSync), and
# the app sets the display to FPS limit x multiplier (MainActivity.pickRefreshRate).
# It is LIVE: the app's Frame generation menu rewrites bbport/lsfg.state (BB_LSFG_STATE_FILE) while the
# game runs and libbbgpu follows it (turns it on, off, or to another multiplier / flow scale / model with
# no restart). That file is the one source of truth for the four settings; here is only its start,
# written from bbport.ini (like cheats.state), so no BB_LSFG_MULTIPLIER/FLOW_SCALE/PERFORMANCE/FP16 are exported.
# BB_LSFG_DLL is exported when a Lossless.dll is found; libbbgpu looks again when frame generation is
# turned on (the player may sign in to Steam meanwhile).
# What cannot follow it live is decided here, from the multiplier at start: that FSR 3.1 frame
# generation is off (it is a setting of the upscaler: exclusive with this) and the 30 FPS limit's
# frame rate patches (below: patches load when the game starts).
lsfg_requested=$(setting lsfg_multiplier)
case $lsfg_requested in 2|3|4) ;; *) lsfg_requested=0 ;; esac
lsfg=$lsfg_requested
[ "$lsfg" = 0 ] && lsfg=
lsfg_dll=${BB_LSFG_DLL:-}
if [ ! -f "$lsfg_dll" ]; then
    lsfg_dll=
    for dll in "$base/Lossless.dll" /storage/emulated/0/Bloodborne/Lossless.dll; do
        [ -f "$dll" ] && { lsfg_dll=$dll; break; }
    done
fi
unset BB_LSFG_DLL BB_LSFG_MULTIPLIER BB_LSFG_FLOW_SCALE BB_LSFG_PERFORMANCE BB_LSFG_FP16
[ -n "$lsfg_dll" ] && export BB_LSFG_DLL=$lsfg_dll
flow=$(setting lsfg_flow_scale)
case $flow in 0.25|0.[3-9]|0.[3-9]0|0.[3-9]5|1.00|1.0|1) ;; *) flow=0.75 ;; esac
performance=1; [ "$(setting lsfg_performance)" = 0 ] && performance=0
fp16=1; [ "$(setting lsfg_fp16)" = 0 ] && fp16=0
export BB_LSFG_STATE_FILE="$base/lsfg.state"
printf 'multiplier=%s\nflow=%s\nperformance=%s\nfp16=%s\n' "$lsfg_requested" "$flow" "$performance" "$fp16" \
    > "$BB_LSFG_STATE_FILE.tmp" && mv "$BB_LSFG_STATE_FILE.tmp" "$BB_LSFG_STATE_FILE"
if [ -n "$lsfg" ]; then
    if [ -z "$lsfg_dll" ]; then
        echo "Lossless Scaling frame generation: no Lossless.dll ($base/Lossless.dll or /storage/emulated/0/Bloodborne/Lossless.dll): off"
        lsfg=
    elif [ ! -f "$bionic/lib/libbloodframe.so" ]; then
        echo "Lossless Scaling frame generation: $bionic/lib/libbloodframe.so is missing: off"
        lsfg=
    else
        unset BB_FRAME_GEN
        echo "Lossless Scaling frame generation (Bloodframe): x$lsfg, flow scale $flow, performance mode $performance, half precision $fp16, $lsfg_dll (live: $BB_LSFG_STATE_FILE)"
    fi
else
    echo "Lossless Scaling frame generation: off at start (the app's Frame generation menu turns it on while you play)"
fi
# The GPU's check of indirect dispatch counts (vk_indirect_guard.h) is off; BB_INDIRECT_GUARD=1 in
# bionic-local.sh turns it on.
export BB_INDIRECT_GUARD=${BB_INDIRECT_GUARD:-0}
# The two-stage draw pipeline (bb:DrawRec) frees the GPU command thread: 31 -> 34 FPS on the Thor
# with all cores. BB_DRAW_PIPE=0 in bionic-local.sh turns it off.
export BB_DRAW_PIPE=${BB_DRAW_PIPE:-1}
export BB_WINDOW_SIZE=$output
export BB_OUTPUT_RES=$output BB_LIVE_RES=0
if [ -n "$render" ]; then export BB_RENDER_RES=$render; else unset BB_RENDER_RES; fi
# Unified memory v2 (experimental, off by default; Debug options -> Unified memory, bbport.ini unified_memory=1,
# or BB_UMA=1 in the environment): the game's buffers (index and vertex buffers first; BB_UMA_CLASSES) live in
# dma-buf granules (BB_UMA_GRANULE_KB, 2048) the GPU binds in place through the sparse arena, instead of the game's
# copy plus a GPU copy; textures and every other copy stay CPU snapshots (docs/ANDROID_UNIFIED_MEMORY_PLAN.md).
# libbbgpu probes the driver (coherence, sparse binding of shared memory...) and falls back to copies by itself,
# with one logged reason (Mali/PanVK always, a failed probe...). Writes by the game are caught by page protection
# (BB_WRITE_TRACKING); BB_UMA_VERIFY=N reports data that changed while the GPU used it. BB_UNIFIED=1 (v1: whole
# allocations through the layer memory module) stays for reference, only from the environment.
# BB_UMA_DMA_GPU=1: DMA fills and copies into shared granules go to the GPU again (default: the CPU, as copy mode).
if [ -z "${BB_UMA:-}" ] && [ "$(setting unified_memory)" = 1 ]; then BB_UMA=1; fi
if [ "${BB_UMA:-0}" = 1 ]; then
    export BB_UMA=1 BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=0 BB_WRITE_TRACKING=1 BB_CHUNK_UPLOADS=0
    unset BB_UNIFIED
    for uma_var in BB_UMA_CLASSES BB_UMA_VERIFY BB_UMA_GRANULE_KB BB_UMA_DMA_GPU; do
        eval "uma_val=\${$uma_var:-}"
        [ -z "$uma_val" ] || export "$uma_var"
    done
    echo "Unified memory v2: requested (classes ${BB_UMA_CLASSES:-index,vertex}, granule ${BB_UMA_GRANULE_KB:-2048} KiB, verify ${BB_UMA_VERIFY:-0}; the driver is probed at start, falls back to copies with a logged reason)"
elif [ "${BB_UNIFIED:-0}" = 1 ]; then
    unset BB_UMA
    export BB_UNIFIED=1 BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=1 BB_WRITE_TRACKING=1
    echo "Unified memory v1 (BB_UNIFIED=1): requested (the driver is probed at start; falls back to copies with a logged reason)"
else
    unset BB_UMA BB_UNIFIED BB_LAYER_MEMORY BB_WRITE_TRACKING
    export BB_GUEST_IN_PLACE=0
fi
export BB_DMEM_MB=$dmem_mb BB_PREUPLOAD=0 BB_UFFD=0
export BB_COPY_GPU_BUFFERS=1 BB_GPU_WRITE_TWINS=1 BB_GPU_WRITE_TWINS_MAX=65536
# GPU memory is the phone's system RAM, shared with the game's direct memory ($dmem_mb, taken right
# after launch) and Android: the texture collector works to a budget instead of the driver's heap
# (the app was killed for memory once the world loaded). Its start value is what is free now less
# the direct memory and a 1536 MB reserve for Android and the app, 1024..4096 MB (4096 when
# /proc/meminfo cannot be read); BB_GC_BUDGET_MB in the environment or bionic-local.sh overrides it.
# libbbgpu then follows the free memory and swap while the game runs: it lowers the budget when the
# system runs short, and raises it back (never above this start) when it recovers (BB_GC_ADAPT=0:
# keep it fixed). Idle copies of game memory go after 20 s instead of 60 s.
meminfo_mb() {
    kb=$(sed -n "s/^$1: *\([0-9][0-9]*\) kB.*/\1/p" /proc/meminfo 2>/dev/null | tail -n 1)
    case $kb in ''|*[!0-9]*) echo 0 ;; *) echo $((kb / 1024)) ;; esac
}
if [ -z "${BB_GC_BUDGET_MB:-}" ]; then
    mem_total_mb=$(meminfo_mb MemTotal); mem_avail_mb=$(meminfo_mb MemAvailable)
    if [ "$mem_avail_mb" -gt 0 ]; then
        gc_budget_mb=$((mem_avail_mb - dmem_mb - 1536))
        [ "$gc_budget_mb" -ge 1024 ] || gc_budget_mb=1024
        [ "$gc_budget_mb" -le 4096 ] || gc_budget_mb=4096
    else
        gc_budget_mb=4096
    fi
    export BB_GC_BUDGET_MB=$gc_budget_mb
    echo "Texture budget: $gc_budget_mb MB (MemAvailable $mem_avail_mb MB of $mem_total_mb MB, direct memory $dmem_mb MB, reserve 1536 MB; live, BB_GC_ADAPT=0 keeps it)"
else
    export BB_GC_BUDGET_MB
    echo "Texture budget: $BB_GC_BUDGET_MB MB (set by the environment)"
fi
export BB_VRAM_IDLE_SECONDS=${BB_VRAM_IDLE_SECONDS:-20}
# VSync (vsync=1): the app shows every frame for at least one display refresh, in order (a FIFO in
# the app: libbbgpu says so in HELLO, bb_present_proto.h), which also holds the game to the refresh
# rate when there is no FPS limit; 0: the newest frame is shown, frames that finish within one
# refresh are skipped. Lossless Scaling needs the FIFO whatever this says: libbbgpu asks the app for it
# (BB_PRESENT_MODE) while frame generation runs, so BB_VSYNC is the setting alone. BB_VSYNC=0/1 in
# bionic-local.sh overrides.
if [ "$(setting vsync)" = 1 ]; then export BB_VSYNC=1; else export BB_VSYNC=0; fi
# CPU cores (thor_cpus, a taskset list such as 3-7): empty for all of them.
[ -z "${BB_THOR_CPUS:-}" ] && BB_THOR_CPUS=$(setting thor_cpus)

# ---- The GPU driver (bbport.ini gpu_driver) ----
# "system" is the phone's stock driver through its Vulkan loader (BB_VULKAN_DRIVER unset); an id is
# files/drivers/<name>/ made by DriverManager.java, whose driver.lib names the library (an
# adrenotools-style Turnip / PanVK for Android), loaded directly. Empty, "auto", or an id whose folder is
# gone: the automatic choice, the driver the app resolved for this GPU and named in BB_GPU_DRIVER (it installs
# the one bundled in the APK; AutoDriver.java; BB_GPU_DRIVER_WHY is its log line), else the stock driver.
gpu_driver=$(setting gpu_driver)
gpu_auto=0
case $gpu_driver in
    ''|auto) gpu_auto=1; gpu_driver= ;;
    .*|*/*|system) gpu_driver= ;;
    *) if [ ! -f "$files/drivers/$gpu_driver/driver.lib" ]; then
           echo "GPU driver $gpu_driver is gone: automatic choice"; gpu_auto=1; gpu_driver=
       fi ;;
esac
if [ "$gpu_auto" = 1 ]; then
    gpu_pick=${BB_GPU_DRIVER:-}
    case $gpu_pick in ''|.*|*/*|system) gpu_pick= ;; esac
    if [ -n "$gpu_pick" ] && [ -f "$files/drivers/$gpu_pick/driver.lib" ]; then
        gpu_driver=$gpu_pick
        echo "GPU driver: automatic -> $gpu_pick (${BB_GPU_DRIVER_WHY:-no reason given})"
    elif [ -n "${BB_GPU_DRIVER:-}" ] && [ "$BB_GPU_DRIVER" != system ]; then
        echo "GPU driver: automatic -> system (stock): the app's choice $BB_GPU_DRIVER is not installed"
    else
        echo "GPU driver: automatic -> system (stock): ${BB_GPU_DRIVER_WHY:-the app did not name a driver}"
    fi
fi
unset BB_GPU_DRIVER BB_GPU_DRIVER_WHY
unset BB_VULKAN_DRIVER
if [ -n "$gpu_driver" ]; then
    driver_lib=$(cat "$files/drivers/$gpu_driver/driver.lib" 2>/dev/null || true)
    case $driver_lib in
        ''|*/*|.*) echo "GPU driver $gpu_driver is not installed: system driver" ;;
        *)
            if [ -f "$files/drivers/$gpu_driver/$driver_lib" ]; then
                export BB_VULKAN_DRIVER="$files/drivers/$gpu_driver/$driver_lib"
                echo "GPU driver: $gpu_driver (installed from a zip)"
            else
                echo "GPU driver $gpu_driver has no library: system driver"
            fi ;;
    esac
fi
[ -n "${BB_VULKAN_DRIVER:-}" ] || echo 'GPU driver: system (stock)'

# ---- Sound and picture ----
# The mixer writes s16 stereo 48 kHz into the FIFO the app plays (AudioBridge.java), only when the app
# made it: a FIFO nobody reads would fill. libbbgpu connects to the app's socket to present.
[ -p "$base/audio.fifo" ] && export BB_AUDIO_FIFO="$base/audio.fifo"
export BB_PRESENT_SOCKET="$files/bbpresent.sock"
export BB_SCREEN_W="${BB_SCREEN_W:-1280}" BB_SCREEN_H="${BB_SCREEN_H:-720}"
# libbbgpu: SPIR-V of pipelines the driver rejects is written here (vk_shader_util.cpp).
export BB_FAILED_SHADER_DIR="$base/logs/failed-shaders"
cp "$base/fex-compatible.json" "$HOME/.fex-emu/Config.json"
# ---- Debug options (Debug options in the app menu; bbport.ini debug_* keys, a missing key is its default) ----
# Write-fault tracking: how the game's writes to GPU memory are tracked. Only the non-default values are
# exported; bionic-local.sh (below) and the environment can still override them.
dbg_prepare=on; dbg_drain=on; dbg_lock=on; dbg_stats=off; dbg_profile=off
if [ "$(setting debug_fault_prepare)" = 0 ]; then export BB_FAULT_PREPARE=0; dbg_prepare=off; fi
if [ "$(setting debug_fault_smart_drain)" = 0 ]; then export BB_FAULT_DRAIN=always; dbg_drain=off; fi
if [ "$(setting debug_fault_sleep_lock)" = 0 ]; then export BB_FAULT_SPINLOCK=1; dbg_lock=off; fi
# Free old GPU memory (FIFO; on by default, debug_vram_copy_queue=0 turns it off; copy mode, and the copies that
# unified memory v2 still makes: its shared blocks are never queued): BB_VRAM_COPY_QUEUE=1, with the cap (MB) of debug_vram_copy_cap_mb when it is a positive
# integer, else 512 (tested on the AYN Thor: copies held ~480 MiB, 33-39 FPS in Central Yharnam).
dbg_fifo=off
if [ "$(setting debug_vram_copy_queue)" != 0 ]; then
    fifo_cap="$(setting debug_vram_copy_cap_mb)"
    case "$fifo_cap" in
        ''|*[!0-9]*|0*) fifo_cap=512 ;;
    esac
    export BB_VRAM_COPY_QUEUE=1 BB_VRAM_COPY_CAP_MB="$fifo_cap"; dbg_fifo="on (cap $fifo_cap MB)"
else
    unset BB_VRAM_COPY_QUEUE BB_VRAM_COPY_CAP_MB
fi
# Diagnostics (off by default).
if [ "$(setting debug_frame_stats)" = 1 ]; then export BB_FRAME_STATS=1; dbg_stats=on; fi
if [ "$(setting debug_gpu_profile)" = 1 ]; then export BB_GPU_PROFILE=1; dbg_profile=on; fi
echo "Debug: fault prepare $dbg_prepare, smart drain $dbg_drain, sleeping lock $dbg_lock, FIFO VRAM copies $dbg_fifo, frame stats $dbg_stats, GPU profile $dbg_profile"
# Local overrides for this device (experiments, tuning): sourced last, not part of the runtime.
[ -f "$base/bionic-local.sh" ] && . "$base/bionic-local.sh"
# FPS limit (fps_limit: 30, 40, 45, 60 or 0 for none) and the game's frame rate. Hooks in the game's code
# (gpu/shim/bbport_frame_rate.cpp, installed when the game loads) give the game each frame's real time
# for its steps, at any limit including 30, and leave the limiting to the present thread (BB_FPS_LIMIT;
# vblank 480 Hz, frames shown at once): frame drops below the limit keep the game's speed. They replace the
# community patches (fps=uncap.bin: Uncap FPS++ with Sprint Fix), which are loaded only if the hooks
# cannot install (a site of the game's code differs: bb-probe's --fallback-patches, below), or with
# BB_FRAME_HOOKS=0 (comparisons: today's timing; 30 without frame generation is the game's own).
# After bionic-local.sh, so that it can set BB_FRAME_HOOKS=0.
fps_limit=$(setting fps_limit)
case $fps_limit in 30|40|45|60|0) ;; *) fps_limit=30 ;; esac
frame_hooks=1
if [ "${BB_FRAME_HOOKS:-1}" = 0 ]; then frame_hooks=0; export BB_FRAME_HOOKS=0; else unset BB_FRAME_HOOKS; fi
if [ "$frame_hooks" = 1 ]; then
    export BB_VBLANK_HZ=480 BB_FPS_LIMIT=$fps_limit
else
    case $fps_limit in 40|45|60|0) export BB_VBLANK_HZ=480 BB_FPS_LIMIT=$fps_limit ;;
        *) export BB_VBLANK_HZ=60 ;; esac
    # Frame generation adds frames on screen only: at the game's own 30 FPS timing a slower frame still
    # slows the game down. With it, 30 means the delta-time patch at 30 real frames a second. This is
    # decided at start (patches load with the game): Lossless Scaling turned on later, at the 30 FPS limit,
    # runs on the game's own timing until the next start, and turned off later it keeps the patch.
    if { [ "${BB_FRAME_GEN:-0}" = 1 ] || [ -n "$lsfg" ]; } && [ "$fps_limit" = 30 ]; then
        fps_limit=30-delta; export BB_VBLANK_HZ=480 BB_FPS_LIMIT=30
    fi
fi
# speed_floor (20, the default, or 30): how slow the game may get before it slows down. Below 30 FPS the
# game's own frame time is 1/30 s and it runs in slow motion; 20 gives it real time down to 20 FPS (a step of
# up to 50 ms: physics and animations take bigger steps), 30 is the game's own limit (upstream's hooks).
speed_floor=$(setting speed_floor)
case $speed_floor in 20|30) ;; *) speed_floor=20 ;; esac
export BB_FRAME_DT_MAX_FPS=$speed_floor
echo "Frame rate: limit $fps_limit, hooks $([ "$frame_hooks" = 1 ] && echo on || echo 'off (BB_FRAME_HOOKS=0)'), game speed kept down to $speed_floor FPS"
cd "$base"
[ -x "$bionic/bin/bb-probe" ] || { echo "The Bionic runtime is missing: $bionic" >&2; exit 2; }

# ---- The game files and the patches ----
# The game folder chosen in the app (BB_GAME_DIR), else the runtime's own; bb-prepare builds the image
# in data/ (a no-op when it matches). The folder to run from (BB_APP0_DIR) is the app's mods overlay of
# the game folder, or the game folder itself.
if [ "${1:-game}" = game ]; then
    game_dir=${BB_GAME_DIR:-$base/game}
    [ -f "$game_dir/eboot.bin" ] || { echo "Game missing: $game_dir"; exit 2; }
    "$bionic/bin/bb-prepare" "$game_dir" "$base/data"
    mods_dir=$(setting mods_dir)
    [ -n "$mods_dir" ] || mods_dir=/storage/emulated/0/Bloodborne/mods
    export BB_MODS_DIR="$mods_dir"
    [ -n "${BB_MOD_TRACE:-}" ] && export BB_MOD_TRACE
    app0_dir=${BB_APP0_DIR:-$game_dir}
    # The settings' pre-compiled patches (build_patch_parts.py) as the words of $patch_args (none of the
    # paths has a space).
    parts="$base/patch-parts"
    [ -d "$parts" ] || { echo "The patches are missing: $parts" >&2; exit 2; }
    patch_args=
    if [ -n "$render" ]; then
        [ -f "$parts/res-scene.bin" ] || { echo "The scene size patch is missing: $parts/res-scene.bin" >&2; exit 2; }
        # The patch holds a 1280x720 scene: libbbgpu writes the real size (BB_RENDER_RES) into it at load.
        patch_args="--patches $parts/res-scene.bin"
        [ "$big_scene" = 1 ] && [ -f "$parts/graphics-heap.bin" ] && patch_args="$patch_args --patches $parts/graphics-heap.bin"
    fi
    for key in effect_chromatic_aberration effect_dof effect_motion_blur effect_ssao \
               effect_game_aa effect_dynamic_shadows effect_ssr skip_intro debug_camera debug_menu model_lod; do
        value=$(setting "$key")
        [ -f "$parts/$key=$value.bin" ] && patch_args="$patch_args --patches $parts/$key=$value.bin"
    done
    # Frame rate: the hooks (libbbgpu) or, with BB_FRAME_HOOKS=0, the community patch above 30 FPS (and with
    # frame generation at 30: fps_limit=30-delta). If the hooks cannot install, bb-probe applies the patch
    # as the fallback (the hooks write nothing then).
    if [ "$frame_hooks" = 1 ]; then
        patch_args="$patch_args --fallback-patches $parts/fps=uncap.bin"
    elif [ "$fps_limit" != 30 ]; then
        patch_args="$patch_args --patches $parts/fps=uncap.bin"
    fi
    # The GX per-frame command heaps at 1 MiB (upstream #162; BB_GX_COMMAND_HEAP=0: the game's 256 KiB).
    [ "${BB_GX_COMMAND_HEAP:-1}" != 0 ] && [ -f "$parts/gx-heap.bin" ] &&
        patch_args="$patch_args --patches $parts/gx-heap.bin"
    # No PSN here: the title screen goes straight to the main menu offline.
    [ "$(setting skip_network_choice)" != 0 ] && [ -f "$parts/skip_network_choice=1.bin" ] &&
        patch_args="$patch_args --patches $parts/skip_network_choice=1.bin"
    # Cheats (the app's Cheats menu): their hooks are always in, switched live by the flags
    # BB_CHEATS_FILE holds (the app rewrites it; here its start, from bbport.ini).
    if [ -f "$parts/cheats.bin" ]; then
        patch_args="$patch_args --patches $parts/cheats.bin"
        export BB_CHEATS_FILE="$base/cheats.state"
        printf '%s%s\n' "$([ "$(setting cheat_health)" = 1 ] && echo 1 || echo 0)" \
            "$([ "$(setting cheat_items)" = 1 ] && echo 1 || echo 0)" > "$BB_CHEATS_FILE"
    fi
fi

# The Bionic libraries only: the app's own environment has no library path.
LD_LIBRARY_PATH="$bionic/lib"; export LD_LIBRARY_PATH
unset LD_PRELOAD
case ${1:-game} in
    vulkan)
        exec "$bionic/bin/bb-probe" --vulkan-only
        ;;
    game)
        set -- "$bionic/bin/bb-probe"
        # BB_THOR_CPUS: the CPUs the game may run on (taskset -c list), e.g. 3-7 big cores only.
        [ -n "${BB_THOR_CPUS:-}" ] && set -- /system/bin/taskset -c "$BB_THOR_CPUS" "$@"
        set -- "$@" "$base/data/boot-linked.bin" $patch_args
        exec "$@" \
            --content-profile "$base/data/content.bin" \
            --app0 "$app0_dir" --user "$base/user" --timeout "${BB_TIMEOUT:-0}"
        ;;
    *) echo 'usage: run-bionic.sh game|vulkan' >&2; exit 2 ;;
esac
