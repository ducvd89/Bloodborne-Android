#!/usr/bin/env bash
# Third-party libraries for the Bionic (Android NDK) build, into .local-deps/android/bionic-prefix.
# Versions match the Debian trixie ones the former glibc Android build used (and the PC build).
# Sources are downloaded to .local-deps/android/bionic-src, builds go to bionic-build.
# Host-side prerequisites: curl, tar, cmake, ninja, make, python3.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD/.local-deps/android
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
prefix=$root/bionic-prefix src=$root/bionic-src bld=$root/bionic-build
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo "No NDK: $ndk" >&2; exit 1; }
mkdir -p "$prefix" "$src" "$bld"
J=${BB_JOBS:-22}
run() { taskset -c 2-23 nice -n 19 "$@"; }

fetch() { # name url archive-extension
    local dir=$src/$1
    [[ -d $dir ]] && return
    curl -fsSL "$2" -o "$src/$1.tar" && mkdir -p "$dir.tmp" && tar -xf "$src/$1.tar" -C "$dir.tmp" --strip-components=1 \
        && mv "$dir.tmp" "$dir" && rm "$src/$1.tar"
}
fetch_sha() { # name url sha256: like fetch, but the archive must match the pinned checksum
    local dir=$src/$1 tmp=$src/$1.tar
    [[ -d $dir ]] && return
    curl -fsSL "$2" -o "$tmp"
    echo "$3  $tmp" | sha256sum -c --quiet - || { echo "Checksum mismatch: $2" >&2; rm -f "$tmp"; exit 1; }
    mkdir -p "$dir.tmp" && tar -xf "$tmp" -C "$dir.tmp" --strip-components=1 && mv "$dir.tmp" "$dir" && rm "$tmp"
}
cm() { # name source-dir extra cmake args...
    local name=$1 sdir=$2; shift 2
    [[ -f $prefix/.done-$name ]] && return
    cmake -S "$sdir" -B "$bld/$name" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" "$@" >/dev/null
    run cmake --build "$bld/$name" -j "$J" >"$bld/$name.log" 2>&1 || { tail -30 "$bld/$name.log" >&2; exit 1; }
    cmake --install "$bld/$name" >/dev/null
    touch "$prefix/.done-$name"
}

fetch fmt-10.1.1 https://github.com/fmtlib/fmt/archive/refs/tags/10.1.1.tar.gz
fetch zycore-1.5.2 https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz
fetch zydis-4.1.1 https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz
fetch xxhash-0.8.3 https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
fetch miniz-3.1.2 https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
fetch SDL3-3.2.10 https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz
fetch Vulkan-Headers-1.4.363 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz
fetch SPIRV-Headers-1.4.309 https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.309.0.tar.gz
fetch ffmpeg-7.1.5 https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz

# Header-only: vulkan-hpp (the NDK ships Vulkan 1.3.275 C headers without it) and SPIR-V.
cm vulkan-headers "$src/Vulkan-Headers-1.4.363"
cm spirv-headers "$src/SPIRV-Headers-1.4.309"
cm fmt "$src/fmt-10.1.1" -DFMT_TEST=OFF -DFMT_DOC=OFF
cm zycore "$src/zycore-1.5.2" -DBUILD_SHARED_LIBS=OFF
# The release tarball lacks the zycore submodule: build against the same Zycore source tree.
[[ -f $src/zydis-4.1.1/dependencies/zycore/CMakeLists.txt ]] || cp -a "$src/zycore-1.5.2/." "$src/zydis-4.1.1/dependencies/zycore/"
cm zydis "$src/zydis-4.1.1" -DBUILD_SHARED_LIBS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF \
    -DZYDIS_BUILD_DOXYGEN=OFF
cm xxhash "$src/xxhash-0.8.3/cmake_unofficial" -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cm miniz "$src/miniz-3.1.2" -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON
# SDL3: shared (bb-probe and libbbgpu must share one SDL), Android backend only; the window, audio
# and input paths are phase 2 (an exec'd process has no SDLActivity/JNI behind SDL's Android drivers).
cm sdl3 "$src/SDL3-3.2.10" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

# FFmpeg (avplayer/videodec): static, PIC, no programs, only built-in codecs/demuxers.
if [[ ! -f $prefix/.done-ffmpeg ]]; then
    tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
    mkdir -p "$bld/ffmpeg" && pushd "$bld/ffmpeg" >/dev/null
    "$src/ffmpeg-7.1.5/configure" --prefix="$prefix" --enable-cross-compile --target-os=android --arch=aarch64 \
        --cc="$tc/bin/aarch64-linux-android$api-clang" --cxx="$tc/bin/aarch64-linux-android$api-clang++" \
        --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-network --disable-autodetect --disable-mediacodec --disable-jni >"$bld/ffmpeg-configure.log" 2>&1 \
        || { tail -30 "$bld/ffmpeg-configure.log" >&2; exit 1; }
    run make -j"$J" >"$bld/ffmpeg.log" 2>&1 || { tail -30 "$bld/ffmpeg.log" >&2; exit 1; }
    make install >/dev/null
    # configure's Android target links libandroid/libmediandk for MediaCodec/JNI, both disabled here.
    sed -i 's/ -landroid -lmediandk//' "$prefix"/lib/pkgconfig/libavutil.pc
    popd >/dev/null
    touch "$prefix/.done-ffmpeg"
fi

# ---- Online module (libbbnet20.so): OpenSSL, protobuf (+ a host protoc of the same version), nlohmann_json,
# miniupnpc, a CA bundle. All static, so libbbnet20.so needs only Bionic's own libraries. zlib is the NDK's libz.a.
fetch_sha openssl-3.5.4 https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz \
    967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
# protobuf 3.21.12 (the Debian trixie version): the last release without an abseil dependency. The shadNet
# protocol's generated code must come from a protoc of exactly the runtime library's version, so the host protoc
# is built from the same source into host-tools/ (not into the arm64 prefix).
fetch_sha protobuf-3.21.12 https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-all-21.12.tar.gz \
    2c6a36c7b5a55accae063667ef3c55f2642e67476d96d355ff0acb13dbb47f09
fetch_sha nlohmann-json-3.12.0 https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz \
    4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
fetch_sha miniupnpc-2.3.3 https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz \
    8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222

if [[ ! -f $root/host-tools/.done-protoc ]]; then
    cmake -S "$src/protobuf-3.21.12/cmake" -B "$bld/protoc-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$root/host-tools" -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=ON -Dprotobuf_WITH_ZLIB=OFF >/dev/null
    run cmake --build "$bld/protoc-host" -j "$J" --target protoc >"$bld/protoc-host.log" 2>&1 || { tail -30 "$bld/protoc-host.log" >&2; exit 1; }
    mkdir -p "$root/host-tools/bin" && cp "$bld/protoc-host/protoc" "$root/host-tools/bin/protoc"
    touch "$root/host-tools/.done-protoc"
fi
[[ $("$root/host-tools/bin/protoc" --version) == "libprotoc 3.21.12" ]] || { echo "host protoc is not 3.21.12" >&2; exit 1; }
cm protobuf "$src/protobuf-3.21.12/cmake" -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
    -Dprotobuf_WITH_ZLIB=OFF -Dprotobuf_BUILD_LIBPROTOC=OFF
cm nlohmann_json "$src/nlohmann-json-3.12.0" -DJSON_BuildTests=OFF -DJSON_Install=ON
cm miniupnpc "$src/miniupnpc-2.3.3/miniupnpc" -DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF \
    -DUPNPC_BUILD_SAMPLE=OFF -DUPNPC_NO_INSTALL=OFF

# OpenSSL: no shared libraries (libssl.a / libcrypto.a go into libbbnet20.so), PIC, API level as the other libraries.
if [[ ! -f $prefix/.done-openssl ]]; then
    tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
    rm -rf "$bld/openssl" && mkdir -p "$bld/openssl" && pushd "$bld/openssl" >/dev/null
    ANDROID_NDK_ROOT=$ndk PATH=$tc/bin:$PATH "$src/openssl-3.5.4/Configure" android-arm64 -D__ANDROID_API__=$api no-shared no-tests no-docs \
        no-apps no-dso no-ui-console -fPIC --prefix="$prefix" --openssldir=/system/etc/security/cacerts >"$bld/openssl-configure.log" 2>&1 \
        || { tail -30 "$bld/openssl-configure.log" >&2; exit 1; }
    ANDROID_NDK_ROOT=$ndk PATH=$tc/bin:$PATH run make -j"$J" build_libs >"$bld/openssl.log" 2>&1 || { tail -30 "$bld/openssl.log" >&2; exit 1; }
    ANDROID_NDK_ROOT=$ndk PATH=$tc/bin:$PATH make install_dev >>"$bld/openssl.log" 2>&1 || { tail -30 "$bld/openssl.log" >&2; exit 1; }
    popd >/dev/null
    touch "$prefix/.done-openssl"
fi

# Mozilla's CA bundle via curl.se (the dated file does not change), shipped in the runtime for TLS (BB_CA_BUNDLE).
cacert_date=2026-09-25
cacert_sha=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
if [[ ! -f $prefix/share/cacert.pem ]]; then
    mkdir -p "$prefix/share"
    curl -fsSL "https://curl.se/ca/cacert-$cacert_date.pem" -o "$prefix/share/cacert.pem.tmp"
    echo "$cacert_sha  $prefix/share/cacert.pem.tmp" | sha256sum -c --quiet - || { echo "cacert.pem checksum mismatch" >&2; rm -f "$prefix/share/cacert.pem.tmp"; exit 1; }
    mv "$prefix/share/cacert.pem.tmp" "$prefix/share/cacert.pem"
fi
echo "Bionic deps in $prefix"
