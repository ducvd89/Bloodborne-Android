#!/usr/bin/env bash
# (Historical since phase 7: the app is native-code only; "off" restores a 1.2.0 release runtime, the last one with the FEX JIT. Needs an old out/release-1.2.0.)
# Switch the AYN Thor's installed app (com.ducvd89.bloodborne 1.2.0) between the stock runtime (FEX JIT) and this branch's runtime
# with ahead-of-time units (docs/AOT_DEVICE_STATUS.md). Run from the repository root.
#   aot_device.sh on        branch libbbcpu.so + bb-probe + run-bionic.sh (out/deploy) and out/aot/libbbaot.so; libbbgpu.so and
#                           libbbnet20.so stay the release's (this branch's GPU library hit "Out of order flip IRQ" more often)
#   aot_device.sh on-gpu    the same with this branch's libbbgpu.so / libbbnet20.so (out/deploy) too
#   aot_device.sh off       the release runtime files back (out/release-1.2.0/runtime/bbport); the .so stays in files/bbport/aot
#   aot_device.sh status    which runtime is installed
# out/deploy holds stripped copies of out/bionic/{lib,bin} (llvm-strip) and tools/android/run-bionic.sh.
set -euo pipefail
serial=${ANDROID_SERIAL:-70538e86}
pkg=com.ducvd89.bloodborne
adb_() { adb -s "$serial" "$@"; }
rel=out/release-1.2.0/runtime/bbport
push_runtime() {  # files...: to /data/local/tmp, then into the app's runtime
    adb_ shell am force-stop "$pkg"
    adb_ push "$@" /data/local/tmp/ >/dev/null
    local cmd="true"
    for f in "$@"; do
        local b; b=$(basename "$f")
        case $b in
            bb-probe) cmd="$cmd && cp /data/local/tmp/$b files/bbport/bionic/bin/$b && chmod 755 files/bbport/bionic/bin/$b" ;;
            run-bionic.sh) cmd="$cmd && cp /data/local/tmp/$b files/bbport/$b" ;;
            libbbaot.so) cmd="$cmd && mkdir -p files/bbport/aot && cp /data/local/tmp/$b files/bbport/aot/$b" ;;
            *.so) cmd="$cmd && cp /data/local/tmp/$b files/bbport/bionic/lib/$b" ;;
        esac
    done
    adb_ shell "run-as $pkg sh -c '$cmd'"
    for f in "$@"; do adb_ shell rm -f "/data/local/tmp/$(basename "$f")"; done
}
case ${1:-status} in
    on) push_runtime out/deploy/bb-probe out/deploy/libbbcpu.so out/deploy/run-bionic.sh out/aot/libbbaot.so \
            "$rel/bionic/lib/libbbgpu.so" "$rel/bionic/lib/libbbnet20.so" ;;
    on-gpu) push_runtime out/deploy/bb-probe out/deploy/libbbcpu.so out/deploy/run-bionic.sh out/aot/libbbaot.so \
            out/deploy/libbbgpu.so out/deploy/libbbnet20.so ;;
    off) push_runtime "$rel/bionic/bin/bb-probe" "$rel/bionic/lib/libbbcpu.so" "$rel/bionic/lib/libbbgpu.so" \
            "$rel/bionic/lib/libbbnet20.so" "$rel/run-bionic.sh" ;;
    status) ;;
    *) echo "usage: $0 on|on-gpu|off|status" >&2; exit 2 ;;
esac
adb_ shell "run-as $pkg sh -c 'md5sum files/bbport/bionic/bin/bb-probe files/bbport/bionic/lib/libbbcpu.so files/bbport/bionic/lib/libbbgpu.so files/bbport/run-bionic.sh; ls -la files/bbport/aot 2>/dev/null; grep -n BB_AOT files/bbport/bionic-local.sh || true'"
md5sum "$rel/bionic/bin/bb-probe" "$rel/bionic/lib/libbbcpu.so" | sed 's/$/  (release)/'
