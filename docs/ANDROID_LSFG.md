# Lossless Scaling frame generation on Android (experimental)

The app includes [lsfg-vk](https://lsfg-vk.dev) 2.0.0, which runs Lossless Scaling's frame generation as a Vulkan layer. Lossless Scaling itself is not included: it needs your own copy (Steam, app 993090).

1. Install Lossless Scaling on a PC through Steam and copy `Lossless.dll` from its folder (`steamapps/common/Lossless Scaling/Lossless.dll`).
2. Put it next to your game folder on the phone, e.g. `/sdcard/Bloodborne/Lossless.dll` beside `/sdcard/Bloodborne/CUSA03173`.
3. In the settings pick **Lossless Scaling frame generation: 2×, 3× or 4×**. Lower flow scale and performance mode are faster.

It works with any upscaler and replaces FSR 3.1 frame generation. It sees only finished frames, without the game's motion vectors, so expect more artifacts around fast motion and the HUD than with FSR 3.1 frame generation. Its log is `files/bbport/logs/lsfg.log`.

Building: `tools/android/build_lsfg.sh` compiles lsfg-vk from its unmodified source. lsfg-vk is licensed CC BY-NC-ND 4.0: free, non-commercial distribution only, and no modified versions.
