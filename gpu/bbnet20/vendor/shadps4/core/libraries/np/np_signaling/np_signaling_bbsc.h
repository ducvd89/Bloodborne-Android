// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// BBSC packets on the np_signaling socket that the Bloodborne code also builds and reads. Kept out
// of np_signaling_state.h, which pulls in the platform headers (their `near`/`far` macros break
// bloodborne_re.cpp).

#include <cstddef>

#include "common/types.h"

namespace Libraries::Np::NpSignaling {

inline constexpr u8 kSignalingBbscWorldFlagsType = 0x22;

// One chunk of a host world-flag snapshot: up to 8 event-flag groups (1000 flags, 125 bytes each)
// of the host's current map block, in the fixed order of Core::Bloodborne's BbscWorldGroupKey.
// ~1 KB, below the 1280-byte tunnel MTU (Tailscale) with the UDP and v-port headers.
inline constexpr size_t kBbscWorldFlagGroupBytes = 125;
inline constexpr size_t kBbscWorldFlagGroupsPerChunk = 8;

#pragma pack(push, 1)
struct SignalingBbscWorldFlags {
    u8 magic[4] = {'S', 'H', 'A', 'D'};
    u8 type = kSignalingBbscWorldFlagsType;
    u8 version = 1;
    u32 packed_map = 0;
    u32 snapshot = 0;   // the host's snapshot counter: chunks of one snapshot belong together
    u8 chunk = 0;       // index of this chunk
    u8 chunk_count = 0; // chunks in the snapshot
    u8 present = 0;     // bit n: group n of this chunk exists in the host's flag table
    u8 group_count = 0; // groups carried (the last chunk may carry fewer than 8)
    u8 groups[kBbscWorldFlagGroupsPerChunk][kBbscWorldFlagGroupBytes]{};
};
static_assert(sizeof(SignalingBbscWorldFlags) == 18 + 8 * 125);
#pragma pack(pop)

} // namespace Libraries::Np::NpSignaling
