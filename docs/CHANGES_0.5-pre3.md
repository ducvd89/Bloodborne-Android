# 0.5-pre3 — pre-release for testing

Third 0.5 prerelease, built from the `testing` branch. It addresses the first hardware logs of the
new memory module on NVIDIA (GTX 1660 Ti laptop, 6 GB, driver 615.71). Changes are listed relative
to [0.5-pre2](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre2). Details and
measurements: [NVIDIA_MEMORY_MODEL.ru.md](NVIDIA_MEMORY_MODEL.ru.md).

## English

- **VRAM copies were disabled on NVIDIA.** The fixed-arena workaround for NVIDIA's sparse binding
  was also applied to the memory module, which has no sparse arena: it made no VRAM copies of the
  game's memory at all (10–15 FPS on the GTX 1660 Ti). The workaround no longer applies to the
  module; `BB_FIXED_ARENA=0` is no longer needed.
- **One VRAM budget for textures and copies of the game's memory.** The texture collector counts
  all of our VRAM. On cards below ~9 GB its critical mark (3.9 GB on the 6 GB card) was below what
  the copies and the textures need together, so it kept evicting textures in use (155–1255 every
  5 s, stutters) while the copies kept growing up to 90 % of the driver's budget. Copies now stop
  384 MiB short of that mark; close to it the module frees room itself, first the unused room of
  its copies, then copies not used for 3 s (`BB_VRAM_PRESSURE_IDLE_SECONDS`).
- **Fewer reallocations of VRAM copies.** A copy that met newly loaded data was replaced by a
  bigger one each time: loading a level allocated 15–27 GB for 1.3 GB of data, with up to 7.4 GB
  of VRAM in use at once, and streaming areas did the same while playing. A growing copy now gets
  room to grow (up to 64 MiB, `BB_LAYER_MIRROR_GROWTH_MB`): 5–10 GB, and the worst loading frame of
  a test route went from 555 to 87 ms (RX 7800 XT). Replaced copies are freed as soon as the GPU is
  done with them.
- **Command-processor writes (WRITE_DATA)** update the VRAM copy as well, instead of sending the
  block back to system memory (with watched blocks, below, up to 2 GB a minute went back and forth).
- **Watched blocks** (`BB_LAYER_WATCH`, on by default). Memory the CPU writes without announcing
  it was always read by the GPU from system memory, over the bus on NVIDIA. It now gets a VRAM
  copy for reading bindings too; a write trap catches the next CPU write and the block is uploaded
  again before its next use. This also replaces most volatile-block refreshes (RX 7800 XT:
  1.3 GB/s of copies → 36 MB/s; the NVIDIA log showed 455 MB/s). Blocks the CPU writes nearly every
  frame still stay in system memory: uploading them before draws ended render passes (GPU frame
  4.8 → 7.2 ms on the RX 7800 XT); `BB_LAYER_WATCH_HOT=1` turns that on for experiments.
- **File reads retry on EFAULT** (a page protected again between the runtime's touch and the
  kernel's copy).
- **Testing aids:** `BB_VRAM_LIMIT_MB=N` takes the driver's VRAM budget as at most N MiB (5600
  gives the texture-collector marks of a 6 GB card); `BB_RESIDENCY_MIX=1` also says why blocks are
  not watched; the `Layer memory:` lines report watched blocks and write traps by kind.

**How to test:** enable *New memory and translation model* in the launcher, or set
`BB_PC_MODEL=1`; the mode remains **off by default**. Logs with `BB_FRAME_STATS=1
BB_GPU_PROFILE=1` at one place, once as is, once with `BB_LAYER_WATCH=0` and once with the mode off,
show where the time goes.

**Validation and limits:** RX 7800 XT/RADV (PCIe 3.0) with the memory module forced on and
`BB_VRAM_LIMIT_MB=5600`: model of 0.3 ~195 FPS, module ~195–213 FPS (GPU 4.6–5.2 ms a frame; runs
vary by about ±10 %), VRAM kept under the collector's critical mark, a recorded route without
frames over 25 ms, a 3-minute walk without errors, unchanged images; runtime, unit (101) and GPU
tests passed. Not tested on physical NVIDIA or Intel hardware: on AMD reads of the game's memory
in place cost little, so whether NVIDIA now reaches the speed of the 0.3 model is not established.

The AppImage contains no game files. Proprietary FSR 4 model assets and NVIDIA's DLSS SDK/libraries
are not bundled. `SHA256SUMS` accompanies the download.

## Русский

- **На NVIDIA не было копий в VRAM.** Обход sparse binding на NVIDIA («фиксированная арена»)
  применялся и к модулю памяти, у которого sparse-арены нет: модуль вообще не копировал память
  игры в VRAM (10–15 FPS на GTX 1660 Ti). Теперь обход на модуль не действует,
  `BB_FIXED_ARENA=0` больше не нужен.
- **Общий бюджет VRAM для текстур и копий памяти игры.** Сборщик кэша текстур считает всю нашу
  VRAM. На картах меньше ~9 ГБ его критическая отметка (3,9 ГБ на 6 ГБ) оказалась ниже того, что
  нужно копиям и текстурам вместе: он постоянно выселял используемые текстуры (155–1255 за 5 с,
  фризы), а копии росли до 90 % бюджета драйвера. Теперь копии встают за 384 МиБ до отметки, а
  рядом с ней модуль сам освобождает место: сначала неиспользуемый запас своих копий, затем
  копии, не нужные 3 с (`BB_VRAM_PRESSURE_IDLE_SECONDS`).
- **Меньше перевыделений копий.** Копия, к которой примыкали новые данные, каждый раз заменялась
  большей: загрузка уровня выделяла 15–27 ГБ на 1,3 ГБ данных, до 7,4 ГБ VRAM одновременно; то же
  при подгрузке районов во время игры. Растущая копия теперь получает запас под рост (до 64 МиБ,
  `BB_LAYER_MIRROR_GROWTH_MB`): 5–10 ГБ, худший кадр загрузки в тестовом маршруте 555 → 87 мс
  (RX 7800 XT). Заменённые копии освобождаются сразу, как только видеочип с ними закончил.
- **Записи командного процессора (WRITE_DATA)** обновляют и копию в VRAM, а не возвращают блок
  в системную память (с наблюдаемыми блоками, ниже, туда и обратно ходило до 2 ГБ в минуту).
- **Наблюдаемые блоки** (`BB_LAYER_WATCH`, по умолчанию включено). Память, в которую процессор
  пишет без предупреждения, видеочип всегда читал из системной памяти, на NVIDIA через шину.
  Теперь для читающих привязок у неё тоже есть копия в VRAM: ловушка записи ловит следующую
  запись процессора, и блок перезаливается перед следующим использованием. Это же заменило
  большую часть обновлений летучих блоков (RX 7800 XT: 1,3 ГБ/с копий → 36 МБ/с; в логе с NVIDIA
  было 455 МБ/с). Блоки, которые процессор пишет почти каждый кадр, остаются в системной памяти:
  их заливка перед отрисовками завершала render pass (кадр видеочипа 4,8 → 7,2 мс на RX 7800 XT);
  `BB_LAYER_WATCH_HOT=1` включает это для экспериментов.
- **Чтение файлов повторяется при EFAULT** (страница снова защищена между касанием рантайма и
  копированием ядром).
- **Для проверок:** `BB_VRAM_LIMIT_MB=N` — бюджет VRAM драйвера считается не больше N МиБ (5600
  даёт пороги сборщика текстур карты на 6 ГБ); `BB_RESIDENCY_MIX=1` объясняет, почему блоки не
  наблюдаются; строки `Layer memory:` показывают наблюдаемые блоки и срабатывания ловушек по видам.

**Как проверить:** включить *Новую модель памяти и трансляции* в лаунчере или задать
`BB_PC_MODEL=1`; режим по-прежнему **выключен по умолчанию**. Логи с `BB_FRAME_STATS=1
BB_GPU_PROFILE=1` в одном месте — как есть, с `BB_LAYER_WATCH=0` и с выключенным режимом —
покажут, куда уходит время.

**Проверки и ограничения:** RX 7800 XT/RADV (PCIe 3.0), модуль памяти включён вручную,
`BB_VRAM_LIMIT_MB=5600`: модель 0.3 ~195 FPS, модуль ~195–213 FPS (видеочип 4,6–5,2 мс за кадр;
разброс между запусками около ±10 %), VRAM держится ниже критической отметки сборщика,
записанный маршрут без кадров дольше 25 мс, 3 минуты прогулки без ошибок, картинка не
изменилась; тесты рантайма, юнит-тесты (101) и GPU-тесты пройдены. На физических NVIDIA и Intel
не проверялось: на AMD чтения памяти игры на месте почти бесплатны, поэтому достигает ли NVIDIA
теперь скорости модели 0.3, не установлено.

Игровых файлов в AppImage нет. Проприетарные модели FSR 4 и SDK/библиотеки DLSS не включены.
К загрузке приложен `SHA256SUMS`.
