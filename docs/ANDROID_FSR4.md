# FSR 4 on Android (experimental)

The settings screen offers **FSR 4** and **FSR 4.1.1** besides FSR 3.1. Both are INT8 machine-learning upscalers: heavy for phone GPUs, untested on Android since the driver fixes of 0.2. On first use Turnip compiles their shaders for a minute or more (black screen); the Mesa shader cache makes later starts quick. If they fail, the game falls back to FSR 3.1. Frame generation works with FSR 3.1 only.

- **FSR 4** (model v07): assets are in the APK (`bbport/arm64/fsr4_shaders`, fetched with `tools/fetch_fsr4_assets.sh`).
- **FSR 4.1.1**: assets are built from your own AMD DLL and are not in the APK.

## FSR 4.1.1 assets

1. On a Linux PC with a **Radeon RX 7000 or 9000** (AMD's DLL enables FSR 4.1 only there; integrated Radeon 890M did not), build them from `amd_fidelityfx_upscaler_dx12.dll` 4.1.x:
   ```bash
   bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll>
   ```
   The set lands in `fsr4_411/`. Its `portable/` subfolder is the one Android uses: Turnip lacks `VK_VALVE_shader_mixed_float_dot_product`, so those passes do that dot product with plain arithmetic.
2. Copy the whole `fsr4_411` folder next to your game folder on the phone, e.g. `/sdcard/Bloodborne/fsr4_411` beside `/sdcard/Bloodborne/CUSA03173`.
3. Pick **FSR 4.1.1** in the settings. The log (`files/bbport/logs/android-game.log`) shows `Upscaler: FSR 4.1.1 without VK_VALVE_shader_mixed_float_dot_product: …/portable`.
