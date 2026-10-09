# Experimental native DLSS 4.5 Super Resolution

This local build adds NVIDIA NGX Super Resolution to bbport's existing temporal
upscaler inputs. It is **not gameplay-validated**. Frame generation, Multi Frame
Generation, Ray Reconstruction and Reflex are not implemented.

The GTK launcher and in-game menu expose `DLSS 4.5 (experimental)`. The saved
setting is `upscaler=dlss`; `BB_UPSCALER=dlss` overrides it. Existing output
resolution and quality presets are retained. Native AA uses DLAA; other presets
use their corresponding DLSS quality modes. Model M is requested for Native AA,
Quality, Balanced and Performance; model L is requested for Ultra Performance.
NGX failure selects FSR 3.1 and logs the reason.

## Local SDK and rebuild

NVIDIA SDK: https://github.com/NVIDIA/DLSS

Tested SDK commit: `374959484e79a640feaba44c93ac8cfb0a03f5b5`.
Linux runtime: `libnvidia-ngx-dlss.so.310.9.1`.
The SDK and proprietary runtime stay in `.local-deps/src/dlss`; they are not
vendored into this project's source. Review NVIDIA's SDK license before any
redistribution. `BB_DLSS_SDK` is an optional CMake path; without it the backend
reports unavailable. `BB_DLSS_DIR` overrides the runtime search directory.

Set `BB_DLSS_SDK` to your SDK checkout before running `bash build.sh --test`.
The normal Linux build dependencies must already be installed (see
`README.linux.md`). Keep the SDK/runtime at that path while running the build.
Select FSR 3.1 to return to the existing upscaler. DLSS is a Linux/NVIDIA feature;
it is not supported on the Thor's Adreno GPU.

## Validation and remaining work

Hardware: NVIDIA GeForce RTX 5080 Laptop GPU, driver 615.78.08.

- Native NGX initialization and Super Resolution capability query succeeded,
  including on bbport's actual Vulkan device (`dlss-device-test`).
- All 104 Python tests and the settings round-trip test passed.
- Synthetic GPU frames passed for all five quality modes, three frames each,
  with model/context recreation and output readback. The test checks that at
  least 99% of red-channel pixels remain finite and close to the known input.
- The test uses constant color, depth and zero motion. It does **not** validate
  moving-object reconstruction, jitter signs, disocclusion, image quality or
  frame pacing in Bloodborne.
- Vulkan validation reports `VUID-vkCmdDraw-None-09600` for internal NGX images
  on initial evaluation. Output readback still passes; this diagnostic remains
  unresolved. Do not describe this integration as validation-clean.
- CUSA03173 v1.09 is installed in `game/CUSA03173`. A 75-second startup run
  reached and rendered the online/offline title menu on the RTX 5080. This does
  not exercise the 3D scene's full DLSS color/depth path; gameplay remains unverified.

Test targets: `dlss-vulkan-test` (standalone GPU dispatch/readback),
`dlss-device-test` (NGX on bbport's actual Vulkan device), `settings-test`.
Logs are in `out/dlss-test.log`, `out/dlss-validation.log`,
`out/dlss-device-test.log`, and `out/dlss-python-tests.log`.
