// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// BBSC cinematic camera (2026-09-28): a free camera for filming, flown with keyboard and mouse while
// the controller keeps playing the hunter. The window side (sdl_window.cpp) feeds it input; the
// game side (bloodborne_re.cpp, BbscCinematicCameraHook) turns the game's own debug free camera on
// and moves it. Switch SHADPS4_BBSC_CINEMATIC (default on). F1 toggles it.

union SDL_Event;
struct SDL_Window;

namespace Core::Bbsc {

// Window thread. True when the event belongs to the cinematic camera and must not reach the game.
bool CinematicHandleEvent(const SDL_Event& event, SDL_Window* window);

struct CinematicInput {
    bool enabled = false;
    bool forward = false, back = false, left = false, right = false, up = false, down = false;
    bool fast = false, slow = false;
    bool zoom_in = false, zoom_out = false, zoom_reset = false;
    bool roll_left = false, roll_right = false, roll_reset = false;
    bool look_up = false, look_down = false, look_left = false, look_right = false;
    float mouse_dx = 0.0f; // accumulated since the last TakeCinematicInput
    float mouse_dy = 0.0f;
    int wheel = 0;         // speed steps, accumulated
    int smooth_steps = 0;  // smoothing steps, accumulated
    // Controller (mod only, -1..1, added to the keys): left stick move, right stick look, L1/R1
    // down/up, L2/R2 zoom out/in.
    float pad_move_x = 0.0f, pad_move_y = 0.0f, pad_rise = 0.0f;
    float pad_look_x = 0.0f, pad_look_y = 0.0f, pad_zoom = 0.0f;
};

// Game thread: the current keys plus everything accumulated since the last call.
CinematicInput TakeCinematicInput();

// Game thread: the camera could not be switched on (logged); the toggle goes back to off.
void CinematicForceOff();

// Game thread: switches the camera on (you died in co-op); nothing when it is already on.
void CinematicForceOn();

} // namespace Core::Bbsc
