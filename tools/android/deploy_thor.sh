#!/usr/bin/env bash
# Deploy the Linux experiment into the installed, debuggable fexdroid app.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
serial=${1:?usage: deploy_thor.sh ADB_SERIAL [bundle-directory]}
bundle=${2:-out/thor/bundle}
app=ro.cobrabm.fexdroid
files=/data/data/$app/files
adb_cmd=(adb -s "$serial")
"${adb_cmd[@]}" shell run-as "$app" test -x "$files/rootfs/usr/bin/FEX"
"${adb_cmd[@]}" shell run-as "$app" mkdir -p "$files/bbport"
tar -C "$bundle" -cf - . | "${adb_cmd[@]}" shell -T run-as "$app" tar -xf - -C "$files/bbport"
# Use the runtime's matching x86 Vulkan thunk, not an emulated x86 driver.
"${adb_cmd[@]}" shell run-as "$app" cp \
    "$files/rootfs/usr/share/fex-emu/GuestThunks/libvulkan-guest.so" \
    "$files/bbport/rootfs/usr/lib/libvulkan.so.1"
"${adb_cmd[@]}" shell run-as "$app" ln -sfn "$files/rootfs/tmp" "$files/bbport/rootfs/tmp"
echo "Deployed Linux bundle to $serial. Game data is copied separately."
