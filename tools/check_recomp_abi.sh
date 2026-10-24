#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Checks that this runtime and the Rebuild Tool (the tools/recomp submodule, https://github.com/ducvd89/Bloodborne-android-rebuild-tool) agree on the
# contract of libbbaot.so (docs/RECOMP_ABI.md). The runtime compiles without the submodule; this check needs it (git submodule update --init tools/recomp)
# and says so when it is not there. Checked:
#   1. the ABI version: cpu/guest_cpu_aot.cpp (BbrAbiVersion), the app's NativeCpu.ABI_VERSION, the tool's bbr_abi_version (compile.cpp) and
#      kAbiVersion (player.cpp) are the same number;
#   2. the GuestState offsets the tool's lowering has hand-written (the x87 control word FCW and the x87 invalid-operation flag in lower_vector.cpp)
#      are the ones of cpu/guest_state.h;
#   3. with BB_FEX_DIR=<FEX source tree> (the tool's out/recomp-fex): tests/guest_state_check.cpp compares GuestState with FEX's CPUState field by field.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
tool=tools/recomp
if [[ ! -f $tool/src/compile.cpp ]]; then
    echo "check_recomp_abi: the Rebuild Tool submodule is not checked out (git submodule update --init tools/recomp); nothing to compare" >&2
    exit 2
fi
fail=0
runtime=$(sed -n 's/.*BbrAbiVersion = \([0-9]*\);.*/\1/p' cpu/guest_cpu_aot.cpp | head -1)
app=$(sed -n 's/.*ABI_VERSION = \([0-9]*\);.*/\1/p' tools/android/frontend/src/ro/cobrabm/fexdroid/NativeCpu.java | head -1)
emitted=$(sed -n 's/.*ConstantInt::get(i32, \([0-9]*\)), "bbr_abi_version".*/\1/p' $tool/src/compile.cpp | head -1)
player=$(sed -n 's/.*kAbiVersion = \([0-9]*\);.*/\1/p' $tool/src/player.cpp | head -1)
echo "ABI version: runtime $runtime, app $app, tool emits $emitted, tool's player expects $player"
[[ -n $runtime && $runtime == "$app" && $runtime == "$emitted" && $runtime == "$player" ]] || { echo "ABI version mismatch" >&2; fail=1; }
# 2. offsets
offsets=$(mktemp -d); trap 'rm -rf "$offsets"' EXIT
cat > "$offsets/o.cpp" <<'CPP'
#include "guest_state.h"
#include <cstdio>
int main() { std::printf("%zu %zu\n", offsetof(GuestState, FCW), offsetof(GuestState, flags) + 1040 - 1008); }
CPP
c++ -std=c++20 -Icpu "$offsets/o.cpp" -o "$offsets/o"
read -r fcw flag <<<"$("$offsets/o")"
tool_fcw=$(grep -o 'FCW[^;]*1200\|1200[^;]*FCW' $tool/src/lower_vector.cpp | head -1 || true)
echo "GuestState::FCW is at $fcw here; the tool's lowering says: ${tool_fcw:-(not found by pattern)}"
[[ $fcw == 1200 ]] || { echo "FCW offset is not 1200" >&2; fail=1; }
[[ $flag == 1040 ]] || { echo "x87 flag offset is not 1040" >&2; fail=1; }
# 3. FEX's own layout
if [[ -n ${BB_FEX_DIR:-} ]]; then
    c++ -std=c++20 -I"$BB_FEX_DIR/FEXCore/include" -I"$BB_FEX_DIR" -I"$BB_FEX_DIR/External/fmt/include" -fsyntax-only tests/guest_state_check.cpp \
        && echo "GuestState matches FEX's CPUState" || { echo "GuestState differs from FEX's CPUState" >&2; fail=1; }
else
    echo "(BB_FEX_DIR not set: GuestState was not compared with FEX's CPUState)"
fi
exit $fail
