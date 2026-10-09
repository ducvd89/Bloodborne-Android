// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Optional host window size for small displays, independent of guest/UI resolution.
    if (const char* size = std::getenv("BB_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 &&
            w >= 320 && h >= 180 && w <= 16384 && h <= 16384) {
            width_ = w;
            height_ = h;
        }
    }
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    if (std::getenv("BB_WINDOW_SIZE")) {
        // Xvfb has no window manager to focus a newly mapped Android game window.
        SDL_RaiseWindow(window);
    }
    if (const char* config = std::getenv("BB_CONFIG")) {
        std::ifstream file(config);
        std::string line;
        while (std::getline(file, line)) {
            if (line.starts_with("mouse_look=")) mouse_look = std::atoi(line.c_str() + 11) != 0;
        }
    }
    mouse_input.Reset(SDL_GetTicks(), false);

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    UpdatePointer();
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            last_mouse_motion_ms = SDL_GetTicks();
            if (mouse_relative) {
                std::scoped_lock lock{mouse_mutex};
                mouse_input.Add(event.motion.xrel, event.motion.yrel);
            }
        }
        if (!text_active && !BbOverlay::CapturesInput() &&
            event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F10) {
            mouse_wanted = !mouse_wanted;
            UpdatePointer();
            continue;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    UpdatePointer();
    UpdateCursor();
    return is_open;
}

BbMouseState WindowSDL::GetMouseState() {
    std::scoped_lock lock{mouse_mutex};
    return mouse_input.State();
}

void WindowSDL::UpdatePointer() {
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool relative = mouse_look && mouse_wanted && focused && !text_active &&
                          !BbOverlay::CapturesInput();
    if (relative != mouse_relative) {
        mouse_relative = relative && SDL_SetWindowRelativeMouseMode(window, true);
        if (!relative) SDL_SetWindowRelativeMouseMode(window, false);
        if (relative && !mouse_relative) {
            LOG_ERROR(Frontend, "Mouse capture failed: {}", SDL_GetError());
            mouse_wanted = false;
        }
        std::scoped_lock lock{mouse_mutex};
        mouse_input.Reset(SDL_GetTicks(), focused);
    }
    std::scoped_lock lock{mouse_mutex};
    if (mouse_relative) mouse_input.Update(SDL_GetTicks(), SDL_GetMouseState(nullptr, nullptr));
    else mouse_input.Reset(SDL_GetTicks(), focused);
}

// Issue #3: the OS cursor over the game. Hidden in fullscreen, and in a window after 3 s without
// moving the mouse; always shown while the settings menu is open.
void WindowSDL::UpdateCursor() {
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool hide = !text_active && !BbOverlay::CapturesInput() &&
                      (mouse_look ? mouse_relative :
                       (fullscreen || SDL_GetTicks() - last_mouse_motion_ms > 3000));
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend
