// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC: find bytes in the game's memory (the param tables are found by their header text). Kept
// out of bloodborne_re.cpp so <windows.h> stays out of it. Shared by the emulator and the mod DLL.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Core::Bbsc {

// Every address in [lo, hi) where `needle` sits in committed, readable memory (at most max_hits).
// Slow (a full walk of the address range): call it once, off the game thread.
std::vector<std::uint64_t> FindInMemory(std::span<const std::uint8_t> needle, std::uint64_t lo,
                                        std::uint64_t hi, std::size_t max_hits);

} // namespace Core::Bbsc
