// SPDX-License-Identifier: GPL-3.0-or-later
// Progress reporting of a compile for the GUI and for `--machine` runs. compile.cpp calls these; player.cpp installs the hooks (the other
// tools leave them empty).
#pragma once
#include <cstdint>

struct ProgressHooks {
  void (*stage)(const char* id, const char* text) = nullptr;  // a new stage begins
  void (*progress)(uint64_t done, uint64_t total) = nullptr;  // inside the current stage
};
extern ProgressHooks g_progress;  // bundle.cpp
inline void bbrStage(const char* id, const char* text) { if (g_progress.stage) g_progress.stage(id, text); }
inline void bbrProgress(uint64_t done, uint64_t total) { if (g_progress.progress) g_progress.progress(done, total); }
