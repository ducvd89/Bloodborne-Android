**English** · [Русский](#русский)

bbport is the counterpart of Wine + DXVK for a single game: Bloodborne (v1.09) on Linux.
Changes since [0.5](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5); details and
measurements: [CHANGES_after_0.5.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_after_0.5.md) (Russian).

Questions about this project: Discord https://discord.gg/KYZRKk9CB — not the shadPS4 server.

### New

- **Online play (shadNet)**, as a separate module (`libbbnet.so`, loaded only when online play is
  on): messages, bloodstains and ghosts from The Hunter's Dream, bells and summons through a
  shadNet server (`srv.shadps4.net:31313` by default) with a shadNet account. Launcher → *Online*:
  servers, account, UPnP, connection test. The router port is opened by UPnP (#121). The same
  servers as the Windows port. The network and PSN libraries come from shadp2p (a shadPS4 fork,
  GPL-2.0) by way of the Linux and Windows forks of bbport.
- **Mouse look**: click into the game to take the mouse, F1 releases it; mouse buttons and the
  wheel can be bound like keys (Mouse Left = R1, Mouse Right = L2, Middle = R3, side buttons
  R2/L1, wheel = quick items). Launcher → *Controls*: mouse look, sensitivity, invert Y (#51, #52).
- **DLSS in the AppImage** (NVIDIA GeForce RTX): the bridge and NVIDIA's DLSS 310.9.1 library are
  included (NVIDIA's RTX SDK license allows it within an application; its text is in the package).
  A newer `libnvidia-ngx-dlss.so.*` can be chosen in the launcher (*Upscaler → DLSS: NVIDIA's
  library*), like the FSR 4.1.1 DLL.
- **Every retail release** of Bloodborne with update 1.09 is accepted (US CUSA00900, EU, UK, JP,
  Asia and The Old Hunters editions; PR #117); an `eboot.bin` with a 60 fps patch baked in is
  reported.
- **Quit the game** from the overlay (Insert / L3+R3; press twice) — #74, #116.

### Fixes

- The US release (and other regions) crashed with code 139 on entering the world in the new
  memory model: they ran without the game's profile (#119).
- Motion blur smeared the whole frame while the camera turned in the new memory model (#122).
- Cards with 4 GB (NVIDIA): "Failed to allocate 64 MiB for guest memory copies" stopped the game;
  now it frees textures sooner and uses other memory instead.
- The main menu flickered with output resolutions above 1080p (live resolution).
- Online, "Could not get information from the Bloodborne server" in the AppImage on NixOS (HTTPS
  certificates).
- NVIDIA on Wayland: the swapchain is recreated through `oldSwapchain` (PR #93).
- Building from git after updating from 0.4: "patch failed: src/ffx_vk_fsr4_v07.c" (#124).
- `bb-launcher.sh --play` through nix-shell (PR #120); FSR 4.1.1 recording from the AppImage on
  SteamOS (PR #89).

### Smaller download

The AppImage is **399 MB instead of 925 MB**: only Mesa's Vulkan drivers (RADV without LLVM and
ANV) instead of all of Mesa, no debug info in the game binary, MangoHud without its 32-bit half,
the launcher's GTK without GStreamer.

**Known limits:** online login and summons were not tested by the author (no shadNet account);
the UDP side of the public shadNet server did not answer in #121. Mouse look and DLSS need
testing on more systems.

The AppImage contains no game files. AMD's FSR 4 model assets are not bundled. `SHA256SUMS`
accompanies the download.

## Русский

bbport — аналог Wine + DXVK для одной игры: Bloodborne (v1.09) на Linux. Изменения после
[0.5](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5); подробности и замеры —
[CHANGES_after_0.5.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_after_0.5.md).

Вопросы по проекту: Discord https://discord.gg/KYZRKk9CB — не сервер shadPS4.

### Новое

- **Игра по сети (shadNet)** — отдельным модулем (`libbbnet.so`, загружается, только когда онлайн
  включён): сообщения, пятна крови и призраки — с The Hunter's Dream, колокола и призывы — через
  сервер shadNet (по умолчанию `srv.shadps4.net:31313`) с учётной записью shadNet. Лаунчер →
  *Онлайн*: серверы, учётная запись, UPnP, проверка соединения. Порт на роутере открывается по UPnP
  (#121). Серверы те же, что у Windows-версии. Сетевые и PSN-библиотеки — из shadp2p (форк shadPS4,
  GPL-2.0), через Linux- и Windows-форки bbport.
- **Камера мышью**: щелчок в окне игры захватывает мышь, F1 — отпускает; кнопки мыши и колесо
  назначаются как клавиши (левая — R1, правая — L2, средняя — R3, боковые — R2/L1, колесо —
  быстрые предметы). Лаунчер → *Управление*: камера мышью, чувствительность, инверсия (#51, #52).
- **DLSS в AppImage** (NVIDIA GeForce RTX): мост и библиотека DLSS 310.9.1 от NVIDIA в пакете
  (лицензия RTX SDK разрешает это в составе приложения; её текст — в пакете). Более новую
  `libnvidia-ngx-dlss.so.*` можно выбрать в лаунчере (*Апскейлер → DLSS: библиотека NVIDIA*), как DLL
  для FSR 4.1.1.
- **Любое розничное издание** Bloodborne с обновлением 1.09 (США CUSA00900, Европа, Великобритания,
  Япония, Азия и издания с The Old Hunters; PR #117); о `eboot.bin` со вшитым патчем 60 FPS
  сообщается.
- **Выход из игры** из оверлея (Insert / L3+R3; нажать дважды) — #74, #116.

### Исправления

- Американское и другие издания падали с кодом 139 при входе в мир в новой модели памяти: они шли
  без профиля игры (#119).
- Размытие в движении размазывало весь кадр при повороте камеры в новой модели памяти (#122).
- Карты на 4 ГБ (NVIDIA): «Failed to allocate 64 MiB for guest memory copies» останавливало игру;
  теперь текстуры освобождаются раньше, используется другая память.
- Мерцание главного меню при выводе выше 1080p (живая смена разрешения).
- Онлайн: «Не удалось получить информацию от сервера Bloodborne» в AppImage на NixOS (сертификаты
  HTTPS).
- NVIDIA на Wayland: swapchain пересоздаётся через `oldSwapchain` (PR #93).
- Сборка из git после обновления с 0.4: «patch failed: src/ffx_vk_fsr4_v07.c» (#124).
- `bb-launcher.sh --play` через nix-shell (PR #120); запись FSR 4.1.1 из AppImage на SteamOS (PR #89).

### Меньше скачивать

AppImage — **399 МБ вместо 925 МБ**: из Mesa только Vulkan-драйверы (RADV без LLVM и ANV), без
отладочной информации в бинарнике игры, MangoHud без 32-битной части, GTK лаунчера без GStreamer.

**Ограничения:** вход в shadNet и призыв автор не проверял (нет учётной записи); UDP-часть
публичного сервера shadNet в #121 не отвечала. Камеру мышью и DLSS нужно проверить на большем числе
систем.

Файлов игры в AppImage нет. Ассеты моделей FSR 4 от AMD не входят. К загрузке прилагается
`SHA256SUMS`.
