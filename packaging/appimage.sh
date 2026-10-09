#!/usr/bin/env bash
# Builds dist/Bloodborne-bbport-<arch>.AppImage (x86_64, aarch64): the port's current build (run
# build.sh first), its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, bbport.ini): ~/.local/share/bbport (BB_DATA_DIR).
# BB_MANGOHUD_SRC=<MangoHud tree>: bundle that MangoHud (and its MangoHud/MangoHud.conf) instead
# of nixpkgs' release (packaging/default.nix).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
arch=$(uname -m)
libs=(out/bb-probe out/gpu/libbbgpu.so)
if [[ $arch == aarch64 ]]; then
    [[ -f out/fex/libbbcpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
    libs+=(out/fex/libbbcpu.so)
fi
# NIX: the nix command (default nix; e.g. "nix-portable nix" without a system Nix).
# BB_NIXPKGS: the nixpkgs the build's nix-shell used (-I nixpkgs=...), when not <nixpkgs>.
read -r -a nix <<< "${NIX:-nix}"
include=()
if [[ -n ${BB_NIXPKGS:-} ]]; then include=(-I "nixpkgs=$BB_NIXPKGS"); fi
root=$PWD
# The libraries' store paths (RUNPATH entries and their closures come along).
{
    echo '['
    for elf in "${libs[@]}"; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && "${nix[@]}" bundle --impure "${include[@]}" --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
image=$(readlink "$work/bbport.AppImage")
# nix-portable: its /nix/store is ~/.nix-portable/nix/store outside its sandbox.
if [[ ! -e $image && -e ${NP_LOCATION:-$HOME}/.nix-portable$image ]]; then
    image=${NP_LOCATION:-$HOME}/.nix-portable$image
fi
install -m755 "$image" "dist/Bloodborne-bbport-$arch.AppImage"
ls -lh "dist/Bloodborne-bbport-$arch.AppImage"
