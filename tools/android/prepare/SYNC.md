# Kept in sync with the Rebuild Tool

These sources (the game folder check and the game image builder, `bb-prepare`) exist twice: here, where the Android app runs them on the phone,
and in the Bloodborne Android Rebuild Tool repository (https://github.com/ducvd89/Bloodborne-android-rebuild-tool, `shared/prepare/`), where the
PC tool builds the same image. The image hash in `libbbaot.so` depends on them being the same code: **change both, or neither.**

The repository's `tools/recomp` submodule is the tool repository, so after `git submodule update --init tools/recomp`:

    bash tools/android/check_prepare_sync.sh        # diffs this directory with tools/recomp/shared/prepare (ignoring SPDX header lines)

The check exits 0 when they are the same, 1 with the differing files when not, 2 when the submodule is not checked out.
