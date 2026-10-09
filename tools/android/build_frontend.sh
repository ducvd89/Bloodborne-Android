#!/usr/bin/env bash
# Build a Bloodborne-only Android UI using the installed runtime's native bridge ABI.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${BB_ANDROID_SDK:-"$PWD/.local-deps/android/sdk"}
build_tools="$sdk/build-tools/35.0.0"
android_jar="$sdk/platforms/android-35/android.jar"
runtime_apk=${BB_FEXDROID_APK:-"$PWD/.local-deps/android/fexdroid-legacy.apk"}
local_jdk="$PWD/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk"
if [[ -x $local_jdk/bin/javac ]]; then
    export JAVA_HOME="$local_jdk" PATH="$local_jdk/bin:$PATH"
fi
out="$PWD/out/thor/frontend"
rm -rf "$out/classes" "$out/dex" "$out/res" "$out/assets"
mkdir -p "$out/classes" "$out/dex" "$out/res/drawable" "$out/assets"
cp -r tools/android/frontend/res/. "$out/res/"
if [[ -n ${BB_RUNTIME_ARCHIVE:-} ]]; then
    cp "$BB_RUNTIME_ARCHIVE" "$out/assets/runtime.payload"
    cp "${BB_RUNTIME_ARCHIVE%.tar.gz}.sha256" "$out/assets/runtime.sha256"
    cp "${BB_RUNTIME_ARCHIVE%.tar.gz}.python-packages.txt" "$out/assets/python-packages.txt"
fi
# Use the user's own Bloodborne icon locally; game art is not committed to this repository.
python3 - "$runtime_apk" "$out" <<'PY'
import hashlib, sys, zipfile
from pathlib import Path
from PIL import Image
apk, out = Path(sys.argv[1]), Path(sys.argv[2])
assert hashlib.file_digest(apk.open('rb'), 'sha256').hexdigest() == '2074fcee551dab0c6b0719c3f90f6389e517f194aaabacb5b0080bb2977ed847'
icon = Path('game/CUSA03173/sce_sys/icon0.dds')
if __import__('os').environ.get('BB_RUNTIME_ARCHIVE') and not icon.exists():
    raise SystemExit('Release build needs the user-owned game icon at game/CUSA03173/sce_sys/icon0.dds')
if icon.exists():
    (out/'res/drawable/icon.xml').unlink(missing_ok=True)
    Image.open(icon).save(out/'res/drawable/icon.png')
with zipfile.ZipFile(apk) as z:
    for name in ('lib/arm64-v8a/libfxdisplay.so', 'lib/arm64-v8a/libfxio.so'):
        z.extract(name, out)
PY
cp tools/android/frontend/THIRD_PARTY_LICENSES.txt "$out/assets/"
mapfile -t sources < <(rg --files tools/android/frontend/src -g '*.java')
javac --release 8 -classpath "$android_jar" -d "$out/classes" "${sources[@]}"
mapfile -t classes < <(rg --files "$out/classes" -g '*.class')
"$build_tools/d8" --min-api 28 --lib "$android_jar" --output "$out/dex" "${classes[@]}"
"$build_tools/aapt" package -f -M tools/android/frontend/AndroidManifest.xml \
    -S "$out/res" -A "$out/assets" -I "$android_jar" -0 payload -F "$out/unsigned.apk"
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
