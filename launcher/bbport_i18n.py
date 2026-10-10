# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher translations. The Russian text is the key; other languages are looked up by it.

ui_language in the launcher settings: "ru", "en", "pt_BR", "zh_CN" or "" (the system locale:
Russian for ru_*, Brazilian Portuguese for pt_BR*, Simplified Chinese for zh_*, English
otherwise).
"""
import os

EN = {
    # Window, pages, buttons
    "Запустить": "Play",
    "Остановить": "Stop",
    "Настройки": "Settings",
    "Журнал": "Log",
    "Не удалось запустить: {}": "Could not start: {}",
    "Копировать": "Copy",
    "Экспорт журнала…": "Export log…",
    "Экспорт журнала": "Export log",
    "Журнал скопирован": "Log copied",
    "Журнал сохранён: {}": "Log saved: {}",
    "Не удалось сохранить журнал: {}": "Could not save the log: {}",
    "\n— игра завершилась (код {}) —\n": "\n— the game exited (code {}) —\n",
    # Launcher language
    "Язык лаунчера": "Launcher language",
    "Как в системе": "System",
    "Применится после перезапуска лаунчера, пока игра запущена":
        "Applies after restarting the launcher while the game is running",
    # Game
    "Игра": "Game",
    "Папка игры (CUSA03173)": "Game folder (CUSA03173)",
    "Выбрать папку с eboot.bin": "Choose the folder with eboot.bin",
    "Открыть в файловом менеджере": "Open in the file manager",
    "Папка сохранений": "Saves folder",
    "Выбрать папку сохранений": "Choose the saves folder",
    "Вернуть папку по умолчанию": "Back to the default folder",
    "Язык системы": "System language",
    "По умолчанию: {}": "Default: {}",
    "Найдены сохранения": "Saves found",
    "Сохранений пока нет: игра создаст их здесь": "No saves yet: the game will create them here",
    "не выбрана": "not chosen",
    "Найден eboot.bin": "eboot.bin found",
    "Нет eboot.bin в папке": "No eboot.bin in the folder",
    "Управление": "Controls",
    "Клавиатура": "Keyboard",
    "Геймпад": "Gamepad",
    "Назначение кнопок; применяется при запуске игры": "Button assignments; applied when the game starts",
    "Назначить": "Assign",
    "Сбросить": "Reset",
    "{} (по умолчанию)": "{} (default)",
    "не назначено": "not assigned",
    "Нажмите клавишу или кнопку… (Esc — отмена)": "Press a key or a button… (Esc cancels)",
    "Крест": "Cross",
    "Круг": "Circle",
    "Квадрат": "Square",
    "Треугольник": "Triangle",
    "Тачпад, левая половина (жесты)": "Touchpad, left half (gestures)",
    "Тачпад, правая половина (личные вещи)": "Touchpad, right half (key items)",
    "Крестовина вверх": "D-pad up",
    "Крестовина вниз": "D-pad down",
    "Крестовина влево": "D-pad left",
    "Крестовина вправо": "D-pad right",
    "Движение вперёд": "Move forward",
    "Движение назад": "Move back",
    "Движение влево": "Move left",
    "Движение вправо": "Move right",
    "Камера вверх": "Camera up",
    "Камера вниз": "Camera down",
    "Камера влево": "Camera left",
    "Камера вправо": "Camera right",
    "Контроллер": "Controller",
    "Выбранный берётся, как только подключится": "The chosen one is used as soon as it connects",
    "Первый подключённый": "First connected",
    "{} (не подключён)": "{} (not connected)",
    "Монитор": "Monitor",
    "Пропускать выбор «по сети / вне сети»": "Skip the \u201cplay online / offline\u201d choice",
    "Игра сразу открывает главное меню в режиме вне сети (патч игры)":
        "The game opens the main menu offline at once (a game patch)",
    "Основной": "Primary",
    ", основной": ", primary",
    "Нужно обновление 1.09: скопируйте файлы дампа обновления 1.09 в папку игры с заменой (найдена версия {})":
        "The 1.09 update is needed: copy the dumped 1.09 update into the game folder, replacing files (found version {})",
    "eboot.bin не от версии 1.09: скопируйте eboot.bin из дампа обновления 1.09 в папку игры с заменой":
        "eboot.bin is not from 1.09: copy eboot.bin from the dumped 1.09 update into the game folder, replacing it",
    "Поддерживается только CUSA03173 с обновлением 1.09 (найдено {})":
        "Only CUSA03173 with update 1.09 is supported (found {})",
    "eboot.bin не читается как расшифрованный исполняемый файл PS4: сделайте дамп заново":
        "eboot.bin cannot be read as a decrypted PS4 executable: dump the game again",
    "Файлы игры повреждены при распаковке: шейдеры не распаковываются, игра зависнет на загрузке. Распакуйте игру и обновление 1.09 заново исправленным инструментом (issue #81)":
        "The game files were damaged when extracted: the shaders do not unpack and the game would hang while loading. Extract the game and the 1.09 update again with a fixed tool (issue #81)",
    "Bloodborne CUSA03173, версия 1.09": "Bloodborne CUSA03173, version 1.09",
    "Папка игры (с eboot.bin)": "Game folder (with eboot.bin)",
    # Languages
    "Английский": "English",
    "Русский": "Russian",
    "Японский": "Japanese",
    "Французский": "French",
    "Испанский": "Spanish",
    "Немецкий": "German",
    "Итальянский": "Italian",
    # Mods
    "Моды": "Mods",
    "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
    "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске.":
        "Extract each mod into its own folder (with dvdroot_ps4, or chr/, parts/, ... directly). "
        "When files collide, the mod lower in the list wins. Applied at start.",
    "Загружать моды": "Load mods",
    "Папка модов": "Mods folder",
    "Выбрать папку модов": "Choose the mods folder",
    "Открыть папку модов": "Open the mods folder",
    "Обновить список": "Refresh the list",
    "Загрузить раньше": "Load earlier",
    "Загрузить позже": "Load later",
    "Модов нет": "No mods",
    # Patches
    "Сторонние патчи": "Third-party patches",
    "XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. Применяются при запуске.":
        "shadPS4-format XML patches for version 01.09 from the patches folder. Applied at start.",
    "Папка патчей": "Patches folder",
    "Выбрать папку патчей": "Choose the patches folder",
    "Открыть папку патчей": "Open the patches folder",
    "Патчей нет": "No patches",
    "Автор: {}": "Author: {}",
    # Screen
    "Экран": "Display",
    "Разрешение вывода": "Output resolution",
    "Апскейлер дорисовывает кадр; Steam Deck — 720p": "The upscaler fills the frame; Steam Deck: 720p",
    "Полноэкранный режим": "Fullscreen",
    "Смена разрешения на лету": "Live resolution changes",
    "Без перезапуска, но медленнее на Steam Deck и старых GPU":
        "No restart needed, but slower on the Steam Deck and older GPUs",
    "Авто (по видеокарте)": "Auto (by GPU)",
    "Выключена (быстрее)": "Off (faster)",
    "Включена": "On",
    "Режим показа кадров": "Present mode",
    "Разрешить HDR": "Allow HDR",
    # Upscaler
    "Апскейлер": "Upscaler",
    "Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)":
        "Stored in bbport.ini; in game, change it in the menu (Insert or L3+R3)",
    "TAA (нативное сглаживание)": "TAA (native anti-aliasing)",
    "Выключен": "Off",
    "Пресет": "Preset",
    "Резкость (RCAS)": "Sharpening (RCAS)",
    "Сила резкости": "Sharpness",
    "Векторы движения объектов": "Object motion vectors",
    "Меньше гостинга на персонажах; стоит около 10% FPS": "Less ghosting on characters; costs about 10% FPS",
    "Показывать FPS": "Show FPS",
    "Ассеты найдены": "Assets found",
    "Нет ассетов: tools/fetch_fsr4_assets.sh": "No assets: tools/fetch_fsr4_assets.sh",
    "Ассеты для выбранного режима найдены": "Assets for the selected mode found",
    "{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже":
        "{}. Build FSR 4.1.1 from your own DLL with the “Choose DLL…” button below",
    "FSR 4.1.1 из своей DLL AMD": "FSR 4.1.1 from your own AMD DLL",
    "Отменить": "Cancel",
    "Выбрать DLL…": "Choose DLL…",
    "DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)": "AMD upscaler DLL (amd_fidelityfx_upscaler_dx12.dll)",
    "нет": "none",
    "Эта DLL не подходит": "This DLL does not fit",
    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и подобные) устроены иначе и пока не поддерживаются.":
        "{}: version {}, FSR 4 models: {}.\n\nbbport replays AMD's official FSR 4.1.x only (model v07_fp8_no_scale): for example FSR4_LATEST from OptiScaler, or amd_fidelityfx_upscaler_dx12.dll from a game with FSR 4.1. Community builds (4.0.2b and the like) are laid out differently and are not supported yet.",
    "Загрузчик не подходит": "The loader does not fit",
    "Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или amd_fidelityfx_dx12.dll)":
        "FidelityFX loader (amd_fidelityfx_loader_dx12.dll or amd_fidelityfx_dx12.dll)",
    "\n— сборка FSR 4.1.1 из {} —\n": "\n— building FSR 4.1.1 from {} —\n",
    "Подготовка…": "Preparing…",
    "Запись проходов DLL под Proton: {} из {}": "Recording the DLL's passes under Proton: {} of {}",
    "Перевод проходов в SPIR-V…": "Translating the passes to SPIR-V…",
    "— сборка FSR 4.1.1 завершилась (код {}) —\n": "— FSR 4.1.1 build finished (code {}) —\n",
    "FSR 4.1.1 собран и выбран": "FSR 4.1.1 built and selected",
    "Сборка FSR 4.1.1 отменена": "FSR 4.1.1 build cancelled",
    "FSR 4.1.1 не собран": "FSR 4.1.1 was not built",
    "DLL не подходит: подробности в журнале": "The DLL does not fit: details in the log",
    "Не хватает программ для сборки: список в журнале": "Tools for the build are missing: the list is in the log",
    "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или новая видеокарта?): подробности в журнале":
        "The recorded passes do not match what bbport replays (another DLL version, or a new GPU?): details in the log",
    "Не найден загрузчик FidelityFX 2.x": "No FidelityFX loader 2.x found",
    "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с этим Proton, чтобы Steam поставил его рантайм":
        "No suitable Proton found: install GE-Proton 10 or newer (ProtonUp-Qt), or Proton Experimental / Proton-CachyOS in Steam. If you have them, start any game with that Proton in Steam so that Steam installs its runtime",
    "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте папку fsr4_411. Подробности в журнале":
        "No Proton started this DLL's FSR 4.1. Either they are too old (GE-Proton 10+, Proton Experimental, Proton-CachyOS 11 work), or the DLL does not enable FSR 4.1 on this GPU (Steam Deck?): then build on a PC with a Radeon RX 7000/9000 and copy the fsr4_411 folder. Details in the log",
    "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале":
        "On NixOS the recording runs on the system itself (systemd --user) with umu-launcher from nix-shell, and neither was found: install umu-launcher or Nix. Details in the log",
    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.":
        "{}: FSR {}. This is AMD's official FSR 4.0.x: it is enabled on RDNA4 GPUs (RX 9000) only and does not start under Proton on other GPUs, so it cannot be recorded.\n\nFSR 4.1.x is needed: for example FSR4_LATEST from OptiScaler, or the DLL of a game with FSR 4.1.",
    "{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll версии 4.1.x.":
        "{}: this DLL has no FSR 4 inside. amd_fidelityfx_upscaler_dx12.dll version 4.1.x is needed.",
    "Это не загрузчик":
        "This is not the loader",
    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll (у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и DLL апскейлера.":
        "{} is the upscaler DLL. The loader is called amd_fidelityfx_loader_dx12.dll (amd_fidelityfx_dx12.dll in OptiScaler) and lies in the same game folder as the upscaler DLL.",
    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL апскейлера.":
        "{}: version {}. A FidelityFX loader 2.x is needed, from the same game as the upscaler DLL.",
    "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры":
        "No loader next to the DLL: choose amd_fidelityfx_loader_dx12.dll from the game's folder",
    "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)":
        "Build FSR 4.1.1 from amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: the FSR4_LATEST folder, or from a game with FSR 4.1). Needs GE-Proton 10+, Proton Experimental or Proton-CachyOS; 2–5 minutes (RDNA4: twice as long, with the FP8 variant)",
    "Сборка не удалась (код {}): подробности в журнале": "The build failed (code {}): details in the log",
    "Сглаживание в разрешении вывода без модели FSR": "Anti-aliasing at the output resolution, no FSR model",
    "Нет или повреждён файл {}": "Missing or damaged file {}",
    # Effects
    "Эффекты игры": "Game effects",
    "Патчи игры, применяются при запуске": "Game patches, applied at start",
    "Детализация моделей": "Model detail",
    "Как в игре": "As in the game",
    "Максимальная (-2)": "Highest (-2)",
    "Ниже (1)": "Lower (1)",
    "Минимальная (2)": "Lowest (2)",
    "Хроматическая аберрация": "Chromatic aberration",
    "Глубина резкости (DoF)": "Depth of field (DoF)",
    "Размытие в движении": "Motion blur",
    "Затенение SSAO": "SSAO",
    "Собственное сглаживание игры": "The game's own anti-aliasing",
    "Тени от динамических источников": "Dynamic light shadows",
    "Отражения SSR (не было в игре)": "SSR reflections (not in the original game)",
    "Пропуск заставок при запуске": "Skip the intro videos",
    "Свободная камера (Cross + L3 / Space + Z)": "Free camera (Cross + L3 / Space + Z)",
    "Debug menu (левый touchpad / Tab; нужны шрифты)": "Debug menu (left touchpad / Tab; needs fonts)",
    "Нужна папка adhoc из мода Nexus #253 (шрифты adhoc/font) — в dvdroot_ps4 игры или модом; без неё патч не применяется":
        "Needs the adhoc folder from Nexus mod #253 (fonts in adhoc/font), in the game's dvdroot_ps4 or as a mod; without it the patch is not applied",
    # Frame rate
    "Частота кадров": "Frame rate",
    "Режим": "Mode",
    "Какой патч частоты кадров применить к игре": "Which frame rate patch to apply to the game",
    "Без ограничения (патч)": "Unlocked (patch)",
    "30 (как на PS4)": "30 (as on PS4)",
    "Ограничение FPS": "FPS limit",
    "0 — без ограничения; укажите число, чтобы ограничить FPS":
        "0: no limit; set a number to cap the frame rate",
    # Performance
    "Производительность": "Performance",
    "Двухстадийный конвейер GPU": "Two-stage GPU pipeline",
    "Быстрее на 20–30%; при нестабильности выключите": "20–30% faster; turn off if unstable",
    "Авто": "Auto",
    "Включён при 8 и более потоках процессора": "On with 8 or more CPU threads",
    "Включён": "On",
    "Стабильнее, но медленнее": "More stable, but slower",
    "Чтение данных GPU процессором": "GPU data readbacks by the CPU",
    "По умолчанию": "Default",
    "Фоновая загрузка в видеопамять": "Background pre-upload into VRAM",
    "Меньше рывков при подгрузке зон": "Fewer hitches when areas stream in",
    "Обычная": "Normal",
    "Без лишней видеопамяти": "No extra VRAM",
    "Полная": "Full",
    "Около 3 ГБ видеопамяти сверху": "About 3 GB more VRAM",
    # Mode
    "Режим работы": "Mode",
    "Новая модель памяти и трансляции": "New memory and translation model",
    "Новая: видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются. Старая — модель памяти 0.3 со всеми исправлениями. Если драйвер не проходит проверку при запуске — старая":
        "The new one: the GPU works with the game's memory directly, as in a PC game, and graphics commands are translated rather than emulated. The old one: the memory model of 0.3 with all the fixes. If the driver fails the startup check, the old one",
    "Модель памяти и трансляции": "Memory and translation model",
    "Авто: новая на AMD, старая на других": "Auto: new on AMD, old on others",
    "Новая": "New",
    "Старая (как в 0.3)": "Old (as in 0.3)",
    "Синхронизация как в 0.3": "Synchronisation as in 0.3",
    "Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3":
        "For finding regressions: the old memory model and copy waits as released in 0.3",
    "Выключена": "Off",
    "Выключены": "Off",
    # Developer
    "Для разработчика": "Developer",
    "Статистика кадров в журнале": "Frame statistics in the log",
    "Сохранять журнал и статистику в файл": "Save the log and statistics to a file",
    "Диагностика вылетов": "Crash diagnostics",
    "Проверяет кучу игры и записывает записи в её память; немного медленнее":
        "Checks the game's heap and logs writes into its memory; a little slower",
    "В папку logs в каталоге данных: для разбора рывков и вылетов":
        "Into the logs folder of the data directory: to look into stutters and crashes",
    "Профиль GPU в журнале": "GPU profile in the log",
    "Слои валидации Vulkan": "Vulkan validation layers",
    "Сильно замедляет": "Much slower",
    "Доп. переменные (ИМЯ=значение через пробел)": "Extra variables (NAME=value, space-separated)",
}

PT_BR = {
    # Window, pages, buttons
    "Запустить": "Jogar",
    "Остановить": "Parar",
    "Настройки": "Configurações",
    "Журнал": "Log",
    "Не удалось запустить: {}": "Não foi possível iniciar: {}",
    "Копировать": "Copiar",
    "Экспорт журнала…": "Exportar log…",
    "Экспорт журнала": "Exportar log",
    "Журнал скопирован": "Log copiado",
    "Журнал сохранён: {}": "Log salvo: {}",
    "Не удалось сохранить журнал: {}": "Não foi possível salvar o log: {}",
    "\n— игра завершилась (код {}) —\n": "\n— o jogo foi encerrado (código {}) —\n",
    # Launcher language
    "Язык лаунчера": "Idioma do launcher",
    "Как в системе": "Sistema",
    "Применится после перезапуска лаунчера, пока игра запущена":
        "Será aplicado após reiniciar o launcher enquanto o jogo estiver em execução",
    # Game
    "Игра": "Jogo",
    "Папка игры (CUSA03173)": "Pasta do jogo (CUSA03173)",
    "Выбрать папку с eboot.bin": "Selecionar a pasta com eboot.bin",
    "Открыть в файловом менеджере": "Abrir no gerenciador de arquivos",
    "Папка сохранений": "Pasta de salvamentos",
    "Выбрать папку сохранений": "Selecionar a pasta de salvamentos",
    "Вернуть папку по умолчанию": "Restaurar a pasta padrão",
    "Язык системы": "Idioma do sistema",
    "По умолчанию: {}": "Padrão: {}",
    "Найдены сохранения": "Salvamentos encontrados",
    "Сохранений пока нет: игра создаст их здесь": "Ainda não há salvamentos: o jogo os criará aqui",
    "не выбрана": "não selecionada",
    "Найден eboot.bin": "eboot.bin encontrado",
    "Нет eboot.bin в папке": "Não há eboot.bin na pasta",
    "Папка игры (с eboot.bin)": "Pasta do jogo (com eboot.bin)",
    # Languages
    "Английский": "Inglês",
    "Русский": "Russo",
    "Японский": "Japonês",
    "Французский": "Francês",
    "Испанский": "Espanhol",
    "Немецкий": "Alemão",
    "Итальянский": "Italiano",
    "English": "Inglês",
    # Mods
    "Моды": "Mods",
    "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
    "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске.":
        "Extraia cada mod para sua própria pasta (com dvdroot_ps4 ou diretamente com chr/, parts/, ...). "
        "Em caso de conflito de arquivos, o mod que estiver mais abaixo na lista terá prioridade. "
        "Aplicado ao iniciar.",
    "Загружать моды": "Carregar mods",
    "Папка модов": "Pasta de mods",
    "Выбрать папку модов": "Selecionar a pasta de mods",
    "Открыть папку модов": "Abrir a pasta de mods",
    "Обновить список": "Atualizar a lista",
    "Загрузить раньше": "Carregar antes",
    "Загрузить позже": "Carregar depois",
    "Модов нет": "Nenhum mod",
    # Patches
    "Сторонние патчи": "Patches de terceiros",
    "XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. Применяются при запуске.":
        "Patches XML no formato do shadPS4 para a versão 01.09, carregados da pasta de patches. "
        "Aplicados ao iniciar.",
    "Папка патчей": "Pasta de patches",
    "Выбрать папку патчей": "Selecionar a pasta de patches",
    "Открыть папку патчей": "Abrir a pasta de patches",
    "Патчей нет": "Nenhum patch",
    "Автор: {}": "Autor: {}",
    # Screen
    "Экран": "Tela",
    "Разрешение вывода": "Resolução de saída",
    "Апскейлер дорисовывает кадр; Steam Deck — 720p": "O upscaler preenche o quadro; Steam Deck: 720p",
    "Полноэкранный режим": "Tela cheia",
    "Смена разрешения на лету": "Alteração de resolução em tempo real",
    "Без перезапуска, но медленнее на Steam Deck и старых GPU":
        "Não exige reinicialização, mas é mais lento no Steam Deck e em GPUs mais antigas",
    "Авто (по видеокарте)": "Automático (pela GPU)",
    "Выключена (быстрее)": "Desativado (mais rápido)",
    "Включена": "Ativado",
    "Режим показа кадров": "Modo de apresentação",
    "Разрешить HDR": "Permitir HDR",
    # Upscaler
    "Апскейлер": "Upscaler",
    "Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)":
        "Armazenado em bbport.ini; no jogo, altere pelo menu (Insert ou L3+R3)",
    "TAA (нативное сглаживание)": "TAA (antisserrilhamento nativo)",
    "Выключен": "Desativado",
    "Пресет": "Predefinição",
    "Резкость (RCAS)": "Nitidez (RCAS)",
    "Сила резкости": "Intensidade da nitidez",
    "Векторы движения объектов": "Vetores de movimento dos objetos",
    "Меньше гостинга на персонажах; стоит около 10% FPS": "Menos ghosting nos personagens; custa cerca de 10% de FPS",
    "Показывать FPS": "Mostrar FPS",
    "Ассеты найдены": "Recursos encontrados",
    "Нет ассетов: tools/fetch_fsr4_assets.sh": "Recursos ausentes: tools/fetch_fsr4_assets.sh",
    "Ассеты для выбранного режима найдены": "Recursos encontrados para o modo selecionado",
    "{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже":
        "{}. Gere o FSR 4.1.1 a partir da sua própria DLL com o botão “Escolher DLL…” abaixo",
    "FSR 4.1.1 из своей DLL AMD": "FSR 4.1.1 da sua própria DLL da AMD",
    "Отменить": "Cancelar",
    "Выбрать DLL…": "Escolher DLL…",
    "DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)": "DLL do upscaler da AMD (amd_fidelityfx_upscaler_dx12.dll)",
    "нет": "nenhum",
    "Эта DLL не подходит": "Esta DLL não serve",
    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и подобные) устроены иначе и пока не поддерживаются.":
        "{}: versão {}, modelos do FSR 4: {}.\n\nO bbport reproduz apenas o FSR 4.1.x oficial da AMD (modelo v07_fp8_no_scale): por exemplo, o FSR4_LATEST do OptiScaler ou a amd_fidelityfx_upscaler_dx12.dll de um jogo com FSR 4.1. Builds da comunidade (4.0.2b e similares) têm outra estrutura e ainda não são suportados.",
    "Загрузчик не подходит": "O carregador não serve",
    "Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или amd_fidelityfx_dx12.dll)":
        "Carregador do FidelityFX (amd_fidelityfx_loader_dx12.dll ou amd_fidelityfx_dx12.dll)",
    "\n— сборка FSR 4.1.1 из {} —\n": "\n— gerando o FSR 4.1.1 a partir de {} —\n",
    "Подготовка…": "Preparando…",
    "Запись проходов DLL под Proton: {} из {}": "Gravando as passagens da DLL no Proton: {} de {}",
    "Перевод проходов в SPIR-V…": "Traduzindo as passagens para SPIR-V…",
    "— сборка FSR 4.1.1 завершилась (код {}) —\n": "— geração do FSR 4.1.1 concluída (código {}) —\n",
    "FSR 4.1.1 собран и выбран": "FSR 4.1.1 gerado e selecionado",
    "Сборка FSR 4.1.1 отменена": "Geração do FSR 4.1.1 cancelada",
    "FSR 4.1.1 не собран": "O FSR 4.1.1 não foi gerado",
    "DLL не подходит: подробности в журнале": "A DLL não serve: detalhes no log",
    "Не хватает программ для сборки: список в журнале": "Faltam programas para a geração: a lista está no log",
    "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или новая видеокарта?): подробности в журнале":
        "As passagens gravadas não batem com o que o bbport reproduz (outra versão da DLL ou GPU nova?): detalhes no log",
    "Не найден загрузчик FidelityFX 2.x": "Carregador do FidelityFX 2.x não encontrado",
    "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с этим Proton, чтобы Steam поставил его рантайм":
        "Nenhum Proton adequado encontrado: instale o GE-Proton 10 ou mais novo (ProtonUp-Qt), ou o Proton Experimental / Proton-CachyOS na Steam. Se já os tiver, abra qualquer jogo com esse Proton na Steam para que a Steam instale o runtime dele",
    "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте папку fsr4_411. Подробности в журнале":
        "Nenhum Proton iniciou o FSR 4.1 desta DLL. Ou eles são antigos demais (funcionam GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), ou a DLL não ativa o FSR 4.1 nesta GPU (Steam Deck?): nesse caso, gere em um PC com Radeon RX 7000/9000 e copie a pasta fsr4_411. Detalhes no log",
    "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале":
        "No NixOS a gravação roda no próprio sistema (systemd --user) com o umu-launcher do nix-shell, e eles não foram encontrados: instale o umu-launcher ou o Nix. Detalhes no log",
    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.":
        "{}: FSR {}. Este é o FSR 4.0.x oficial da AMD: ele só é ativado em GPUs RDNA4 (RX 9000) e não inicia no Proton em outras GPUs, então não pode ser gravado.\n\nÉ preciso o FSR 4.1.x: por exemplo, o FSR4_LATEST do OptiScaler ou a DLL de um jogo com FSR 4.1.",
    "{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll версии 4.1.x.":
        "{}: esta DLL não contém o FSR 4. É preciso a amd_fidelityfx_upscaler_dx12.dll versão 4.1.x.",
    "Это не загрузчик":
        "Este não é o carregador",
    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll (у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и DLL апскейлера.":
        "{} é a DLL do upscaler. O carregador se chama amd_fidelityfx_loader_dx12.dll (amd_fidelityfx_dx12.dll no OptiScaler) e fica na mesma pasta do jogo que a DLL do upscaler.",
    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL апскейлера.":
        "{}: versão {}. É preciso um carregador do FidelityFX 2.x, do mesmo jogo que a DLL do upscaler.",
    "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры":
        "Não há carregador ao lado da DLL: escolha a amd_fidelityfx_loader_dx12.dll da pasta do jogo",
    "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)":
        "Gerar o FSR 4.1.1 a partir de amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: pasta FSR4_LATEST, ou de um jogo com FSR 4.1). Requer GE-Proton 10+, Proton Experimental ou Proton-CachyOS; 2–5 minutos (RDNA4: o dobro, também com a variante FP8)",
    "Сборка не удалась (код {}): подробности в журнале": "A geração falhou (código {}): detalhes no log",
    "Сглаживание в разрешении вывода без модели FSR": "Antisserrilhamento na resolução de saída, sem modelo FSR",
    "Нет или повреждён файл {}": "Arquivo {} ausente ou corrompido",
    # Presets that are English source strings in the launcher
    "Native AA": "AA nativo",
    "Quality (x1.5)": "Qualidade (x1.5)",
    "Balanced (x1.7)": "Balanceado (x1.7)",
    "Performance (x2)": "Desempenho (x2)",
    "Ultra Performance (x3)": "Desempenho extremo (x3)",
    # Effects
    "Эффекты игры": "Efeitos do jogo",
    "Патчи игры, применяются при запуске": "Patches do jogo, aplicados ao iniciar",
    "Детализация моделей": "Detalhe dos modelos",
    "Как в игре": "Como no jogo",
    "Максимальная (-2)": "Máximo (-2)",
    "Ниже (1)": "Menor (1)",
    "Минимальная (2)": "Mínimo (2)",
    "Хроматическая аберрация": "Aberração cromática",
    "Глубина резкости (DoF)": "Profundidade de campo (DoF)",
    "Размытие в движении": "Desfoque de movimento",
    "Затенение SSAO": "SSAO",
    "Собственное сглаживание игры": "Antisserrilhamento nativo do jogo",
    "Тени от динамических источников": "Sombras de luzes dinâmicas",
    "Отражения SSR (не было в игре)": "Reflexos SSR (não existiam no jogo original)",
    "Пропуск заставок при запуске": "Pular vídeos de introdução",
    "Свободная камера (Cross + L3 / Space + Z)": "Câmera livre (Cross + L3 / Space + Z)",
    "Debug menu (левый touchpad / Tab; нужны шрифты)": "Menu de depuração (touchpad esquerdo / Tab; requer fontes)",
    "Нужна папка adhoc из мода Nexus #253 (шрифты adhoc/font) — в dvdroot_ps4 игры или модом; без неё патч не применяется":
        "Requer a pasta adhoc do mod #253 do Nexus (fontes em adhoc/font), no dvdroot_ps4 do jogo ou como mod; sem ela o patch não é aplicado",
    # Frame rate
    "Частота кадров": "Taxa de quadros",
    "Режим": "Modo",
    "Какой патч частоты кадров применить к игре": "Qual patch de taxa de quadros aplicar ao jogo",
    "Без ограничения (патч)": "Sem limite (patch)",
    "30 (как на PS4)": "30 (como no PS4)",
    "Ограничение FPS": "Limite de FPS",
    "0 — по частоте экрана (не выше 120 Гц); укажите число, чтобы ограничить иначе":
        "0: taxa de atualização da tela (até 120 Hz); defina um número para usar outro limite",
    # Performance
    "Производительность": "Desempenho",
    "Двухстадийный конвейер GPU": "Pipeline de GPU em dois estágios",
    "Быстрее на 20–30%; при нестабильности выключите": "20–30% mais rápido; desative se houver instabilidade",
    "Авто (8+ потоков)": "Automático (8+ threads)",
    "Включён": "Ativado",
    "Выключен (стабильнее)": "Desativado (mais estável)",
    "Чтение данных GPU процессором": "Leitura de dados da GPU pela CPU",
    "Relaxed (по умолчанию)": "Relaxed (padrão)",
    "Выключены": "Desativado",
    # Developer
    "Для разработчика": "Desenvolvedor",
    "Режим работы": "Modo",
    "Новая модель памяти и трансляции": "Novo modelo de memória e tradução",
    "Новая: видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются. Старая — модель памяти 0.3 со всеми исправлениями. Если драйвер не проходит проверку при запуске — старая":
        "O novo: a GPU usa a memória do jogo diretamente, como em um jogo de PC, e os comandos gráficos são traduzidos em vez de emulados. O antigo: o modelo de memória da 0.3 com todas as correções. Se o driver falhar na verificação inicial, o antigo",
    "Модель памяти и трансляции": "Modelo de memória e tradução",
    "Авто: новая на AMD, старая на других": "Automático: novo na AMD, antigo nas outras",
    "Новая": "Novo",
    "Старая (как в 0.3)": "Antigo (como na 0.3)",
    "Синхронизация как в 0.3": "Sincronização como na 0.3",
    "Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3":
        "Para encontrar regressões: o modelo de memória antigo e as esperas de cópias como na versão 0.3",
    "Статистика кадров в журнале": "Estatísticas de quadros no log",
    "Профиль GPU в журнале": "Perfil da GPU no log",
    "Слои валидации Vulkan": "Camadas de validação do Vulkan",
    "Сильно замедляет": "Muito mais lento",
    "Доп. переменные (ИМЯ=значение через пробел)": "Variáveis extras (NOME=valor, separadas por espaço)",
}


ZH_CN = {
    # Window, pages, buttons
    "Запустить": "启动",
    "Остановить": "停止",
    "Настройки": "设置",
    "Журнал": "日志",
    "Не удалось запустить: {}": "无法启动：{}",
    "\n— игра завершилась (код {}) —\n": "\n— 游戏已退出（代码 {}）—\n",
    # Launcher language
    "Язык лаунчера": "启动器语言",
    "Как в системе": "跟随系统",
    "Применится после перезапуска лаунчера, пока игра запущена":
        "游戏运行期间更改语言，需要重启启动器后才会生效",
    # Game
    "Игра": "游戏",
    "Папка игры (CUSA03173)": "游戏文件夹（CUSA03173）",
    "Выбрать папку с eboot.bin": "选择包含 eboot.bin 的文件夹",
    "Открыть в файловом менеджере": "在文件管理器中打开",
    "Папка сохранений": "存档文件夹",
    "Выбрать папку сохранений": "选择存档文件夹",
    "Вернуть папку по умолчанию": "恢复默认文件夹",
    "Язык системы": "系统语言",
    "По умолчанию: {}": "默认：{}",
    "Найдены сохранения": "已找到存档",
    "Сохранений пока нет: игра создаст их здесь": "暂无存档：游戏会在此创建",
    "не выбрана": "未选择",
    "Найден eboot.bin": "已找到 eboot.bin",
    "Нет eboot.bin в папке": "文件夹中没有 eboot.bin",
    "Управление": "操作设置",
    "Клавиатура": "键盘",
    "Геймпад": "手柄",
    "Назначение кнопок; применяется при запуске игры": "按键绑定；在游戏启动时生效",
    "Назначить": "绑定",
    "Сбросить": "重置",
    "{} (по умолчанию)": "{}（默认）",
    "не назначено": "未绑定",
    "Нажмите клавишу или кнопку… (Esc — отмена)": "按下一个按键或手柄按钮…（按 Esc 取消）",
    "Крест": "叉键",
    "Круг": "圆圈键",
    "Квадрат": "方块键",
    "Треугольник": "三角键",
    "Тачпад, левая половина (жесты)": "触摸板左侧（手势）",
    "Тачпад, правая половина (личные вещи)": "触摸板右侧（重要物品）",
    "Крестовина вверх": "方向键上",
    "Крестовина вниз": "方向键下",
    "Крестовина влево": "方向键左",
    "Крестовина вправо": "方向键右",
    "Движение вперёд": "向前移动",
    "Движение назад": "向后移动",
    "Движение влево": "向左移动",
    "Движение вправо": "向右移动",
    "Камера вверх": "镜头向上",
    "Камера вниз": "镜头向下",
    "Камера влево": "镜头向左",
    "Камера вправо": "镜头向右",
    "Контроллер": "控制器",
    "Выбранный берётся, как только подключится": "所选手柄一旦连接即会被使用",
    "Первый подключённый": "第一个已连接的手柄",
    "{} (не подключён)": "{}（未连接）",
    "Нужно обновление 1.09: скопируйте файлы дампа обновления 1.09 в папку игры с заменой (найдена версия {})":
        "需要 1.09 更新：请将转储出的 1.09 更新文件复制到游戏文件夹并覆盖（当前检测到版本 {}）",
    "eboot.bin не от версии 1.09: скопируйте eboot.bin из дампа обновления 1.09 в папку игры с заменой":
        "eboot.bin 不是 1.09 版本：请将 1.09 更新转储中的 eboot.bin 复制到游戏文件夹并覆盖",
    "Поддерживается только CUSA03173 с обновлением 1.09 (найдено {})":
        "仅支持已安装 1.09 更新的 CUSA03173（当前检测到 {}）",
    "eboot.bin не читается как расшифрованный исполняемый файл PS4: сделайте дамп заново":
        "无法将 eboot.bin 识别为已解密的 PS4 可执行文件：请重新转储游戏",
    "Файлы игры повреждены при распаковке: шейдеры не распаковываются, игра зависнет на загрузке. Распакуйте игру и обновление 1.09 заново исправленным инструментом (issue #81)":
        "游戏文件在解包时已损坏：着色器无法解压，游戏会在加载时卡住。请使用修复后的工具重新解包游戏和 1.09 更新（issue #81）",
    "Bloodborne CUSA03173, версия 1.09": "血源诅咒 CUSA03173，版本 1.09",
    "Папка игры (с eboot.bin)": "游戏文件夹（含 eboot.bin）",
    # Languages
    "Английский": "英语",
    "Русский": "俄语",
    "Японский": "日语",
    "Французский": "法语",
    "Испанский": "西班牙语",
    "Немецкий": "德语",
    "Итальянский": "意大利语",
    "English": "英语",
    # Mods
    "Моды": "模组",
    "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
    "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске.":
        "将每个模组解压到独立的文件夹中（文件夹内可以是 dvdroot_ps4，也可以直接是 chr/、parts/ 等）。"
        "文件冲突时，列表中靠下的模组优先。设置会在游戏启动时生效。",
    "Загружать моды": "加载模组",
    "Папка модов": "模组文件夹",
    "Выбрать папку модов": "选择模组文件夹",
    "Открыть папку модов": "打开模组文件夹",
    "Обновить список": "刷新列表",
    "Загрузить раньше": "提前加载",
    "Загрузить позже": "延后加载",
    "Модов нет": "没有模组",
    # Patches
    "Сторонние патчи": "第三方补丁",
    "XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. Применяются при запуске.":
        "补丁文件夹中、shadPS4 格式、适用于 01.09 版本的 XML 补丁。会在游戏启动时生效。",
    "Папка патчей": "补丁文件夹",
    "Выбрать папку патчей": "选择补丁文件夹",
    "Открыть папку патчей": "打开补丁文件夹",
    "Патчей нет": "没有补丁",
    "Автор: {}": "作者：{}",
    # Screen
    "Экран": "显示",
    "Разрешение вывода": "输出分辨率",
    "Апскейлер дорисовывает кадр; Steam Deck — 720p": "超分辨率会将画面放大到此分辨率；Steam Deck 建议 720p",
    "Полноэкранный режим": "全屏模式",
    "Смена разрешения на лету": "实时切换分辨率",
    "Без перезапуска, но медленнее на Steam Deck и старых GPU":
        "无需重启，但在 Steam Deck 和较旧的显卡上速度更慢",
    "Авто (по видеокарте)": "自动（根据显卡）",
    "Выключена (быстрее)": "关闭（更快）",
    "Включена": "开启",
    "Режим показа кадров": "呈现模式",
    "Разрешить HDR": "允许 HDR",
    # Upscaler
    "Апскейлер": "超分辨率",
    "Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)":
        "保存在 bbport.ini 中；游戏内可通过菜单切换（Insert 或 L3+R3）",
    "TAA (нативное сглаживание)": "TAA（原生抗锯齿）",
    "Выключен": "关闭",
    "Пресет": "预设",
    "Резкость (RCAS)": "锐化（RCAS）",
    "Сила резкости": "锐化强度",
    "Векторы движения объектов": "物体运动矢量",
    "Меньше гостинга на персонажах; стоит около 10% FPS": "减少角色拖影；大约损失 10% 帧率",
    "Показывать FPS": "显示帧率",
    "Ассеты найдены": "已找到资源文件",
    "Нет ассетов: tools/fetch_fsr4_assets.sh": "缺少资源文件：请运行 tools/fetch_fsr4_assets.sh",
    "Ассеты для выбранного режима найдены": "已找到所选模式对应的资源文件",
    "{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже":
        "{}。请用下方的「选择 DLL…」按钮，从你自己的 DLL 构建 FSR 4.1.1",
    "FSR 4.1.1 из своей DLL AMD": "用你自己的 AMD DLL 构建 FSR 4.1.1",
    "Отменить": "取消",
    "Выбрать DLL…": "选择 DLL…",
    "DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)": "AMD 超分辨率 DLL（amd_fidelityfx_upscaler_dx12.dll）",
    "нет": "无",
    "Эта DLL не подходит": "此 DLL 不适用",
    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и подобные) устроены иначе и пока не поддерживаются.":
        "{}：版本 {}，FSR 4 模型：{}。\n\nbbport 仅支持复现 AMD 官方的 FSR 4.1.x（v07_fp8_no_scale 模型）：例如 OptiScaler 中的 FSR4_LATEST，或来自支持 FSR 4.1 的游戏的 amd_fidelityfx_upscaler_dx12.dll。社区构建版本（如 4.0.2b 等）结构不同，暂不支持。",
    "Загрузчик не подходит": "此加载器不适用",
    "Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или amd_fidelityfx_dx12.dll)":
        "FidelityFX 加载器（amd_fidelityfx_loader_dx12.dll 或 amd_fidelityfx_dx12.dll）",
    "\n— сборка FSR 4.1.1 из {} —\n": "\n— 正在从 {} 构建 FSR 4.1.1 —\n",
    "Подготовка…": "准备中…",
    "Запись проходов DLL под Proton: {} из {}": "正在 Proton 下录制 DLL 渲染通道：第 {} / {} 个",
    "Перевод проходов в SPIR-V…": "正在将渲染通道转换为 SPIR-V…",
    "— сборка FSR 4.1.1 завершилась (код {}) —\n": "— FSR 4.1.1 构建已完成（代码 {}）—\n",
    "FSR 4.1.1 собран и выбран": "FSR 4.1.1 已构建并选用",
    "Сборка FSR 4.1.1 отменена": "FSR 4.1.1 构建已取消",
    "FSR 4.1.1 не собран": "FSR 4.1.1 尚未构建",
    "DLL не подходит: подробности в журнале": "DLL 不适用：详情见日志",
    "Не хватает программ для сборки: список в журнале": "缺少构建所需的程序：清单见日志",
    "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или новая видеокарта?): подробности в журнале":
        "录制的渲染通道与 bbport 复现的内容不一致（可能是 DLL 版本不同，或显卡型号较新？）：详情见日志",
    "Не найден загрузчик FidelityFX 2.x": "未找到 FidelityFX 2.x 加载器",
    "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с этим Proton, чтобы Steam поставил его рантайм":
        "未找到合适的 Proton：请安装 GE-Proton 10 或更高版本（可用 ProtonUp-Qt），或在 Steam 中使用 Proton Experimental / Proton-CachyOS。如果已经安装，请在 Steam 中用该 Proton 启动任意游戏，让 Steam 安装它的运行时",
    "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте папку fsr4_411. Подробности в журнале":
        "没有任何 Proton 能从此 DLL 启动 FSR 4.1。要么是 Proton 版本太旧（需要 GE-Proton 10+、Proton Experimental 或 Proton-CachyOS 11），要么是这块显卡无法启用 FSR 4.1（例如 Steam Deck？）：这种情况下，请在装有 Radeon RX 7000/9000 的电脑上构建，再把 fsr4_411 文件夹复制过来。详情见日志",
    "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале":
        "在 NixOS 上，录制通过 nix-shell 中的 umu-launcher 在系统本身（systemd --user）上进行，但未能找到它们：请安装 umu-launcher 或 Nix。详情见日志",
    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.":
        "{}：FSR {}。这是 AMD 官方的 FSR 4.0.x：仅在 RDNA4 显卡（RX 9000）上启用，在其他显卡上通过 Proton 无法运行，因此无法录制。\n\n需要 FSR 4.1.x：例如 OptiScaler 中的 FSR4_LATEST，或来自支持 FSR 4.1 的游戏的 DLL。",
    "{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll версии 4.1.x.":
        "{}：此 DLL 中不包含 FSR 4。需要版本为 4.1.x 的 amd_fidelityfx_upscaler_dx12.dll。",
    "Это не загрузчик":
        "这不是加载器",
    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll (у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и DLL апскейлера.":
        "{} 是超分辨率 DLL。加载器名为 amd_fidelityfx_loader_dx12.dll（在 OptiScaler 中为 amd_fidelityfx_dx12.dll），与超分辨率 DLL 位于同一个游戏文件夹中。",
    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL апскейлера.":
        "{}：版本 {}。需要来自同一游戏、与超分辨率 DLL 配套的 FidelityFX 2.x 加载器。",
    "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры":
        "DLL 旁边没有加载器：请从游戏文件夹中选择 amd_fidelityfx_loader_dx12.dll",
    "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)":
        "从 amd_fidelityfx_upscaler_dx12.dll 4.1.x 构建 FSR 4.1.1（OptiScaler：FSR4_LATEST 文件夹，或来自支持 FSR 4.1 的游戏）。需要 GE-Proton 10+、Proton Experimental 或 Proton-CachyOS；耗时 2–5 分钟（RDNA4：耗时加倍，并额外构建 FP8 版本）",
    "Сборка не удалась (код {}): подробности в журнале": "构建失败（代码 {}）：详情见日志",
    "Сглаживание в разрешении вывода без модели FSR": "在输出分辨率下进行抗锯齿，不使用 FSR 模型",
    "Нет или повреждён файл {}": "文件 {} 缺失或已损坏",
    # Effects
    "Эффекты игры": "游戏效果",
    "Патчи игры, применяются при запуске": "游戏补丁，会在启动时生效",
    "Детализация моделей": "模型细节",
    "Как в игре": "游戏默认",
    "Максимальная (-2)": "最高（-2）",
    "Ниже (1)": "较低（1）",
    "Минимальная (2)": "最低（2）",
    "Хроматическая аберрация": "色差",
    "Глубина резкости (DoF)": "景深（DoF）",
    "Размытие в движении": "动态模糊",
    "Затенение SSAO": "SSAO 环境光遮蔽",
    "Собственное сглаживание игры": "游戏自带抗锯齿",
    "Тени от динамических источников": "动态光源阴影",
    "Отражения SSR (не было в игре)": "SSR 反射（原版游戏中没有）",
    "Пропуск заставок при запуске": "跳过开场动画",
    "Свободная камера (Cross + L3 / Space + Z)": "自由镜头（叉键 + L3 / 空格 + Z）",
    "Debug menu (левый touchpad / Tab; нужны шрифты)": "调试菜单（左触摸板 / Tab；需要字体文件）",
    "Установите DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода Nexus #253":
        "请从 Nexus 模组 #253 中，将 DbgFont14h.ccm 和 DbgFont14h.tpf 安装到 dvdroot_ps4/font",
    # Frame rate
    "Частота кадров": "帧率",
    "Режим": "模式",
    "Какой патч частоты кадров применить к игре": "对游戏应用哪种帧率补丁",
    "Без ограничения (патч)": "不限制（补丁解锁）",
    "30 (как на PS4)": "30（与 PS4 一致）",
    "Ограничение FPS": "帧率上限",
    "0 — без ограничения; укажите число, чтобы ограничить FPS":
        "0 表示不限制；填写一个数字可限制帧率",
    # Performance
    "Производительность": "性能",
    "Двухстадийный конвейер GPU": "双阶段 GPU 管线",
    "Быстрее на 20–30%; при нестабильности выключите": "可提速 20–30%；如不稳定请关闭",
    "Авто": "自动",
    "Включён при 8 и более потоках процессора": "处理器有 8 个或以上线程时自动开启",
    "Включён": "开启",
    "Стабильнее, но медленнее": "更稳定，但速度更慢",
    "Чтение данных GPU процессором": "由 CPU 读取 GPU 数据（readback）",
    "По умолчанию": "默认",
    "Фоновая загрузка в видеопамять": "后台预加载至显存",
    "Меньше рывков при подгрузке зон": "区域加载时卡顿更少",
    "Обычная": "普通",
    "Без лишней видеопамяти": "不占用额外显存",
    "Полная": "完整",
    "Около 3 ГБ видеопамяти сверху": "额外占用约 3 GB 显存",
    # Mode
    "Режим работы": "工作模式",
    "Новая модель памяти и трансляции": "新内存与指令转译模型",
    "Новая: видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются. Старая — модель памяти 0.3 со всеми исправлениями. Если драйвер не проходит проверку при запуске — старая":
        "新模型：显卡像 PC 游戏那样直接读写游戏内存，图形指令被转译而非模拟。旧模型：0.3 版的内存模型，包含所有修复。如果驱动未通过启动检查，则使用旧模型",
    "Модель памяти и трансляции": "内存与指令转译模型",
    "Авто: новая на AMD, старая на других": "自动：AMD 用新模型，其他显卡用旧模型",
    "Новая": "新模型",
    "Старая (как в 0.3)": "旧模型（与 0.3 版相同）",
    "Синхронизация как в 0.3": "与 0.3 版相同的同步方式",
    "Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3":
        "用于排查回归问题：采用与 0.3 版发布时相同的旧内存模型与复制等待方式",
    "Выключена": "关闭",
    "Выключены": "关闭",
    # Developer
    "Для разработчика": "开发者选项",
    "Статистика кадров в журнале": "在日志中记录帧统计",
    "Сохранять журнал и статистику в файл": "将日志和统计信息保存到文件",
    "Диагностика вылетов": "崩溃诊断",
    "Проверяет кучу игры и записывает записи в её память; немного медленнее":
        "检查游戏堆内存并记录对其的写入操作；速度会略有下降",
    "В папку logs в каталоге данных: для разбора рывков и вылетов":
        "保存到数据目录下的 logs 文件夹：便于排查卡顿和崩溃问题",
    "Профиль GPU в журнале": "在日志中记录 GPU 性能剖析",
    "Слои валидации Vulkan": "Vulkan 验证层",
    "Сильно замедляет": "会大幅降低速度",
    "Доп. переменные (ИМЯ=значение через пробел)": "附加环境变量（用空格分隔的 名称=值）",
}


def system_language():
    for key in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        value = os.environ.get(key, "")
        if value:
            locale = value.lower().replace("-", "_")
            if locale.startswith("ru"):
                return "ru"
            if locale.startswith("pt_br"):
                return "pt_BR"
            if locale.startswith("zh"):
                return "zh_CN"
            return "en"
    return "en"


_language = "ru"


def set_language(choice):
    """choice: "ru", "en", "pt_BR", "zh_CN" or "" (the system's)."""
    global _language
    _language = choice if choice in ("ru", "en", "pt_BR", "zh_CN") else system_language()


def language():
    return _language


def tr(text):
    if _language == "en":
        return EN.get(text, text)
    if _language == "pt_BR":
        # Strings added after the translation fall back to English, not to the Russian key.
        return PT_BR.get(text, EN.get(text, text))
    if _language == "zh_CN":
        return ZH_CN.get(text, EN.get(text, text))
    return text

