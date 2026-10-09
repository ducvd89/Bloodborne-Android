// Mouse velocity is published at a fixed cadence, independent of guest pad read rate.
#pragma once
#include <cstdint>
#include "../bbgpu.h"

namespace Frontend {
class MouseInput {
public:
    void Reset(uint64_t now, bool focused) {
        state = {0, 0, 0, focused};
        dx = dy = 0;
        since = now;
    }
    void Add(float x, float y) { dx += x; dy += y; }
    void Update(uint64_t now, uint32_t buttons) {
        state.buttons = buttons;
        if (now - since < 16) return;
        const float seconds = (now - since) / 1000.0f;
        state.x = dx / seconds;
        state.y = dy / seconds;
        dx = dy = 0;
        since = now;
    }
    BbMouseState State() const { return state; }
private:
    BbMouseState state{};
    float dx{}, dy{};
    uint64_t since{};
};
}
