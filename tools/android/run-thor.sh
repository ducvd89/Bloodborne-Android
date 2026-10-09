#!/bin/sh
# Runs inside fexdroid's Android app sandbox, with its patched ARM Linux runtime.
set -eu
files=${FXD_FILES:-/data/data/com.ducvd89.bloodborne/files}
host="$files/rootfs"
base="$files/bbport"
export FXD_FILES="$files" FXD_ROOT="$host"
export PATH="$host/usr/bin:$host/bin:/system/bin"
export HOME="$files/home/bbport"
export TMPDIR="$host/tmp" XDG_RUNTIME_DIR="$host/tmp"
export FEX_ROOTFS="$base/rootfs"
export FEX_APP_CONFIG_LOCATION="$HOME/.fex-emu/"
export FEX_APP_DATA_LOCATION="$HOME/.fex-emu/"
export FEX_APP_CACHE_LOCATION="$HOME/.cache/fex-emu/"
export VK_DRIVER_FILES="$host/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'
export BB_CONFIG="$base/bbport.ini"
[ -f "$BB_CONFIG" ] || cp "$base/bbport.default.ini" "$BB_CONFIG"
if [ "${BB_ANDROID_INPUT:-0}" = 1 ]; then
    export BB_PAD_FILE="$base/android-pad.state" BB_PAD_QUIET=1
    # Text the game asks for (the character name): the app's touch keyboard.
    export BB_IME_FILE="$base/ime.request"
    # Sound: SDL's disk driver mixes into the FIFO the app plays (AudioBridge.java). Only when the
    # app made it: a FIFO nobody reads would stall the game's audio. Its reads pace the output.
    if [ -p "$base/audio.fifo" ]; then
        export SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$base/audio.fifo"
        export SDL_AUDIO_DISK_TIMESCALE=0 SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512
    fi
fi
# Graphics settings (the app's settings screen writes bbport.ini). The output is the Thor's
# 1280x720 display; the preset sets the scene size, FSR 3.1 upscales it to the output.
setting() { sed -n "s/^$1=//p" "$BB_CONFIG" 2>/dev/null | tail -n 1; }
upscaler=$(setting upscaler); preset=$(setting preset)
# FSR 4 is off on the Thor for now (Turnip compiles its model passes for over a minute, then
# crashed in a dispatch): fsr4, like anything else but off, means FSR 3.1.
case $upscaler in off) ;; *) upscaler=fsr3 ;; esac
case $preset in 0|1|2|3|4) ;; *) preset=3 ;; esac
render=1280x720
if [ "$upscaler" != off ]; then
    case $preset in 1) render=854x480 ;; 2) render=752x424 ;; 3) render=640x360 ;; 4) render=426x240 ;; esac
fi
export BB_FULLSCREEN=1 BB_UPSCALER=$upscaler BB_UPSCALE_PRESET=$preset
# FSR 3.1 frame generation (frame_generation=1): a frame between each two, with FSR 3.1 only.
[ "$(setting frame_generation)" = 1 ] && [ "$upscaler" = fsr3 ] && export BB_FRAME_GEN=1
# FSR 4 model assets (fetch_fsr4_assets.sh), in the native bundle when fetched.
[ -d "$base/arm64/fsr4_shaders" ] && export BB_FSR4_DIR="$base/arm64/fsr4_shaders"
export BB_WINDOW_SIZE=1280x720
export BB_RENDER_RES=$render BB_OUTPUT_RES=1280x720 BB_LIVE_RES=0
export BB_DMEM_MB=5056 BB_PREUPLOAD=0 BB_GUEST_IN_PLACE=0 BB_UFFD=0
export BB_COPY_GPU_BUFFERS=1 BB_GPU_WRITE_TWINS=1 BB_GPU_WRITE_TWINS_MAX=65536
# GPU memory is the Thor's system RAM, shared with the game's 5 GB and Android: the texture
# collector works to a 4 GB budget instead of the driver's 11 GB heap (the app was killed for
# memory once the world loaded), and idle copies of game memory go after 20 s instead of 60 s.
export BB_GC_BUDGET_MB=${BB_GC_BUDGET_MB:-4096} BB_VRAM_IDLE_SECONDS=${BB_VRAM_IDLE_SECONDS:-20}
# FPS limit (fps_limit): 30 is the game's own timing; above it the delta-time patch
# (fps=uncap.bin) with presents at once (vblank 480 Hz, as run.sh) and BB_FPS_LIMIT (0: none).
fps_limit=$(setting fps_limit)
case $fps_limit in 40|45|60|0) export BB_VBLANK_HZ=480 BB_FPS_LIMIT=$fps_limit ;;
    *) fps_limit=30; export BB_VBLANK_HZ=60 ;; esac
# Frame generation adds frames on screen only: at the game's own 30 FPS timing a slower frame
# still slows the game down. With it, 30 means the delta-time patch at 30 real frames a second.
if [ "${BB_FRAME_GEN:-0}" = 1 ] && [ "$fps_limit" = 30 ]; then
    fps_limit=30-delta; export BB_VBLANK_HZ=480 BB_FPS_LIMIT=30
fi
# CPU cores (thor_cpus, a taskset list such as 3-7): empty for all of them.
[ -z "${BB_THOR_CPUS:-}" ] && BB_THOR_CPUS=$(setting thor_cpus)
export BB_FRAME_STATS=1
export BB_PRESENT_DUMP_TRIGGER="$base/logs/present.capture" BB_PRESENT_DUMP_COUNT=1
export BB_DUMP_DIR="$base/logs/frames"
export SDL_VIDEODRIVER=x11 DISPLAY=:0
export MESA_VK_WSI_DEBUG=sw MESA_VK_WSI_PRESENT_MODE=relaxed
export FEXDROID_PRESENT="$host/tmp/fxpresent.sock"
export SDL_JOYSTICK_HIDAPI=0
# Find the built-in controller again after a reboot (event numbers can change).
for thor_node in /sys/class/input/event*; do
    [ -r "$thor_node/device/id/vendor" ] && [ -r "$thor_node/device/id/product" ] || continue
    IFS= read -r thor_vendor < "$thor_node/device/id/vendor"
    IFS= read -r thor_product < "$thor_node/device/id/product"
    if [ "$thor_vendor:$thor_product" = '2020:0112' ]; then
        export SDL_JOYSTICK_DEVICE="/dev/input/${thor_node##*/}"
        break
    fi
done
export SDL_GAMECONTROLLERCONFIG_FILE="$base/thor-gamecontrollerdb.txt"
export BB_GAMEPAD='030018dc202000001201000000000000'
export PULSE_SERVER="unix:$host/tmp/pulse/native" PULSE_LATENCY_MSEC=60
mkdir -p "$HOME/.fex-emu" "$HOME/.cache/fex-emu" "$base/user" "$base/logs/frames"
cp "$base/fex-compatible.json" "$HOME/.fex-emu/Config.json"
# Local overrides for this device (experiments, tuning): sourced last, not part of the bundle.
[ -f "$base/thor-local.sh" ] && . "$base/thor-local.sh"
cd "$base"
case ${1:-game} in
    gamepads)
        exec "$host/usr/bin/FEX" "$base/bin/bb-gpu-capabilities" --gamepads
        ;;
    controller)
        exec "$host/usr/bin/FEX" "$base/bin/controller-probe" 120
        ;;
    cpu)
        exec "$host/usr/bin/FEX" "$base/bin/bb-probe" "$base/data/cpu-fixture.bin" --cpu-only
        ;;
    vulkan)
        exec "$host/usr/bin/FEX" "$base/bin/bb-probe" --vulkan-only
        ;;
    renderer)
        exec "$host/usr/bin/FEX" "$base/bin/renderer-probe"
        ;;
    window)
        exec "$host/usr/bin/FEX" "$base/bin/bb-probe" "$base/data/cpu-fixture.bin" --timeout 60
        ;;
    game)
        # The game folder chosen in the app (shared storage), else the bundle's own.
        game_dir=${BB_GAME_DIR:-$base/game}
        [ -f "$game_dir/eboot.bin" ] || { echo "Game missing: $game_dir"; exit 2; }
        if [ -x "$host/usr/bin/python3.13" ]; then
            PYTHONHOME="$host/usr" "$host/usr/bin/python3.13" \
                "$base/scripts/prepare_game.py" "$game_dir" "$base/data"
        elif [ ! -f "$base/data/boot-linked.bin" ]; then
            echo 'Python runtime missing; cannot prepare the selected game files' >&2
            exit 2
        fi
        # The native ARM64 build (runtime and GPU native, the game's code in FEXCore) when it is
        # installed; BB_THOR_X86=1: the whole x86-64 loader under FEX instead.
        if [ -x "$base/arm64/bin/bb-probe" ] && [ "${BB_THOR_X86:-0}" != 1 ]; then
            unset GLIBC_TUNABLES
            set -- "$base/arm64/bin/bb-probe"; native=1
            # BB_THOR_CPUS: the CPUs the game may run on (taskset -c list), e.g. 3-7 big cores only.
            [ -n "${BB_THOR_CPUS:-}" ] && set -- taskset -c "$BB_THOR_CPUS" "$@"
        else
            set -- "$host/usr/bin/FEX" "$base/bin/bb-probe"; native=0
        fi
        # Pre-compiled patches for the settings (build_thor_patch_parts.py; the native loader
        # takes several), else the bundle's single file.
        parts="$base/arm64/patch-parts"
        if [ "$native" = 1 ] && [ -d "$parts" ]; then
            set -- "$@" "$base/data/boot-linked.bin" --patches "$parts/res-$render.bin"
            for key in effect_chromatic_aberration effect_dof effect_motion_blur effect_ssao \
                       effect_game_aa effect_dynamic_shadows effect_ssr skip_intro debug_camera debug_menu model_lod; do
                value=$(setting "$key")
                [ -f "$parts/$key=$value.bin" ] && set -- "$@" --patches "$parts/$key=$value.bin"
            done
            [ "$fps_limit" != 30 ] && set -- "$@" --patches "$parts/fps=uncap.bin"
        else
            set -- "$@" "$base/data/boot-linked.bin" --patches "$base/data/patches.bin"
        fi
        exec "$@" \
            --content-profile "$base/data/content.bin" \
            --app0 "$game_dir" --user "$base/user" --timeout "${BB_TIMEOUT:-0}"
        ;;
    *) echo 'usage: run-thor.sh cpu|vulkan|renderer|window|gamepads|controller|game' >&2; exit 2 ;;
esac
