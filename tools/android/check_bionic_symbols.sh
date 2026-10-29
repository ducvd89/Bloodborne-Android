#!/usr/bin/env bash
# Verifies the Bionic outputs: NEEDED entries are Bionic/NDK/our own libraries only, the executable
# is PIE with /system/bin/linker64, and every undefined symbol of every output is provided by the
# libraries it is linked against, the executable (runtime_* symbols libbbgpu takes from it), or
# Bionic itself. No libvulkan anywhere: the driver is dlopen'ed (gpu/shim/vk_driver_loader.cpp), and
# libbloodframe.so (Lossless Scaling, dlopen'ed by libbbgpu) gets its Vulkan entry point from libbbgpu.
# libbloodframe.so must export only the bb_lsfg_* C API. libbbnet20.so (online play, dlopen'ed by bb-probe with
# BB_ONLINE=1) is linked against libbbgpu.so and libbbcpu.so (both DT_NEEDED: bb-probe's own NEEDED libraries are not visible to a
# dlopen'ed library), takes runtime_* from bb-probe, has
# OpenSSL / protobuf / miniupnpc / zlib inside (static), and must export only the bbnet_* C API (bbnet.h).
# dlopen'ed modules are additionally checked the way Android's linker resolves them (see below).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=${BB_OUT:-out/bionic}
ndk=${BB_NDK:-$PWD/.local-deps/android/sdk/ndk/29.0.14206865}
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
api=${BB_ANDROID_API:-30}
sysroot_libs=$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/$api
fail=0
allowed='^(libc\.so|libm\.so|libdl\.so|liblog\.so|libandroid\.so|libnativewindow\.so|libc\+\+_shared\.so|libbbgpu\.so|libbbcpu\.so|libSDL3\.so)$'
defined() { "$llvm/llvm-nm" -D --defined-only "$@" 2>/dev/null | awk 'NF>=3{print $3}' | sed 's/@.*//'; }
provided=$(mktemp); trap 'rm -f "$provided"' EXIT
{ defined "$out/bin/bb-probe" "$out/bin/bb-prepare" "$out/bin/bb-texpack" "$out/lib/libbbgpu.so" "$out/lib/libbbcpu.so" "$out/lib/libbloodframe.so" "$out/lib/libc++_shared.so"
  for lib in libc.so libm.so libdl.so liblog.so libandroid.so libnativewindow.so; do defined "$sysroot_libs/$lib"; done; } | sort -u > "$provided"
for f in "$out/bin/bb-probe" "$out/bin/kgsl-chip-id" "$out/bin/bb-prepare" "$out/bin/bb-texpack" "$out/lib/libbbgpu.so" "$out/lib/libbbcpu.so" "$out/lib/libbloodframe.so" "$out/lib/libbbnet20.so"; do
    echo "== $f"
    "$llvm/llvm-readelf" -d "$f" | grep -E 'NEEDED|SONAME|RUNPATH' | sed 's/^ *0x[0-9a-f]* *//'
    for lib in $("$llvm/llvm-readelf" -d "$f" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        [[ $lib =~ $allowed ]] || { echo "  NOT ALLOWED: $lib"; fail=1; }
        [[ $lib == libvulkan.so ]] && { echo "  libvulkan.so must not be linked"; fail=1; }
    done
    case $f in *bin/*) "$llvm/llvm-readelf" -l "$f" | grep -E 'Type: DYN|interpreter' ; "$llvm/llvm-readelf" -h "$f" | grep -E 'Type:';; esac
    missing=$("$llvm/llvm-nm" -D --undefined-only "$f" | awk '{print $2}' | sed 's/@.*//' | sort -u | comm -23 - "$provided" || true)
    # Weak undefined (optional) symbols are not errors.
    weak=$("$llvm/llvm-nm" -D "$f" | awk '$1=="w"||$1=="v"{print $2}' | sed 's/@.*//' | sort -u)
    missing=$(comm -23 <(echo "$missing") <(echo "$weak") | grep -v '^$' || true)
    if [[ -n $missing ]]; then echo "  UNDEFINED:"; echo "$missing" | sed 's/^/    /'; fail=1; else echo "  no unresolved symbols"; fi
done
# dlopen'ed modules (runtime_net.c: libbbnet20.so; libbbgpu: libbloodframe.so): Android's linker resolves a dlopen'ed
# library's undefined symbols only from the global group (the executable itself, LD_PRELOADs, RTLD_GLOBAL libraries) and the
# library's OWN DT_NEEDED closure. The executable's DT_NEEDED libraries (libbbcpu.so) are NOT in that group, so a symbol that
# only lives there (guest_cpu_call) fails dlopen with "cannot locate symbol" unless the module itself has it as NEEDED.
# So for each module: undefined symbols must be defined in bb-probe's dynamic symbol table (the executable) or in the
# module's NEEDED closure (our libraries in lib/, the NDK sysroot's system libraries). Weak undefined symbols are optional.
needed_of() { "$llvm/llvm-readelf" -d "$1" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'; }
find_lib() { local d; for d in "$out/lib" "$sysroot_libs"; do [[ -f $d/$1 ]] && { echo "$d/$1"; return; }; done; return 0; }
closure_walk() { # $1 = ELF file; appends the NEEDED closure's files to $closure_list, unresolvable names to $closure_bad
    local n p
    for n in $(needed_of "$1"); do
        grep -qxF "$n" "$closure_seen" && continue
        echo "$n" >> "$closure_seen"
        p=$(find_lib "$n")
        if [[ -z $p ]]; then echo "$n" >> "$closure_bad"; continue; fi
        echo "$p" >> "$closure_list"
        closure_walk "$p"
    done
}
exe_syms=$(mktemp); closure_seen=$(mktemp); closure_list=$(mktemp); closure_bad=$(mktemp)
trap 'rm -f "$provided" "$exe_syms" "$closure_seen" "$closure_list" "$closure_bad"' EXIT
defined "$out/bin/bb-probe" | sort -u > "$exe_syms"
for module in "$out/lib/libbbnet20.so" "$out/lib/libbloodframe.so"; do
    [[ -f $module ]] || continue
    : > "$closure_seen"; : > "$closure_list"; : > "$closure_bad"
    closure_walk "$module"
    echo "== dlopen module $(basename "$module"): NEEDED closure: $(sed 's|.*/||' "$closure_list" | sort -u | tr '\n' ' ')"
    if [[ -s $closure_bad ]]; then echo "  NEEDED library not found: $(tr '\n' ' ' < "$closure_bad")"; fail=1; fi
    visible=$({ cat "$exe_syms"; while read -r lib; do defined "$lib"; done < "$closure_list"; } | sort -u)
    undef=$("$llvm/llvm-nm" -D --undefined-only "$module" | awk '{print $2}' | sed 's/@.*//' | sort -u)
    weak=$("$llvm/llvm-nm" -D "$module" | awk '$1=="w"||$1=="v"{print $2}' | sed 's/@.*//' | sort -u)
    unresolved=$(comm -23 <(echo "$undef") <(echo "$visible") | comm -23 - <(echo "$weak") | grep -v '^$' || true)
    if [[ -n $unresolved ]]; then
        echo "  UNRESOLVED at dlopen (not in bb-probe's dynsym nor in the module's NEEDED closure; add the defining library to its link line):"
        while read -r sym; do
            where=""
            for lib in "$out/lib/libbbcpu.so" "$out/lib/libbbgpu.so" "$out/lib/libbloodframe.so"; do
                defined "$lib" | grep -qxF "$sym" && where="$where $(basename "$lib")"
            done
            echo "    $sym${where:+   (defined in:$where)}"
        done <<<"$unresolved"
        fail=1
    else echo "  every undefined symbol resolves from bb-probe or the NEEDED closure"; fi
done
extra=$("$llvm/llvm-nm" -D --defined-only "$out/lib/libbbnet20.so" | awk '{print $3}' | sed 's/@.*//' | grep -v '^bbnet_' || true)
if [[ -n $extra ]]; then echo "== libbbnet20.so exports more than bbnet_*:"; echo "$extra" | head -20 | sed 's/^/    /'; fail=1; fi
exported=$("$llvm/llvm-nm" -D --defined-only "$out/lib/libbbnet20.so" | awk '{print $3}')
for name in bbnet_init bbnet_resolve bbnet_image_mapped bbnet_image_ready bbnet_start; do
    grep -qx "$name" <<<"$exported" || { echo "== libbbnet20.so does not export $name"; fail=1; }
done
# The one library libbbgpu dlopens: its exported symbols are the C API of tools/android/lsfg/bb_lsfg.h, no more.
extra=$("$llvm/llvm-nm" -D --defined-only "$out/lib/libbloodframe.so" | awk '{print $3}' | sed 's/@.*//' | grep -v '^bb_lsfg_' || true)
if [[ -n $extra ]]; then echo "== libbloodframe.so exports more than bb_lsfg_*:"; echo "$extra" | sed 's/^/    /'; fail=1; fi
# ... and all of it (vk_lsfg.cpp looks these up with dlsym).
exported=$("$llvm/llvm-nm" -D --defined-only "$out/lib/libbloodframe.so" | awk '{print $3}')
for name in bb_lsfg_init bb_lsfg_create_context bb_lsfg_present bb_lsfg_wait bb_lsfg_destroy_context bb_lsfg_shutdown bb_lsfg_precision bb_lsfg_diagnostics; do
    grep -qx "$name" <<<"$exported" || { echo "== libbloodframe.so does not export $name"; fail=1; }
done
# No FEX anywhere: the game runs as ahead-of-time native code (libbbaot.so), the runtime has no FEXCore. No symbol (dynamic or, when not stripped, static)
# and no string of an output may name FEX, except the two constants the CPUID model presents to the game (the hypervisor vendor "FEXIFEXIEMU" and its
# version leaf "FEX-2609.1": cpu/guest_cpu_cpuid.h, pinned by tests/test_aot_native.cpp).
for f in "$out/bin/bb-probe" "$out/bin/bb-prepare" "$out/bin/bb-texpack" "$out/bin/ps4-pkg-tool" "$out/lib/libbbgpu.so" "$out/lib/libbbcpu.so" "$out/lib/libbloodframe.so" "$out/lib/libbbnet20.so"; do
    hits=$({ "$llvm/llvm-nm" "$f" 2>/dev/null | awk '{print $NF}'; "$llvm/llvm-nm" -D "$f" 2>/dev/null | awk '{print $NF}'; strings -a "$f"; } \
        | grep -E 'FEX|fextl|[Jj]emalloc' | grep -E 'FEXCore|FEX-Emu|FEXInterpreter|fextl|FEX::|FEX_|[Jj]emalloc' | grep -vxE 'FEXIFEXIEMU|FEX-2609\.1' | sort -u | head -5 || true)
    if [[ -n $hits ]]; then echo "== $f names FEX/jemalloc:"; echo "$hits" | sed 's/^/    /'; fail=1; fi
done
[[ $fail == 0 ]] && echo "== no FEX symbols or strings in the outputs (but the two CPUID constants)"
exit $fail
