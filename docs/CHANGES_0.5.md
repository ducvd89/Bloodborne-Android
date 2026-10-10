**English** · [Русский](#русский)

bbport is the counterpart of Wine + DXVK for a single game: Bloodborne (CUSA03173, v1.09) on Linux.
The game's x86-64 code runs directly on the CPU, a runtime written for this game replaces the PS4
system libraries, and the graphics are translated to Vulkan.

Questions about this project: Discord https://discord.gg/KYZRKk9CB — not the shadPS4 server.

### What's new since 0.4

Details and measurements of each step are in the prerelease notes:
[pre1](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre1),
[pre2](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre2),
[pre3](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre3),
[pre4](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre4).

**The new memory and translation model — the default on AMD**
- Launcher: *Mode → Memory and translation model*. *Auto* (the default) gives AMD GPUs the new
  model and NVIDIA, Intel and others the memory model of 0.3 with all its fixes; *New* and *Old*
  choose by hand. Without the launcher: `BB_PC_MODEL=1/0` (unset: as *Auto*).
- The GPU uses the game's memory where it is, as a PC game's buffers; what it reads often is kept
  in VRAM — on AMD in a sparse buffer (the arena), on other GPUs by the layer's memory module
  without sparse rebinding (`BB_LAYER_MEMORY=1/0` by hand). The PS4 command processor's work is
  translated rather than emulated: memory writes and fences are done by the GPU in stream order,
  `COPY_DATA`, `COND_EXEC`, predication, `MEM_SEMAPHORE` and the GPU clock (100 MHz, as on the
  PS4) follow the GPU's timeline.
- The game's occlusion queries as Vulkan queries are available with `BB_OCCLUSION_QUERIES=1`, off
  by default: with an upscaler the scene is drawn smaller and jittered, and a lamp's glow then
  flashed grey over the whole frame now and then. Off, everything counts as visible, as in the
  0.3 model.
- The layer's memory module (NVIDIA and others): some launches no longer run the GPU 1.3–1.7 times
  slower (copies that broke render passes up to ~100 times a frame).
- On NVIDIA the new model can be chosen by hand; testers' results differ by scene (some scenes are
  slower than with the old model), so it is not the default there yet. Logs are welcome.

**Settings inside the game's own menu**
- *System* has *Display* and *Effects*, real pages of the game's menu, on the title screen and
  in the pause menu: output resolution, quality preset and sharpness as sliders, FPS counter,
  model detail and upscaler; chromatic aberration, depth of field, motion blur, SSAO, dynamic
  shadows and SSR. The game's patches and its own AA are in the overlay menu and the launcher.
- The overlay menu (Insert / L3+R3) looks as in 0.3 again.
- No "Play online / offline" screen: there is no PSN, the game opens its main menu offline.

**Smoother play**
- New graphics pipelines of passes drawn every frame compile in the background: fewer hitches.
  Only geometry goes without its pipeline meanwhile (it shows a frame later); full-screen passes
  (lighting, fog, post-processing) wait for theirs — skipping one left whole frames grey. The
  drivers' shader caches are kept between sessions. In the game's menu the output resolution and
  preset sliders apply once they rest for 0.4 s, not at every step.
- Cards with little VRAM: FPS no longer falls the longer you play (the texture collector evicted
  textures used a moment ago and read them again over the bus: 32 915 in ~7 minutes in an NVIDIA
  log); the game no longer stops when VRAM for its memory copies runs out; VRAM is given back
  after travelling.

**New**
- DLC: add-on folders under `user/addcont/` are reported as installed; saves with The Old Hunters
  load (#33, PR #84).
- DLSS on Linux for NVIDIA RTX when built with NVIDIA's DLSS SDK (PR #83; not in the AppImage).
- Launcher: monitor choice (#69), *Copy* and *Export log…* (PR #78), Simplified Chinese (PR #75).
- Damaged game extractions are reported instead of hanging while loading shaders (#81);
  `scripts/game_check.py GAME_DIR` checks every file.
- NVIDIA in the AppImage: the host's EGL vendor files are passed along (without them NVIDIA's
  driver gave no `vkCreateInstance` on some hosts, #107; `BB_NVIDIA_EGL=0`: off); `--vulkan-info`
  prints the driver report together with the summary.
- AMD Polaris, Vega and their APUs under RADV: `RADV_DEBUG=nohiz` is set automatically (black
  halos and vanishing geometry, #54, #99; your own `RADV_DEBUG` or `BB_AUTO_NOHIZ=0` keeps it off).
- The FSR 4.1.1 build also finds Proton installed system-wide (PR #112). The bundled Dear ImGui
  is private to the GPU library: Vulkan layers with their own copy no longer crash the game
  (PR #113).

**Fixes**
- Saves are no longer damaged by a crash during saving (#64).
- FSR 4.1.1 build: Proton runs without Steam's container where it cannot start (#61).
- The item picture on the loading screen at outputs other than 1080p (#67).
- GPU hangs from garbage indirect dispatch sizes (#34); a crash window while recording commands
  (the same crash was reported for a Windows build, #100).
- Small mips of tiled textures are detiled the way the GPU stores them (from shadPS4 #5196).
- Chinese/Japanese/Korean Windows locales in the game preparation (#55); an `eboot.bin` dumped as a
  plain ELF (#49); stick calibration for Switch Pro clones (#70, PR #71); `BB_FSR4_STATS=1`
  (PR #80).

**Known limits:** one machine was tested thoroughly (RX 7800 XT, Mesa/RADV). Banding in dark
gradients is not confirmed fixed. The Windows build is a separate port.

The AppImage contains no game files. Proprietary FSR 4 model assets and NVIDIA's DLSS SDK/libraries
are not bundled. `SHA256SUMS` accompanies the download.

## Русский

bbport — аналог Wine + DXVK для одной игры: Bloodborne (CUSA03173, v1.09) на Linux. Код игры x86-64
выполняется прямо на процессоре, системные библиотеки PS4 заменяет рантайм, написанный под эту игру,
графика переводится в Vulkan.

Вопросы по проекту: Discord https://discord.gg/KYZRKk9CB — не сервер shadPS4.

### Что нового после 0.4

Подробности и замеры каждого шага — в описаниях пререлизов:
[pre1](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre1),
[pre2](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre2),
[pre3](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre3),
[pre4](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre4).

**Новая модель памяти и трансляции — по умолчанию на AMD**
- Лаунчер: *Режим работы → Модель памяти и трансляции*. *Авто* (по умолчанию) даёт видеокартам
  AMD новую модель, а NVIDIA, Intel и остальным — модель памяти 0.3 со всеми исправлениями;
  *Новая* и *Старая* — вручную. Без лаунчера — `BB_PC_MODEL=1/0` (не задано — как *Авто*).
- Видеокарта работает с памятью игры на месте, как с буферами игры для ПК; то, что она читает
  часто, держится в VRAM — на AMD в sparse-буфере (арене), на остальных видеокартах модулем
  памяти прослойки без sparse-перепривязок (`BB_LAYER_MEMORY=1/0` — вручную). Работа командного
  процессора PS4 переводится, а не эмулируется: записи в память и метки делает видеочип в порядке
  потока, `COPY_DATA`, `COND_EXEC`, предикация, `MEM_SEMAPHORE` и часы видеочипа (100 МГц, как у
  PS4) — во времени видеочипа.
- Occlusion-запросы игры как запросы Vulkan — по `BB_OCCLUSION_QUERIES=1`, по умолчанию выключены:
  с апскейлером сцена рисуется меньше и со сдвигом, и свечение лампы время от времени вспыхивало
  серым на весь кадр. Без них всё считается видимым, как в модели 0.3.
- Модуль памяти прослойки (NVIDIA и другие): некоторые запуски больше не гоняют видеокарту в
  1,3–1,7 раза медленнее (копирования, разрывавшие проходы рендера до ~100 раз за кадр).
- На NVIDIA новую модель можно выбрать вручную; у тестеров результат зависит от сцены (в части
  сцен медленнее старой модели), поэтому там она пока не по умолчанию. Логи приветствуются.

**Настройки — в меню самой игры**
- В *Системе* — *Изображение* и *Эффекты*, настоящие страницы меню игры, и на титульном экране,
  и в паузе: разрешение вывода, пресет качества и резкость ползунками, счётчик FPS, детализация
  моделей и апскейлер; хроматическая аберрация, глубина резкости, размытие в движении, SSAO,
  динамические тени и SSR. Патчи игры и её собственное сглаживание — в меню оверлея и лаунчере.
- Меню оверлея (Insert / L3+R3) снова выглядит как в 0.3.
- Без экрана «Играть онлайн / офлайн»: PSN нет, игра сразу открывает главное меню офлайн.

**Ровнее**
- Новые графические конвейеры проходов, которые рисуются каждый кадр, собираются в фоне: меньше
  рывков. Без своего конвейера в это время остаётся только геометрия (она появляется кадром
  позже); полноэкранные проходы (освещение, туман, постобработка) ждут свой — пропуск такого
  прохода давал целые серые кадры. Дисковые кэши шейдеров драйверов сохраняются между сессиями. В
  меню игры ползунки разрешения вывода и пресета применяются, когда простоят 0,4 с, а не на
  каждом шаге.
- Карты с малым объёмом VRAM: FPS больше не падает со временем (сборщик текстур выгружал только что
  использованные текстуры и читал их снова через шину: 32 915 за ~7 минут в логе с NVIDIA); игра
  не останавливается, когда кончается VRAM для копий её памяти; после перемещений VRAM
  освобождается.

**Новое**
- DLC: папки дополнений в `user/addcont/` сообщаются игре как установленные, сохранения с The Old
  Hunters загружаются (#33, PR #84).
- DLSS на Linux для NVIDIA RTX при сборке с SDK DLSS от NVIDIA (PR #83; в AppImage не входит).
- Лаунчер: выбор монитора (#69), *Копировать* и *Экспорт лога…* (PR #78), упрощённый китайский
  (PR #75).
- Повреждённая распаковка игры называется прямо, а не приводит к зависанию на загрузке шейдеров
  (#81); `scripts/game_check.py ПАПКА_ИГРЫ` проверяет все файлы.
- NVIDIA в AppImage: передаются файлы EGL-вендоров хоста (без них драйвер NVIDIA на некоторых
  системах не отдавал `vkCreateInstance`, #107; `BB_NVIDIA_EGL=0` — выключить); `--vulkan-info`
  печатает отчёт о драйвере вместе со сводкой.
- AMD Polaris, Vega и их APU под RADV: `RADV_DEBUG=nohiz` ставится сам (чёрные ореолы и пропадающая
  геометрия, #54, #99; свой `RADV_DEBUG` или `BB_AUTO_NOHIZ=0` — не ставить).
- Сборка FSR 4.1.1 находит и Proton, установленный в систему (PR #112). Встроенный Dear ImGui
  скрыт внутри библиотеки GPU: слои Vulkan со своей копией больше не роняют игру (PR #113).

**Исправления**
- Сохранения не портятся при вылете во время сохранения (#64).
- Сборка FSR 4.1.1: Proton запускается без контейнера Steam там, где тот не стартует (#61).
- Картинка предмета на экране загрузки при выводе не в 1080p (#67).
- Зависания видеочипа от мусорных размеров непрямых запусков (#34); окно для вылета при записи
  команд (такой же вылет был у сборки для Windows, #100).
- Мелкие мипы тайловых текстур раскладываются так, как их хранит видеокарта (из shadPS4 #5196).
- Китайская/японская/корейская локали Windows при подготовке игры (#55); `eboot.bin`, снятый как
  обычный ELF (#49); калибровка стиков у клонов Switch Pro (#70, PR #71); `BB_FSR4_STATS=1` (PR #80).

**Ограничения:** подробно проверена одна машина (RX 7800 XT, Mesa/RADV). Полосы в тёмных
градиентах — исправление не подтверждено. Сборка для Windows — отдельный порт.

Игровых файлов в AppImage нет. Проприетарные модели FSR 4 и SDK/библиотеки DLSS не включены.
К загрузке приложен `SHA256SUMS`.
