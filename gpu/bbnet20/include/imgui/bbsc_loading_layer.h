// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Bbsc {

// What the guest is loading into, for the loading screen's line.
enum class LoadingScreenKind { Summon, Follow };

// BBSC's own loading screen, drawn by the ImGui overlay over the black load cover while a guest's
// summon / cross-map / follow load runs (the game shows nothing there). Any thread.
void ShowLoadingScreen(LoadingScreenKind kind);
void HideLoadingScreen();

} // namespace Core::Bbsc
