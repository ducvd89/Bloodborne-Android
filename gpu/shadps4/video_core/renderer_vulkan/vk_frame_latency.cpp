// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see vk_frame_latency.h.
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_frame_latency.h"

namespace Vulkan::FrameLatency {

namespace {

using Clock = std::chrono::steady_clock;
constexpr u32 StageCount = static_cast<u32>(Stage::Count);
constexpr const char* StageNames[StageCount] = {
    "game submit", "flip", "present start", "GPU done", "copy start", "copy done",
    "framegen done", "gen handed", "gen ready", "real handed", "real ready",
};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Watch {
    u64 id;
    Stage stage;
    vk::Device device;
    vk::Semaphore semaphore; ///< null: `fd`
    u64 value;
    int fd;
};

struct Record {
    std::array<s64, StageCount> t{}; ///< 0: not stamped
    u32 watches = 0;                 ///< unresolved watches
    bool finished = false;
    u32 lsfg = 0;
};

struct Window {
    std::array<std::vector<double>, StageCount> ms; ///< per stage, ms after the flip
    u32 frames = 0;
};

struct State {
    std::mutex mutex;
    std::map<u64, Record> records;
    std::vector<Watch> watches;
    u64 next_id = 1;
    std::map<u32, Window> windows; ///< by multiplier (0: off)
    Clock::time_point window_start = Clock::now();
    u64 depth_sum = 0;
    u32 depth_samples = 0;
    u32 depth_max = 0;
    std::thread poller;
};

// Game submissions not yet matched with a flip (oldest first; sceGnmSubmitAndFlip comes before its
// flip reaches the driver).
std::mutex submit_mutex;
std::deque<s64> game_submits;

State& Get() {
    static State* state = [] {
        auto* s = new State; // never destroyed: the poller runs for the process
        s->poller = std::thread([s] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                std::scoped_lock lock{s->mutex};
                const s64 now = NowNs();
                for (auto it = s->watches.begin(); it != s->watches.end();) {
                    bool signaled = false;
                    if (it->semaphore) {
                        const auto [result, value] = it->device.getSemaphoreCounterValue(it->semaphore);
                        signaled = result != vk::Result::eSuccess || value >= it->value;
                    } else {
                        pollfd pfd{it->fd, POLLIN, 0};
                        signaled = poll(&pfd, 1, 0) != 0;
                    }
                    if (!signaled) {
                        ++it;
                        continue;
                    }
                    if (!it->semaphore) {
                        close(it->fd);
                    }
                    if (auto rec = s->records.find(it->id); rec != s->records.end()) {
                        rec->second.t[static_cast<u32>(it->stage)] = now;
                        --rec->second.watches;
                    }
                    it = s->watches.erase(it);
                }
                // Finished records with every watch resolved go into the window.
                for (auto it = s->records.begin(); it != s->records.end();) {
                    Record& r = it->second;
                    if (!r.finished || r.watches != 0) {
                        // A record that never finishes (a dropped frame) must not pile up.
                        if (it->first + 512 < s->next_id) {
                            it = s->records.erase(it);
                            continue;
                        }
                        ++it;
                        continue;
                    }
                    const s64 flip = r.t[static_cast<u32>(Stage::Flip)];
                    Window& w = s->windows[r.lsfg];
                    ++w.frames;
                    for (u32 i = 0; i < StageCount; ++i) {
                        if (r.t[i] != 0 && flip != 0) {
                            w.ms[i].push_back((r.t[i] - flip) / 1e6);
                        }
                    }
                    it = s->records.erase(it);
                }
                const double elapsed = std::chrono::duration<double>(Clock::now() - s->window_start).count();
                if (elapsed < 5.0) {
                    continue;
                }
                for (auto& [lsfg, w] : s->windows) {
                    if (w.frames == 0) {
                        continue;
                    }
                    char line[1536];
                    int n = std::snprintf(line, sizeof line,
                                          "Latency (%s, %u frames, %.1f/s; ms after the flip, mean/p95):",
                                          lsfg ? (lsfg == 2 ? "LSFG x2" : lsfg == 3 ? "LSFG x3" : "LSFG x4")
                                               : "LSFG off",
                                          w.frames, w.frames / elapsed);
                    for (u32 i = 0; i < StageCount && n < static_cast<int>(sizeof line) - 64; ++i) {
                        auto& v = w.ms[i];
                        if (v.empty() || i == static_cast<u32>(Stage::Flip)) {
                            continue;
                        }
                        double sum = 0;
                        for (const double x : v) {
                            sum += x;
                        }
                        std::sort(v.begin(), v.end());
                        const double p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
                        n += std::snprintf(line + n, sizeof line - n, " %s %.1f/%.1f%s", StageNames[i],
                                           sum / v.size(), p95, i + 1 < StageCount ? ";" : "");
                    }
                    std::printf("%s\n", line);
                    w = Window{};
                }
                if (s->depth_samples) {
                    std::printf("Latency: present thread queue %.2f frames on average, %u at most\n",
                                double(s->depth_sum) / s->depth_samples, s->depth_max);
                }
                std::fflush(stdout);
                s->depth_sum = 0;
                s->depth_samples = 0;
                s->depth_max = 0;
                s->window_start = Clock::now();
            }
        });
        s->poller.detach();
        return s;
    }();
    return *state;
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LSFG_LATENCY");
        return env && env[0] == '1';
    }();
    return on;
}

void NoteGameSubmit() {
    if (Enabled()) {
        std::scoped_lock lock{submit_mutex};
        game_submits.push_back(NowNs());
        if (game_submits.size() > 8) {
            game_submits.pop_front();
        }
    }
}

u64 Begin() {
    if (!Enabled()) {
        return 0;
    }
    s64 submitted = 0;
    {
        std::scoped_lock lock{submit_mutex};
        if (!game_submits.empty()) {
            submitted = game_submits.front();
            game_submits.pop_front();
        }
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    const u64 id = s.next_id++;
    Record& r = s.records[id];
    r.t[static_cast<u32>(Stage::Flip)] = NowNs();
    r.t[static_cast<u32>(Stage::GameSubmit)] = submitted;
    return id;
}

void Mark(u64 id, Stage stage) {
    if (id == 0) {
        return;
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (auto it = s.records.find(id); it != s.records.end()) {
        it->second.t[static_cast<u32>(stage)] = NowNs();
    }
}

void WatchTimeline(u64 id, Stage stage, vk::Device device, vk::Semaphore semaphore, u64 value) {
    if (id == 0 || !semaphore) {
        return;
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (auto it = s.records.find(id); it != s.records.end()) {
        ++it->second.watches;
        s.watches.push_back({id, stage, device, semaphore, value, -1});
    }
}

void WatchFd(u64 id, Stage stage, int fd) {
    if (fd < 0) {
        return;
    }
    if (id == 0) {
        close(fd);
        return;
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (auto it = s.records.find(id); it != s.records.end()) {
        ++it->second.watches;
        s.watches.push_back({id, stage, {}, {}, 0, fd});
    } else {
        close(fd);
    }
}

void Finish(u64 id, u32 lsfg) {
    if (id == 0) {
        return;
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (auto it = s.records.find(id); it != s.records.end()) {
        it->second.finished = true;
        it->second.lsfg = lsfg;
    }
}

void NoteQueueDepth(u32 depth) {
    if (!Enabled()) {
        return;
    }
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.depth_sum += depth;
    ++s.depth_samples;
    s.depth_max = std::max(s.depth_max, depth);
}

} // namespace Vulkan::FrameLatency
