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

# ---- FULL DEBUG BUILD for issue #15 (Mali-G720 device lost): not for release ----
# Every useful diagnostic is on by default and the game runs slowly. Each default can be replaced from a
# player-editable override file: debug.env in the Bloodborne folder of shared storage (the game folder's parent:
# /storage/emulated/0/Bloodborne/debug.env; the app has legacy external storage access). Lines NAME=value only,
# NAME one of BB_*, PANVK_*, PAN_*, MESA_* (capitals, digits, underscore), the value letters, digits and
# _ . / : , = + @ % - only. The file is read, never sourced. NAME= (empty) removes the variable (turns off a
# trace the code only looks for by name, such as BB_IMAGE_SYNC_LOG); NAME=0 turns off a check that reads its value.
echo "Full debug build for issue #15: all diagnostics on, the game runs slowly (0.9.2-debug15-full)"
for prop in ro.product.manufacturer ro.product.model ro.board.platform ro.hardware.vulkan ro.hardware.egl \
            ro.build.version.release ro.build.version.sdk ro.soc.model; do
    echo "  $prop=$(getprop "$prop" 2>/dev/null || true)"
done
# libbbgpu's own log (vk_instance: GPU, driver, extensions and features; pipeline and shader compiles; warnings),
# with a budget in the log sink (log.cpp): 20000 debug/info lines, 40000 warnings, then one in 1000.
export BB_GPU_LOG=${BB_GPU_LOG:-debug}
# Breadcrumbs (timestamp markers polled so they survive a device loss; the device-lost report lists the newest
# commands per stream), the first 400 submissions as they go out, the HLE copy shader's pipeline and first batches.
export BB_BREADCRUMBS=${BB_BREADCRUMBS:-1}
export BB_SUBMIT_TRACE=${BB_SUBMIT_TRACE:-1}
export BB_COPY_SHADER_TRACE=${BB_COPY_SHADER_TRACE:-1}
# The command processor's reads of GPU-written memory (first 300: drain, finish, read).
export BB_SYNC_TRACE=${BB_SYNC_TRACE:-1}
# Images: GPU writes over images and refreshes from memory (first 200 / 100 lines, top 5 every 5 s), the compute
# copies recognised as image copies (first 40), tiling back into guest memory (first 400), large layer writes (64).
export BB_IMAGE_SYNC_LOG=${BB_IMAGE_SYNC_LOG:-1}
export BB_IMAGE_COPY_LOG=${BB_IMAGE_COPY_LOG:-1}
export BB_TILE_BACK_LOG=${BB_TILE_BACK_LOG:-1}
export BB_LAYER_WRITE_LOG=${BB_LAYER_WRITE_LOG:-1}
# Present path steps (first 160 lines).
export BB_PRESENT_TRACE=${BB_PRESENT_TRACE:-1}
# Checks: two threads recording into one scheduler (first 8 reports), the draw pipe's resource tables read again
# on every 16th packet (a change after the GPU thread read them; first 32 reports), the GPU's own check of
# indirect dispatch counts (skipped dispatches are reported every 5 s), GPU-written data in VRAM copies.
export BB_PRODUCER_CHECK=${BB_PRODUCER_CHECK:-1}
export BB_PIPE_VERIFY=${BB_PIPE_VERIFY:-16}
export BB_INDIRECT_GUARD=${BB_INDIRECT_GUARD:-1}
export BB_GPU_DATA_CHECK=${BB_GPU_DATA_CHECK:-1}
# The Mali driver (Mesa PanVK): startup = what it found and enabled, sync = wait for the GPU after every submit (the
# hanging submit is then the one the process is in). Its log goes to stderr (this log) and to Android's logcat.
# No trace / dump (pandecode: huge files), no abort on device loss (it would kill the process before our report).
export PANVK_DEBUG=${PANVK_DEBUG:-startup,sync}
export MESA_LOG=${MESA_LOG:-file,android}
export MESA_LOG_LEVEL=${MESA_LOG_LEVEL:-info}
debug_env=/storage/emulated/0/Bloodborne/debug.env
debug_cr=$(printf '\r')
apply_debug_env() { # $1 = 1: say what is applied (the second pass is silent)
    if [ ! -r "$debug_env" ]; then
        [ "$1" = 1 ] && echo "debug.env: none ($debug_env is not there or not readable)"
        return 0
    fi
    [ "$1" = 1 ] && echo "debug.env: reading $debug_env"
    debug_n=0
    while IFS= read -r debug_line || [ -n "$debug_line" ]; do
        debug_n=$((debug_n + 1))
        debug_line=${debug_line%"$debug_cr"}
        case $debug_line in ''|'#'*) continue ;; esac
        case $debug_line in
            *=*) ;;
            *) [ "$1" = 1 ] && echo "debug.env line $debug_n ignored (not NAME=value): $debug_line"; continue ;;
        esac
        debug_name=${debug_line%%=*}
        debug_value=${debug_line#*=}
        case $debug_name in
            BB_?*|PANVK_?*|PAN_?*|MESA_?*) ;;
            *) [ "$1" = 1 ] && echo "debug.env line $debug_n ignored (the name must start with BB_, PANVK_, PAN_ or MESA_): $debug_line"; continue ;;
        esac
        case $debug_name in
            *[!A-Z0-9_]*) [ "$1" = 1 ] && echo "debug.env line $debug_n ignored (bad characters in the name): $debug_line"; continue ;;
        esac
        case $debug_value in
            *[!A-Za-z0-9_./:,=+@%-]*) [ "$1" = 1 ] && echo "debug.env line $debug_n ignored (the value may only have letters, digits and _ . / : , = + @ % -): $debug_line"; continue ;;
        esac
        if [ -z "$debug_value" ]; then
            unset "$debug_name"
            [ "$1" = 1 ] && echo "debug.env applied: $debug_name removed"
            continue
        fi
        export "$debug_name=$debug_value"
        [ "$1" = 1 ] && echo "debug.env applied: $debug_name=$debug_value"
    done < "$debug_env"
    return 0
}

# ---- Settings: bbport.ini (the app's settings screen writes it) turned into BB_* variables ----
export BB_CONFIG="$base/bbport.ini"
[ -f "$BB_CONFIG" ] || cp "$base/bbport.default.ini" "$BB_CONFIG"
setting() { sed -n "s/^$1=//p" "$BB_CONFIG" 2>/dev/null | tail -n 1; }
# The app's pad state file (PadBridge.java) and text requests (the character name: its touch keyboard).
export BB_ANDROID_INPUT=1
export BB_PAD_FILE="$base/android-pad.state" BB_PAD_QUIET=1
export BB_IME_FILE="$base/ime.request"
# The output (output_res: 1920x1080, 1280x720 or 854x480; 720p when missing) is the size of the final
# frame and the UI; the preset sets the scene size relative to it, FSR 3.1 upscales it to the output.
upscaler=$(setting upscaler); preset=$(setting preset); output=$(setting output_res)
# FSR 4 (INT8 models) is not offered: on the Adreno 740 it took ~3 min to compile in Turnip, then ran
# at 2.4 FPS with a black scene. Any value but off means FSR 3.1.
case $upscaler in off) ;; *) upscaler=fsr3 ;; esac
echo "Upscaler setting: $upscaler"
case $preset in 0|1|2|3|4) ;; *) preset=3 ;; esac
case $output in 1920x1080|1280x720|854x480) ;; *) output=1280x720 ;; esac
# The scene size: the output / the preset's scale (1.5, 1.7, 2, 3), each side rounded to even, as
# build_patch_parts.py (render_sizes()) names the res-*.bin patches.
render=$output
if [ "$upscaler" != off ]; then
    case $output:$preset in
        1920x1080:1) render=1280x720 ;; 1920x1080:2) render=1130x636 ;; 1920x1080:3) render=960x540 ;;
        1920x1080:4) render=640x360 ;;
        1280x720:1) render=854x480 ;; 1280x720:2) render=752x424 ;; 1280x720:3) render=640x360 ;;
        1280x720:4) render=426x240 ;;
        854x480:1) render=570x320 ;; 854x480:2) render=502x282 ;; 854x480:3) render=428x240 ;;
        854x480:4) render=284x160 ;;
    esac
fi
# A scene of the game's own 1920x1080 (1080p Native, or 1080p without the upscaler) needs no resolution
# patch and no BB_RENDER_RES: the renderer then takes the game's targets as they are (with
# BB_RENDER_RES it tells the scene from the 1920x1080 UI by its size).
if [ "$render" = 1920x1080 ]; then render=; preset=0; fi
export BB_FULLSCREEN=1 BB_UPSCALER=$upscaler BB_UPSCALE_PRESET=$preset
# FSR 3.1 frame generation (frame_generation=1): a frame between each two, with FSR 3.1 only.
[ "$(setting frame_generation)" = 1 ] && [ "$upscaler" = fsr3 ] && export BB_FRAME_GEN=1
# Lossless Scaling frame generation (lsfg_multiplier 2-4): libbbgpu loads liblsfg-bb.so (tools/android/lsfg:
# the framegen of GameNative's lsfg-vk fork, on the game's own Vulkan driver) which reads its shaders from
# the player's own Lossless.dll: bbport/Lossless.dll (the app's Steam sign-in or file picker), else the
# Bloodborne folder of shared storage. It replaces FSR 3.1 frame generation. Generated frames only look
# right when each is shown for one refresh: while it runs libbbgpu asks the app for its FIFO (VSync), and
# the app sets the display to FPS limit x multiplier (MainActivity.pickRefreshRate).
# It is LIVE: the app's Frame generation menu rewrites bbport/lsfg.state (BB_LSFG_STATE_FILE) while the
# game runs and libbbgpu follows it (turns it on, off, or to another multiplier / flow scale / model with
# no restart). That file is the one source of truth for the three settings; here is only its start,
# written from bbport.ini (like cheats.state), so no BB_LSFG_MULTIPLIER/FLOW_SCALE/PERFORMANCE are exported.
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
unset BB_LSFG_DLL BB_LSFG_MULTIPLIER BB_LSFG_FLOW_SCALE BB_LSFG_PERFORMANCE
[ -n "$lsfg_dll" ] && export BB_LSFG_DLL=$lsfg_dll
flow=$(setting lsfg_flow_scale)
case $flow in 0.25|0.[3-9]|0.[3-9]0|0.[3-9]5|1.00|1.0|1) ;; *) flow=0.75 ;; esac
performance=1; [ "$(setting lsfg_performance)" = 0 ] && performance=0
export BB_LSFG_STATE_FILE="$base/lsfg.state"
printf 'multiplier=%s\nflow=%s\nperformance=%s\n' "$lsfg_requested" "$flow" "$performance" \
    > "$BB_LSFG_STATE_FILE.tmp" && mv "$BB_LSFG_STATE_FILE.tmp" "$BB_LSFG_STATE_FILE"
if [ -n "$lsfg" ]; then
    if [ -z "$lsfg_dll" ]; then
        echo "Lossless Scaling frame generation: no Lossless.dll ($base/Lossless.dll or /storage/emulated/0/Bloodborne/Lossless.dll): off"
        lsfg=
    elif [ ! -f "$bionic/lib/liblsfg-bb.so" ]; then
        echo "Lossless Scaling frame generation: $bionic/lib/liblsfg-bb.so is missing: off"
        lsfg=
    else
        unset BB_FRAME_GEN
        echo "Lossless Scaling frame generation: x$lsfg, flow scale $flow, performance mode $performance, $lsfg_dll (live: $BB_LSFG_STATE_FILE)"
    fi
else
    echo "Lossless Scaling frame generation: off at start (the app's Frame generation menu turns it on while you play)"
fi
# The GPU's check of indirect dispatch counts (vk_indirect_guard.h): on in this debug build (set above).
# The two-stage draw pipeline (bb:DrawRec) frees the GPU command thread: 31 -> 34 FPS on the Thor
# with all cores. BB_DRAW_PIPE=0 in bionic-local.sh turns it off.
export BB_DRAW_PIPE=${BB_DRAW_PIPE:-1}
export BB_WINDOW_SIZE=$output
export BB_OUTPUT_RES=$output BB_LIVE_RES=0
if [ -n "$render" ]; then export BB_RENDER_RES=$render; else unset BB_RENDER_RES; fi
export BB_DMEM_MB=5056 BB_PREUPLOAD=0 BB_GUEST_IN_PLACE=0 BB_UFFD=0
export BB_COPY_GPU_BUFFERS=1 BB_GPU_WRITE_TWINS=1 BB_GPU_WRITE_TWINS_MAX=65536
# GPU memory is the phone's system RAM, shared with the game's 5 GB and Android: the texture
# collector works to a 4 GB budget instead of the driver's heap (the app was killed for memory once
# the world loaded), and idle copies of game memory go after 20 s instead of 60 s.
export BB_GC_BUDGET_MB=${BB_GC_BUDGET_MB:-4096} BB_VRAM_IDLE_SECONDS=${BB_VRAM_IDLE_SECONDS:-20}
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
# Local overrides for this device (experiments, tuning): sourced last, not part of the runtime.
[ -f "$base/bionic-local.sh" ] && . "$base/bionic-local.sh"
# Full debug build for issue #15: the player's overrides (before the frame rate logic below reads BB_FRAME_HOOKS, and
# once more, silently, before the launch so that nothing computed in between replaces them).
apply_debug_env 1
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
apply_debug_env 0
echo "Full debug build for issue #15: the effective environment of the game process:"
env | grep -E '^(BB|PANVK|PAN|MESA|VK|FEX|LD)_' | sort | sed 's/^/  /' || true
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
        [ -f "$parts/res-$render.bin" ] || { echo "The patch for the scene size $render is missing: $parts/res-$render.bin" >&2; exit 2; }
        patch_args="--patches $parts/res-$render.bin"
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
