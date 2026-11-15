#!/usr/bin/env bash
# The MAKO-FG V1.24 model files the APK carries (assets/mako-fg/, build_frontend.sh) for Bloodframe 2.0's
# experimental MAKO-FG backend; the app copies them into bbport/mako-fg on start (MakoModel.java).
# "MAKO FG by Eugenio Segala", CC BY-NC 4.0 (non-commercial use only; the release also passes on the
# X4K1000FPS training data's non-commercial research-and-education condition). The files are unchanged;
# their LICENSE ships next to them and in THIRD_PARTY_LICENSES.txt.
# Pinned: the model's Hugging Face release at a fixed revision, every file checked against the SHA-256 of
# Bloodframe's mako/release.json (the same pin). A cached copy that does not match is downloaded again; a
# download that does not match fails the build.
# Output: .local-deps/android/mako-fg/{LICENSE,model/*}
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$PWD/.local-deps/android/mako-fg
revision=18d7d690515a658c993f84029b245cb710cb83f2
base=https://huggingface.co/eugeniosegala/MAKO-FG/resolve/$revision
mkdir -p "$out/model"

# path  sha256
files=(
"LICENSE e990112837dcd5af880065d292f22f394f87c69912f957eabc54dc0285c6a7fc"
"model/weights.bin 08ed3acfefa71a6d29a208e9a64c73d6593bfc2e83eb0230fe4f8d62ff7ea781"
"model/transport.spv d2285a08b046fcb40ef63254589df3e1defa43e11e8f1026dd9a10c922db992d"
"model/ops.spv 431ec6d0224111435a62e9e1235d844cc9180083e9e5c79461b47d46a2b950ea"
"model/mixture.spv f5395a370e03783273c68295c1b11215e8c0c7c978e07299b68d7e843d6c0e8e"
)
for entry in "${files[@]}"; do
    read -r name sha <<< "$entry"
    file=$out/$name
    if [[ -f $file ]] && echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        echo "ok      $name"
        continue
    fi
    rm -f "$file"
    echo "fetch   $name"
    curl -sfL --retry 3 -o "$file.part" "$base/$name" || { rm -f "$file.part"; echo "Cannot download $base/$name" >&2; exit 1; }
    if ! echo "$sha  $file.part" | sha256sum -c --quiet - 2>/dev/null; then
        echo "sha256 mismatch for $name (expected $sha, got $(sha256sum "$file.part" | cut -d' ' -f1))" >&2
        rm -f "$file.part"; exit 1
    fi
    mv "$file.part" "$file"
done
echo "MAKO-FG model in $out"
