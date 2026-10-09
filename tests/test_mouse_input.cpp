#include <cassert>
#include <cmath>
#include "../gpu/shim/mouse_input.h"

int main() {
    Frontend::MouseInput mouse;
    mouse.Reset(100, true);
    mouse.Add(4, -2);
    mouse.Add(4, -2);
    mouse.Update(108, 1);
    assert(mouse.State().x == 0 && mouse.State().buttons == 1);
    mouse.Update(116, 1);
    assert(std::abs(mouse.State().x - 500) < 0.01f && std::abs(mouse.State().y + 250) < 0.01f);
    // Multiple guest reads see the same motion, then stop without new input.
    const auto first = mouse.State();
    const auto second = mouse.State();
    assert(first.x == second.x && first.y == second.y && second.buttons == 1);
    mouse.Update(132, 0);
    assert(mouse.State().x == 0 && mouse.State().y == 0 && mouse.State().buttons == 0);
    // Pending motion is discarded on focus loss, overlay opening or mouse release.
    mouse.Add(999, 999);
    mouse.Reset(140, false);
    mouse.Update(156, 0);
    assert(!mouse.State().focused && mouse.State().x == 0);
    mouse.Reset(160, true);
    mouse.Update(176, 0);
    assert(mouse.State().focused && mouse.State().x == 0);
}
