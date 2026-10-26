#!/usr/bin/env bash
# The Rebuild Tool's Android package (the command line build of the tool, run on the phone by AotBuildService): libbbrecomp.so (an Android NDK
# executable on the phone's bionic), bundle/, variants/, licenses/, VERSION, README-ANDROID.md. build_frontend.sh puts libbbrecomp.so in lib/arm64-v8a and the rest in assets/recomp.
# Pinned: the tarball is checked against its sha256 on every run. Source: the tool's release (tag v1.2.1, bb-recomp-android-aarch64.tar.xz), or
# BB_RECOMP_ANDROID_TARBALL=<file> (a local build of the tool: arm64/package-android.sh, out/dist/; the sha256 must be the pinned one, or
# BB_RECOMP_ANDROID_SHA256=<sha256> for a test build of an unreleased tool).
# Output: .local-deps/android/recomp/android/ (libbbrecomp.so, bundle/, variants/, licenses/, VERSION).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sha=${BB_RECOMP_ANDROID_SHA256:-7d1a9f075b538aa922b7e40bafeb6908dde9f436124f472b154c1164763f9b87}
url=https://github.com/ducvd89/Bloodborne-android-rebuild-tool/releases/download/v1.2.1/bb-recomp-android-aarch64.tar.xz
out=$PWD/.local-deps/android/recomp
mkdir -p "$out"
tarball=$out/bb-recomp-android-aarch64.tar.xz
if [[ -n ${BB_RECOMP_ANDROID_TARBALL:-} ]]; then
    cp "$BB_RECOMP_ANDROID_TARBALL" "$tarball.part"
elif [[ -f $tarball ]] && echo "$sha  $tarball" | sha256sum -c --quiet - 2>/dev/null; then
    cp "$tarball" "$tarball.part"
else
    echo "fetch   bb-recomp-android-aarch64.tar.xz"
    curl -sfL --retry 3 -o "$tarball.part" "$url" || { rm -f "$tarball.part"; echo "Cannot download $url (BB_RECOMP_ANDROID_TARBALL=<file> uses a local one)" >&2; exit 1; }
fi
if ! echo "$sha  $tarball.part" | sha256sum -c --quiet - 2>/dev/null; then
    echo "sha256 mismatch for the Rebuild Tool package (expected $sha, got $(sha256sum "$tarball.part" | cut -d' ' -f1))" >&2
    rm -f "$tarball.part"; exit 1
fi
mv "$tarball.part" "$tarball"
if [[ ! -x $out/android/libbbrecomp.so || ! -f $out/android/.sha256 || $(cat "$out/android/.sha256") != "$sha" ]]; then
    rm -rf "$out/android"
    tar -C "$out" -xJf "$tarball"
    echo "$sha" > "$out/android/.sha256"
fi
[[ -x $out/android/libbbrecomp.so && -d $out/android/bundle && -d $out/android/variants && -f $out/android/VERSION ]] \
    || { echo "The Rebuild Tool package is incomplete: $out/android" >&2; exit 1; }
echo "Rebuild Tool package: $(head -1 "$out/android/VERSION") in $out/android"
