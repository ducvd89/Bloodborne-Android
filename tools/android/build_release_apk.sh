#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
python3 tools/android/build_release_runtime.py
BB_RUNTIME_ARCHIVE="$PWD/out/release/runtime.tar.gz" bash tools/android/build_frontend.sh
cp out/thor/frontend/Bloodborne-Thor.apk out/release/Bloodborne-0.1.apk
"${BB_ANDROID_SDK:-$PWD/.local-deps/android/sdk}/build-tools/35.0.0/apksigner" verify \
    --verbose out/release/Bloodborne-0.1.apk
echo "Release APK: out/release/Bloodborne-0.1.apk"
