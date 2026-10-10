#!/usr/bin/env bash
# Turnip (Mesa's Adreno Vulkan driver) for the Thor's Debian trixie arm64 rootfs, KGSL only,
# cross-compiled with Clang, in one of two variants (README in tools/android/mesa-patches):
#   main: Mesa at the pinned commit + mesa-patches/*.patch -> out/arm64/turnip
#         (libvulkan_freedreno.so, freedreno_icd.aarch64.json, drirc.d/)
#   gen8: the turnip/gen8 branch (Adreno 8xx) + mesa-patches/gen8/*.patch (upstream KGSL fixes it
#         predates) + mesa-patches/*.patch -> out/arm64/turnip-gen8 (libvulkan_freedreno_gen8.so,
#         freedreno_gen8_icd.aarch64.json); run-thor.sh picks it on Adreno 8xx.
# build_release_runtime.py ships both.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
variant=${1:-main}
case $variant in
    main)
        repo=https://gitlab.freedesktop.org/mesa/mesa.git
        MESA_COMMIT=${MESA_COMMIT:-35b085c4c067f198ce0b6437d94678e47b9c645f}
        patches=(tools/android/mesa-patches/*.patch)
        suffix= ;;
    gen8)
        repo=https://github.com/whitebelyash/mesa-unified.git
        MESA_COMMIT=${MESA_COMMIT:-9c7e022677dfa3abb356c2b6732cbd2e25783d01}
        patches=(tools/android/mesa-patches/gen8/*.patch tools/android/mesa-patches/*.patch)
        suffix=_gen8 ;;
    *) echo 'usage: build_turnip.sh [main|gen8]' >&2; exit 2 ;;
esac
rootfs=/data/data/com.ducvd89.bloodborne/files/rootfs
deps=$PWD/.local-deps/android
sysroot=$deps/mesa-sysroot
src=$deps/src/mesa${suffix/_/-}
build=$PWD/out/arm64/turnip${suffix/_/-}-build
out=$PWD/out/arm64/turnip${suffix/_/-}

# Only libraries the rootfs has: no xcb-keysyms (it would add a NEEDED the Thor lacks).
if [[ ! -f $sysroot/packages.json ]]; then
    python3 tools/android/fetch_arm64_sysroot.py --out "$sysroot" --packages \
        libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev zlib1g-dev libzstd-dev \
        libexpat1-dev libxcb1-dev libx11-xcb-dev libxcb-xfixes0-dev libxcb-randr0-dev \
        libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-shm0-dev libxshmfence-dev \
        libx11-dev libxrandr-dev libdrm-dev
fi
rm -f "$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig/xcb-keysyms.pc"

# Mesa's build tools (meson, mako) in a local virtual environment, not the host's Python.
venv=$deps/mesa-venv
if [[ ! -x $venv/bin/meson ]]; then
    python3 -m venv "$venv"
    "$venv/bin/python" -m pip install -q meson mako pyyaml packaging
fi
export PATH="$venv/bin:$PATH"

# A pristine tree at the pinned commit, then the patches.
if [[ ! -d $src/.git ]]; then
    git init -q "$src"
    git -C "$src" remote add origin "$repo"
fi
git -C "$src" fetch -q --depth 1 origin "$MESA_COMMIT"
git -C "$src" checkout -q --force FETCH_HEAD
git -C "$src" clean -q -fdx
for patch in "${patches[@]}"; do
    git -C "$src" apply "$PWD/$patch"
done

# cpu 'aarch64': the manifest is then freedreno_icd.aarch64.json with an absolute library_path.
cross=$build.cross
mkdir -p "$(dirname "$cross")"
cat > "$cross" <<EOF
[binaries]
c = ['clang', '--target=aarch64-linux-gnu', '--sysroot=$sysroot']
cpp = ['clang++', '--target=aarch64-linux-gnu', '--sysroot=$sysroot']
c_ld = 'lld'
cpp_ld = 'lld'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-O2']
cpp_args = ['-O2']
[properties]
sys_root = '$sysroot'
pkg_config_libdir = ['$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig', '$sysroot/usr/share/pkgconfig']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
rm -rf "$build" "$out"
meson setup "$build" "$src" --cross-file "$cross" --prefix="$rootfs/usr" \
    --libdir=lib/aarch64-linux-gnu --wrap-mode=nofallback --buildtype=release -Db_ndebug=true \
    -Dplatforms=x11 -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers= \
    -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dzstd=enabled -Dexpat=enabled -Dvulkan-layers= -Dtools= -Dbuild-tests=false \
    > "$build.log" 2>&1 || { tail -40 "$build.log" >&2; exit 1; }
ninja -C "$build" >> "$build.log" 2>&1 || { tail -40 "$build.log" >&2; exit 1; }
DESTDIR="$build/install" meson install -C "$build" --no-rebuild >> "$build.log" 2>&1

installed=$build/install$rootfs/usr
mkdir -p "$out"
library=libvulkan_freedreno$suffix.so
manifest=freedreno${suffix}_icd.aarch64.json
llvm-strip --strip-debug -o "$out/$library" \
    "$installed/lib/aarch64-linux-gnu/libvulkan_freedreno.so"
sed "s|/libvulkan_freedreno.so\"|/$library\"|" \
    "$installed/share/vulkan/icd.d/freedreno_icd.aarch64.json" > "$out/$manifest"
grep -q "/$library\"" "$out/$manifest" || { echo "Manifest does not name $library" >&2; exit 1; }
cp -r "$installed/share/drirc.d" "$out/drirc.d"
if readelf -d "$out/$library" | grep -q keysyms; then
    echo "$library needs libxcb-keysyms, which the rootfs lacks" >&2
    exit 1
fi
echo "Turnip $variant: $out (Mesa $(git -C "$src" rev-parse --short HEAD) + ${#patches[@]} patches)"
