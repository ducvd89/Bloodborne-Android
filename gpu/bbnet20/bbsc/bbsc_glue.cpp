// SPDX-License-Identifier: GPL-2.0-or-later
// bbport online module: what BBSC (shadps4/core/bloodborne_re.cpp, GPL-2.0+)
// needs from shadPS4's UI. Its loading screen is a line in the port's overlay; its animated title
// card (off by default in BBSC too) is not ported.
#include <cstdio>
#include <string_view>

#include "bbgpu.h"

#include "imgui/bbsc_intro_layer.h"
#include "imgui/bbsc_loading_layer.h"

namespace Core::Bbsc {

void ShowLoadingScreen(LoadingScreenKind kind) {
    // The port's overlay line (BBSC draws its own loading screen in shadPS4's ImGui layer).
    bbgpu_overlay_banner(kind == LoadingScreenKind::Summon ? "Joining the host's world..."
                                                           : "Following the host...");
}

void HideLoadingScreen() {
    bbgpu_overlay_banner(nullptr);
}

void StartIntro(std::string_view) {}

} // namespace Core::Bbsc
