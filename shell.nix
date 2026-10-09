# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell ({
  packages = with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis spirv-headers miniz libx11 libxcb xorgproto wayland
  ];
} // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isAarch64 {
  # Guest CPU library (cpu/: FEXCore) builds with Clang only (build.sh); the rest with GCC.
  FEX_CC = "${pkgs.clang}/bin/clang";
  FEX_CXX = "${pkgs.clang}/bin/clang++";
})
