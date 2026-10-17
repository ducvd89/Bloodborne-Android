# Snapdragon Game Super Resolution 2 (SGSR 2), vendored shaders

- Upstream: https://github.com/SnapdragonStudios/snapdragon-gsr (directory `sgsr/v2/include/glsl_2_pass_cs`)
- Upstream commit: d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e
- License: BSD-3-Clause, Copyright (c) 2023 (LICENSE) and 2024 (shader headers) Qualcomm Innovation Center, Inc. (see `LICENSE`)
- Variant: 2-pass compute (convert + upscale). Chosen over 2-pass-fs (no render-pass plumbing
  needed around this port's push-descriptor compute passes) and 3-pass-cs (heavier, aimed at
  desktop).

`glsl_2_pass_cs/` holds the upstream files unmodified. The shaders the port builds are derived
copies in `gpu/shadps4/video_core/host_shaders/gsr2_convert.comp` and `gsr2_upscale.comp`
(they keep the Qualcomm copyright header); the changes are listed at the top of each.
The pass wiring is `gpu/shadps4/video_core/renderer_vulkan/vk_gsr2.{h,cpp}`.
