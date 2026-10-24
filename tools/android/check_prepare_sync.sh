#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/android/prepare (bb-prepare, run on the phone) and shared/prepare of the Rebuild Tool repository (the tools/recomp submodule, run on the PC) are
# the same code (tools/android/prepare/SYNC.md). Compares them, ignoring the "SPDX-License-Identifier" header lines (the files carry different ones).
# Exit: 0 the same, 1 they differ, 2 the submodule is not checked out.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
other=${1:-tools/recomp/shared/prepare}
if [[ ! -d $other ]]; then
    echo "check_prepare_sync: $other is missing (git submodule update --init tools/recomp)" >&2
    exit 2
fi
fail=0
for f in tools/android/prepare/*; do
    name=${f##*/}
    [[ $name == SYNC.md ]] && continue
    if [[ ! -f $other/$name ]]; then echo "only here: $name"; fail=1; continue; fi
    if ! diff -q <(grep -v '^// SPDX-License-Identifier' "$f") <(grep -v '^// SPDX-License-Identifier' "$other/$name") >/dev/null; then
        echo "differs: $name"; fail=1
    fi
done
for f in "$other"/*; do
    name=${f##*/}
    [[ -f tools/android/prepare/$name ]] || { echo "only in the tool: $name"; fail=1; }
done
[[ $fail == 0 ]] && echo "prepare sources are in sync with the Rebuild Tool"
exit $fail
