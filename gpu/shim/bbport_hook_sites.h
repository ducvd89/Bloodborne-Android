// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's own code patchers (frame rate, display, guest guards, scene size) as native hook slots when the game's code runs as
// ahead-of-time recompiled units (cpu/guest_cpu_aot.cpp, docs/RECOMP_HOOKS.md). Each patcher keeps its x86 stubs for the other CPUs (FEXCore alone,
// the host CPU) and, when every one of its sites has a slot, installs C++ callbacks instead: no stub page, no detour, the function stays native.
//
// A site is declared where it is installed (address, length of the replaced instructions, the addresses the callback may continue at). The
// recompiler's catalog tools/recomp/variants/hooks-patchers.txt is generated from exactly these calls: tools/recomp/variants/gen_patchers.cpp builds the
// patchers with BB_HOOK_SITE_DUMP, which makes Slot() print the declaration instead of installing it.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <initializer_list>

#include "../../src/guest_cpu.h"

namespace BbHookSites {

/// A site the recompiled library has a slot for, in a function that runs natively now.
inline bool Available(std::uint64_t address) {
#ifdef BB_HOOK_SITE_DUMP
    (void)address;
    return true;
#else
    return guest_cpu_has_hook_slot(static_cast<std::uintptr_t>(address)) != 0;
#endif
}

#ifdef BB_HOOK_SITE_DUMP
/// Prints `hook <image offset> <length> snap [resume=...]`, the catalog line (the generator reads stdout).
inline bool Slot(std::uint64_t address, std::uint64_t image_base, std::size_t length, GuestHookEx, void*,
                 std::initializer_list<std::uint64_t> resume = {}) {
    std::printf("hook %llx %zu snap", static_cast<unsigned long long>(address - image_base), length);
    bool first = true;
    for (const std::uint64_t r : resume) {
        std::printf("%s%llx", first ? " resume=" : ",", static_cast<unsigned long long>(r - image_base));
        first = false;
    }
    std::printf("\n");
    return true;
}
#else
/// Sets `fn` at `address` (an address in the image). `resume`: the addresses `fn` may continue at (set `regs->rip` and return
/// GUEST_HOOK_JUMP); they are read only by the generator.
inline bool Slot(std::uint64_t address, std::uint64_t, std::size_t length, GuestHookEx fn, void* context,
                 std::initializer_list<std::uint64_t> = {}) {
    return guest_cpu_hook_slot(static_cast<std::uintptr_t>(address), length, fn, context) != 0;
}
#endif

} // namespace BbHookSites
