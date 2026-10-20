// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core::Bbsc {

// BBSC's animated title card, drawn by the ImGui overlay over the game's own boot logos (so it
// adds no loading time). Bloodborne (CUSA03173) only; switch SHADPS4_BBSC_INTRO, default on.
void StartIntro(std::string_view serial);

} // namespace Core::Bbsc
