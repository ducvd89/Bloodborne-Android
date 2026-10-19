#!/usr/bin/env bash
# Build the Bloodborne Android app (Java UI + libbbpresent.so, the frame path of the Bionic game process).
# With BB_RUNTIME_ARCHIVE (build_release_runtime.py) the runtime archive goes inside the APK as an asset.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${BB_ANDROID_SDK:-"$PWD/.local-deps/android/sdk"}
build_tools="$sdk/build-tools/35.0.0"
android_jar="$sdk/platforms/android-35/android.jar"
local_jdk="$PWD/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk"
if [[ -x $local_jdk/bin/javac ]]; then
    export JAVA_HOME="$local_jdk" PATH="$local_jdk/bin:$PATH"
fi
out="$PWD/out/thor/frontend"
rm -rf "$out/classes" "$out/dex" "$out/res" "$out/assets" "$out/lib"
mkdir -p "$out/classes" "$out/dex" "$out/res/drawable" "$out/assets"
cp -r tools/android/frontend/res/. "$out/res/"
if [[ -n ${BB_RUNTIME_ARCHIVE:-} ]]; then
    cp "$BB_RUNTIME_ARCHIVE" "$out/assets/runtime.payload"
    cp "${BB_RUNTIME_ARCHIVE%.tar.gz}.sha256" "$out/assets/runtime.sha256"
fi
# Use the user's own Bloodborne icon locally; game art is not committed to this repository.
python3 - "$out" <<'PY'
import os, sys
from pathlib import Path
from PIL import Image
out = Path(sys.argv[1])
icon = Path('game/CUSA03173/sce_sys/icon0.dds')
if os.environ.get('BB_RUNTIME_ARCHIVE') and not icon.exists():
    raise SystemExit('Release build needs the user-owned game icon at game/CUSA03173/sce_sys/icon0.dds')
if icon.exists():
    (out/'res/drawable/icon.xml').unlink(missing_ok=True)
    Image.open(icon).save(out/'res/drawable/icon.png')
PY
# The GPU drivers inside the APK (assets/drivers/, installed by the app's Automatic GPU driver choice:
# AutoDriver.java, BundledDrivers.java) with their licence notices appended to THIRD_PARTY_LICENSES.txt.
# fetch_bundled_drivers.sh downloads and checks the pinned zips (BB_SKIP_DRIVERS=1: a development build
# without them, which uses the phone's own driver when Automatic cannot install one).
drivers=$PWD/.local-deps/android/drivers
if [[ ${BB_SKIP_DRIVERS:-} != 1 ]]; then
    bash tools/android/fetch_bundled_drivers.sh >/dev/null \
        || { echo "The bundled GPU drivers are missing or do not match their sha256: bash tools/android/fetch_bundled_drivers.sh (BB_SKIP_DRIVERS=1 builds without them)" >&2; exit 1; }
    mkdir -p "$out/assets/drivers"
    cp "$drivers"/Turnip_Gen8_V37.zip "$drivers"/PanVK-android-v3.2-universal-2a9358a.zip "$out/assets/drivers/"
    # Our Turnip 26.3.0 zip is not packed (V37 is the Adreno driver); it gives the Mesa license text.
    { cat tools/android/frontend/THIRD_PARTY_LICENSES.txt tools/android/frontend/DRIVERS_NOTICE.txt
      unzip -p "$drivers/Turnip-android-main-26.3.0-35b085c.zip" LICENSE-mesa.txt
      printf '\n--------------------------------------------------------------------------------\nGNU General Public License, version 3 (StevenMXZ/Adreno-Tools-Drivers)\n--------------------------------------------------------------------------------\n\n'
      cat "$drivers/GPL-3.0.txt"
    } > "$out/assets/THIRD_PARTY_LICENSES.txt"
else
    cp tools/android/frontend/THIRD_PARTY_LICENSES.txt "$out/assets/"
fi
# The libraries of the Steam sign-in that downloads Lossless.dll (SteamDllDialog.java: JavaSteam and its
# dependencies, ZXing, zstd-jni) and of the Mods menu (ModArchive.java: Commons Compress and xz).
steam_libs=$PWD/.local-deps/android/steam-libs
[[ -f $steam_libs/jars.txt ]] || bash tools/android/fetch_steam_libs.sh
mapfile -t lib_jars < <(sed "s|^|$steam_libs/jars/|" "$steam_libs/jars.txt")
mkdir -p "$out/lib/arm64-v8a" && cp "$steam_libs"/jni/*.so "$out/lib/arm64-v8a/"
# libbbpresent.so: the Bionic game's frame path (AHardwareBuffers -> ASurfaceControl), built with the
# NDK at minSdk 28 (the API 29 calls are looked up at run time). NEEDED: libandroid, liblog,
# libnativewindow and libc/libm/libdl only.
ndk=${BB_NDK:-$PWD/.local-deps/android/sdk/ndk/29.0.14206865}
[[ -x $ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang ]] \
    || { echo "No NDK: $ndk (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang" -fuse-ld=lld -std=c11 -O2 -g \
    -Wall -Wextra -Werror -fPIC -shared -Wl,-soname,libbbpresent.so -Wl,-z,max-page-size=16384 \
    -Itools/android/present tools/android/frontend/jni/bbpresent/bbpresent.c \
    -landroid -llog -lnativewindow -o "$out/lib/arm64-v8a/libbbpresent.so"
mapfile -t sources < <(rg --files tools/android/frontend/src -g '*.java')
javac --release 8 -classpath "$android_jar:$(IFS=:; echo "${lib_jars[*]}")" -d "$out/classes" "${sources[@]}"
mapfile -t classes < <(rg --files "$out/classes" -g '*.class')
"$build_tools/d8" --release --min-api 28 --lib "$android_jar" --output "$out/dex" "${classes[@]}" "${lib_jars[@]}"
"$build_tools/aapt" package -f -M tools/android/frontend/AndroidManifest.xml \
    -S "$out/res" -A "$out/assets" -I "$android_jar" -0 payload -0 zip -F "$out/unsigned.apk"
python3 - "$out" <<'PY'
import sys, zipfile
from pathlib import Path
out = Path(sys.argv[1])
with zipfile.ZipFile(out/'unsigned.apk','a',compression=zipfile.ZIP_DEFLATED) as z:
    for p in sorted((out/'dex').glob('*.dex')): z.write(p,p.name)
    for p in sorted((out/'lib').rglob('*.so')): z.write(p,str(p.relative_to(out)))
PY
"$build_tools/zipalign" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
key="$PWD/.local-deps/android/bloodborne-thor.keystore"
if [[ ! -f $key ]]; then
    keytool -genkeypair -keystore "$key" -storepass android -keypass android \
        -alias thor -dname 'CN=Bloodborne Thor local build' -keyalg RSA -keysize 2048 -validity 10000
fi
"$build_tools/apksigner" sign --ks "$key" --ks-pass pass:android --key-pass pass:android \
    --out "$out/Bloodborne-Thor.apk" "$out/aligned.apk"
"$build_tools/apksigner" verify --verbose "$out/Bloodborne-Thor.apk"
echo "Built $out/Bloodborne-Thor.apk"
