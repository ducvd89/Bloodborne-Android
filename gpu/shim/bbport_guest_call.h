// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <type_traits>

#include "../../src/guest_cpu.h"

/// A call of the game's code (a function pointer the game handed over) from host code: a plain
/// call when the game runs on the host CPU, through the guest CPU otherwise. Integer, enum and
/// pointer arguments and results only.
template <typename R, typename... P, typename... A>
R GuestCall(R (*fn)(P...), A... args) {
    static_assert(sizeof...(P) == sizeof...(A));
#ifdef GUEST_CPU_NATIVE
    return fn(args...);
#else
    static_assert(((std::is_integral_v<P> || std::is_enum_v<P> || std::is_pointer_v<P>) && ...));
    const std::uint64_t packed[sizeof...(P) ? sizeof...(P) : 1] = {[](P value) -> std::uint64_t {
        if constexpr (std::is_pointer_v<P>) {
            return reinterpret_cast<std::uintptr_t>(value);
        } else {
            return static_cast<std::uint64_t>(value);
        }
    }(P(args))...};
    const std::uint64_t result =
        guest_cpu_call(reinterpret_cast<std::uintptr_t>(fn), sizeof...(P), packed);
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(static_cast<std::uintptr_t>(result));
    } else {
        static_assert(std::is_integral_v<R> || std::is_enum_v<R>);
        return static_cast<R>(result);
    }
#endif
}
