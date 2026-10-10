#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
version=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' tools/android/frontend/AndroidManifest.xml)
apk=out/release/Bloodborne-$version.apk
python3 tools/android/build_release_runtime.py
BB_RUNTIME_ARCHIVE="$PWD/out/release/runtime.tar.gz" bash tools/android/build_frontend.sh
cp out/thor/frontend/Bloodborne-Thor.apk "$apk"
"${BB_ANDROID_SDK:-$PWD/.local-deps/android/sdk}/build-tools/35.0.0/apksigner" verify --verbose "$apk"
(cd out/release && sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256")
echo "Release APK: $apk"
