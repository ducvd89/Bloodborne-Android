// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC byte patches for Bloodborne CUSA03173 01.09: the sites, their expected bytes and what they
// become. Shared by the emulator build (bbsc_persistence.cpp) and the injected mod DLL
// (mod/bbsc_mod.cpp), so both apply exactly the same list. Standard library only.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "core/bbsc_persistence.h"

namespace Core::Bbsc::PatchTables {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

// CUSA03173 01.09 addresses (eboot sha256 d65f0b4f...). See docs/SESSION-LIFECYCLE.md.
constexpr u64 MultiPlayStopOffset = 0x01ED07A0;       // CSMultiPlayMan::Stop
constexpr u64 MatchingExistsOffset = 0x01ED00A0;      // CSMultiPlayMan "matching controller exists"
constexpr u64 RemoteCleanupOffset = 0x015BED10;       // WorldChrMan remote-player cleanup
constexpr u64 ClearInsertTasksOffset = 0x01E54830;    // CSMultiPlayMan: destroy every member task
constexpr u64 RemoveMemberOffset = 0x01E54DB0;        // CSMultiPlayMan::RemoveMember(id)
constexpr u64 RemoveMemberTwinOffset = 0x01E55050;    // second member list removal
constexpr u64 MarkSlotsLeavingOffset = 0x014EB860;    // marks every remote slot leaving
constexpr u64 DeathReloadRequestOffset = 0x013CDE30;  // StageTransition request at the end of death

enum class Group : u8 {
    Stage,       // map loads: StageTransition, stage helper, stage-UID policy, stage flow, destructor
    Revive,      // OnReviveMagic_1 reload
    Boss,        // BlockClearAnim / boss-area leave
    MatchCheck,  // OnMatchingCheck (missing remote character) — weakens disconnect detection
    Lamp,        // lamp prompt 4th availability byte (+0x4B) from 0x0191B6E0
    Tasks,       // keep the per-member CSMultiPlayerIns tasks across a kept load (host death)
    HostDead,    // guest ignores the host's death instead of being sent home through a load
    NoFogWalls,  // co-op area fog walls never appear
    LeaveLimit,  // leaving the co-op area does not send a guest home
    Spectate,    // the death steps run in full except for the reload they request at the end
    Echoes,      // a summoned guest keeps the whole blood echo reward, not half of it
    GuestItems,  // a summoned guest sees, picks up and keeps the items of the host's world
    SolidNpcs,   // a guest sees the host world's NPCs as they are, not as grey wandering ghosts
    Scaling,     // vanilla's boss co-op buff (Lua_MultiDoping) off - BBSC enemy scaling instead
    KitBells,    // the BBSC kit's Hunt Bell gets the Beckoning Bell's special checks (Dream use)
    SendBacklog, // one member that never answers does not end the session for everyone
    Hunters,     // the host keeps looking for co-op signs past vanilla's two guests
};

// Lamp action update 0x012F5C40 hides the lamp prompt unless bytes +0x48..+0x4B are all zero.
// +0x49 (multiplayer) is cleared for the host by the shadp2p seamless hook; +0x4B is written
// later from `call 0x0191B6E0` (WorldChrMan scan of characters bound to the lamp entity), which
// the shadp2p hook runs too early to affect. Retargeting that single call to a native
// `xor eax,eax; ret` clears the fourth gate at this call site only.
constexpr u64 LampCharacterGateOffset = 0x0191B6E0;
constexpr u64 ReturnZeroOffset = 0x01300AA0;

struct CallRetarget {
    const char* name;
    u64 site;       // offset of the 5-byte call/jmp rel32
    u8 opcode;      // 0xE8 call or 0xE9 jmp
    u64 original;   // expected current target
    u64 target;     // replacement target
    Group group;
};

// Every Stop site below is followed by `test al,al; je` and stores a session field only on
// success; MatchingExists returns true exactly when a room is active, so callers keep their
// native success branch without the session being torn down. The paired cleanup call is
// redirected to a return-only function.
constexpr std::array<CallRetarget, 27> Retargets{{
    // SoloPlayDeath_2 (0x01381DE0) and PartyGhostDeath_2 (0x013824C0) are the last step of the
    // death script. Both end with `0x01332BC0()` (pick the reload destination) and
    // `0x013CDE30()` (request the StageTransition that rebuilds the world). BBSC used to skip the
    // whole step, which left the step's own bookkeeping undone: the death UI objects at
    // global+0x1690/+0x1698/+0x16B0/+0x16B8 were never released, so a SECOND death never reached
    // the step again and the player stayed dead with only camera control (run 2026-09-19, host
    // log: death armed, no "death held", room timed out 60 s later). Dropping only the reload
    // call lets the step finish - including its own blood echo drop - while the world stays up.
    {"SoloPlayDeath_2.Reload", 0x01381EDE, 0xE8, DeathReloadRequestOffset, PersistNoOpOffset,
     Group::Spectate},
    {"PartyGhostDeath_2.Reload", 0x01382663, 0xE8, DeathReloadRequestOffset, PersistNoOpOffset,
     Group::Spectate},
    {"Lamp.CharacterGate", 0x012F83BC, 0xE8, LampCharacterGateOffset, ReturnZeroOffset, Group::Lamp},
    {"StageTransition.Stop", 0x013CDEF6, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Stage},
    {"StageTransition.Cleanup", 0x013CDF17, 0xE8, RemoteCleanupOffset, PersistNoOpOffset, Group::Stage},
    {"StageHelper.Stop", 0x013CDDF5, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Stage},
    {"StageHelper.Cleanup", 0x013CDE26, 0xE9, RemoteCleanupOffset, PersistNoOpOffset, Group::Stage},
    {"StageUidPolicy.Stop", 0x0193C657, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Stage},
    {"StageUidPolicy.Cleanup", 0x0193C689, 0xE8, RemoteCleanupOffset, PersistNoOpOffset, Group::Stage},
    {"StageFlow.Stop", 0x01939876, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Stage},
    // Also retargeted by the shadp2p seamless patches; accepted in either state.
    {"StageTaskDestructor.Stop", 0x019471B1, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Stage},
    {"Revive.Stop", 0x01389DDD, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Revive},
    {"Revive.Cleanup", 0x01389DFE, 0xE8, RemoteCleanupOffset, PersistNoOpOffset, Group::Revive},
    {"BossArea.Stop", 0x0138B410, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::Boss},
    {"BossArea.Cleanup", 0x0138B437, 0xE8, RemoteCleanupOffset, PersistNoOpOffset, Group::Boss},
    {"MatchingCheck.Stop", 0x013809B7, 0xE8, MultiPlayStopOffset, MatchingExistsOffset, Group::MatchCheck},
    {"MatchingCheck.Cleanup", 0x013809E8, 0xE9, RemoteCleanupOffset, PersistNoOpOffset, Group::MatchCheck},
    // Stage task destructor (0x019470F0) destroys every member task when the summon reload phase
    // is not 3. A host death is exactly that case, so the host loses the task that recreates the
    // guest's character and can no longer see the guest (observed 2026-09-16: host shape ins=[]).
    {"StageTaskDestructor.ClearIns", 0x01947207, 0xE8, ClearInsertTasksOffset, PersistNoOpOffset,
     Group::Tasks},
    // PlayerIns death handler 0x018C49F0: when the LOCAL player dies and the session state is 3
    // (host), it calls RemoveMember for every member task, the twin removal for the second list,
    // and marks every remote slot as leaving. That is the host-death kick (trace 2026-09-16:
    // RemoveMember x2 from 0x018C4D82 right at the host's death). Only the host branch is patched.
    {"HostOnDeath.RemoveMember", 0x018C4D7D, 0xE8, RemoveMemberOffset, PersistNoOpOffset,
     Group::HostDead},
    {"HostOnDeath.RemoveMemberTwin", 0x018C4DAC, 0xE8, RemoveMemberTwinOffset, PersistNoOpOffset,
     Group::HostDead},
    {"HostOnDeath.MarkSlotsLeaving", 0x018C4DBA, 0xE8, MarkSlotsLeavingOffset, PersistNoOpOffset,
     Group::HostDead},
    // Guest item pickup, part 6 (2026-10-04, "items cannot be picked up"). Guest log 2026-10-02:
    // the map's items were there right after every load (list +0x28: 132 items, 119 pickable) and
    // the whole list was gone 2-3 s after each "reinsert v8: restarted ... FirstSyncWait". The item
    // manager's network update 0x017D2F30 reads packet 0x2C; one message kind is "drop your items"
    // with a list selector ([rbp-0x19C] + 1, jump table 0x017D4C2C): -1 = all three lists
    // (0x017D3498/34A0/34A8), or one list (0x017D4089/409A/40AB). 0x017D59F0 / 5B40 / 5C40 free the
    // list's items with +0x48 bit 0x80 and then zero the list head, so every map item vanished
    // while its glow could stay. In vanilla the guest never owns map items, so the host's join sync
    // clears them; with BBSC the guest makes its own (GuestItems.SpawnOwn), so the clear is dropped.
    // Nothing else calls these three.
    {"GuestItems.KeepOnSyncAll1", 0x017D3498, 0xE8, 0x017D59F0, ReturnZeroOffset, Group::GuestItems},
    {"GuestItems.KeepOnSyncAll2", 0x017D34A0, 0xE8, 0x017D5B40, ReturnZeroOffset, Group::GuestItems},
    {"GuestItems.KeepOnSyncAll3", 0x017D34A8, 0xE8, 0x017D5C40, ReturnZeroOffset, Group::GuestItems},
    {"GuestItems.KeepOnSync1", 0x017D4089, 0xE8, 0x017D59F0, ReturnZeroOffset, Group::GuestItems},
    {"GuestItems.KeepOnSync3", 0x017D409A, 0xE8, 0x017D5C40, ReturnZeroOffset, Group::GuestItems},
    {"GuestItems.KeepOnSync2", 0x017D40AB, 0xE8, 0x017D5B40, ReturnZeroOffset, Group::GuestItems},
}};

// Functions made to return immediately. Only for Lua-registered C functions whose whole job is a
// session-ending action, so skipping all of it leaves nothing half-applied.
struct EntryReturn {
    const char* name;
    u64 site;
    std::array<u8, 6> prologue;  // expected first bytes
    Group group;
};

constexpr std::array<EntryReturn, 3> EntryReturns{{
    // OnLeave_Limit (Lua, 0x0138A4A0): a living guest who leaves the co-op area is sent home
    // (event state 2, forced warp global+0x1520, home respawn point 0x01332BC0(-1), StageTransition).
    // With the fog walls gone the guest may walk past their old spot, so the step is skipped.
    // Skipping it also removes the only way a guest gets sent home, so it is off by default:
    // with it on the players reported not being able to leave the world (run 2026-09-19).
    {"OnLeave_Limit", 0x0138A4A0, {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50}, Group::LeaveLimit},
    // HostDead (Lua, 0x01381870): the first step on the guest when the host dies (returns at once
    // on the host itself, state 3). It sets event flag 4047 (0x0132E5A0), shows the host-dead
    // message and sets death-pending [0x0553B130]+0x70 (0x01386960), and schedules HostDead_1 and
    // EventMenuBrake (menu brake MenuMan+0x2DC). With the guest staying in the world none of that
    // is wanted; after it ran the guest could not use ladders (run 2026-09-16).
    {"HostDead", 0x01381870, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57}, Group::HostDead},
    // HostDead_1 (Lua, registered at 0x0138DADB): runs on the guest when the host dies. It sets the
    // forced-warp byte (global+0x1520), selects the home respawn point (0x01332BC0(-1)), starts a
    // StageTransition (0x013CDE30) and queues the original player type (+0x8C). Skipping it keeps
    // the guest standing where he is, with no loading screen, while the host respawns.
    {"HostDead_1", 0x01381A60, {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50}, Group::HostDead},
}};

// Two-byte instruction rewrites, verified against the bytes that must be there.
struct ByteEdit {
    const char* name;
    u64 site;
    std::array<u8, 2> original;
    std::array<u8, 2> replacement;
    Group group;
};

constexpr std::array<ByteEdit, 17> ByteEdits{{
    // The lamp prompt in 0x012F5C40 needs all four of +0x48, +0x49, +0x4A and +0x4B to be zero.
    // +0x49 is the "I am in a multiplayer session" flag, built right here:
    //     CMP dword ptr [RBX + 0x14], 0x1
    //     SETG AL                            ; > 1 members
    //     TEST ECX, ECX
    //     SETG CL                            ; SprjSessionManager member vector is non-empty
    //     OR   CL, AL                        <- 0x012F8321, the two bytes we replace
    //     MOV  byte ptr [R13 + 0x49], CL
    // "OR CL,AL" (08 C1) becomes "XOR CL,CL" (30 C9), so the flag stores 0 and the prompt appears
    // in co-op. Same length, same register, nothing after it moves.
    //
    // This replaces what Lamp.CharacterGate was meant to do. That entry neutralises 0x0191B6E0,
    // but the decompile shows that call only runs when +0x49 is already zero - so in a session it
    // never executed and the patch did nothing. Reported 2026-09-22: no lamp prompt at all, with
    // lamp=true in the log.
    {"Lamp.MultiplayerGate", 0x012F8321, {0x08, 0xC1}, {0x30, 0xC9}, Group::Lamp},

    // The "Travel" prompt itself comes from the lamp's talk script, not from 0x012F5C40. The talk
    // condition dispatcher 0x0203AD60 answers the script function "is multiplayer in progress"
    // (case 0x34) with the same expression as +0x49 above:
    //     CMP dword ptr [RBX + 0x14], 0x1 / SETG AL / TEST ECX, ECX / SETG CL
    //     OR   CL, AL                        <- 0x0203C564
    //     MOVZX EAX, CL                      ; the script's answer
    // Clearing it makes every talk script on the HOST see a solo game (lamp prompt, and anything
    // else the scripts gate on co-op). The guest keeps the vanilla answer: Group::Lamp is a role
    // group that is only switched on while hosting.
    {"Lamp.TalkMultiplayerCheck", 0x0203C564, {0x08, 0xC1}, {0x30, 0xC9}, Group::Lamp},

    // Guest item pickup, part 2 (part 1 is below in WideEdits). Picking up a map treasure is
    // 0x017D8570(item manager [0x0553D6E0], &item id). For a treasure (item +0x48 bit 0x80) it asks
    // the session state first:
    //     CMP dword ptr [RAX + 0x124], 0x6    ; SprjSessionManager: summoned guest
    //     JNZ 0x017D8768                      <- 0x017D85FB: not a guest -> take it locally
    //     ...                                 ; a guest only sends packet 0x2C to the host
    // and later, on that local path:
    //     CMP dword ptr [RAX + 0x124], 0x6
    //     JZ  0x017D88D3                      <- 0x017D88C2: a guest skips 0x017D89F0 (give items)
    // So a guest takes the host's path: the item is marked taken in the guest's game and its lot is
    // given to the guest. The inventory is saved live (the guest's save writer 0x0156AA50 only swaps
    // world snapshots), so the guest keeps it; the host's world is not touched and the host can pick
    // the same item up too. 0F 85 -> 90 E9 keeps the displacement and the target (0x017D8768).
    {"GuestItems.PickupAsHost", 0x017D85FB, {0x0F, 0x85}, {0x90, 0xE9}, Group::GuestItems},
    {"GuestItems.GiveToGuest", 0x017D88C2, {0x74, 0x0F}, {0x90, 0x90}, Group::GuestItems},
    // Guest item pickup, part 3 (2026-09-26, "guest can't see items", NG+ guest that took nothing):
    // the guest's map items were never created. Spawning an item (0x017D8370, called for the map's
    // treasures and for drops) does, for a summoned guest,
    //     CMP dword ptr [RAX + 0x124], 0x6
    //     JNZ 0x017D8511                      <- 0x017D83D2: host/solo -> create it here (0x017DE870)
    // and otherwise only sends packet 0x2C type 3 to the host - nothing ever comes back as an item
    // the guest can see. 0F 85 -> 90 E9: the guest creates its own, from its own map load and its
    // own pickup flags, so each player sees the items they have not taken (personal loot).
    {"GuestItems.SpawnOwn", 0x017D83D2, {0x0F, 0x85}, {0x90, 0xE9}, Group::GuestItems},
    // Part 4: marking a picked-up item taken (its pickup flag +0x28 and count flag +0xB8, in
    // 0x017D6780) is skipped for a guest outside its own world:
    //     CMP dword ptr [RAX + 0x124], 0x6 / MOV R14, RBX
    //     JZ  0x017D6AE8                      <- 0x017D6ADC: guest -> "own world?" -> skip
    //     CMP dword ptr [R15 + 0x98], -1 / JZ 0x017D6AFC (set the flags)
    // Without the flag a guest's item came back after every load. 74 0A -> EB 1E jumps straight
    // to 0x017D6AFC. The flag lives in the live table of the host's world only; the guest's save
    // keeps its own world (see bloodborne_re.cpp, world state).
    {"GuestItems.MarkTaken", 0x017D6ADC, {0x74, 0x0A}, {0xEB, 0x1E}, Group::GuestItems},
    // Part 5 (2026-09-29, "items cannot be seen by the guest", still on 5B726D9D): a guest's log
    // showed 0x017D8370 and 0x017DE870 never run at all, host or guest, so parts 3 and 3b patched
    // functions map treasures do not go through. Map treasures are placed by 0x017DC300(item man,
    // entity, MSB event, ...), called per MSB event of type 4 (treasure) at map load, which for a
    // summoned guest or anyone in another world skips the whole thing (see WideEdits, part 5).
    // It then hands each treasure to 0x017D4C40 / 0x017D4FB0 / 0x017D5320 by lot kind, and each
    // of those numbers the new item only when hosting or solo:
    //     CMP dword ptr [RAX + 0x124], 0x6 ; JZ skip     <- guest
    //     CMP dword ptr [R15 + 0x98], -1   ; JNZ skip    <- in another world
    //     MOV EAX, [RBX + 8] / INC / store: item number = counter << 4
    // An item left unnumbered cannot be looked up by its pickup, so both jumps become NOPs: the
    // guest numbers its own items from its own counter, as its game does in its own world.
    {"GuestItems.NumberKind1", 0x017D4F62, {0x74, 0x26}, {0x90, 0x90}, Group::GuestItems},
    {"GuestItems.NumberKind1Away", 0x017D4F6F, {0x75, 0x1C}, {0x90, 0x90}, Group::GuestItems},
    {"GuestItems.NumberKind3", 0x017D52D9, {0x74, 0x26}, {0x90, 0x90}, Group::GuestItems},
    {"GuestItems.NumberKind3Away", 0x017D52E6, {0x75, 0x1C}, {0x90, 0x90}, Group::GuestItems},
    {"GuestItems.NumberKind2", 0x017D5649, {0x74, 0x26}, {0x90, 0x90}, Group::GuestItems},
    {"GuestItems.NumberKind2Away", 0x017D5656, {0x75, 0x1C}, {0x90, 0x90}, Group::GuestItems},
    // Grey NPCs (2026-09-29, guest, outside the Dream): the NPC constructor 0x018E4870 gives an
    // NpcParam row with isChangeWanderGhost (+0x145 bit 7) its ghost model (+0x10C) and type 4
    // whenever the local player is in someone else's world:
    //     CMP dword ptr [RAX + 0x124], 0x6 ; JZ ... / CMP [RBX + 0x98], -1 ; JZ 0x018E4FDC
    //     MOV ECX, [RAX + 0x145] / TEST CL, 0x80
    //     JZ  0x018E4FDC                     <- 0x018E4FBF: not a ghost NPC -> keep its own look
    //     ... ghost model, R12D = 4
    // 74 1B -> EB 1B: every NPC keeps its own model and type for a guest, as the host sees it.
    // Host and solo never reach this test. The Dream-only row edit (Doll, Gehrman) stays as well.
    {"SolidNpcs.NoWanderGhost", 0x018E4FBF, {0x74, 0x1B}, {0xEB, 0x1B}, Group::SolidNpcs},
    // Lua_MultiDoping (0x0138BC70): CMP EAX,2 / JNZ (2 guests -> SpEffect 7501) and CMP EAX,1 / JNZ
    // (1 guest -> 7500). Both JNZ -> JMP: the boss never gets the vanilla buff, the game still
    // sets its event flag 4070 after it. BBSC's own scaling (bloodborne_re.cpp) replaces it.
    {"Scaling.NoVanillaTwoGuests", 0x0138BCC2, {0x75, 0x32}, {0xEB, 0x32}, Group::Scaling},
    {"Scaling.NoVanillaOneGuest", 0x0138BCF9, {0x75, 0x54}, {0xEB, 0x54}, Group::Scaling},
    // Boss insight for the guest (2026-10-08, testers: "guest only gets insight for discovering the
    // boss, not for killing it"). The kill reward 0x018C5070, run in every player's game for every
    // death, gives a boss's insight ("HumanityDrop", boss row via 0x01F1B4C0, then 0x01F1B760 /
    // 0x01F1B830 / 0x028986B0) only behind
    //     CMP byte ptr [global + 0x1592], 0   ; in our own world?
    //     JZ  0x018C59A6                      <- 0x018C52F9 (0F 84 A7 06 00 00): no -> nothing
    // A guest is never in its own world. EB 04 jumps over the rest of the JZ to the next
    // instruction (0x018C52FF), so the guest's copy gives the guest's insight too; host and solo
    // never took the jump anyway.
    {"Echoes.GuestBossInsight", 0x018C52F9, {0x0F, 0x84}, {0xEB, 0x04}, Group::Echoes},
    // Five hunters (test 2026-10-08, host with 2 guests: a third player's co-op sign got
    // 18 empty sign lists in a row, the same player's invader sign was found at once). The host's
    // sign search 0x01872360 asks the server for co-op signs (type 7) only while one of vanilla's
    // two co-op slots is free - the slot table 0x0186FE40 fills [rbp-0x9C] / [rbp-0x90] with the
    // first and second guest:
    //     CMP [rbp-0x9C], 0 ; JZ  0x018723B1   ; first slot free -> search
    //     MOV EAX, [rbp-0x90] ; TEST EAX, EAX
    //     JNZ 0x018723FF                        <- 0x018723AF (75 4E): both taken -> no co-op search
    // 75 4E -> 90 90: the co-op search goes on; the summon builder's own cap (4 cooperators,
    // ApplyBbscHunterCap) still decides who is let in.
    {"Hunters.KeepSearchingCoop", 0x018723AF, {0x75, 0x4E}, {0x90, 0x90}, Group::Hunters},
}};

// Four-byte rewrites (a RIP-relative displacement), verified against the bytes that must be there.
struct WideEdit {
    const char* name;
    u64 site;
    std::array<u8, 4> original;
    std::array<u8, 4> replacement;
    Group group;
};

constexpr std::array<WideEdit, 11> WideEdits{{
    // Every blood echo reward the local player receives is computed by 0x01335400 and left in a
    // float at [[0x0553B130]+8]+0xF8. Halfway through:
    //     MOV EAX, dword ptr [R15 + 0x78]    ; the local player's character type
    //     OR  EAX, 0x8 / CMP EAX, 0x8
    //     JZ  0x01335596                     ; host (type 0) and type 8 skip the next three
    //     VMOVSS XMM0, [RBP + -0x44]
    //     VMULSS XMM0, XMM0, [RIP + 0x035F058B]   <- 0x01335589, disp32 at 0x0133558D
    //     VMOVSS [RBP + -0x44], XMM0         ;    that address is 0x04925B1C = 0.5
    // Skipping the multiply (JZ -> JMP, builds up to 39A08659) did not help: a guest still got
    // exactly half the host's echoes for the same kill (+671 against +1341, both players NG+,
    // 2026-09-24). That is because 0x01335400 is only the EVENT reward (registered as a Lua/event
    // function at 0x013421AC); ordinary kills go through 0x018FC860, below. Doubling here
    // (build 43B41805) changed nothing for kills either, and would have paid a guest 2x for event
    // rewards, so the multiply now reads 1.0 - the float at 0x04925B20, right after the 0.5.
    // New displacement: 0x04925B20 - 0x01335591 = 0x035F058F.
    {"Echoes.GuestEventFull", 0x0133558D, {0x8B, 0x05, 0x5F, 0x03}, {0x8F, 0x05, 0x5F, 0x03},
     Group::Echoes},
    // The kill reward: 0x018FC860(rate, chr, enemy, bonus, guest_share), called by 0x018C5070
    // with guest_share = [[0x05556678]+0x16F8]+0x98 != -1 || SprjSessionManager+0x124 == 6 (a
    // summoned guest), multiplies by 0.5 when guest_share is set:
    //     0x018FC947  VMOVSS XMM3, [RIP + 0x0302B571]   ; 0x04927EC0 = 0.5
    // and pays it through chr->vtable[0x3D0]. Pointing the load at 0x04927EBC (1.0, the float
    // just before) pays a guest what the host gets: +671 against +1341 in every 2026-09-24 test.
    // New displacement: 0x04927EBC - 0x018FC94F = 0x0302B56D.
    {"Echoes.GuestKillFull", 0x018FC94B, {0x71, 0xB5, 0x02, 0x03}, {0x6D, 0xB5, 0x02, 0x03},
     Group::Echoes},
    // Guest item pickup, part 1. 0x017D9E70(item manager, &item id, on) makes an item pickable
    // (+0x48 bit 0x40, which the pickup prompt search 0x017D9FB0 requires). For map treasures
    // (bit 0x80) it refuses a summoned guest with the same two tests as the echo halving:
    //     CMP dword ptr [RCX + 0x124], 0x6    ; JZ  return 0   (0F 84 B6000000 at 0x017D9EE7)
    //     CMP dword ptr [R15 + 0x98], -1      ; JNZ return 0   (0F 85 A8000000 at 0x017D9EF5)
    // Zeroing both displacements makes each jump land on the next instruction, so the treasure is
    // enabled for the guest exactly as for the host.
    {"GuestItems.ShowTreasure", 0x017D9EE9, {0xB6, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00},
     Group::GuestItems},
    {"GuestItems.ShowTreasureShare", 0x017D9EF7, {0xA8, 0x00, 0x00, 0x00},
     {0x00, 0x00, 0x00, 0x00}, Group::GuestItems},
    // Part 3, second half: the create function 0x017DE870 itself starts with
    //     CMP dword ptr [RAX + 0x124], 0x6 ; JZ 0x017DEB34 (return)   (0F 84 5C020000 at 0x017DE8D2)
    // A zero displacement lands the jump on the next instruction, so the guest's item is made.
    // It then only broadcasts the new item when hosting (+0x124 == 3), so a guest sends nothing.
    {"GuestItems.CreateOwn", 0x017DE8D4, {0x5C, 0x02, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00},
     Group::GuestItems},
    // Part 5: placing the map's treasures, 0x017DC300 (see ByteEdits, part 5):
    //     CMP dword ptr [RAX + 0x124], 0x6 ; JZ  0x017DD081   (0F 84 A50C0000 at 0x017DC3D6)
    //     CMP dword ptr [RDX + 0x98], -1   ; JNZ 0x017DD081   (0F 85 980C0000 at 0x017DC3E3)
    // 0x017DD081 is the end of the function: a guest placed no treasure at all. Zero
    // displacements fall through, so the guest's game places them from its own map load. Which
    // are shown is decided by the pickup flags (guest loot view in bloodborne_re.cpp); the
    // character type table 0x047348A0 used right after allows type 1 (co-op phantom).
    {"GuestItems.PlaceTreasures", 0x017DC3D8, {0xA5, 0x0C, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00},
     Group::GuestItems},
    {"GuestItems.PlaceTreasuresAway", 0x017DC3E5, {0x98, 0x0C, 0x00, 0x00},
     {0x00, 0x00, 0x00, 0x00}, Group::GuestItems},
    // Report 2026-10-07: "cannot use the bells in the Dream". The item availability check 0x0157F200
    // gives the bells their own branches by goods id: CMP R12D, 0xCD / 0xE1 (Small Resonant /
    // Sinister Bell) and at 0x0157F855 CMP EBX, 0xC8 (Beckoning Bell, 81 FB C8000000) - the branch
    // that holds the seamless patches letting a host ring it where vanilla says no (0x0157F8C8,
    // 0x0157F960). The kit's Hunt Bell is goods 4016 (a copy of 200), so it never got there; the
    // vanilla bell itself is disabled by the kit. Its id goes into the compare: 4016 = 0x00000FB0.
    // (The Kin Bell already works in the Dream through the general path - left as it is.)
    {"KitBells.HuntBellChecks", 0x0157F857, {0xC8, 0x00, 0x00, 0x00}, {0xB0, 0x0F, 0x00, 0x00},
     Group::KitBells},
    // Three players, 2026-10-08 (host, guest cls_amg; a tester's game had crashed but his
    // sign was still listed): the host summoned the dead sign and sat in CreateWait for 6 minutes,
    // then its own game raised "OnIrregularLeaveSession" (0x017897DA) and ended the session for the
    // guest too. That is the reliable-packet send 0x017894E0: a packet it cannot deliver to a member
    // is queued (list [SprjSessionManager]+0x188, counts at +0x30, plus +0x1B0), and once
    //     CMP EAX, 0x3E9                      <- 0x017897AD (3D E9 03 00 00)
    // 1001 are waiting it leaves. A member that never connects fills it at ~3 a second. The limit
    // goes to 30000 (~2-3 hours); "BBSC send backlog:" lines log a queue that keeps growing.
    {"SendBacklog.Limit", 0x017897AE, {0xE9, 0x03, 0x00, 0x00}, {0x30, 0x75, 0x00, 0x00},
     Group::SendBacklog},
    // Five hunters, part 2 (test 2026-10-08 on mod 37: with Hunters.KeepSearchingCoop the host's
    // sign lists DID hold a third co-op sign - 3 signs, 2742-2745 bytes - but no summon was ever
    // built for it). After the search, the same slot table decides which sign types may be taken:
    //     both co-op slots taken -> MOV ESI, 0xFFF7FF7F (co-op types masked out), else 0xFFFFFFFF
    // at 0x0187304A (sign search 0x01872360) and 0x01879382 (0x01878D90). The mask stays all-ones;
    // the summon builder's own cap (4 cooperators) still decides who gets in.
    {"Hunters.CoopMaskSearch", 0x0187304B, {0x7F, 0xFF, 0xF7, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF},
     Group::Hunters},
    {"Hunters.CoopMaskSelect", 0x01879383, {0x7F, 0xFF, 0xF7, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF},
     Group::Hunters},
}};

// Function entries replaced by a jmp to a sibling function with the same arguments.
struct EntryJump {
    const char* name;
    u64 site;
    std::array<u8, 5> prologue; // expected first 5 bytes (whole instructions)
    u64 target;
    Group group;
};

constexpr std::array<EntryJump, 1> EntryJumps{{
    // Lua_MultiWall(id) (0x0138B720) shows the co-op fog wall entity: 0x013CABD0(id, 1) draw on,
    // 0x013CA580(id, 1) collision on, 0x01315FA0(id + 1) its effect. Lua_InvalidMultiWall (0x0138B890)
    // is the same function with 0 / 0x01315E50, i.e. hides the wall. Jumping there keeps every wall
    // hidden and walkable (CONFIRMED statically from both listings, 2026-09-17).
    {"Lua_MultiWall", 0x0138B720, {0x55, 0x48, 0x89, 0xE5, 0x53}, 0x0138B890, Group::NoFogWalls},
}};

// ---- Which groups are on (same switches in the emulator and the mod DLL) ----

inline bool EnvFlag(const char* name) {
    return SwitchEnabled(name);
}

inline bool GroupEnabled(Group group) {
    switch (group) {
    case Group::Stage:
    case Group::Revive:
        return true;
    case Group::Boss:
        return EnvFlag("SHADPS4_BBSC_PERSIST_BOSS");
    case Group::MatchCheck:
        return EnvFlag("SHADPS4_BBSC_PERSIST_MATCHCHECK");
    case Group::Lamp:
        return EnvFlag("SHADPS4_BBSC_LAMP");
    case Group::Tasks:
        return EnvFlag("SHADPS4_BBSC_PERSIST_TASKS");
    case Group::HostDead:
        return EnvFlag("SHADPS4_BBSC_PERSIST_HOSTDEAD");
    case Group::NoFogWalls:
        return EnvFlag("SHADPS4_BBSC_NO_FOG_WALLS");
    case Group::LeaveLimit:
        return EnvFlag("SHADPS4_BBSC_SKIP_LEAVE_LIMIT");
    case Group::Spectate:
        return EnvFlag("SHADPS4_BBSC_SPECTATE");
    case Group::Echoes:
        return EnvFlag("SHADPS4_BBSC_FULL_ECHOES");
    case Group::GuestItems:
        return EnvFlag("SHADPS4_BBSC_GUEST_ITEMS");
    case Group::SolidNpcs:
        return EnvFlag("SHADPS4_BBSC_SOLID_NPCS");
    case Group::Scaling:
        return EnvFlag("SHADPS4_BBSC_SCALING");
    case Group::KitBells:
        return true; // the kit is always given (bloodborne_re.cpp, PrepareBbscKitRows)
    case Group::SendBacklog:
        return true;
    case Group::Hunters:
        return EnvFlag("SHADPS4_BBSC_FIVE_HUNTERS");
    }
    return false;
}

// Groups a suspension (quit to title, partner gone) switches off so the vanilla teardown can run.
// Only the ones that keep a session alive: the echo fix, the lamp fix, the fog walls, the death
// reload retarget and host-death handling have nothing to do with ending a session, and switching
// them off for 20 s halved echoes, blocked lamps and let fog walls appear (code review B2).
inline bool IsSessionGroup(Group group) {
    switch (group) {
    case Group::Stage:
    case Group::Revive:
    case Group::Boss:
    case Group::MatchCheck:
    case Group::Tasks:
        return true;
    default:
        return false;
    }
}

// The lamp edits follow the player's role at runtime instead of being on all the time: a guest
// must never get the lamp prompt (vanilla never allows it, and a guest's lamp travel starts a load
// in the host's world), and single player must keep the vanilla gates (code review B3/B4).
inline bool IsRoleGroup(Group group) {
    return group == Group::Lamp;
}

enum class Scope : u8 { Install, Session, Role };

inline bool InScope(Group group, Scope scope) {
    switch (scope) {
    case Scope::Install:
        return !IsRoleGroup(group); // role groups start in their vanilla state
    case Scope::Session:
        return IsSessionGroup(group);
    case Scope::Role:
        return IsRoleGroup(group);
    }
    return false;
}

inline std::array<u8, 5> EncodeRel32(u8 opcode, u64 site, u64 target) {
    const std::int64_t displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + 5);
    const std::int32_t rel = static_cast<std::int32_t>(displacement);
    std::array<u8, 5> bytes{opcode};
    std::memcpy(bytes.data() + 1, &rel, sizeof(rel));
    return bytes;
}

} // namespace Core::Bbsc::PatchTables
