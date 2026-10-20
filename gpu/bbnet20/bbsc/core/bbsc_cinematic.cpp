// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// Input side of the BBSC cinematic camera. While it is on, the keyboard and mouse fly the camera
// and never reach the game (shadPS4 would otherwise map them to the pad and move the hunter too);
// the controller is untouched. Keys:
//   F1 on/off | hold right mouse: look | WASD move | E / Space up, Q / C down
//   Shift fast, Alt slow | wheel: speed | Z / X zoom, V reset zoom | , . roll, / reset roll
//   arrow keys: look | [ ] smoothing (glide) less / more

#include <mutex>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>

#include "common/logging/log.h"
#include "core/bbsc_cinematic.h"
#include "core/bbsc_persistence.h"

namespace Core::Bbsc {
namespace {

std::mutex cine_mutex;
CinematicInput cine_state;
bool looking = false;

bool CinematicEnabledBySwitch() {
    static const bool enabled = SwitchEnabled("SHADPS4_BBSC_CINEMATIC");
    return enabled;
}

void SetKey(SDL_Keycode key, bool down) {
    switch (key) {
    case SDLK_W: cine_state.forward = down; break;
    case SDLK_S: cine_state.back = down; break;
    case SDLK_A: cine_state.left = down; break;
    case SDLK_D: cine_state.right = down; break;
    case SDLK_E:
    case SDLK_SPACE: cine_state.up = down; break;
    case SDLK_Q:
    case SDLK_C: cine_state.down = down; break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: cine_state.fast = down; break;
    case SDLK_LALT:
    case SDLK_RALT: cine_state.slow = down; break;
    case SDLK_Z: cine_state.zoom_in = down; break;
    case SDLK_X: cine_state.zoom_out = down; break;
    case SDLK_V: cine_state.zoom_reset = cine_state.zoom_reset || down; break;
    case SDLK_COMMA: cine_state.roll_left = down; break;
    case SDLK_PERIOD: cine_state.roll_right = down; break;
    case SDLK_SLASH: cine_state.roll_reset = cine_state.roll_reset || down; break;
    case SDLK_UP: cine_state.look_up = down; break;
    case SDLK_DOWN: cine_state.look_down = down; break;
    case SDLK_LEFT: cine_state.look_left = down; break;
    case SDLK_RIGHT: cine_state.look_right = down; break;
    case SDLK_LEFTBRACKET:
        if (down) --cine_state.smooth_steps;
        break;
    case SDLK_RIGHTBRACKET:
        if (down) ++cine_state.smooth_steps;
        break;
    default: break;
    }
}

void ReleaseAll(SDL_Window* window) {
    const bool enabled = cine_state.enabled;
    cine_state = CinematicInput{};
    cine_state.enabled = enabled;
#ifndef __ANDROID__ // bbport-android: no SDL window (the event filter is never called: bbgpu_add_event_filter)
    if (looking && window != nullptr) {
        SDL_SetWindowRelativeMouseMode(window, false);
    }
#endif
    looking = false;
}

} // namespace

bool CinematicHandleEvent(const SDL_Event& event, SDL_Window* window) {
    if (!CinematicEnabledBySwitch()) {
        return false;
    }
    std::scoped_lock lock{cine_mutex};
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1) {
        if (!event.key.repeat) {
            cine_state.enabled = !cine_state.enabled;
            ReleaseAll(window);
            LOG_INFO(Debug, "BBSC cinematic camera: {} (F1)", cine_state.enabled ? "on" : "off");
        }
        return true;
    }
    if (event.type == SDL_EVENT_KEY_UP && event.key.key == SDLK_F1) {
        return true;
    }
    if (!cine_state.enabled) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const SDL_Keycode key = event.key.key;
        if (key >= SDLK_F2 && key <= SDLK_F12) {
            return false; // the emulator's own hotkeys (fullscreen, fps, pause...) keep working
        }
        SetKey(key, event.type == SDL_EVENT_KEY_DOWN);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT) {
            looking = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
#ifndef __ANDROID__ // bbport-android: see ReleaseAll
            if (window != nullptr) {
                SDL_SetWindowRelativeMouseMode(window, looking);
            }
#endif
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (looking) {
            cine_state.mouse_dx += event.motion.xrel;
            cine_state.mouse_dy += event.motion.yrel;
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        cine_state.wheel += event.wheel.y > 0.0f ? 1 : (event.wheel.y < 0.0f ? -1 : 0);
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ReleaseAll(window);
        return false;
    default:
        return false;
    }
}

CinematicInput TakeCinematicInput() {
    std::scoped_lock lock{cine_mutex};
    CinematicInput out = cine_state;
    cine_state.mouse_dx = 0.0f;
    cine_state.mouse_dy = 0.0f;
    cine_state.wheel = 0;
    cine_state.smooth_steps = 0;
    cine_state.zoom_reset = false;
    cine_state.roll_reset = false;
    return out;
}

void CinematicForceOff() {
    std::scoped_lock lock{cine_mutex};
    cine_state = CinematicInput{};
}

void CinematicForceOn() {
    std::scoped_lock lock{cine_mutex};
    if (!cine_state.enabled) {
        cine_state = CinematicInput{};
        cine_state.enabled = true;
    }
}

} // namespace Core::Bbsc
