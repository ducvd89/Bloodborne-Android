// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: what the online module (gpu/bbnet20) asks of the window layer, which Android does not
// have: a line of text over the game (bbgpu_overlay_banner) and a look at the window's events
// (bbgpu_add_event_filter, used by BBSC's cinematic camera).
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

#include "../bbgpu.h"

extern "C" void bbgpu_add_event_filter(BbGpuEventFilter) {
    // No SDL events here: the app's input arrives through the runtime's pad file, not a window.
}

// The banner goes to BB_NET_BANNER_FILE (written whole under another name and renamed, so a reader never
// sees half a line; removed when the banner ends), else to the log.
extern "C" void bbgpu_overlay_banner(const char* utf8) {
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    const char* path = std::getenv("BB_NET_BANNER_FILE");
    if (!path || !*path) {
        if (utf8 && *utf8) {
            std::printf("Banner: %s\n", utf8);
        }
        return;
    }
    if (!utf8 || !*utf8) {
        unlink(path);
        return;
    }
    const std::string temporary = std::string(path) + ".tmp";
    if (FILE* file = std::fopen(temporary.c_str(), "w")) {
        std::fprintf(file, "%s\n", utf8);
        if (std::fclose(file) != 0 || std::rename(temporary.c_str(), path) != 0) {
            unlink(temporary.c_str());
        }
    }
}
