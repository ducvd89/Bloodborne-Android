#!/usr/bin/env bash
# The GPU drivers the app carries in its APK (assets/drivers/, tools/android/build_frontend.sh), which its
# "Automatic" GPU driver choice installs on the first start (AutoDriver.java, BundledDrivers.java):
#   Adreno          Turnip Gen8 V37 by StevenMXZ for 6xx/7xx and 8xx alike (Mesa Turnip; the build repository
#                   is GPL-3.0); other Turnip builds are download options on this project's releases
#   Mali            PanVK v3.2 (ours, a universal CSF build; Mesa, MIT; needs Android 15)
# plus the GPL-3.0 text of the notices and our Turnip 26.3.0 zip, not packed: build_frontend.sh takes the
# Mesa license text from it. Pinned: each file is checked against its sha256 on every run, a
# cached copy that does not match is downloaded again, a download that does not match fails the build.
# Output: .local-deps/android/drivers/{*.zip,GPL-3.0.txt}; BundledDrivers.java names the same zips.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$PWD/.local-deps/android/drivers
mkdir -p "$out"

# name  sha256  url. Turnip_Gen8_V37.zip is StevenMXZ's build (https://github.com/StevenMXZ/Adreno-Tools-Drivers,
# tag v37), mirrored byte for byte on this project's release.
files=(
"Turnip-android-main-26.3.0-35b085c.zip 5d08fb35c4add918ff6e3c0e435ae0fcc7e9b848173db95377c3ff20b3e7709b https://github.com/ducvd89/Bloodborne-Android/releases/download/turnip-android-main-26.3.0-35b085c/Turnip-android-main-26.3.0-35b085c.zip"
"Turnip_Gen8_V37.zip d0335542ec996fe33e83dfbec0d86aed301da95d22b671a88b40a4722d115b48 https://github.com/ducvd89/Bloodborne-Android/releases/download/turnip-android-gen8-26.3.0-9c7e022/Turnip_Gen8_V37.zip"
"PanVK-android-v3.2-universal-2a9358a.zip 68d4eccc1442cb0bef08228c90127388e68bf6087898abc492c717b9a06ca61f https://github.com/ducvd89/Bloodborne-Android/releases/download/panvk-android-v3.2-2a9358a/PanVK-android-v3.2-universal-2a9358a.zip"
"GPL-3.0.txt 3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986 https://www.gnu.org/licenses/gpl-3.0.txt"
)
for entry in "${files[@]}"; do
    read -r name sha url <<< "$entry"
    file=$out/$name
    if [[ -f $file ]] && echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        echo "ok      $name"
        continue
    fi
    rm -f "$file"
    echo "fetch   $name"
    curl -sfL --retry 3 -o "$file.part" "$url" || { rm -f "$file.part"; echo "Cannot download $url" >&2; exit 1; }
    if ! echo "$sha  $file.part" | sha256sum -c --quiet - 2>/dev/null; then
        echo "sha256 mismatch for $name from $url (expected $sha, got $(sha256sum "$file.part" | cut -d' ' -f1))" >&2
        rm -f "$file.part"; exit 1
    fi
    mv "$file.part" "$file"
done
echo "Bundled GPU drivers in $out"
