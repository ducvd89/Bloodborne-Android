#!/usr/bin/env bash
# The release APK: the app (build_frontend.sh) with the runtime archive (build_release_runtime.py) inside.
# Prerequisites: build_bionic.sh (the game process, bb-prepare), build_pkg_tool.sh android (ps4-pkg-tool),
# out/eboot.elf (for the patch files; not packaged) and the user-owned game icon (see build_frontend.sh).
# Also needs the pinned GPU drivers (fetch_bundled_drivers.sh, run here) and the Rebuild Tool's Android package (fetch_recomp_android.sh, run by
# build_frontend.sh; BB_RECOMP_ANDROID_TARBALL=<file> takes a local tarball of it, e.g. before the tool's release exists).
# BB_APK_NAME=<name>: the file name (without .apk) in out/release/, default Bloodborne-<versionName>.
# BB_TEST_BUILD=1: a test build of the current commit: versionName "<manifest version>-test.<short commit>" (for example 2.0.0-test.a1b2c3d), which
# the in-app update checker reads as the manifest version (text after the leading number is ignored: never a downgrade offer), same versionCode.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
version=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' tools/android/frontend/AndroidManifest.xml)
if [[ ${BB_TEST_BUILD:-0} == 1 ]]; then
    version=$version-test.$(git rev-parse --short=7 HEAD)
    export BB_VERSION_NAME=$version
fi
apk=out/release/${BB_APK_NAME:-Bloodborne-$version}.apk
# The three GPU drivers the app carries (build_frontend.sh puts them in the APK): downloaded and checked here so a
# release build never goes without them.
bash tools/android/fetch_bundled_drivers.sh
python3 tools/android/build_release_runtime.py
BB_RUNTIME_ARCHIVE="$PWD/out/release/runtime.tar.gz" bash tools/android/build_frontend.sh
cp out/thor/frontend/Bloodborne-Thor.apk "$apk"
"${BB_ANDROID_SDK:-$PWD/.local-deps/android/sdk}/build-tools/35.0.0/apksigner" verify --verbose "$apk"
(cd out/release && sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256")
echo "Release APK: $apk"
