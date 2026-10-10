// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Game {

// One per game (games/<game>.cpp).
extern const Profile BloodborneProfile;

namespace {
constexpr std::array<const Profile*, 1> Profiles = {&BloodborneProfile};
std::atomic<const Profile*> active{nullptr};
} // namespace

void Select(const char* serial) {
    const char* env = std::getenv("BB_GAME_PROFILE");
    if (env && std::strcmp(env, "none") == 0) {
        std::printf("Game profile: none (BB_GAME_PROFILE=none): the game runs as an unknown one\n");
        return;
    }
    for (const Profile* profile : Profiles) {
        const bool match = serial && std::ranges::any_of(profile->serials, [&](const char* s) {
                               return std::strcmp(s, serial) == 0;
                           });
        if (match) {
            active.store(profile, std::memory_order_release);
            std::printf("Game profile: %s (%s)\n", profile->name, serial);
            return;
        }
    }
    std::printf("Game profile: none for %s: the generic paths only\n", serial ? serial : "?");
}

const Profile* Active() {
    return active.load(std::memory_order_acquire);
}

bool IsDynamicWriter(std::uint64_t guest_rip) {
    const Profile* profile = Active();
    return profile && std::ranges::any_of(profile->dynamic_writers, [&](const CodeRange& r) {
               return guest_rip >= r.begin && guest_rip < r.end;
           });
}

} // namespace Game
