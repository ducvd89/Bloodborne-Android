# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: device-local images on unified-memory GPUs (Adreno/Turnip): FFX skipped every
  host-visible type for device-local requests, and there every allocatable type is one, so FSR 3
  context creation failed (`FFX_ERROR_BACKEND_API_ERROR`) once the world loaded.
- `0003-...`: frame generation frees its per-frame descriptor pools and constant buffers. It
  never began or retired frames on its bridge, so each job's pool and buffer stayed: on Turnip a
  mapped KGSL buffer each, which reached Android's 65530 mapping limit within two minutes.
