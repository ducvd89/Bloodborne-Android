// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_menu.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bbport_settings.h"

namespace BbGameMenu {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The game's menu code (eboot 1.09 vaddrs; found from the System menu builder 0x1bb3ad0).
constexpr u64 MsgLookup = 0xf6f340;      ///< (repository, table, category, id) -> UTF-16 text
constexpr u64 AddPageItem = 0x1b4e2a0;   ///< (menu, name+help texts, page factory, out)
constexpr u64 MakeText = 0x1ae8cc0;      ///< (out 0x40 bytes, category, id): a text entry
constexpr u64 FactoryToFunction = 0x1b6ed90; ///< ({vtable, fn}, out std::function) -> its __f_
constexpr u64 CreatePage = 0x1bb4c70;    ///< (out, arg, content std::function): an options page
constexpr u64 OpenPage = 0x1b20900;      ///< (a, b, layout name, rows fn, 0, 0): the page's content
constexpr u64 ListPush = 0x1b1bbd0;      ///< (value list, {u32 value, text}): up to 32 values
constexpr u64 ListRow = 0x1b78ab0;   ///< (page, texts, int* value, value list, int* default): pop-up list
constexpr u64 PairPush = 0x1b2c200;  ///< (two-value list, {u8 value, text})
constexpr u64 PairRow = 0x1b2a100;   ///< (page, texts, u8* value, two-value list, u8* default): left/right
constexpr u64 SliderRow = 0x1b2ac00; ///< (page, texts, u8* value 0..10, u8* default)
constexpr u64 FactoryVtable = 0x533d120; ///< a System item's {vtable, page factory fn}
constexpr u64 ContentVtable = 0x5343880; ///< an options page's {vtable, content fn}
constexpr u64 ControlsLayout = 0x4934065; ///< "ControllSetting": six plain rows, the layout used
constexpr u32 TextName = 0xc8, TextHelp = 0xc9; // SP_menu text / SP_one-line help
constexpr u32 ScreenSoundItem = 110001;         // "Screen/Sound" in the System menu: ours follow it
constexpr u32 ControlsTitle = 113020;           // the ControllSetting layout's title ("Controls")
constexpr std::size_t RowsPerPage = 6;          // ControllSetting's rows

const u8 MsgLookupPrologue[] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6};
const u8 AddPageItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

u64 image_base = 0;
template <typename F>
F Game(u64 va) {
    return reinterpret_cast<F>(image_base + va);
}

// ---- Texts: ids from IdBase up, looked up by the game like its own messages. ----
constexpr u32 IdBase = 0x7f0000;
struct Text {
    std::u16string english, russian;
};
std::vector<Text> texts;

std::u16string Utf16(const char* utf8) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        u32 c = *p++;
        if (c >= 0xf0) {
            c = (c & 7) << 18 | (p[0] & 0x3f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
            p += 3;
        } else if (c >= 0xe0) {
            c = (c & 15) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0) {
            c = (c & 31) << 6 | (p[0] & 0x3f);
            p += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}
u32 AddText(const char* english, const char* russian) {
    texts.push_back({Utf16(english), Utf16(russian)});
    return IdBase + u32(texts.size() - 1);
}
/// The game's own language decides (its "Screen/Sound" text in Cyrillic: Russian, else English);
/// the port's menu language until the game has looked that text up.
enum class GameLanguage { Unknown, English, Russian };
std::atomic<GameLanguage> game_language{GameLanguage::Unknown};

const char16_t* TextOf(u32 id) {
    const Text& t = texts[id - IdBase];
    const GameLanguage game = game_language.load();
    const bool russian = game == GameLanguage::Unknown
                             ? BbSettings::Get().menu_language == BbSettings::MenuLanguage::Russian
                             : game == GameLanguage::Russian;
    return russian ? t.russian.c_str() : t.english.c_str();
}

// ---- The port's values the game's rows edit (ints: the list rows bind ints). ----
enum Field : int {
    OutputRes, Upscaler, Preset, Sharpness, ShowFps,
    ModelLod, FirstEffect, FieldCount = FirstEffect + BbSettings::EffectCount
};
int values[FieldCount];  // what the game's rows show and change (list rows bind ints)
u8 bytes[FieldCount];    // the same for toggle and slider rows (they bind bytes)
bool byte_field[FieldCount]; // fields shown by toggle or slider rows
int default_values[FieldCount]; // the port's defaults: what the page's "Default" sets
u8 default_bytes[FieldCount];
int applied[FieldCount]; // what Poll applied last
std::atomic<bool> values_valid{false};
constexpr float SharpnessStep = 0.2f; // the game's slider: 0..10 -> 0.0 .. 2.0
constexpr int SharpnessSteps = 11;
constexpr int LodValues[] = {-2, 0, 1, 2};
/// Rows with a few ordered choices on the game's 0..10 slider (the quality preset, the output
/// resolution): a pop-up list let the rows below it show through, so only the last row of a page
/// is a list. The game keeps its own copy of a slider's value while the page is open, so every
/// position stands for a choice (ranges, as the row's help says), and a choice is shown at
/// ChoicePosition.
int steps_count[FieldCount]; ///< choices of a slider-with-choices row, else 0

int ChoiceFromPosition(u8 position, int count) {
    return std::clamp((int(position) * (count - 1) + 5) / 10, 0, count - 1);
}

u8 ChoicePosition(int choice, int count) {
    return u8((std::clamp(choice, 0, count - 1) * 10 * 2 + (count - 1)) / (2 * (count - 1)));
}

/// The rows' values of settings `s`.
void Fill(const BbSettings::Values& s, int* values) {
    values[OutputRes] = s.output_res;
    values[Upscaler] = s.upscaler;
    values[Preset] = s.preset;
    // One slider for both: 0 is sharpening off.
    values[Sharpness] =
        s.sharpen ? std::clamp(int(std::lround(s.sharpness / SharpnessStep)), 1, SharpnessSteps - 1) : 0;
    values[ShowFps] = s.show_fps;
    values[ModelLod] = 1;
    for (int i = 0; i < 4; ++i) {
        if (LodValues[i] == s.model_lod) values[ModelLod] = i;
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        values[FirstEffect + e] = s.effects[e];
    }
}

void LoadValues() {
    Fill(BbSettings::Get(), values);
    for (int f = 0; f < FieldCount; ++f) {
        bytes[f] = steps_count[f] ? ChoicePosition(values[f], steps_count[f]) : u8(values[f]);
    }
    std::memcpy(applied, values, sizeof(values));
    values_valid = true;
}

// ---- Pages and rows ----
enum class Kind { List, Toggle, Slider, Steps };
struct Row {
    Field field;
    u32 name, help;
    std::vector<u32> choices; ///< value texts, value = index (a toggle: off, on)
    Kind kind = Kind::List;
};
struct Page {
    u32 name, help;
    std::vector<Row> rows; ///< at most RowsPerPage (the layout's)
};
std::vector<Page> pages;

void DefineTexts() {
    const u32 off = AddText("Off", "Выкл"), on = AddText("On", "Вкл");
    const std::vector<u32> toggle = {off, on};
    const auto page = [](const char* en, const char* ru, const char* help_en, const char* help_ru) {
        const u32 name = AddText(en, ru);
        pages.push_back({name, AddText(help_en, help_ru), {}});
    };
    const auto row = [](Field f, const char* en, const char* ru, const char* help_en,
                        const char* help_ru, std::vector<u32> choices, Kind kind = Kind::List) {
        if (kind == Kind::List && choices.size() == 2) {
            kind = Kind::Toggle; // left/right in place, as the game's own on/off rows
        }
        pages.back().rows.push_back(
            {f, AddText(en, ru), AddText(help_en, help_ru), std::move(choices), kind});
    };

    // Two pages of the ControllSetting layout's six rows (the game's other layouts are fixed
    // forms): the picture, then the game's effects. The other patches (intros, the free camera,
    // the debug menu) and the game's own AA are in the overlay menu and the launcher.
    page("Display", "Изображение", "Resolution, upscaler, sharpness and model detail (bbport)",
         "Разрешение, апскейлер, резкость и детализация (bbport)");
    // Sliders and toggles first, the pop-up list last: a list opens downwards over the rows
    // below it, and their values showed through it.
    row(Preset, "Quality preset", "Пресет",
        "Scene resolution: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
        "Разрешение сцены: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
        {AddText("Native AA", "Native AA"), AddText("Quality", "Quality"), AddText("Balanced", "Balanced"),
         AddText("Performance", "Performance"), AddText("Ultra Performance", "Ultra Performance")},
        Kind::Steps);
    row(Sharpness, "Sharpness", "Резкость", "RCAS after the upscaler: 0 off, 10 strongest",
        "RCAS после апскейлера: 0 выкл, 10 сильнее всего", {}, Kind::Slider);
    row(OutputRes, "Output resolution", "Разрешение вывода",
        "Final frame: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
        "Готовый кадр: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
        {AddText("1280 x 720", "1280 x 720"), AddText("1920 x 1080", "1920 x 1080"),
         AddText("2560 x 1440", "2560 x 1440"), AddText("3840 x 2160", "3840 x 2160")},
        Kind::Steps);
    row(ShowFps, "FPS counter", "Счётчик FPS", "Frame rate in the top right corner",
        "Частота кадров в правом верхнем углу", toggle);
    row(ModelLod, "Model detail", "Детализация",
        "After a restart: 0-1 highest, 2-4 game default, 5-7 lower, 8-10 lowest",
        "После перезапуска: 0-1 максимальная, 2-4 как в игре, 5-7 ниже, 8-10 минимальная",
        {AddText("Highest", "Максимальная"), AddText("Game default", "Как в игре"),
         AddText("Lower", "Ниже"), AddText("Lowest", "Минимальная")},
        Kind::Steps);
    // The page's only pop-up list, last: it opens below itself.
    row(Upscaler, "Upscaler", "Апскейлер", "Temporal upscaler and anti-aliasing",
        "Временной апскейлер и сглаживание",
        {off, AddText("FSR 3.1", "FSR 3.1"), AddText("FSR 4", "FSR 4"), AddText("FSR 4.1.1", "FSR 4.1.1"),
         AddText("TAA", "TAA"), AddText("DLSS", "DLSS")});

    page("Effects", "Эффекты", "The game's effects (bbport; after a restart)",
         "Эффекты игры (bbport; после перезапуска)");

    // Short names for the game's narrow name column (BbSettings::Effects order); the help line
    // has the details.
    struct Short {
        const char *key, *en, *ru, *help_en, *help_ru;
        bool graphics = true;
    };
    static const Short shorts[] = {
        {"effect_chromatic_aberration", "Chromatic aberration", "Хром. аберрация",
         "Colour fringes at the frame edges", "Цветные каймы по краям кадра"},
        {"effect_dof", "Depth of field", "Глубина резкости", "Blur of distant and near objects (DoF)",
         "Размытие дальних и близких объектов (DoF)"},
        {"effect_motion_blur", "Motion blur", "Размытие в движении", "Blur when the camera turns",
         "Размытие при повороте камеры"},
        {"effect_ssao", "Ambient occlusion", "Затенение SSAO", "Shading in corners and contacts (SSAO)",
         "Затенение в углах и местах касания (SSAO)"},
        {"effect_game_aa", "Game's own AA", "Сглаживание игры", "The game's own anti-aliasing",
         "Собственное сглаживание игры", false},
        {"effect_dynamic_shadows", "Dynamic shadows", "Динамические тени", "Shadows from dynamic lights",
         "Тени от динамических источников света"},
        {"effect_ssr", "SSR reflections", "Отражения SSR", "Screen-space reflections (not in the original)",
         "Экранные отражения (не было в оригинале)"},
        {"skip_intro", "Skip intros", "Пропуск заставок", "Skip the startup logos and intro",
         "Пропуск логотипов и заставки при запуске", false},
        {"debug_camera", "Free camera", "Свободная камера", "Toggled with Cross + L3",
         "Включается Cross + L3", false},
        {"debug_menu", "Debug menu", "Debug menu", "The game's debug menu (requires font files)",
         "Отладочное меню игры (нужны файлы шрифтов)", false},
    };
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const Short* name = nullptr;
        for (const Short& s : shorts) {
            if (std::strcmp(s.key, effect.key) == 0) name = &s;
        }
        if ((name && !name->graphics) || pages.back().rows.size() == RowsPerPage) {
            continue; // the overlay menu and the launcher have it
        }
        // A new effect without a short name: its full label.
        const std::string help_en = std::string(name ? name->help_en : effect.label) + " (after a restart)";
        const std::string help_ru =
            std::string(name ? name->help_ru : effect.label_ru) + " (после перезапуска)";
        row(Field(FirstEffect + e), name ? name->en : effect.label, name ? name->ru : effect.label_ru,
            help_en.c_str(), help_ru.c_str(), toggle);
    }
}

// ---- The game's structures, as its own code builds them. ----
constexpr std::size_t TextSize = 0x40;     // text entry (MakeText): text pointer, string, flag
constexpr std::size_t ValueSize = 0x48;    // {u32 value; text}
constexpr std::size_t ListSize = 0x920;    // 32 values + count at +0x908 (+ alignment slack)
constexpr std::size_t PairListSize = 0xb0; // 2 values + count at +0x98 (+ alignment slack)
constexpr std::size_t ListCountOffset = 0x908, PairCountOffset = 0x98;
constexpr std::size_t FunctionSize = 0x28; // std::function: 32-byte buffer, __f_ at +0x20

/// Destroys a std::function the way the game's code does (its __f_ at +0x20).
void DestroyFunction(u8* f) {
    void* impl = *reinterpret_cast<void**>(f + 0x20);
    if (!impl) {
        return;
    }
    using Destroy = void (*)(void*, int);
    Destroy destroy = (*reinterpret_cast<Destroy**>(impl))[4];
    destroy(impl, impl != f);
    *reinterpret_cast<void**>(f + 0x20) = nullptr;
}

/// Frees a text entry's string the way the game's code does (heap buffer once capacity >= 8).
void DestroyText(u8* text) {
    if (*reinterpret_cast<u64*>(text + 0x28) >= 8) {
        void* allocator = *reinterpret_cast<void**>(text + 0x30);
        using Free = void (*)(void*, void*);
        (*reinterpret_cast<Free**>(allocator))[0x70 / 8](allocator, *reinterpret_cast<void**>(text + 0x10));
    }
    *reinterpret_cast<u64*>(text + 0x28) = 7;
    *reinterpret_cast<u64*>(text + 0x20) = 0;
    *reinterpret_cast<char16_t*>(text + 0x10) = 0;
}
void DestroyTexts(u8* pair) {
    DestroyText(pair);
    DestroyText(pair + TextSize);
}
/// A value list's entries (count at `count_offset`), after the row has copied them.
void DestroyValues(u8* list, std::size_t count_offset) {
    const u64 count = *reinterpret_cast<u64*>(list + count_offset);
    for (u64 i = 0; i < count; ++i) {
        DestroyText(list + i * ValueSize + 8);
    }
}

void MakeTexts(u8* pair, u32 name, u32 help) {
    using Make = void* (*)(void*, u32, u32);
    Game<Make>(MakeText)(pair, TextName, name);
    Game<Make>(MakeText)(pair + TextSize, TextHelp, help);
}

/// Upscalers this GPU and build can run (known once the device exists, before any menu).
bool UpscalerAvailable(int i) {
    const auto& s = BbSettings::Get();
    return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
           : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
           : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                             : true;
}

void AddRows(void* page, const std::vector<Row>& rows) {
    using Push = void (*)(void*, void*);
    using AddRow = void (*)(void*, void*, int*, void*, const int*);
    using Make = void* (*)(void*, u32, u32);
    for (const Row& row : rows) {
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, row.name, row.help);
        if (row.kind == Kind::Slider || row.kind == Kind::Steps) {
            using Slider = void (*)(void*, void*, u8*, const u8*);
            Game<Slider>(SliderRow)(page, pair, &bytes[row.field], &default_bytes[row.field]);
            DestroyTexts(pair);
            continue;
        }
        if (row.kind == Kind::Toggle) {
            using PairRowFn = void (*)(void*, void*, u8*, void*, const u8*);
            u8* two = static_cast<u8*>(std::aligned_alloc(16, PairListSize));
            std::memset(two, 0, PairListSize);
            for (std::size_t i = 0; i < 2; ++i) {
                alignas(16) u8 value[ValueSize] = {};
                value[0] = u8(i);
                Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
                Game<Push>(PairPush)(two, value);
                DestroyText(value + 8);
            }
            Game<PairRowFn>(PairRow)(page, pair, &bytes[row.field], two, &default_bytes[row.field]);
            DestroyValues(two, PairCountOffset);
            std::free(two);
            DestroyTexts(pair);
            continue;
        }
        u8* list = static_cast<u8*>(std::aligned_alloc(16, ListSize));
        std::memset(list, 0, ListSize);
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            if (row.field == Upscaler && !UpscalerAvailable(int(i))) {
                continue; // the list's values are the upscaler numbers, not positions
            }
            alignas(16) u8 value[ValueSize] = {};
            *reinterpret_cast<u32*>(value) = u32(i);
            Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
            Game<Push>(ListPush)(list, value);
            DestroyText(value + 8);
        }
        Game<AddRow>(ListRow)(page, pair, &values[row.field], list, &default_values[row.field]);
        DestroyValues(list, ListCountOffset);
        std::free(list);
        DestroyTexts(pair);
    }
}

std::atomic<const char16_t*> title_override{nullptr}; // while the game builds one of our pages (its menu thread)
// Called by the game's menu code (guest threads, System V ABI like the game), one set per page.
template <int P>
void Rows(void* page, void*) {
    LoadValues();
    AddRows(page, pages[P].rows);
}
template <int P>
void* Content(void* a, void* b) {
    using Open = void* (*)(void*, void*, const char*, void*, u64, u64);
    title_override = nullptr; // the title was looked up just before
    return Game<Open>(OpenPage)(a, b, Game<const char*>(ControlsLayout),
                                reinterpret_cast<void*>(&Rows<P>), 0, 0);
}
void* Factory(void* out, void* arg, void* content, const char16_t* title) {
    using Create = void* (*)(void*, void*, void*);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = image_base + ContentVtable;
    *reinterpret_cast<void**>(function + 8) = content;
    *reinterpret_cast<void**>(function + 0x20) = function;
    // The page is built later (the game's menu task): its layout title is looked up right before
    // the content function runs, which takes the override back.
    title_override = title;
    Game<Create>(CreatePage)(out, arg, function);
    DestroyFunction(function);
    return out;
}
template <int P>
void* PageFactory(void* out, void* arg) {
    return Factory(out, arg, reinterpret_cast<void*>(&Content<P>), TextOf(pages[P].name));
}
constexpr std::size_t MaxPages = 6;
void* const factories[MaxPages] = {
    reinterpret_cast<void*>(&PageFactory<0>), reinterpret_cast<void*>(&PageFactory<1>),
    reinterpret_cast<void*>(&PageFactory<2>), reinterpret_cast<void*>(&PageFactory<3>),
    reinterpret_cast<void*>(&PageFactory<4>), reinterpret_cast<void*>(&PageFactory<5>)};

// ---- Hooks ----
using LookupFn = const char16_t* (*)(void*, u32, u32, u32);
using AddFn = void* (*)(void*, void*, void*, void*);
LookupFn lookup_original = nullptr;
AddFn add_original = nullptr;
std::atomic<const void*> screen_sound_text{nullptr};

const char16_t* LookupHook(void* repository, u32 table, u32 category, u32 id) {
    if (id >= IdBase && id < IdBase + texts.size()) {
        // BB_GAME_MENU_TRACE=1: how often the game asks for our texts (per 2 s).
        static const bool count = [] {
            const char* env = std::getenv("BB_GAME_MENU_TRACE");
            return env && env[0] == '1';
        }();
        if (count) {
            static std::atomic<u64> lookups{0};
            static std::atomic<long long> printed{0};
            ++lookups;
            const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (now - printed.load() >= 2) {
                printed = now;
                std::printf("Game menu: %llu lookups of the port's texts in 2 s (last id %u)\n",
                            (unsigned long long)lookups.exchange(0), id - IdBase);
            }
        }
        return TextOf(id);
    }
    if (title_override && category == TextName && id == ControlsTitle) {
        return title_override;
    }
    const char16_t* text = lookup_original(repository, table, category, id);
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    if (trace && id >= 110000 && id < 120000) {
        std::printf("Game menu: text %#x/%u%s from %#llx\n", category, id, title_override ? " (our page)" : "",
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (category == TextName && id == ScreenSoundItem && text) {
        screen_sound_text = text;
        bool cyrillic = false;
        for (const char16_t* c = text; *c; ++c) {
            cyrillic = cyrillic || (*c >= 0x400 && *c < 0x500);
        }
        game_language = cyrillic ? GameLanguage::Russian : GameLanguage::English;
    }
    return text;
}

void AddItem(void* menu, u32 name, u32 help, void* factory) {
    using ToFunction = void* (*)(void*, void*);
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 source[FunctionSize] = {};
    *reinterpret_cast<u64*>(source) = image_base + FactoryVtable;
    *reinterpret_cast<void**>(source + 8) = factory;
    *reinterpret_cast<void**>(source + 0x20) = source;
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<void**>(function + 0x20) = Game<ToFunction>(FactoryToFunction)(source, function);
    alignas(16) u8 out[16] = {};
    add_original(menu, pair, function, out);
    DestroyFunction(function);
    DestroyTexts(pair);
}

void* AddHook(void* menu, void* pair, void* function, void* out) {
    void* result = add_original(menu, pair, function, out);
    const void* screen_sound = screen_sound_text.load();
    if (screen_sound && *reinterpret_cast<const void* const*>(pair) == screen_sound) {
        for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
            AddItem(menu, pages[i].name, pages[i].help, factories[i]);
        }
    }
    return result;
}

/// `jmp hook` at the function (via a near thunk); its moved prologue + `jmp` back is the original.
bool Detour(unsigned char* image, u64 va, const u8* prologue, std::size_t length, void* hook,
            void** original, u8*& cursor) {
    u8* site = image + va;
    if (std::memcmp(site, prologue, length) != 0) {
        std::printf("Game menu: unexpected code at %#llx, not hooked\n", (unsigned long long)va);
        return false;
    }
    u8* trampoline = cursor;
    std::memcpy(trampoline, prologue, length);
    trampoline[length] = 0xe9;
    const std::int64_t back = std::int64_t(site + length) - std::int64_t(trampoline + length + 5);
    const std::int32_t back32 = std::int32_t(back);
    std::memcpy(trampoline + length + 1, &back32, 4);
    u8* thunk = trampoline + 32;
    thunk[0] = 0x48;
    thunk[1] = 0xb8; // movabs rax, hook
    std::memcpy(thunk + 2, &hook, 8);
    thunk[10] = 0xff;
    thunk[11] = 0xe0; // jmp rax
    const std::int64_t to = std::int64_t(thunk) - std::int64_t(site + 5);
    if (back != back32 || to != std::int32_t(to)) {
        std::printf("Game menu: stubs out of jump range, not hooked\n");
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    site[0] = 0xe9;
    std::memcpy(site + 1, &to32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        site[i] = 0xcc;
    }
    *original = trampoline;
    cursor += 64;
    return true;
}

/// A mapping within +-2 GiB of the image, for rel32 jumps both ways.
u8* MapNear(unsigned char* image, u64 image_size, std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + image_size + k * (24ull << 20)}) {
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
        }
    }
    return nullptr;
}

} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* env = std::getenv("BB_GAME_MENU"); env && env[0] == '0') {
        return;
    }
#if !defined(__x86_64__)
    // bbport: the pages hook the game's code with jumps to x86 trampolines next to the image that
    // call this library: the game's code must run on an x86-64 CPU. Under FEX (AArch64) it cannot
    // (an unknown mapping, then host code): the Android app shows these settings in its own menu.
    (void)image;
    (void)size;
    std::printf("Game menu: the port's pages need an x86-64 CPU; settings are in the app's menu\n");
    return;
#endif
    image_base = reinterpret_cast<u64>(image);
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    u8* stubs = MapNear(image, size, page);
    if (!stubs) {
        std::printf("Game menu: no memory near the image, the port's pages are off\n");
        return;
    }
    DefineTexts();
    for (const Page& p : pages) {
        for (const Row& r : p.rows) {
            byte_field[r.field] = r.kind != Kind::List;
            steps_count[r.field] = r.kind == Kind::Steps ? int(r.choices.size()) : 0;
        }
    }
    static const BbSettings::Values defaults;
    Fill(defaults, default_values);
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        default_values[FirstEffect + e] = BbSettings::Effects[e].default_on;
    }
    for (int f = 0; f < FieldCount; ++f) {
        default_bytes[f] =
            steps_count[f] ? ChoicePosition(default_values[f], steps_count[f]) : u8(default_values[f]);
    }
    u8* cursor = stubs;
    // The lookup first: the menu hook only acts once it has seen the Screen/Sound text.
    const bool ok =
        Detour(image, MsgLookup, MsgLookupPrologue, sizeof(MsgLookupPrologue),
               reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&lookup_original), cursor) &&
        Detour(image, AddPageItem, AddPageItemPrologue, sizeof(AddPageItemPrologue),
               reinterpret_cast<void*>(&AddHook), reinterpret_cast<void**>(&add_original), cursor);
    mprotect(stubs, page, PROT_READ | PROT_EXEC);
    std::printf("Game menu: %s (%zu pages in System)\n", ok ? "the port's pages are in" : "hooks failed, off",
                pages.size());
}


void Poll() {
    if (!values_valid) {
        return;
    }
    for (int f = 0; f < FieldCount; ++f) {
        if (byte_field[f]) {
            // A slider with choices: its position, not the choice's number.
            values[f] = steps_count[f] ? ChoiceFromPosition(bytes[f], steps_count[f]) : bytes[f];
        }
    }
    // The output resolution and the preset re-create the scene targets and the upscaler (a few
    // grey frames): a slider walking through its steps did that at each one. Applied once they
    // rest for 0.4 s; the other rows at once.
    using Clock = std::chrono::steady_clock;
    static int resting[2] = {-1, -1};
    static Clock::time_point rested_since{};
    const auto now = Clock::now();
    if (resting[0] != values[OutputRes] || resting[1] != values[Preset]) {
        resting[0] = values[OutputRes];
        resting[1] = values[Preset];
        rested_since = now;
    }
    if (now - rested_since < std::chrono::milliseconds(400)) {
        values[OutputRes] = applied[OutputRes];
        values[Preset] = applied[Preset];
    }
    if (std::memcmp(values, applied, sizeof(values)) == 0) {
        return;
    }
    auto& s = BbSettings::Get();
    s.output_res = std::clamp(values[OutputRes], 0, BbSettings::OutputCount - 1);
    const int upscaler = std::clamp(values[Upscaler], 0, BbSettings::UpscalerCount - 1);
    s.upscaler = UpscalerAvailable(upscaler) ? upscaler : int(BbSettings::UpscalerFsr3);
    s.preset = std::clamp(values[Preset], 0, BbSettings::PresetCount - 1);
    s.sharpen = values[Sharpness] != 0;
    if (values[Sharpness] != 0) {
        s.sharpness = std::clamp(values[Sharpness], 0, SharpnessSteps - 1) * SharpnessStep;
    }
    s.show_fps = values[ShowFps] != 0;
    s.model_lod = LodValues[std::clamp(values[ModelLod], 0, 3)];
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        s.effects[e] = values[FirstEffect + e] != 0;
    }
    std::memcpy(applied, values, sizeof(values));
    BbSettings::Save();
}

} // namespace BbGameMenu
