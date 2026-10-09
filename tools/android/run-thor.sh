#!/bin/sh
# Runs inside fexdroid's Android app sandbox, with its patched ARM Linux runtime.
set -eu
files=${FXD_FILES:-/data/data/ro.cobrabm.fexdroid/files}
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
if [ "${BB_ANDROID_INPUT:-0}" = 1 ]; then
    export BB_PAD_FILE="$base/android-pad.state" BB_PAD_QUIET=1
fi
export BB_FULLSCREEN=1 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=3
export BB_WINDOW_SIZE=1280x720
export BB_RENDER_RES=640x360 BB_OUTPUT_RES=1280x720 BB_LIVE_RES=0
export BB_DMEM_MB=5056 BB_PREUPLOAD=0 BB_GUEST_IN_PLACE=0 BB_UFFD=0
export BB_COPY_GPU_BUFFERS=1 BB_GPU_WRITE_TWINS=1 BB_GPU_WRITE_TWINS_MAX=65536
export BB_VBLANK_HZ=60 BB_FRAME_LIMIT=30 BB_FRAME_STATS=1
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
        [ -f "$base/game/eboot.bin" ] || { echo "Game missing: $base/game"; exit 2; }
        exec "$host/usr/bin/FEX" "$base/bin/bb-probe" "$base/data/boot-linked.bin" \
            --content-profile "$base/data/content.bin" --patches "$base/data/patches.bin" \
            --app0 "$base/game" --user "$base/user" --timeout "${BB_TIMEOUT:-0}"
        ;;
    *) echo 'usage: run-thor.sh cpu|vulkan|renderer|window|gamepads|controller|game' >&2; exit 2 ;;
esac
