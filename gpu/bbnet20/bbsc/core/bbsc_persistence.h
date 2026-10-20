// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Core::Bbsc {

// BBSC build defaults: the co-op features confirmed working in game (2026-09-16) are on without
// any environment variable, so the build runs straight from a launcher. Everything still only
// applies to CUSA03173 01.09 (checked where each feature installs). Experimental switches stay
// off, and so is the boot intro (SHADPS4_BBSC_INTRO=1 brings it back). Setting a variable always wins: "0" turns a default off, any other value turns it on.
inline constexpr std::array<std::string_view, 31> DefaultOnSwitches{
    "SHADPS4_BBSC_SHARED_PROGRESS",     // merge the host's progress into the guest's world (adds only)
    "SHADPS4_BBSC_WARP_TOGETHER",        // the guest follows the host through a lamp warp
    "SHADPS4_BBSC_WORLD_SYNC",           // ...and loads the host's map with the host's world flags
    "SHADPS4_BBSC_LOAD_COVER",           // the guest's summon/follow loads behind a smooth black fade
    "SHADPS4_BBSC_FULL_ECHOES",          // a summoned guest is paid the host's rate, not half
    "SHADPS4_BBSC_GUEST_ITEMS",          // a guest picks up and keeps items in the host's world
    "SHADPS4_BBSC_SOLID_NPCS",           // a guest sees NPCs solid, not as grey wandering ghosts
    "SHADPS4_BBSC_FLOOR_GUARD",          // no crash when a character's floor was unloaded
    "SHADPS4_BBSC_DREAM",                // a guest uses the Doll, workshop and messengers in the host's Dream
    "SHADPS4_BBSC_DEATH_PENALTY",        // a kept death still drops a bloodstain and the echoes
    "SHADPS4_BBSC_TRACE_WALLS",          // log which map objects get shown/made solid (fog walls)
    "SHADPS4_BBSC_LAMP",                 // the lamp prompt works in co-op (clears the +0x4B gate)
    "SHADPS4_BBSC_NO_HP_PENALTY",        // clears the summoned guest's 30% max HP cut (effect 9006)
    "SHADPS4_BBSC_NO_FOG_WALLS",         // co-op area fog walls hidden, leaving the area allowed
    "SHADPS4_BBSC_MESSAGES",             // co-op texts that fit seamless play (no "Returning to your world")
    "SHADPS4_BBSC_CONNECT_FIRST",        // cross-map summon: join first, one load into host map
    "SHADPS4_BBSC_WARP_JOIN",            // after a cross-map join, one more real reload (lighting)
    "SHADPS4_BLOODBORNE_SEAMLESS_COOP",  // Wozzardman's seamless summon
    "SHADPS4_BLOODBORNE_RE_TRACE",       // installs the BBSC hooks (and the trace logs)
    "SHADPS4_CAPTURE_BLOODBORNE_SUMMON", // summon capture files for collect-logs
    "SHADPS4_BBSC_PERSIST",              // keep the session through loads and deaths
    "SHADPS4_BBSC_PERSIST_MATCHCHECK",   // ignore the post-load matching check kick
    "SHADPS4_BBSC_REJOIN",               // guest rejoin / reinsert after a kept death
    "SHADPS4_BBSC_PERSIST_TASKS",        // keep member tasks across a kept load
    "SHADPS4_BBSC_PERSIST_HOSTDEAD",     // the guest stays when the host dies
    "SHADPS4_BBSC_SPECTATE",             // respawn in place / spectate, join teleport
    "SHADPS4_BBSC_FREE_BELL",            // the Beckoning Bell costs no Insight
    "SHADPS4_BBSC_FIVE_HUNTERS",         // the host may summon up to 4 guests (5 hunters)
    "SHADPS4_BBSC_CINEMATIC",            // F1: free camera for filming (keyboard + mouse)
    "SHADPS4_BBSC_SCALING",              // enemy/boss HP, damage, stagger per guest (like ERSC)
    "SHADPS4_BBSC_INVADE_ANYWHERE",      // every world open to invaders (Bell Maiden SpEffect 9020)
};

// False in the vanilla build (CMake BBSC_VANILLA): the emulator carries no BBSC of its own and
// BBSC runs as the bbsc.dll mod, which compiles this header itself (BBSC_MOD) with BuiltIn true.
#if defined(BBSC_VANILLA) && !defined(BBSC_MOD)
inline constexpr bool BuiltIn = false;
#else
inline constexpr bool BuiltIn = true;
#endif

inline bool SwitchEnabled(const char* name) {
    if constexpr (!BuiltIn) {
        // Every BBSC feature is off, whatever the environment says. The three real opt-out
        // switches count as set, so the emulator's own seamless-summon helpers (fast sign,
        // placement header) stay out of the way of the mod's.
        const std::string_view key{name};
        return key == "SHADPS4_BBSC_NO_PLACEMENT_HEADER" || key == "SHADPS4_BBSC_NO_FAST_SIGN" ||
               key == "SHADPS4_BBSC_NO_GUEST_HOOKS";
    }
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        for (const std::string_view on : DefaultOnSwitches) {
            if (on == name) {
                return true;
            }
        }
        return false;
    }
    return std::string_view{value} != "0";
}

// Return-only guest function used as the target for skipped remote-player cleanup calls.
// Observed by the RE trace as "BBSC.Persist.SkippedCleanup" to record every persistence decision.
inline constexpr std::uint64_t PersistNoOpOffset = 0x013001E0;

// Phase 1 (docs/PHASE1-DESIGN.md, Option A): keep Bloodborne's co-op session alive across
// structural session ends (stage loads, revive, optionally boss clear and OnMatchingCheck) by
// retargeting the call sites of CSMultiPlayMan::Stop and WorldChrMan remote cleanup.
// On by default (SHADPS4_BBSC_PERSIST=0 disables). CUSA03173 01.09 only; every site is byte-verified.
void InstallPersistentSessionPatches();

// Turns the applied patches off and on again at runtime. A deliberate quit to the title has to
// tear the session down the way the game intends - otherwise the other player keeps a phantom
// partner and has to close the game (reported 2026-09-20). Reverting the patches for a few
// seconds lets the vanilla teardown run, then they go back on. Safe to call repeatedly; does
// nothing when the patches were never applied.
void SetPersistentSessionPatches(bool enabled);

// The lamp edits (prompt gate +0x49 and the character gate) are only on while this player hosts a
// session. Called from the game tick whenever the role changes; the lamp stays vanilla in single
// player and for a guest.
void SetHostLampPatches(bool hosting);

} // namespace Core::Bbsc
