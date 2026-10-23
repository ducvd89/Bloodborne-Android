#!/usr/bin/env bash
# Prepares a FEX source tree with the submodules the front end needs, without touching the checkout
# it comes from (a git worktree or a clone whose FEX submodules are not initialised).
#   tools/recomp/fetch-fex.sh <FEX git checkout> <dest dir>
# Tracked files come from `git archive`; each needed submodule is fetched at the commit the checkout
# pins (shallow, from the URL in .gitmodules). Needs network the first time; later runs are no-ops.
set -euo pipefail
src=$(realpath "$1"); dest=$2
need=(External/fmt External/xxhash External/unordered_dense External/range-v3 Source/Common/cpp-optparse
      External/vixl External/Vulkan-Headers External/drm-headers External/jemalloc_glibc External/rpmalloc)
mkdir -p "$dest"
if [[ ! -f $dest/.bb-fex-head ]]; then
    git -C "$src" archive HEAD | tar -x -C "$dest"
    git -C "$src" rev-parse HEAD > "$dest/.bb-fex-head"
fi
fetch() {
    local path=$1 sha url
    [[ -e $dest/$path/.bb-fetched ]] && return
    sha=$(git -C "$src" ls-tree HEAD "$path" | awk '{print $3}')
    url=$(git -C "$src" show HEAD:.gitmodules | awk -v p="$path" '$1=="path"{cur=($3==p)} cur&&$1=="url"{print $3}')
    rm -rf "${dest:?}/$path"; mkdir -p "$dest/$path"
    git -C "$dest/$path" init -q && git -C "$dest/$path" fetch -q --depth 1 "$url" "$sha" && git -C "$dest/$path" checkout -q FETCH_HEAD
    rm -rf "$dest/$path/.git"; touch "$dest/$path/.bb-fetched"
    echo "fetched $path @ ${sha:0:10}"
}
for p in "${need[@]}"; do fetch "$p" & done
wait
