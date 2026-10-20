// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_re.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/bbsc_memscan.h"
#include "core/bbsc_persistence.h"
#include "core/bbsc_cinematic.h"
#include "imgui/bbsc_loading_layer.h"
#include "core/cpu_patches.h"
#include "bbnet_guest.h"
#include "core/debugger.h"
#include "core/file_format/psf.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_signaling/np_signaling_helpers.h"
#include "core/libraries/np/np_signaling/np_signaling_bbsc.h"
#include "core/memory.h"

namespace Core::Bloodborne {
namespace {

enum class TraceKind : u8 {
    SosCondition,
    SosValidityDispatch,
    SosValidityCode,
    SosEvent,
    MatchingEvent,
    MatchingSignal,
    MatchingLeaveDecision,
    SummonRequest,
    SummonCandidate,
    SummonSelection,
    SummonBuild,
    GetSosRequest,
    GetSosArea,
    GetSosInsert,
    SosStatus,
    SosMessenger,
    ActionFlags,
    UseItem,
    UseItemExecution,
    GoodsActionSubmit,
    GoodsParamLookup,
    BellAvailability,
    ResponderBellAvailability,
    NetworkAreaRegion,
    MultiplayerInsert,
    CrossMapGuestHandoff,
    SosPlacement,
    GlobalState,
    ReceivedPlacementConditional,
    ReceivedPlacementSet,
    PlacementSelect,
    WarpCommand,
    CharacterWarp,
    WarpResolved,
    WarpLocalApply,
    StageWarp,
    RespawnPointWarp,
    StageWarpDescriptor,
    StageWarpAcknowledge,
    StageWarpRespawnResolve,
    StageWarpTransformResolve,
    StageWarpPlacement,
    SummonPoint,
    MapReload,
    HealingFountainRegistration,
    HealingFountainAvailability,
    ChairRespawnNotification,
    WorldStateValidation,
    EnginePanic,
    LuaSessionEvent,
};

struct TraceSite {
    std::string_view name;
    u64 offset;
    TraceKind kind;
    std::array<u8, 16> prologue;
    u8 prologue_size;
};

struct NativeCallSignature {
    std::string_view name;
    u64 offset;
    std::array<u8, 16> prologue;
    u8 prologue_size;
};

constexpr std::array<u8, 16> StandardR15Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr std::array<u8, 16> StandardR14Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56};
constexpr std::array<u8, 16> StandardRbxPrologue{0x55, 0x48, 0x89, 0xE5, 0x53};
constexpr std::array<u8, 16> ReturnAndPadding{0xC3, 0x66, 0x66, 0x66, 0x66, 0x66,
                                              0x66, 0x2E, 0x0F, 0x1F, 0x84};
constexpr u64 GlobalStatePointerOffset = 0x05556678;
constexpr u64 CandidateLocalStatePointerOffset = 0x0553E878;
constexpr u64 SummonManagerRootPointerOffset = 0x0553B120;
constexpr u64 CurrentMapListPointerOffset = 0x0553B148;
constexpr u64 MatchingStatePointerOffset = 0x05540290;
constexpr u64 SummonSessionRulesPointerOffset = 0x0553D6D0;
constexpr u64 RoleMetadataTableOffset = 0x0553D750;
constexpr u64 SummonBuildRoleTableOffset = 0x0556E560;
constexpr u64 RoleMetadataStride = 0x80;
constexpr s32 RoleMetadataCount = 34;
constexpr u64 BeckoningAreaComparisonOffset = 0x0157F8C8;
constexpr std::array<u8, 6> BeckoningAreaComparison{0x0F, 0x87, 0x90, 0x00, 0x00, 0x00};
constexpr std::array<u8, 6> BeckoningSignedAreaComparison{0x0F, 0x8F, 0x90, 0x00, 0x00, 0x00};
constexpr u64 BeckoningAreaFlagResultOffset = 0x0157F960;
constexpr std::array<u8, 2> BeckoningAreaFlagResult{0x34, 0x01};
constexpr std::array<u8, 2> BeckoningAllowedAreaFlagResult{0x31, 0xC0};
constexpr u64 ResponderBellAreaResultOffset = 0x0157F6D1;
constexpr std::array<u8, 2> ResponderBellAreaResult{0x88, 0xC3};
constexpr std::array<u8, 2> ResponderBellAllowedAreaResult{0xB1, 0x01};
constexpr u64 ResponderBellCommonResultOffset = 0x0157F6D3;
constexpr std::array<u8, 8> ResponderBellCommonResult{0x4C, 0x89, 0xF7, 0xE8,
                                                      0xF5, 0x9F, 0x34, 0x00};
constexpr std::array<u8, 8> ResponderBellAllowedCommonResult{0xE9, 0xE9, 0x09, 0x00,
                                                             0x00, 0x90, 0x90, 0x90};
constexpr u64 ActiveBellAreaComparisonOffset = 0x015068BB;
constexpr std::array<u8, 2> ActiveBellAreaComparison{0x77, 0x4A};
constexpr std::array<u8, 2> ActiveBellSignedAreaComparison{0x7F, 0x4A};
constexpr u64 ActiveBellAreaFlagResultOffset = 0x015068FC;
constexpr std::array<u8, 5> ActiveBellAreaFlagResult{0x88, 0xC3, 0x80, 0xF3, 0x01};
constexpr std::array<u8, 5> ActiveBellAllowedAreaFlagResult{0xB3, 0x01, 0x90, 0x90, 0x90};
constexpr u64 ResponderSearchAreaRangeResultOffset = 0x0191A8C3;
constexpr std::array<u8, 6> ResponderSearchAreaRangeResult{0x18, 0xC9, 0x20, 0xC1, 0xEB, 0x02};
constexpr std::array<u8, 6> ResponderSearchSignedAreaRangeResult{0x7D, 0x04, 0x88,
                                                                 0xC1, 0xEB, 0x02};
constexpr u64 SosStatusAreaRestrictionOffset = 0x018700D3;
constexpr std::array<u8, 16> SosStatusAreaRestriction{
    0x84, 0xC0, 0x41, 0xBD, 0xFF, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x44, 0xEB, 0x41, 0xC1, 0xED, 0x1F,
};
constexpr std::array<u8, 16> SosStatusAllowedArea{
    0x45, 0x31, 0xED, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};
constexpr u64 SummonCandidateAreaRestrictionOffset = 0x014B714A;
constexpr std::array<u8, 6> SummonCandidateAreaRestriction{0x0F, 0x85, 0xC0, 0x03, 0x00, 0x00};
constexpr std::array<u8, 6> SummonCandidateAllowedCrossMap{0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr u64 SummonBuildAreaRestrictionOffset = 0x01874B79;
constexpr std::array<u8, 2> SummonBuildAreaRestriction{0x74, 0x0D};
constexpr std::array<u8, 2> SummonBuildAllowedCrossMap{0xEB, 0x0D};
constexpr u64 SummonBuildWorldStateRestrictionOffset = 0x018749E8;
constexpr std::array<u8, 6> SummonBuildWorldStateRestriction{
    0x0F, 0x85, 0x48, 0x01, 0x00, 0x00,
};
constexpr std::array<u8, 6> SummonBuildAllowedWorldState{0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr u64 SummonBuildNegativeEventRestrictionOffset = 0x018749F0;
constexpr std::array<u8, 6> SummonBuildNegativeEventRestriction{
    0x0F, 0x88, 0x40, 0x01, 0x00, 0x00,
};
constexpr std::array<u8, 6> SummonBuildAllowedNegativeEvent{0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr u64 StageTransitionStopMatchingCallOffset = 0x019471B1;
constexpr std::array<u8, 5> StageTransitionStopMatchingCall{0xE8, 0xEA, 0x95, 0x58, 0x00};
constexpr std::array<u8, 5> StageTransitionKeepMatchingCall{0xE8, 0xEA, 0x8E, 0x58, 0x00};
constexpr u64 SummonBuildEntryOffset = 0x01874710;
constexpr u64 SosStatusUpdateOffset = 0x01872360;
constexpr u64 StageWarpDescriptorFinalizeOffset = 0x01944F23;
constexpr u64 MultiPlayStageUidPolicyOffset = 0x0193C5DE;
constexpr u64 MultiPlayStopRequestOffset = 0x01ED07A0;
constexpr u64 MatchingStopTaskEnqueueOffset = 0x00C98070;
// WorldChrMan remote-player cleanup. Called right after CSMultiPlayMan::Stop by 26 session-ending
// paths (stage transition, boss clear, revive, join aborts, OnMatchingCheck). Observed read-only.
constexpr u64 WorldChrManRemoteCleanupOffset = 0x015BED10;
constexpr u64 CrossMapGuestHandoffOffset = 0x01E4FF3A;
constexpr u64 HealingFountainAvailabilityOffset = 0x012F836E;
constexpr u64 CharacterWarpSetOrCopyFloorOffset = 0x018BBA70;
constexpr auto BackgroundFloorCopyReturnOffsets = std::to_array<u64>({0x018BEE67, 0x018BEF34});
constexpr u64 OnlineStatePointerOffset = 0x056C7048;
constexpr u64 SetForcedSummonMapOffset = 0x0156CF10;
constexpr u64 SetForcedSummonPositionOffset = 0x0156CF20;
constexpr u64 SetForcedSummonOrientationOffset = 0x0156CF40;
constexpr u64 SetForcedSummonWarpOffset = 0x0156CF60;
constexpr u64 SelectSummonedPlacementOffset = 0x01332BC0;
constexpr u64 SummonedMapReloadOffset = 0x01336B90;
constexpr u64 StageTransitionOffset = 0x013CDE30;
constexpr u64 SetSummonReloadStateOffset = 0x0178D9A0;
constexpr u64 UseItemNativeApplyOffset = 0x018F9720;
constexpr s32 SmallResonantBellGoodsId = 205;
constexpr s32 SmallResonantBellEffectId = 9005;
constexpr s32 BellUseArgument = 17;
constexpr u32 ResponderResumeStableObservations = 20;
constexpr u32 ResponderResumeMaxAttempts = 2;
constexpr auto ResponderResumeRetryDelay = std::chrono::seconds{15};

constexpr auto CrossMapNativeCalls = std::to_array<NativeCallSignature>({
    {"SetForcedSummonMap",
     SetForcedSummonMapOffset,
     {0x48, 0x8B, 0x05, 0x61, 0x97, 0xFE, 0x03, 0x8B, 0x0F, 0x89, 0x88, 0xF0, 0x14, 0x00, 0x00,
      0xC3},
     16},
    {"SetForcedSummonPosition",
     SetForcedSummonPositionOffset,
     {0x48, 0x8B, 0x05, 0x51, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80,
      0x00},
     16},
    {"SetForcedSummonOrientation",
     SetForcedSummonOrientationOffset,
     {0x48, 0x8B, 0x05, 0x31, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80,
      0x10},
     16},
    {"SetForcedSummonWarp",
     SetForcedSummonWarpOffset,
     {0x48, 0x8B, 0x05, 0x11, 0x97, 0xFE, 0x03, 0xC6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x01, 0xC3},
     15},
    {"SelectSummonedPlacement",
     SelectSummonedPlacementOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18},
     13},
    {"SummonedMapReload",
     SummonedMapReloadOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x05, 0x2D, 0x45, 0x20, 0x04, 0x48, 0x83, 0x38, 0x00},
     15},
    {"StageTransition",
     StageTransitionOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
     14},
    {"SetSummonReloadState",
     SetSummonReloadStateOffset,
     {0xC7, 0x87, 0x84, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xC3},
     11},
    {"UseItemNativeApply",
     UseItemNativeApplyOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
      0xEC},
     16},
});

constexpr auto Sites = std::to_array<TraceSite>({
    {"IsValidSos.Update", 0x01308DC0, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"IsValidSos.NativeValidityDispatch",
     0x01308DF9,
     TraceKind::SosValidityDispatch,
     {0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF},
     6},
    {"IsValidSos.ValidityCode",
     0x01308E20,
     TraceKind::SosValidityCode,
     {0x49, 0x63, 0xCF, 0x48, 0x69, 0xC9, 0x67, 0x66, 0x66, 0x66},
     10},
    {"IsValidSos.Condition", 0x013091B0, TraceKind::SosCondition, StandardR14Prologue, 6},
    {"Condition.SelfSos", 0x0130B030, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"Condition.SOS", 0x0130C590, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"Condition.SosMenu", 0x0130CDE0, TraceKind::SosCondition, StandardR15Prologue, 6},

    {"OnEvent.CallSOS.Dispatch",
     0x01389A3B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00},
     10},
    {"OnEvent.CallBlackSOS.Dispatch",
     0x01389A6B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x02, 0x00, 0x00, 0x00},
     10},
    {"OnEvent.CallDragonewtSOS.Dispatch",
     0x01389A9B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x03, 0x00, 0x00, 0x00},
     10},
    {"Call.WhiteSos.Dispatch",
     0x01389AE2,
     TraceKind::SosEvent,
     {0x31, 0xF6, 0xBA, 0xEA, 0x22, 0x02, 0x00},
     7},
    {"Call.BlackSos.Dispatch",
     0x01389B22,
     TraceKind::SosEvent,
     {0x31, 0xF6, 0xBA, 0xEB, 0x22, 0x02, 0x00},
     7},

    {"OnMatchingCheck", 0x013808B0, TraceKind::MatchingEvent, StandardR15Prologue, 6},
    {"OnMatchingError", 0x013809F0, TraceKind::MatchingEvent, StandardR15Prologue, 6},
    {"OnSummonResult.Empty", 0x01384500, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.Move", 0x01384510, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.TimeOut", 0x01384520, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.OtherError", 0x01384530, TraceKind::MatchingEvent, StandardRbxPrologue, 5},
    {"ForceSummonFail", 0x01384990, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"ForceSummonSuccess", 0x013849A0, TraceKind::MatchingEvent, StandardR15Prologue, 6},

    {"CSMultiPlayMan.StageUidChanged.Policy",
     MultiPlayStageUidPolicyOffset,
     TraceKind::MatchingLeaveDecision,
     {0x8B, 0x87, 0x24, 0x01, 0x00, 0x00},
     6},
    {"CSMultiPlayMan.StopRequest", MultiPlayStopRequestOffset, TraceKind::MatchingLeaveDecision,
     StandardR15Prologue, 6},
    {"Matching2.StopTask.Enqueue", MatchingStopTaskEnqueueOffset, TraceKind::MatchingLeaveDecision,
     StandardR15Prologue, 6},
    {"WorldChrMan.RemoteCleanup", WorldChrManRemoteCleanupOffset,
     TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    // Return-only target of BBSC persistence (bbsc_persistence.cpp): each hit is one skipped
    // remote cleanup, and caller_offset names the session-ending path that was kept alive.
    {"BBSC.Persist.SkippedCleanup", 0x013001E0, TraceKind::MatchingLeaveDecision,
     ReturnAndPadding, 16},
    // BBSC: Dantelion2 panic handler (DL_PANIC). Records the guest call stack so the caller of
    // e.g. "SprjLuaEventMan: accessed uninitialized singleton" can be named.
    {"Engine.Panic", 0x020B55B0, TraceKind::EnginePanic, StandardR15Prologue, 6},
    // BBSC player-type research (logged with caller): who resets a respawned guest's type.
    // 0x0178D9C0 summon reload phase 3 -> 4 (calls ApplySummonType), 0x015C9850 ApplySummonType
    // (type from global+0x16F0), 0x01941C20 stage step that restores own world / original type.
    // Engine Lua event dispatch 0x01339870(lua_state, name): names every session event
    // (OnBeKickOut, OnRoomTimeOut, OnIrregularLeaveSession, OnJoinMutiplay, ...).
    {"BBSC.LuaEvent", 0x01339870, TraceKind::LuaSessionEvent, StandardR15Prologue, 6},
    // CSMultiPlayMan::RemoveMember(member_id): sets the exit flag on that member's insert task.
    // Traced with caller chain to find what drops the guest on the host when the host dies.
    {"BBSC.RemoveMember", 0x01E54DB0, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"BBSC.Type.ReloadFinish", 0x0178D9C0, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"BBSC.Type.ApplySummonType", 0x015C9850, TraceKind::MatchingLeaveDecision, StandardR14Prologue,
     6},
    {"BBSC.Type.OwnWorldStep", 0x01941C20, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"Matching2.LeaveRoom.Begin",
     0x00CC5390,
     TraceKind::MatchingLeaveDecision,
     {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50},
     6},

    // Map items for a guest (2026-09-28, "no item" in the host's world): the treasure setup of a
    // map load, the item spawn (SpawnOwn patch) and the create (CreateOwn patch).
    {"BBSC.Items.TreasureSetup", 0x017D2F30, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"BBSC.Items.Spawn", 0x017D8370, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"BBSC.Items.Create", 0x017DE870, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    // 2026-09-29: Spawn and Create never ran in a whole session; map treasures are placed by these.
    {"BBSC.Items.PlaceTreasure", 0x017DC300, TraceKind::MatchingLeaveDecision, StandardR15Prologue,
     6},
    {"BBSC.Items.NumberKind1", 0x017D4C40, TraceKind::MatchingLeaveDecision, StandardR15Prologue, 6},
    {"CSRequestSummon.Init", 0x014B47C0, TraceKind::SummonRequest, StandardR15Prologue, 6},
    {"CSRequestSummon.Update", 0x014B4890, TraceKind::SummonRequest, StandardRbxPrologue, 5},
    {"CSRequestSummon.CheckWait", 0x014B48C0, TraceKind::SummonRequest, StandardR15Prologue, 6},
    {"CSRequestSummon.CheckComplete",
     0x014B4AE0,
     TraceKind::SummonRequest,
     {0xC7, 0x47, 0x54, 0xFF, 0xFF, 0xFF, 0xFF},
     7},

    {"SummonCandidate.Entry", 0x014BA980, TraceKind::SummonCandidate, StandardR15Prologue, 6},
    {"SummonCandidate.Accepted",
     0x014BAA5C,
     TraceKind::SummonCandidate,
     {0x48, 0x8B, 0x07, 0xBE, 0xF8, 0x00, 0x00, 0x00},
     8},
    {"SummonCandidate.Submit",
     0x014BABCB,
     TraceKind::SummonCandidate,
     {0x48, 0x8B, 0xB3, 0xE8, 0x01, 0x00, 0x00},
     7},
    {"SummonCandidate.FilterCycle",
     0x014B6F09,
     TraceKind::SummonCandidate,
     {0x41, 0x83, 0xFF, 0xFF, 0x4C, 0x89, 0xA4, 0x24, 0x88, 0x00, 0x00, 0x00},
     12},
    {"SummonCandidate.ManagerScan",
     0x014B6F5D,
     TraceKind::SummonCandidate,
     {0x49, 0x8D, 0x7C, 0x24, 0x20},
     5},
    {"SummonCandidate.LocalIdentityPassed",
     0x014B6F9D,
     TraceKind::SummonCandidate,
     {0x4C, 0x8B, 0x28, 0x4D, 0x85, 0xED},
     6},
    {"SummonCandidate.ManagerIdentityStatePassed",
     0x014B6FE1,
     TraceKind::SummonCandidate,
     {0x48, 0x89, 0xCB, 0x4D, 0x89, 0xC6},
     6},
    {"SummonCandidate.ManagerIdentityPassed",
     0x014B7070,
     TraceKind::SummonCandidate,
     {0x41, 0x83, 0xBD, 0xC0, 0x0A, 0x00, 0x00, 0x00},
     8},
    {"SummonCandidate.SessionGatePassed",
     0x014B70FF,
     TraceKind::SummonCandidate,
     {0x41, 0xF6, 0x85, 0x14, 0x0B, 0x00, 0x00, 0x02},
     8},
    {"SummonCandidate.AreaGatePassed",
     0x014B7150,
     TraceKind::SummonCandidate,
     {0x48, 0x89, 0xDF, 0x48, 0x8B, 0x84, 0x24, 0xA8, 0x00, 0x00, 0x00},
     11},
    {"SummonCandidate.NoPendingDuplicate",
     0x014B71B2,
     TraceKind::SummonCandidate,
     {0x41, 0x0F, 0xB6, 0x84, 0x24, 0xED, 0x00, 0x00, 0x00},
     9},
    {"SummonCandidate.FreshnessPassed",
     0x014B71F4,
     TraceKind::SummonCandidate,
     {0x48, 0x8B, 0x38, 0x48, 0x8B, 0x07},
     6},
    {"SummonCandidate.Inserted",
     0x014B8E8D,
     TraceKind::SummonCandidate,
     {0x48, 0x8B, 0xB2, 0xE8, 0x01, 0x00, 0x00},
     7},
    {"SummonCandidate.PendingInserted",
     0x014B750B,
     TraceKind::SummonCandidate,
     {0x0F, 0x1F, 0x44, 0x00, 0x00},
     5},
    {"SummonCandidate.ObserverResult",
     0x014C0238,
     TraceKind::SummonCandidate,
     {0x45, 0x31, 0xFF, 0x84, 0xC0, 0x4D, 0x0F, 0x45, 0xFC},
     9},

    {"SummonSelection.Scan",
     0x01873269,
     TraceKind::SummonSelection,
     {0x48, 0x8B, 0x80, 0xA8, 0x01, 0x00, 0x00},
     7},
    {"SummonSelection.Matched",
     0x018732DD,
     TraceKind::SummonSelection,
     {0xC6, 0x46, 0x10, 0x01, 0x4C, 0x8B, 0x76, 0x08},
     8},
    {"SummonBuild.Entry", SummonBuildEntryOffset, TraceKind::SummonBuild, StandardR15Prologue, 6},
    {"SummonBuild.RoleDelayPassed",
     0x0187477A,
     TraceKind::SummonBuild,
     {0x49, 0x83, 0x7D, 0x00, 0x00},
     5},
    {"SummonBuild.SessionStatePassed",
     0x0187481D,
     TraceKind::SummonBuild,
     {0x48, 0x8B, 0x18, 0x48, 0x85, 0xDB},
     6},
    {"SummonBuild.ControlAndWorldStatePassed",
     0x01874B8F,
     TraceKind::SummonBuild,
     {0x48, 0x8B, 0x90, 0xF8, 0x16, 0x00, 0x00},
     7},
    {"SummonBuild.GlobalCapacityPassed",
     0x01874BC7,
     TraceKind::SummonBuild,
     {0x4D, 0x89, 0xC7, 0x49, 0xC1, 0xE3, 0x07},
     7},
    {"SummonBuild.RoleCapacityPassed",
     0x01874C94,
     TraceKind::SummonBuild,
     {0x43, 0x8B, 0x04, 0x1E, 0x83, 0xF8, 0x02},
     7},
    {"SummonBuild.RoleRules",
     0x01874E5A,
     TraceKind::SummonBuild,
     {0x49, 0x89, 0xCD, 0x4F, 0x8D, 0x7C, 0x1E, 0x14},
     8},
    {"SummonBuild.Return",
     0x018750F7,
     TraceKind::SummonBuild,
     {0x49, 0x8B, 0x06, 0x48, 0x3B, 0x45, 0xD0},
     7},
    {"SummonSelection.QueueInsert", 0x01875320, TraceKind::SummonSelection, StandardR15Prologue, 6},
    {"SummonSelection.RequestQueued",
     0x01873605,
     TraceKind::SummonSelection,
     {0x48, 0x8B, 0x83, 0xC0, 0x01, 0x00, 0x00},
     7},
    {"SummonSelection.RequestStart", 0x014BAEC0, TraceKind::SummonSelection, StandardR15Prologue,
     6},
    {"SummonSelection.HandleAllocated",
     0x014BAFE3,
     TraceKind::SummonSelection,
     {0x49, 0x89, 0x84, 0x24, 0x38, 0x01, 0x00, 0x00},
     8},

    {"CSRequestGetSos.Init", 0x01E5FAF0, TraceKind::GetSosRequest, StandardR15Prologue, 6},
    {"CSRequestGetSos.UpdateSeamless", 0x01E5FBF0, TraceKind::GetSosRequest, StandardR14Prologue,
     6},
    {"CSRequestGetSos.BuildRequest", 0x01E5FCE0, TraceKind::GetSosRequest, StandardR15Prologue, 6},
    {"CSRequestGetSos.SelectedArea",
     0x01E60270,
     TraceKind::GetSosArea,
     {0x8B, 0x02, 0x89, 0x85, 0xE8, 0xFE, 0xFF, 0xFF},
     8},
    {"CSRequestGetSos.InsertType", 0x01E5F970, TraceKind::GetSosInsert, StandardR15Prologue, 6},
    {"SosStatus.Update", SosStatusUpdateOffset, TraceKind::SosStatus, StandardR15Prologue, 6},
    {"SosStatus.NativeResult",
     0x0187239E,
     TraceKind::SosStatus,
     {0x83, 0xBD, 0x64, 0xFF, 0xFF, 0xFF, 0x00},
     7},

    {"Messenger.SosSign.Init", 0x01CC9460, TraceKind::SosMessenger, StandardRbxPrologue, 5},
    {"Messenger.SosSign.Wait", 0x01CC94B0, TraceKind::SosMessenger, StandardRbxPrologue, 5},
    {"Messenger.SosSign.Finish", 0x01CC94E0, TraceKind::SosMessenger, StandardR15Prologue, 6},

    {"ChrAction.Flags.Update",
     0x01A0F910,
     TraceKind::ActionFlags,
     {0x48, 0x8B, 0x47, 0x10, 0x48, 0x8B, 0x4F, 0x18},
     8},
    {"ChrAction.UseItem.Pressed",
     0x018F751C,
     TraceKind::UseItem,
     {0x49, 0x8B, 0x45, 0x00, 0x4C, 0x89, 0xEF},
     7},
    {"ChrAction.UseItem.Inventory",
     0x018F7529,
     TraceKind::UseItem,
     {0x48, 0x8D, 0xB8, 0xD0, 0x01, 0x00, 0x00},
     7},
    {"ChrAction.UseItem.ResolvedGoods",
     0x018F7535,
     TraceKind::UseItem,
     {0x41, 0x8B, 0x8D, 0x7C, 0x04, 0x00, 0x00},
     7},
    {"ChrAction.UseItem.NativeApply", 0x018F9720, TraceKind::UseItemExecution, StandardR15Prologue,
     6},
    {"ChrAction.UseItem.GoodsExecutor", 0x018C9160, TraceKind::UseItemExecution,
     StandardR15Prologue, 6},
    {"ChrAction.UseItem.GoodsExecutorResult",
     0x018C9364,
     TraceKind::UseItemExecution,
     {0x48, 0x8B, 0x03, 0x48, 0x3B, 0x45, 0xD0},
     7},
    {"ChrAction.UseItem.ActionSubmit", 0x01FF0FA0, TraceKind::GoodsActionSubmit,
     StandardR15Prologue, 6},
    {"GameParam.Goods.Lookup", 0x01F1E3D0, TraceKind::GoodsParamLookup, StandardR15Prologue, 6},
    {"Beckoning.Availability.Area",
     0x0157F8BA,
     TraceKind::BellAvailability,
     {0x84, 0xC0, 0x48, 0x0F, 0x45, 0xD9},
     6},
    {"Beckoning.Availability.NativeResult",
     0x0157F962,
     TraceKind::BellAvailability,
     {0x89, 0x85, 0x10, 0xFE, 0xFF, 0xFF},
     6},
    {"ResponderBell.Availability.Entry",
     0x0157F686,
     TraceKind::ResponderBellAvailability,
     {0x41, 0x81, 0xFC, 0xCD, 0x00, 0x00, 0x00},
     7},
    {"ResponderBell.Availability.FinalResult",
     0x015800D1,
     TraceKind::ResponderBellAvailability,
     {0x88, 0xC8, 0x48, 0x81, 0xC4, 0xE8, 0x01, 0x00, 0x00},
     9},
    {"Network.Area.RegionResult",
     0x013DEAC8,
     TraceKind::NetworkAreaRegion,
     {0x45, 0x89, 0x7E, 0x44, 0xB0, 0x01},
     6},

    {"CSMultiPlayerIns.Init", 0x01E4FCF0, TraceKind::MultiplayerInsert, StandardRbxPrologue, 5},
    {"CSMultiPlayerIns.StartNotifyWait", 0x01E4FD50, TraceKind::MultiplayerInsert,
     StandardR15Prologue, 6},
    {"CSMultiPlayerIns.StartWait", 0x01E4FF70, TraceKind::MultiplayerInsert, StandardR14Prologue,
     6},
    {"CSMultiPlayerIns.FirstSyncWait", 0x01E50090, TraceKind::MultiplayerInsert,
     StandardR15Prologue, 6},
    {"CSMultiPlayerIns.Create", 0x01E505E0, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.CreateWait", 0x01E50830, TraceKind::MultiplayerInsert, StandardRbxPrologue,
     5},
    {"CSMultiPlayerIns.Update",
     0x01E50860,
     TraceKind::MultiplayerInsert,
     {0x83, 0xBF, 0xE8, 0x00, 0x00, 0x00, 0x00},
     7},
    {"CSMultiPlayerIns.WaitExit", 0x01E50880, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.Delete", 0x01E50A70, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.DeleteWait",
     0x01E50D70,
     TraceKind::MultiplayerInsert,
     {0x8B, 0x87, 0xE8, 0x00, 0x00, 0x00, 0x00},
     6},
    {"CSMultiPlayerIns.PlacementCopy",
     0x01E4FF1E,
     TraceKind::GlobalState,
     {0x89, 0x88, 0xC4, 0x14, 0x00, 0x00},
     6},
    {"CSMultiPlayerIns.CrossMapGuestHandoff",
     CrossMapGuestHandoffOffset,
     TraceKind::CrossMapGuestHandoff,
     {0x48, 0x8D, 0x83, 0xED, 0x00, 0x00, 0x00},
     7},

    {"SetSosSignWarp",
     0x0132E267,
     TraceKind::GlobalState,
     {0xC6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x01},
     7},
    {"SetSummonedPos",
     0x01332BB7,
     TraceKind::GlobalState,
     {0xC6, 0x40, 0x20, 0x01, 0xB0, 0x01, 0x00},
     6},
    {"SetSosSignPos.NativeCopy",
     0x01333C43,
     TraceKind::SosPlacement,
     {0x89, 0x88, 0xC0, 0x14, 0x00, 0x00},
     6},
    {"SummonedPlacement.SelectMap",
     0x01332C1B,
     TraceKind::PlacementSelect,
     {0x80, 0xB9, 0x20, 0x15, 0x00, 0x00, 0x00},
     7},
    {"SummonedPlacement.SelectedMap",
     0x01332CD0,
     TraceKind::PlacementSelect,
     {0x89, 0x41, 0x0C, 0x48, 0x8B, 0x03},
     6},
    {"SummonedMapReload.Native", 0x0131E5C0, TraceKind::MapReload, StandardR15Prologue, 6},
    {"SummonPoint.Resolve", 0x01582E70, TraceKind::SummonPoint, StandardR15Prologue, 6},
    {"HealingFountain.Register.Native", 0x0133B030, TraceKind::HealingFountainRegistration,
     StandardR15Prologue, 6},
    {"HealingFountain.Availability.Host",
     HealingFountainAvailabilityOffset,
     TraceKind::HealingFountainAvailability,
     {0x41, 0x80, 0x7D, 0x48, 0x00},
     5},
    {"ChairMessenger.RespawnPointNotify.Init", 0x01E5C6C0, TraceKind::ChairRespawnNotification,
     StandardR15Prologue, 6},
    {"ChairMessenger.RespawnPointNotify.Update", 0x01E5C890, TraceKind::ChairRespawnNotification,
     StandardR15Prologue, 6},

    {"ReceivedSummonPlacement.ConditionalSet",
     0x0156E6E7,
     TraceKind::ReceivedPlacementConditional,
     {0x41, 0x80, 0xB8, 0x48, 0x16, 0x00, 0x00, 0x00},
     8},
    {"ReceivedSummonPlacement.Set",
     0x0156E7A7,
     TraceKind::ReceivedPlacementSet,
     {0x8B, 0x12, 0x8B, 0x09, 0xC5, 0xF8, 0x28, 0x07},
     8},

    {"CharacterWarp.Area", 0x013CB3D0, TraceKind::CharacterWarp, StandardR15Prologue, 6},
    {"CharacterWarp.Object", 0x013CB870, TraceKind::CharacterWarp, StandardR15Prologue, 6},
    {"CharacterWarp.Character", 0x013CCBD0, TraceKind::CharacterWarp, StandardR15Prologue, 6},
    {"CharacterWarp.SetOrCopyFloor", 0x018BBA70, TraceKind::CharacterWarp, StandardR15Prologue, 6},

    {"Event.WarpNextStage",
     0x0132E010,
     TraceKind::StageWarp,
     {0x55, 0x48, 0x89, 0xE5, 0xC1, 0xE6},
     6},
    {"Event.WarpNextStage_Bonfire",
     0x0132E050,
     TraceKind::StageWarp,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x63, 0xC6},
     7},
    {"Warp.RespawnPointParam", 0x013CDF30, TraceKind::RespawnPointWarp, StandardR15Prologue, 6},
    {"Warp.StageTransition", 0x013CDE30, TraceKind::StageWarp, StandardR15Prologue, 6},
    {"Warp.StageDescriptor",
     0x01944D76,
     TraceKind::StageWarpDescriptor,
     {0x8B, 0x88, 0x3C, 0x15, 0x00, 0x00},
     6},
    {"Warp.StageDescriptor.Finalize",
     StageWarpDescriptorFinalizeOffset,
     TraceKind::StageWarpDescriptor,
     {0xB8, 0x00, 0x00, 0x00, 0xFF},
     5},
    {"Warp.StageTransition.AcknowledgeCheck",
     0x0194100A,
     TraceKind::StageWarpAcknowledge,
     {0x80, 0xBD, 0x68, 0xFE, 0xFF, 0xFF, 0x00},
     7},
    {"Warp.RespawnPoint.Resolve",
     0x01938F65,
     TraceKind::StageWarpRespawnResolve,
     {0x41, 0x8B, 0x85, 0xA4, 0x00, 0x00, 0x00},
     7},
    {"Warp.RespawnTransform.Resolve",
     0x0154AFC0,
     TraceKind::StageWarpTransformResolve,
     {0x55, 0x53, 0x83, 0xF9, 0xFF, 0x0F, 0x84, 0x9D, 0x00, 0x00, 0x00},
     11},
    {"Warp.RespawnPlacement.Apply", 0x01939C80, TraceKind::StageWarpPlacement, StandardR14Prologue,
     6},

    {"PlayerWarp.Dispatch.RemoteWarpPlayer.Result", 0x01FB2BE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteResetPlayer.Result", 0x01FB2C60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.GetPlayerInfo.Result", 0x01FB2CE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpWorldPos.Result", 0x01FB2D60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpMapStudioPos.Result", 0x01FB2DE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpPlayer.Void", 0x01FB2E60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteResetPlayer.Void", 0x01FB2EE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.GetPlayerInfo.Void", 0x01FB2F60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpWorldPos.Void", 0x01FB2FE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpMapStudioPos.Void", 0x01FB3060, TraceKind::WarpCommand,
     StandardR15Prologue, 6},

    {"PlayerWarp.Executor.RemoteWarpPlayer", 0x0154D180, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.RemoteResetPlayer", 0x0154D5E0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.GetPlayerInfo", 0x0154D7A0, TraceKind::WarpCommand, StandardR15Prologue,
     6},
    {"PlayerWarp.Executor.RemoteWarpWorldPos", 0x0154DC50, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.RemoteWarpMapStudioPos", 0x0154E0B0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.NativeDispatch", 0x0154EA30, TraceKind::WarpResolved, StandardR15Prologue, 6},
    {"PlayerWarp.LocalApply", 0x0154B110, TraceKind::WarpLocalApply, StandardR15Prologue, 6},

    {"WorldStateValidation.018C5360.Result",
     0x018C5365,
     TraceKind::WorldStateValidation,
     {0xBE, 0xFF, 0xFF, 0xFF, 0xFF},
     5},
    {"WorldStateValidation.018CF975.Result",
     0x018CF97A,
     TraceKind::WorldStateValidation,
     {0x41, 0xBE, 0xFF, 0xFF, 0xFF, 0xFF},
     6},
    {"WorldStateValidation.0191A968.Return",
     0x0191A980,
     TraceKind::WorldStateValidation,
     {0x48, 0x83, 0xC4, 0x18, 0x5B, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3},
     11},
    {"WorldStateValidation.0191AA4C.Return",
     0x0191AA71,
     TraceKind::WorldStateValidation,
     {0x48, 0x83, 0xC4, 0x18, 0x5B, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3},
     11},
    {"WorldStateValidation.01AC6543.Return",
     0x01AC6557,
     TraceKind::WorldStateValidation,
     {0x48, 0x83, 0xC4, 0x18, 0x5B, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3},
     11},
    {"WorldStateValidation.01AC65F4.Return",
     0x01AC661E,
     TraceKind::WorldStateValidation,
     {0x48, 0x83, 0xC4, 0x18, 0x5B, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3},
     11},
    {"WorldStateValidation.01FEB60C.Result",
     0x01FEB611,
     TraceKind::WorldStateValidation,
     {0x44, 0x89, 0xE1, 0x41, 0x88, 0xC4},
     6},
    {"WorldStateValidation.01FEBCFC.Result",
     0x01FEBD01,
     TraceKind::WorldStateValidation,
     {0x44, 0x89, 0xE1, 0x41, 0x88, 0xC4},
     6},
    {"WorldStateValidation.01FEC37C.Result",
     0x01FEC381,
     TraceKind::WorldStateValidation,
     {0x44, 0x89, 0xE1, 0x41, 0x88, 0xC4},
     6},
});

std::array<std::atomic<u64>, Sites.size()> site_hits{};
std::atomic<u64> event_sequence{};
std::atomic<s32> observed_sos_area{-1};
std::mutex capture_mutex;
std::ofstream capture_file;
std::filesystem::path capture_path;
uintptr_t image_base{};
bool installed{};
bool negative_area_patch_installed{};
bool area_flag_patch_installed{};
bool responder_bell_area_patch_installed{};
bool responder_bell_common_patch_installed{};
bool active_bell_negative_area_patch_installed{};
bool active_bell_area_flag_patch_installed{};
bool responder_search_negative_area_patch_installed{};
bool sos_status_area_patch_installed{};
bool summon_candidate_area_patch_installed{};
bool summon_build_area_patch_installed{};
bool summon_build_world_state_patch_installed{};
bool summon_build_negative_event_patch_installed{};
bool stage_transition_keep_matching_patch_installed{};
bool summon_build_host_placement_hook_installed{};
bool cross_map_guest_handoff_hook_installed{};
bool summon_reload_state_hook_installed{};
bool deferred_summon_reload_hook_installed{};
bool healing_fountain_host_availability_hook_installed{};
thread_local u64 responder_availability_frame{};
thread_local s32 responder_availability_goods{-1};

struct SummonPlacementDescriptor {
    u32 packed_region{};
    float x{};
    float y{};
    float z{};
    float heading{};
    s32 area{};
};
static_assert(sizeof(SummonPlacementDescriptor) == 0x18);

struct HostPlacementRewriteRecord {
    std::string_view result{"not_attempted"};
    u64 prepared{};
    SummonPlacementDescriptor candidate{};
    SummonPlacementDescriptor host{};
};

thread_local HostPlacementRewriteRecord host_placement_rewrite{};
std::mutex seamless_placement_mutex;
std::optional<SummonPlacementDescriptor> seamless_host_placement;
std::optional<SummonPlacementDescriptor> seamless_received_host_placement;
bool seamless_received_host_placement_consumed{};
// BBSC connect-first: host placement from the prepare reply, kept until the join load uses it.
std::optional<SummonPlacementDescriptor> bbsc_connect_first_placement;
// Set when a connect-first join has loaded the guest into the host's map; TickBbscJoinTeleport then
// adds the join warp's second, real reload (see BbscWarpJoinEnabled).
std::atomic<bool> bbsc_cross_map_join{false};
bool BbscConnectFirstEnabled();
bool BbscConnectFirstPlacementFresh(); // prepare reply received within the timeout
std::atomic<u32> pending_summon_reload_map{};
std::atomic<u32> pending_summon_reload_state_map{};
// BBSC rejoin: when set, the next descriptor-finalize reload skips its placement refresh.
std::atomic<bool> bbsc_rejoin_suppress_placement_refresh{false};
// BBSC rejoin: map this process was inserted into when it last joined a host as a summoned client.
std::atomic<u32> bbsc_joined_host_map{0};

struct GuestPlacementHandoffRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 state{};
    u32 current_map{};
    u32 received_map{};
    u32 selected_map{};
    bool transported_host{};
    bool placement_refreshed{};
    bool select_result{};
    bool summon_reload_armed{};
    s32 transition_result{};
};

thread_local GuestPlacementHandoffRecord guest_placement_handoff{};

struct SummonReloadStateRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 descriptor_map{};
    u64 reload_state{};
    s32 state_before{};
    s32 state_after{};
    bool summon_reload_started{};
    bool state_setter_used{};
    bool placement_refreshed{};
};

thread_local SummonReloadStateRecord summon_reload_state{};

struct DeferredSummonReloadRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    bool summon_reload_started{};
};

thread_local DeferredSummonReloadRecord deferred_summon_reload{};

struct PreMatchGuestWarpRecord {
    std::string_view result{"not_pending"};
    u64 state{};
    u32 current_map{};
    u32 target_map{};
    u32 selected_map{};
    s32 multi_play_state{-1};
    bool placement_written{};
    bool select_result{};
    bool responder_resume_armed{};
    s32 transition_result{};
};

thread_local PreMatchGuestWarpRecord pre_match_guest_warp{};

struct PendingResponderResume {
    u32 target_map{};
    s32 target_area{};
    u32 ready_observations{};
    u32 attempts{};
    bool dispatched{};
    std::chrono::steady_clock::time_point dispatched_at{};
};

thread_local PendingResponderResume pending_responder_resume{};

struct ResponderResumeRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    s32 target_area{};
    s32 current_area{};
    u64 state{};
    u64 multi_play{};
    s32 multi_play_state{-1};
    u64 matching_controller{};
    u64 player{};
    u32 ready_observations{};
    u32 attempts{};
    bool effect_active{};
    bool native_result{};
};

thread_local ResponderResumeRecord responder_resume{};

struct HealingFountainAvailabilityRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 player{};
    bool blocked_before{};
    bool host_effect{};
    bool blocked_after{};
};

thread_local HealingFountainAvailabilityRecord healing_fountain_availability{};

bool EnvFlagEnabled(const char* name) {
#ifdef BBSC_MOD
    // The mod DLL is the whole of BBSC in a stock emulator: its two master switches are its own
    // (BBSC_MOD_SEAMLESS / BBSC_MOD_HOOKS, on unless "0"), so turning the emulator build's copy off
    // with SHADPS4_BLOODBORNE_SEAMLESS_COOP=0 / SHADPS4_BLOODBORNE_RE_TRACE=0 leaves the mod on.
    const std::string_view key{name};
    if (key == "SHADPS4_BLOODBORNE_SEAMLESS_COOP" || key == "SHADPS4_BLOODBORNE_RE_TRACE") {
        const char* own = std::getenv(key == "SHADPS4_BLOODBORNE_RE_TRACE" ? "BBSC_MOD_HOOKS"
                                                                           : "BBSC_MOD_SEAMLESS");
        return own == nullptr || std::string_view{own} != "0";
    }
#endif
    return Core::Bbsc::SwitchEnabled(name);
}

std::string BytesToHex(std::span<const u8> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const u8 byte : bytes) {
        out << std::setw(2) << static_cast<u32>(byte);
    }
    return out.str();
}

bool HasMemoryAccess(u64 address, size_t size, MemoryProt required) {
    if (size == 0 || address > std::numeric_limits<u64>::max() - size) {
        return false;
    }

    auto* memory = Memory::Instance();
    if (memory == nullptr || !memory->IsValidMapping(address, size)) {
        return false;
    }

    void* mapping_start{};
    void* mapping_end{};
    u32 protection{};
    const u32 required_bits = static_cast<u32>(required);
    return memory->QueryProtection(address, &mapping_start, &mapping_end, &protection) == 0 &&
           (protection & required_bits) == required_bits &&
           address >= reinterpret_cast<u64>(mapping_start) &&
           address + size <= reinterpret_cast<u64>(mapping_end);
}

template <typename T>
T ReadValue(u64 address, size_t offset) {
    T value{};
    if (address > std::numeric_limits<u64>::max() - offset) {
        return value;
    }
    const u64 source = address + offset;
    if (source > std::numeric_limits<u64>::max() - sizeof(value)) {
        return value;
    }

    if (!HasMemoryAccess(source, sizeof(value), MemoryProt::CpuRead)) {
        return value;
    }

    std::memcpy(&value, reinterpret_cast<const void*>(source), sizeof(value));
    return value;
}

template <typename T>
bool WriteValue(u64 address, size_t offset, const T& value) {
    if (address > std::numeric_limits<u64>::max() - offset) {
        return false;
    }
    const u64 destination = address + offset;
    if (!HasMemoryAccess(destination, sizeof(value), MemoryProt::CpuWrite)) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(destination), &value, sizeof(value));
    return true;
}

bool WriteTransportedSummonPlacement(u64 state, const SummonPlacementDescriptor& placement) {
    const std::array<float, 4> position{placement.x, placement.y, placement.z, 1.0F};
    const std::array<float, 4> orientation{0.0F, placement.heading, 0.0F, 0.0F};
    return WriteValue(state, 0x14C4, placement.packed_region) &&
           WriteValue(state, 0x14D0, position) && WriteValue(state, 0x14E0, orientation);
}

bool IsUsablePackedMap(u32 packed_region) {
    return packed_region != 0 && packed_region != std::numeric_limits<u32>::max();
}

bool IsSameSummonPlacement(const SummonPlacementDescriptor& left,
                           const SummonPlacementDescriptor& right) {
    return left.packed_region == right.packed_region &&
           std::bit_cast<u32>(left.x) == std::bit_cast<u32>(right.x) &&
           std::bit_cast<u32>(left.y) == std::bit_cast<u32>(right.y) &&
           std::bit_cast<u32>(left.z) == std::bit_cast<u32>(right.z) &&
           std::bit_cast<u32>(left.heading) == std::bit_cast<u32>(right.heading) &&
           left.area == right.area;
}

u64 GetSummonManagerRoot();
u64 GetGlobalState();
u64 GetMatchingState();

u64 GetLocalPlayer() {
    const u64 local_state =
        image_base != 0 ? ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0) : 0;
    return local_state >= 0x10000 ? ReadValue<u64>(local_state, 0x60) : 0;
}

bool HasPlayerEffect(u64 player, s32 effect_id) {
    const u64 manager = player >= 0x10000 ? ReadValue<u64>(player, 0x1C8) : 0;
    if (manager < 0x10000) {
        return false;
    }

    u64 node = ReadValue<u64>(manager, 0x08);
    for (size_t index = 0; node >= 0x10000 && index < 512; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        const u32 flags = ReadValue<u32>(node, 0x1C);
        const u64 param = ReadValue<u64>(node, 0x48);
        if ((flags & 0x800C0003U) == 0 && param >= 0x10000 &&
            ReadValue<s32>(node, 0x40) == effect_id) {
            return true;
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    return false;
}

u32 GetCurrentPackedMap() {
    const u64 map_list =
        image_base != 0 ? ReadValue<u64>(image_base + CurrentMapListPointerOffset, 0) : 0;
    if (map_list < 0x10000 || !HasMemoryAccess(map_list, 0x24, MemoryProt::CpuRead)) {
        return 0;
    }

    const s32 index = ReadValue<s32>(map_list, 0x20);
    const u64 map_array = ReadValue<u64>(map_list, 0x10);
    if (index < 0 || map_array < 0x10000 ||
        !HasMemoryAccess(map_array, 0x28, MemoryProt::CpuRead)) {
        return 0;
    }

    const s32 count = ReadValue<s32>(map_array, 0x18);
    const u64 entries = ReadValue<u64>(map_array, 0x20);
    if (index >= count || entries < 0x10000) {
        return 0;
    }

    constexpr u64 MapEntryStride = 0xA0;
    const u64 entry_offset = static_cast<u64>(index) * MapEntryStride;
    if (entry_offset > std::numeric_limits<u64>::max() - entries) {
        return 0;
    }
    const u64 entry = entries + entry_offset;
    if (entry > std::numeric_limits<u64>::max() - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u32>(entry, sizeof(u64));
}

bool ReadLocalSummonPlacement(SummonPlacementDescriptor& placement, std::string_view& result) {
    const u64 manager_root = GetSummonManagerRoot();
    if (manager_root < 0x10000 ||
        !HasMemoryAccess(manager_root, 0xA80 + sizeof(s32), MemoryProt::CpuRead)) {
        result = "invalid_manager";
        return false;
    }

    placement.packed_region = GetCurrentPackedMap();
    placement.area = ReadValue<s32>(manager_root, 0xA80);
    if (!IsUsablePackedMap(placement.packed_region)) {
        result = "invalid_map";
        return false;
    }

    const u64 local_state = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 local_world = local_state >= 0x10000 ? ReadValue<u64>(local_state, 0x60) : 0;
    const u64 character = local_world >= 0x10000 ? ReadValue<u64>(local_world, 0x58) : 0;
    const u64 character_state = character >= 0x10000 ? ReadValue<u64>(character, 0x08) : 0;
    const u64 character_model =
        character_state >= 0x10000 ? ReadValue<u64>(character_state, 0x3B0) : 0;
    const u64 transform = character_model >= 0x10000 ? ReadValue<u64>(character_model, 0x68) : 0;
    if (transform < 0x10000 || !HasMemoryAccess(transform, 0x1EC, MemoryProt::CpuRead)) {
        result = "invalid_transform";
        return false;
    }

    placement.heading = ReadValue<float>(transform, 0x1D4);
    placement.x = ReadValue<float>(transform, 0x1E0);
    placement.y = ReadValue<float>(transform, 0x1E4);
    placement.z = ReadValue<float>(transform, 0x1E8);
    if (!std::isfinite(placement.x) || !std::isfinite(placement.y) || !std::isfinite(placement.z) ||
        !std::isfinite(placement.heading)) {
        result = "non_finite_transform";
        return false;
    }
    result = "captured";
    return true;
}

void RefreshSeamlessLocalPlacement() {
    SummonPlacementDescriptor placement{};
    std::string_view result;
    if (!ReadLocalSummonPlacement(placement, result)) {
        return;
    }
    std::scoped_lock lock{seamless_placement_mutex};
    seamless_host_placement = placement;
}

void ApplyCrossMapSummonHostPlacement(const GuestRegisterSnapshot& registers) {
    host_placement_rewrite = {};
    host_placement_rewrite.result = "invalid_prepared";
    host_placement_rewrite.prepared = registers.rsi;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        seamless_host_placement.reset();
    }

    const u64 prepared = registers.rsi;
    if (prepared < 0x10000 ||
        !HasMemoryAccess(prepared, 0x04 + sizeof(SummonPlacementDescriptor), MemoryProt::CpuRead)) {
        return;
    }
    std::memcpy(&host_placement_rewrite.candidate, reinterpret_cast<const void*>(prepared + 0x04),
                sizeof(SummonPlacementDescriptor));

    auto& host = host_placement_rewrite.host;
    if (!IsUsablePackedMap(host_placement_rewrite.candidate.packed_region)) {
        host_placement_rewrite.result = "invalid_map";
        return;
    }
    if (!ReadLocalSummonPlacement(host, host_placement_rewrite.result)) {
        return;
    }
    if (host_placement_rewrite.candidate.packed_region == host.packed_region) {
        host_placement_rewrite.result = "same_map";
        return;
    }

    if (!WriteValue(prepared, 0x04, host)) {
        host_placement_rewrite.result = "write_rejected";
        return;
    }
    {
        std::scoped_lock lock{seamless_placement_mutex};
        seamless_host_placement = host;
    }
    host_placement_rewrite.result = "applied";
}

// The summoned reload spawns the guest from the received placement block (CONFIRMED from every
// correctly placed same-map join capture): state+0x1620 x,y,z,1.0; +0x1630 0,heading,0,0;
// +0x1640 area; +0x1644 map; +0x1648 ready = 1. It is normally filled from the claim delivered in
// an advertisement reply; a connect-first join arrives through signaling first, so the block is
// reset (area/map -1) and the guest spawned at the map origin under the level (runs 2026-09-17).
// Filled at the entry of the native summoned reload 0x0131E5C0, after the game's own reset.
// The kept host placement lives until the summoned reload has finished in the host map
// (BbscConnectFirstReloadFinished), so the fork's forced warp in the handoff never runs meanwhile.
std::optional<SummonPlacementDescriptor> BbscConnectFirstHost() {
    if (!BbscConnectFirstEnabled() || !BbscConnectFirstPlacementFresh()) {
        return std::nullopt;
    }
    std::scoped_lock lock{seamless_placement_mutex};
    return bbsc_connect_first_placement;
}

// Writes the host placement into both blocks the summoned reload reads: the summoned placement
// (state+0x14C4 map, +0x14D0 pos, +0x14E0 rot; WriteTransportedSummonPlacement) and the received
// placement block (+0x1620..+0x1648). Overwrites a block the game filled for another map: in the run
// of 2026-09-17 11:59 the game filled it with the guest's own Central Yharnam spot.
void BbscWriteConnectFirstPlacement(std::string_view when) {
    const auto host = BbscConnectFirstHost();
    const u64 state = GetGlobalState();
    if (!host.has_value() || state < 0x10000 ||
        !HasMemoryAccess(state, 0x1650, MemoryProt::CpuRead)) {
        return;
    }
    const bool already = ReadValue<u8>(state, 0x1648) != 0 &&
                         ReadValue<u32>(state, 0x1644) == host->packed_region &&
                         ReadValue<u32>(state, 0x14C4) == host->packed_region &&
                         ReadValue<u32>(state, 0x14C0) == host->packed_region;
    if (already) {
        return;
    }
    const u32 previous_map = ReadValue<u32>(state, 0x1644);
    const u32 previous_sign_map = ReadValue<u32>(state, 0x14C0);
    const std::array<float, 4> position{host->x, host->y, host->z, 1.0F};
    const std::array<float, 4> rotation{0.0F, host->heading, 0.0F, 0.0F};
    // Third block: the guest's own sign placement at state+0x14A0 (pos), +0x14B0 (rot), +0x14C0 (map).
    // In both cross-map runs it still named Central Yharnam while the guest loaded Cathedral Ward
    // with Central Yharnam lighting and low-detail map pieces (a same-map join never differs here;
    // cross-map without co-op renders normally, 2026-09-17). HYPOTHESIS: the load takes the area
    // setup from it.
    const bool ok = WriteValue(state, 0x14A0, position) && WriteValue(state, 0x14B0, rotation) &&
                    WriteValue<u32>(state, 0x14C0, host->packed_region) &&
                    WriteTransportedSummonPlacement(state, *host) &&
                    WriteValue(state, 0x1620, position) && WriteValue(state, 0x1630, rotation) &&
                    WriteValue<s32>(state, 0x1640, host->area) &&
                    WriteValue<u32>(state, 0x1644, host->packed_region) &&
                    WriteValue<u8>(state, 0x1648, 1);
    LOG_INFO(Debug,
             "BBSC connect-first: placement {} at {} (map {:#x} area {} at ({:.2f}, {:.2f}, "
             "{:.2f}); block had map {:#x}, sign map {:#x}; current map {:#x})",
             ok ? "written" : "WRITE FAILED", when, host->packed_region, host->area, host->x,
             host->y, host->z, previous_map, previous_sign_map, GetCurrentPackedMap());
}

void BbscFillConnectFirstReceivedPlacement() {
    BbscWriteConnectFirstPlacement("summoned reload");
}

void BbscConnectFirstReloadFinished() {
    const auto host = BbscConnectFirstHost();
    if (!host.has_value() || GetCurrentPackedMap() != host->packed_region) {
        return;
    }
    std::scoped_lock lock{seamless_placement_mutex};
    bbsc_connect_first_placement.reset();
    bbsc_cross_map_join.store(true, std::memory_order_release);
    LOG_INFO(Debug, "BBSC connect-first: summoned reload finished in host map {:#x}",
             host->packed_region);
}

void ApplyCrossMapSummonGuestPlacement(const GuestRegisterSnapshot& registers) {
    guest_placement_handoff = {};
    guest_placement_handoff.result = "invalid_object";
    guest_placement_handoff.object = registers.rbx;

    const u64 object = registers.rbx;
    if (object < 0x10000 || !HasMemoryAccess(object, 0xEC, MemoryProt::CpuRead)) {
        return;
    }
    if (ReadValue<u32>(object, 0xE8) != 0) {
        guest_placement_handoff.result = "not_summoned_client";
        return;
    }

    auto& record = guest_placement_handoff;
    record.result = "invalid_state";
    record.state = GetGlobalState();
    const u64 state = record.state;
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1649, MemoryProt::CpuRead)) {
        return;
    }
    std::optional<SummonPlacementDescriptor> transported_host;
    std::optional<SummonPlacementDescriptor> connect_first_host;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        transported_host = seamless_received_host_placement;
        connect_first_host = bbsc_connect_first_placement;
    }
    if (connect_first_host.has_value() && !BbscConnectFirstPlacementFresh()) {
        connect_first_host.reset(); // stale prepare reply: never send a later join there
    }
    // Connect-first: the game's own summoned reload already loads the host map (0x14C4 comes from the
    // host's rewritten summon data; run 2026-09-17 00:xx loaded Cathedral Ward with no forced warp),
    // and BbscFillConnectFirstReceivedPlacement supplies the spawn position. A second, forced
    // StageTransition here started a competing load 1.5 s before the summoned reload: missing floors,
    // blurry textures, no lamp and red flashes on the guest (run 2026-09-17 11:31). Leave it native.
    if (connect_first_host.has_value()) {
        BbscWriteConnectFirstPlacement("handoff");
        record.result = "connect_first_native_reload";
        return;
    }
    if (ReadValue<u8>(state, 0x1648) == 0) {
        record.result = "received_placement_not_ready";
        return;
    }
    record.transported_host = transported_host.has_value();

    record.current_map = GetCurrentPackedMap();
    record.received_map = transported_host.has_value() ? transported_host->packed_region
                                                       : ReadValue<u32>(state, 0x14C4);
    const u32 transport_map = transported_host.has_value() ? transported_host->packed_region
                                                           : ReadValue<u32>(state, 0x1644);
    if (!IsUsablePackedMap(record.current_map) || !IsUsablePackedMap(record.received_map)) {
        record.result = "invalid_map";
        return;
    }
    if (record.received_map != transport_map) {
        record.result = "placement_copy_pending";
        return;
    }
    if (record.current_map == record.received_map) {
        record.result = "same_map";
        return;
    }
    if (!HasMemoryAccess(state, 0x1521, MemoryProt::CpuWrite)) {
        record.result = "state_not_writable";
        return;
    }
    if (transported_host.has_value()) {
        if (!WriteTransportedSummonPlacement(state, *transported_host)) {
            record.result = "transport_write_rejected";
            return;
        }
        record.placement_refreshed = true;
    }
    if (ReadValue<u8>(state, 0x08) != 0) {
        record.result = "stage_transition_pending";
        return;
    }
    if (ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "native_forced_warp_pending";
        return;
    }

    for (size_t index = 0; index < 4; ++index) {
        const float position = ReadValue<float>(state, 0x14D0 + index * sizeof(float));
        const float orientation = ReadValue<float>(state, 0x14E0 + index * sizeof(float));
        if (!std::isfinite(position) || !std::isfinite(orientation)) {
            record.result = "non_finite_placement";
            return;
        }
    }

    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || std::ranges::any_of(CrossMapNativeCalls, [image_size](const auto& site) {
            return site.offset >= image_size;
        })) {
        record.result = "native_call_out_of_range";
        return;
    }

    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = s32 PS4_SYSV_ABI (*)();
    const auto set_map = reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
    const auto set_position =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
    const auto set_orientation =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
    const auto set_warp = reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset);
    const auto select =
        reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset);
    const auto transition = reinterpret_cast<StageTransition>(image_base + StageTransitionOffset);

    BbGuestCall(set_position, reinterpret_cast<const void*>(state + 0x14D0));
    BbGuestCall(set_orientation, reinterpret_cast<const void*>(state + 0x14E0));
    BbGuestCall(set_map, reinterpret_cast<const void*>(state + 0x14C4));
    BbGuestCall(set_warp);
    record.select_result = BbGuestCall(select, 0, -1);
    record.selected_map = ReadValue<u32>(state, 0x0C);
    if (!record.select_result || record.selected_map != record.received_map) {
        record.result = "native_select_failed";
        return;
    }

    pending_summon_reload_map.store(record.received_map, std::memory_order_release);
    pending_summon_reload_state_map.store(record.received_map, std::memory_order_release);
    record.summon_reload_armed = true;
    record.transition_result = BbGuestCall(transition);
    if (record.transition_result == 0) {
        pending_summon_reload_map.store(0, std::memory_order_release);
        pending_summon_reload_state_map.store(0, std::memory_order_release);
        record.summon_reload_armed = false;
        record.result = "stage_transition_failed";
        return;
    }
    record.result = "applied";
}

// SummonedMapReload (0x01336B90) is a wrapper: it panics ("SprjLuaEventMan: accessed an
// uninitialized singleton", FD4Singleton.h:177) when [0x0553B0C8] is null, else tail-jumps to
// 0x0131E5C0. The singleton is null while a death load tears down the stage (CONFIRMED by the
// Engine.Panic stack: caller 0x01336BC2), so the wrapper must never be called then.
constexpr u64 SprjLuaEventManPointerOffset = 0x0553B0C8;

bool IsSprjLuaEventManReady() {
    return image_base != 0 && ReadValue<u64>(image_base + SprjLuaEventManPointerOffset, 0) != 0;
}

// BBSC rejoin request handed to ApplyDeferredSummonReload (gated like SummonReloadStart).
std::atomic<bool> bbsc_rejoin_deferred_request{false};
constexpr s64 BbscDeferredSettleMs = 3'000;
std::atomic<s64> bbsc_deferred_ready_since_ms{0};

// [[global+0x16F8]+0x18]: member count checked by SummonReloadStart (0x01389458).
s32 GetBbscSessionMemberCount(u64 global_state) {
    if (global_state < 0x10000 ||
        !HasMemoryAccess(global_state + 0x16F8, sizeof(u64), MemoryProt::CpuRead)) {
        return -1;
    }
    const u64 transition = ReadValue<u64>(global_state, 0x16F8);
    if (transition < 0x10000 || !HasMemoryAccess(transition + 0x18, 4, MemoryProt::CpuRead)) {
        return -1;
    }
    return ReadValue<s32>(transition, 0x18);
}

// [[[[0x0553E878]+0x60]+0x3B0]+0x20]+0xF8: local player value SummonReloadStart requires > 0
// (0x01389471..0x0138949B; STRONGLY INDICATED to be current HP).
s32 GetBbscLocalPlayerHp() {
    u64 pointer = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    for (const u64 offset : {u64{0x60}, u64{0x3B0}, u64{0x20}}) {
        if (pointer < 0x10000 ||
            !HasMemoryAccess(pointer + offset, sizeof(u64), MemoryProt::CpuRead)) {
            return -1;
        }
        pointer = ReadValue<u64>(pointer, offset);
    }
    if (pointer < 0x10000 || !HasMemoryAccess(pointer + 0xF8, 4, MemoryProt::CpuRead)) {
        return -1;
    }
    return ReadValue<s32>(pointer, 0xF8);
}

void ApplySummonReloadStateAtDescriptor(const GuestRegisterSnapshot& registers) {
    summon_reload_state = {};
    auto& record = summon_reload_state;
    record.target_map = pending_summon_reload_state_map.load(std::memory_order_acquire);
    if (!IsUsablePackedMap(record.target_map)) {
        return;
    }

    const u64 descriptor = registers.r9;
    if (descriptor < 0x10000 || !HasMemoryAccess(descriptor, 0x0C, MemoryProt::CpuRead)) {
        record.result = "invalid_descriptor";
        return;
    }
    record.descriptor_map = ReadValue<u32>(descriptor, 0x08);
    if (record.descriptor_map != record.target_map) {
        record.result = "waiting_for_target_descriptor";
        return;
    }

    if (!IsSprjLuaEventManReady()) {
        record.result = "lua_event_man_missing";
        // BBSC rejoin request (death load): hand it to the in-world deferred reload, which runs
        // once the respawned stage has a SprjLuaEventMan again. Fork requests stay pending.
        u32 bbsc_expected = record.target_map;
        if (bbsc_rejoin_suppress_placement_refresh.load(std::memory_order_acquire) &&
            pending_summon_reload_state_map.compare_exchange_strong(
                bbsc_expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            bbsc_rejoin_suppress_placement_refresh.store(false, std::memory_order_release);
            bbsc_rejoin_deferred_request.store(true, std::memory_order_release);
            pending_summon_reload_map.store(record.target_map, std::memory_order_release);
            record.result = "lua_event_man_missing_deferred_in_world";
            LOG_INFO(Debug,
                     "BBSC rejoin v5: SprjLuaEventMan missing at death-load descriptor {:#x}; "
                     "summoned reload deferred until the respawned stage is ready",
                     record.target_map);
        }
        return;
    }

    u32 expected_map = record.target_map;
    if (!pending_summon_reload_state_map.compare_exchange_strong(
            expected_map, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        record.result = "already_dispatched";
        return;
    }

    record.reload_state = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (record.reload_state < 0x10000 ||
        !HasMemoryAccess(record.reload_state, 0x88, MemoryProt::CpuWrite)) {
        pending_summon_reload_state_map.store(record.target_map, std::memory_order_release);
        record.result = "invalid_reload_state";
        return;
    }

    record.state_before = ReadValue<s32>(record.reload_state, 0x84);
    if (const u64 global = GetGlobalState();
        global >= 0x10000 && HasMemoryAccess(global + 0x1592, 1, MemoryProt::CpuRead) &&
        ReadValue<u8>(global, 0x1592) == 0) {
        // Not blocked (fork join path, unverified which value 0x1592 has mid-join), but logged:
        // outside the own world this snapshot overwrites the own-world save data.
        LOG_WARNING(Debug,
                    "BBSC: descriptor summoned reload into {:#x} runs with global+0x1592 == 0 "
                    "(own-world save snapshot may be overwritten)",
                    record.target_map);
    }
    u32 pending_reload_map = record.target_map;
    const bool cleared_deferred_reload = pending_summon_reload_map.compare_exchange_strong(
        pending_reload_map, 0, std::memory_order_acq_rel, std::memory_order_acquire);

    using SummonedMapReload = void PS4_SYSV_ABI (*)();
    const auto reload_summoned_map =
        reinterpret_cast<SummonedMapReload>(image_base + SummonedMapReloadOffset);
    BbGuestCall(reload_summoned_map);
    record.summon_reload_started = true;
    record.state_after = ReadValue<s32>(record.reload_state, 0x84);
    if (record.state_after != 3) {
        using SummonReloadStateSetter = void PS4_SYSV_ABI (*)(u64 state);
        const auto set_reload_state =
            reinterpret_cast<SummonReloadStateSetter>(image_base + SetSummonReloadStateOffset);
        BbGuestCall(set_reload_state, record.reload_state);
        record.state_setter_used = true;
        record.state_after = ReadValue<s32>(record.reload_state, 0x84);
    }
    if (record.state_after != 3) {
        pending_summon_reload_state_map.store(record.target_map, std::memory_order_release);
        if (cleared_deferred_reload) {
            pending_summon_reload_map.store(record.target_map, std::memory_order_release);
        }
        record.result = "state_transition_failed";
        return;
    }

    if (bbsc_rejoin_suppress_placement_refresh.exchange(false, std::memory_order_acq_rel)) {
        record.result = "applied_without_placement_refresh";
        return;
    }
    std::optional<SummonPlacementDescriptor> transported_host;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        transported_host = seamless_received_host_placement;
    }
    const u64 placement_state = GetGlobalState();
    if (transported_host.has_value() && transported_host->packed_region == record.target_map &&
        placement_state >= 0x10000 &&
        WriteTransportedSummonPlacement(placement_state, *transported_host)) {
        using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
        const auto set_map =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
        const auto set_position =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
        const auto set_orientation =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
        BbGuestCall(set_position, reinterpret_cast<const void*>(placement_state + 0x14D0));
        BbGuestCall(set_orientation, reinterpret_cast<const void*>(placement_state + 0x14E0));
        BbGuestCall(set_map, reinterpret_cast<const void*>(placement_state + 0x14C4));
        record.placement_refreshed = true;
    }
    record.result = "applied";
}

void ApplyDeferredSummonReload() {
    deferred_summon_reload = {};
    auto& record = deferred_summon_reload;
    record.target_map = pending_summon_reload_map.load(std::memory_order_acquire);
    if (!IsUsablePackedMap(record.target_map)) {
        return;
    }

    record.current_map = GetCurrentPackedMap();
    if (record.current_map != record.target_map) {
        record.result = "waiting_for_target_map";
        return;
    }

    const u64 state = GetGlobalState();
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1521, MemoryProt::CpuRead)) {
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(state, 0x08) != 0 || ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "waiting_for_transition";
        return;
    }
    if (!IsSprjLuaEventManReady()) {
        record.result = "waiting_for_lua_event_man";
        bbsc_deferred_ready_since_ms = 0;
        return;
    }
    if (HasMemoryAccess(state + 0x1592, 1, MemoryProt::CpuRead) &&
        ReadValue<u8>(state, 0x1592) == 0) {
        // Save protection: the reload native freezes the player's own world into the save
        // snapshot; outside the own world (global+0x1592 == 0) it would store another world.
        u32 expected = record.target_map;
        if (pending_summon_reload_map.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            bbsc_rejoin_deferred_request.store(false, std::memory_order_release);
            LOG_WARNING(Debug,
                        "BBSC: deferred summoned reload into {:#x} dropped: not in own world "
                        "(global+0x1592 == 0), snapshot would overwrite the own-world save data",
                        record.target_map);
        }
        record.result = "not_in_own_world";
        return;
    }
    if (bbsc_rejoin_deferred_request.load(std::memory_order_acquire)) {
        // Mirror the game's SummonReloadStart handlers (0x01389440 / 0x01389620): the reload
        // native only runs with >= 2 session members and a living local player. v5 called it
        // 0.5 s into the respawn load (player not alive yet) and nothing happened.
        const s32 members = GetBbscSessionMemberCount(state);
        const s32 local_hp = GetBbscLocalPlayerHp();
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (members < 2 || local_hp <= 0) {
            record.result = members < 2 ? "waiting_for_session_members" : "waiting_for_alive_player";
            bbsc_deferred_ready_since_ms = 0;
            return;
        }
        s64 ready_since = 0;
        bbsc_deferred_ready_since_ms.compare_exchange_strong(ready_since, now);
        if (now - bbsc_deferred_ready_since_ms.load() < BbscDeferredSettleMs) {
            record.result = "waiting_for_settle";
            return;
        }
    }

    u32 expected_map = record.target_map;
    if (!pending_summon_reload_map.compare_exchange_strong(
            expected_map, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        record.result = "already_dispatched";
        return;
    }
    pending_summon_reload_state_map.store(0, std::memory_order_release);

    const bool bbsc_request = bbsc_rejoin_deferred_request.exchange(false, std::memory_order_acq_rel);
    bbsc_deferred_ready_since_ms = 0;
    const u64 reload_rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const s32 phase_before =
        reload_rules >= 0x10000 && HasMemoryAccess(reload_rules, 0x88, MemoryProt::CpuRead)
            ? ReadValue<s32>(reload_rules, 0x84)
            : -1;

    using SummonedMapReload = void PS4_SYSV_ABI (*)();
    const auto reload_summoned_map =
        reinterpret_cast<SummonedMapReload>(image_base + SummonedMapReloadOffset);
    BbGuestCall(reload_summoned_map);
    record.summon_reload_started = true;
    record.result = "applied";
    if (bbsc_request) {
        const s32 phase_after =
            reload_rules >= 0x10000 && HasMemoryAccess(reload_rules, 0x88, MemoryProt::CpuRead)
                ? ReadValue<s32>(reload_rules, 0x84)
                : -1;
        LOG_INFO(Debug,
                 "BBSC rejoin v6: in-world summoned reload called (map {:#x}, members={}, hp={}, "
                 "reload phase {} -> {})",
                 record.target_map, GetBbscSessionMemberCount(state), GetBbscLocalPlayerHp(),
                 phase_before, phase_after);
    }
}

void ApplyPreMatchCrossMapGuestWarp() {
    pre_match_guest_warp = {};
    auto& record = pre_match_guest_warp;

    std::optional<SummonPlacementDescriptor> transported_host;
    bool placement_consumed = false;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        transported_host = seamless_received_host_placement;
        placement_consumed = seamless_received_host_placement_consumed;
    }
    if (!transported_host.has_value()) {
        record.result = "no_placement";
        return;
    }
    if (placement_consumed) {
        record.result = "placement_consumed";
        return;
    }

    record.current_map = GetCurrentPackedMap();
    record.target_map = transported_host->packed_region;
    if (!IsUsablePackedMap(record.current_map) || !IsUsablePackedMap(record.target_map)) {
        record.result = "invalid_map";
        return;
    }
    if (record.current_map == record.target_map) {
        // BBSC fix: a same-map claim never consumed the transported placement, so a later map
        // change (death, Hunter's Dream) re-armed a stale pre-match warp and teleported the guest
        // back to the old host spot. Already at the destination == the placement has been used.
        {
            std::scoped_lock lock{seamless_placement_mutex};
            if (seamless_received_host_placement.has_value() &&
                IsSameSummonPlacement(*seamless_received_host_placement, *transported_host)) {
                seamless_received_host_placement_consumed = true;
            }
        }
        record.result = "destination_reached";
        return;
    }

    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    record.multi_play_state = multi_play >= 0x10000 ? ReadValue<s32>(multi_play, 0x124) : -1;
    if (multi_play >= 0x10000 &&
        (record.multi_play_state != 0 || ReadValue<u64>(multi_play, 0x18) != 0)) {
        record.result = "matching_already_active";
        return;
    }

    record.state = GetGlobalState();
    const u64 state = record.state;
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1521, MemoryProt::CpuWrite)) {
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(state, 0x08) != 0) {
        record.result = "stage_transition_pending";
        return;
    }
    if (ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "native_forced_warp_pending";
        return;
    }
    if (pending_summon_reload_map.load(std::memory_order_acquire) != 0 ||
        pending_summon_reload_state_map.load(std::memory_order_acquire) != 0) {
        record.result = "summon_reload_pending";
        return;
    }
    if (!WriteTransportedSummonPlacement(state, *transported_host)) {
        record.result = "transport_write_rejected";
        return;
    }
    record.placement_written = true;

    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = s32 PS4_SYSV_ABI (*)();
    const auto set_map = reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
    const auto set_position =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
    const auto set_orientation =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
    const auto set_warp = reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset);
    const auto select =
        reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset);
    const auto transition = reinterpret_cast<StageTransition>(image_base + StageTransitionOffset);

    BbGuestCall(set_position, reinterpret_cast<const void*>(state + 0x14D0));
    BbGuestCall(set_orientation, reinterpret_cast<const void*>(state + 0x14E0));
    BbGuestCall(set_map, reinterpret_cast<const void*>(state + 0x14C4));
    BbGuestCall(set_warp);
    record.select_result = BbGuestCall(select, 0, -1);
    record.selected_map = ReadValue<u32>(state, 0x0C);
    if (!record.select_result || record.selected_map != record.target_map) {
        record.result = "native_select_failed";
        return;
    }

    record.transition_result = BbGuestCall(transition);
    if (record.transition_result == 0) {
        record.result = "stage_transition_failed";
        return;
    }
    {
        std::scoped_lock lock{seamless_placement_mutex};
        if (seamless_received_host_placement.has_value() &&
            IsSameSummonPlacement(*seamless_received_host_placement, *transported_host)) {
            seamless_received_host_placement_consumed = true;
        }
    }
    pending_responder_resume = {
        .target_map = record.target_map,
        .target_area = transported_host->area,
    };
    responder_resume = {};
    responder_resume.result = "armed";
    responder_resume.target_map = record.target_map;
    responder_resume.target_area = transported_host->area;
    record.responder_resume_armed = true;
    record.result = "applied";
}

bool BbscConnectFirstEnabled(); // BBSC connect-first cross-map summon, defined near the end

void ResumePreMatchCrossMapResponder() {
    auto& pending = pending_responder_resume;
    if (!IsUsablePackedMap(pending.target_map)) {
        return;
    }

    responder_resume = {};
    auto& record = responder_resume;
    record.target_map = pending.target_map;
    record.target_area = pending.target_area;
    record.current_map = GetCurrentPackedMap();
    if (record.current_map != record.target_map) {
        pending.ready_observations = 0;
        record.result = "waiting_for_target_map";
        return;
    }

    SummonPlacementDescriptor local{};
    std::string_view placement_result;
    if (!ReadLocalSummonPlacement(local, placement_result)) {
        pending.ready_observations = 0;
        record.result = placement_result;
        return;
    }
    record.current_area = local.area;
    if (record.current_area != record.target_area) {
        pending.ready_observations = 0;
        record.result = "waiting_for_target_area";
        return;
    }

    record.state = GetGlobalState();
    if (record.state < 0x10000 || !HasMemoryAccess(record.state, 0x1521, MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(record.state, 0x08) != 0 || ReadValue<u8>(record.state, 0x1520) != 0) {
        pending.ready_observations = 0;
        record.result = "waiting_for_stage_transition";
        return;
    }

    record.multi_play = GetMatchingState();
    if (record.multi_play < 0x10000 ||
        !HasMemoryAccess(record.multi_play, 0x128, MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_matching_state";
        return;
    }
    record.multi_play_state = ReadValue<s32>(record.multi_play, 0x124);
    record.matching_controller = ReadValue<u64>(record.multi_play, 0x18);
    if (record.multi_play_state != 0 || record.matching_controller != 0) {
        pending.ready_observations = 0;
        record.result = "waiting_for_matching_idle";
        return;
    }

    record.player = GetLocalPlayer();
    if (record.player < 0x10000 ||
        !HasMemoryAccess(record.player, sizeof(u64), MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_player";
        return;
    }
    record.effect_active = HasPlayerEffect(record.player, SmallResonantBellEffectId);
    if (record.effect_active) {
        record.ready_observations = pending.ready_observations;
        record.attempts = pending.attempts;
        record.result = "completed";
        LOG_INFO(Debug,
                 "Bloodborne seamless responder resumed at map={:#x} area={} after {} native "
                 "attempt(s)",
                 record.target_map, record.target_area, record.attempts);
        pending = {};
        return;
    }

    if (pending.dispatched) {
        record.ready_observations = pending.ready_observations;
        record.attempts = pending.attempts;
        if (std::chrono::steady_clock::now() - pending.dispatched_at < ResponderResumeRetryDelay) {
            record.result = "waiting_for_effect";
            return;
        }
        if (pending.attempts >= ResponderResumeMaxAttempts) {
            record.result = "effect_timeout";
            return;
        }
        pending.dispatched = false;
        pending.ready_observations = 0;
    }

    ++pending.ready_observations;
    record.ready_observations = pending.ready_observations;
    record.attempts = pending.attempts;
    if (pending.ready_observations < ResponderResumeStableObservations) {
        record.result = "waiting_for_destination_stability";
        return;
    }

    using UseItemNativeApply = bool PS4_SYSV_ABI (*)(u64 player, s32 goods_id, s32 argument);
    const auto use_item =
        reinterpret_cast<UseItemNativeApply>(image_base + UseItemNativeApplyOffset);
    record.native_result = BbGuestCall(use_item, record.player, SmallResonantBellGoodsId, BellUseArgument);
    ++pending.attempts;
    pending.dispatched = true;
    pending.dispatched_at = std::chrono::steady_clock::now();
    record.attempts = pending.attempts;
    record.result = record.native_result ? "native_dispatched" : "native_rejected";
    LOG_INFO(Debug,
             "Bloodborne seamless responder native bell dispatch map={:#x} area={} attempt={} "
             "result={}",
             record.target_map, record.target_area, record.attempts, record.native_result);
}

void ApplyHealingFountainHostAvailability(const GuestRegisterSnapshot& registers) {
    healing_fountain_availability = {};
    auto& record = healing_fountain_availability;
    record.result = "invalid_object";
    record.object = registers.r13;
    if (record.object < 0x10000 || !HasMemoryAccess(record.object, 0x4C, MemoryProt::CpuRead)) {
        return;
    }

    record.blocked_before = ReadValue<u8>(record.object, 0x49) != 0;
    record.blocked_after = record.blocked_before;
    if (!record.blocked_before) {
        record.result = "already_allowed";
        return;
    }

    record.player = GetLocalPlayer();
    record.host_effect = HasPlayerEffect(record.player, 9001);
    if (!record.host_effect) {
        record.result = "not_multiplayer_host";
        return;
    }

    if (!WriteValue(record.object, 0x49, u8{})) {
        record.result = "write_rejected";
        return;
    }
    record.blocked_after = false;
    record.result = "applied";
}

u64 GetSummonManagerRoot() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        SummonManagerRootPointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + SummonManagerRootPointerOffset, 0);
}

u64 GetSummonManager() {
    const u64 root = GetSummonManagerRoot();
    return root >= 0x10000 ? ReadValue<u64>(root, 0xC50) : 0;
}

u64 GetMatchingState() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        MatchingStatePointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
}

void WriteHex(std::ostream& out, u64 value) {
    out << "\"0x" << std::hex << value << std::dec << '\"';
}

void WriteRoleSelectionMask(std::ostream& out, std::string_view field, s32 role_code) {
    if (role_code < 0 || role_code >= RoleMetadataCount || image_base == 0) {
        return;
    }
    const u64 offset =
        RoleMetadataTableOffset + static_cast<u64>(role_code) * RoleMetadataStride + sizeof(u32);
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_size < sizeof(u32) || offset > image_size - sizeof(u32)) {
        return;
    }
    out << ",\"" << field << "\":";
    WriteHex(out, ReadValue<u32>(image_base + offset, 0));
}

void WriteObjectState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"object\":{";
    out << "\"state_54\":";
    WriteHex(out, ReadValue<u32>(object, 0x54));
    out << ",\"c8\":";
    WriteHex(out, ReadValue<u64>(object, 0xC8));
    out << ",\"d0\":";
    WriteHex(out, ReadValue<u64>(object, 0xD0));
    out << ",\"d8\":";
    WriteHex(out, ReadValue<u32>(object, 0xD8));
    out << ",\"e0\":";
    WriteHex(out, ReadValue<u64>(object, 0xE0));
    out << ",\"e8\":";
    WriteHex(out, ReadValue<u32>(object, 0xE8));
    out << ",\"ec\":";
    WriteHex(out, ReadValue<u32>(object, 0xEC));
    out << ",\"f0\":";
    WriteHex(out, ReadValue<u8>(object, 0xF0));
    out << ",\"f8\":";
    WriteHex(out, ReadValue<u32>(object, 0xF8));
    out << '}';
}

void WriteGetSosState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"get_sos\":{";
    const u64 begin = ReadValue<u64>(object, 0xD0);
    const u64 current = ReadValue<u64>(object, 0xD8);
    const u64 end = ReadValue<u64>(object, 0xE0);
    out << "\"begin\":";
    WriteHex(out, begin);
    out << ",\"current\":";
    WriteHex(out, current);
    out << ",\"end\":";
    WriteHex(out, end);
    if (begin >= 0x10000 && current >= begin && current <= end &&
        (current - begin) % sizeof(s32) == 0) {
        const size_t count = std::min<size_t>((current - begin) / sizeof(s32), 16);
        out << ",\"summon_types\":[";
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) {
                out << ',';
            }
            out << ReadValue<s32>(begin, index * sizeof(s32));
        }
        out << ']';
    }
    out << ",\"request_active\":" << static_cast<u32>(ReadValue<u8>(object, 0x120));
    out << ",\"retry_count\":" << ReadValue<u32>(object, 0x124);
    out << '}';
}

void WriteMultiplayerEffects(std::ostream& out, u64 player) {
    if (player < 0x10000) {
        return;
    }

    const u64 manager = ReadValue<u64>(player, 0x1C8);
    if (manager < 0x10000) {
        return;
    }

    out << ",\"multiplayer_effects\":{";
    out << "\"manager\":";
    WriteHex(out, manager);
    out << ",\"mask\":";
    WriteHex(out, ReadValue<u32>(manager, 0x20));
    out << ",\"entries\":[";

    bool first = true;
    u64 node = ReadValue<u64>(manager, 0x08);
    for (size_t index = 0; node >= 0x10000 && index < 512; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        const u32 flags = ReadValue<u32>(node, 0x1C);
        const u64 param = ReadValue<u64>(node, 0x48);
        if ((flags & 0x800C0003U) == 0 && param >= 0x10000) {
            const s32 effect_id = ReadValue<s32>(node, 0x40);
            const u16 special_state = ReadValue<u16>(param, 0x156);
            if ((special_state >= 188 && special_state <= 190) || effect_id == 9003 ||
                effect_id == 9004) {
                if (!first) {
                    out << ',';
                }
                first = false;
                out << "{\"effect_id\":" << effect_id << ",\"special_state\":" << special_state
                    << '}';
            }
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    out << "]}";
}

void WriteSummonCandidate(std::ostream& out, const TraceSite& site,
                          const GuestRegisterSnapshot& registers) {
    const bool is_entry = site.offset == 0x014BA980;
    const bool is_submit = site.offset == 0x014BABCB;
    const bool is_inserted = site.offset == 0x014B8E8D;
    const bool is_filter_cycle = site.offset == 0x014B6F09;
    const bool is_manager_scan = site.offset == 0x014B6F5D;
    const bool is_filter_progress = site.offset == 0x014B6F9D || site.offset == 0x014B6FE1 ||
                                    site.offset == 0x014B7070 || site.offset == 0x014B70FF ||
                                    site.offset == 0x014B7150 || site.offset == 0x014B71B2 ||
                                    site.offset == 0x014B71F4;
    const bool is_pending_inserted = site.offset == 0x014B750B;
    const bool is_observer_result = site.offset == 0x014C0238;
    const u64 wrapper = is_entry ? registers.rsi : 0;
    const u64 storage =
        is_entry ? (wrapper >= 0x10000 ? ReadValue<u64>(wrapper, 0x08) : 0)
                 : (!is_inserted && !is_filter_cycle && !is_manager_scan && !is_filter_progress &&
                            !is_pending_inserted && !is_observer_result
                        ? registers.r15
                        : 0);

    out << ",\"summon_candidate\":{";
    out << "\"stage\":\"" << site.name << "\"";
    out << ",\"manager\":";
    WriteHex(out, is_entry              ? registers.rdi
                  : is_inserted         ? registers.rdx
                  : is_pending_inserted ? registers.r14
                                        : GetSummonManager());
    out << ",\"wrapper\":";
    WriteHex(out, wrapper);
    out << ",\"storage\":";
    WriteHex(out, storage);
    if (storage >= 0x10000) {
        out << ",\"decoded_size\":" << ReadValue<u16>(storage, 0x02);
        out << ",\"role_code\":" << static_cast<u32>(ReadValue<u8>(storage, 0x7A));
        out << ",\"field_54\":";
        WriteHex(out, ReadValue<u64>(storage, 0x54));
        out << ",\"field_d4\":";
        WriteHex(out, ReadValue<u32>(storage, 0xD4));
        out << ",\"field_dc\":";
        WriteHex(out, ReadValue<u64>(storage, 0xDC));
    }
    if (is_filter_cycle) {
        const u64 manager = GetSummonManager();
        out << ",\"manager_identity_state_register\":" << static_cast<s32>(registers.r15);
        out << ",\"local_identity_pointer\":";
        WriteHex(out, registers.r12);
        if (manager >= 0x10000) {
            out << ",\"primary_head\":";
            WriteHex(out, ReadValue<u64>(manager, 0x30));
            out << ",\"primary_count\":" << ReadValue<u64>(manager, 0x38);
        }
    } else if (is_manager_scan) {
        const u64 root = GetSummonManagerRoot();
        const u64 local_identity =
            registers.rsp >= 0x10000 ? ReadValue<u64>(registers.rsp, 0x88) : 0;
        out << ",\"primary_node\":";
        WriteHex(out, registers.r15);
        out << ",\"constructed_candidate\":";
        WriteHex(out, registers.r12);
        if (registers.r12 >= 0x10000) {
            out << ",\"role_code\":" << static_cast<s32>(ReadValue<s8>(registers.r12, 0x08));
            out << ",\"account_token\":";
            WriteHex(out, ReadValue<u64>(registers.r12, 0x0C));
            out << ",\"user_id\":" << ReadValue<u32>(registers.r12, 0x14);
            out << ",\"character_id\":";
            WriteHex(out, ReadValue<u64>(registers.r12, 0x18));
            out << ",\"candidate_identity\":[";
            WriteHex(out, ReadValue<u64>(registers.r12, 0x20));
            out << ',';
            WriteHex(out, ReadValue<u64>(registers.r12, 0x28));
            out << ']';
            out << ",\"candidate_observer\":";
            WriteHex(out, ReadValue<u64>(registers.r12, 0xF0));
            out << ",\"candidate_area\":" << ReadValue<s32>(registers.r12, 0x98);
            out << ",\"candidate_generation\":"
                << static_cast<u32>(ReadValue<u8>(registers.r12, 0xEC));
            out << ",\"candidate_available_count\":"
                << static_cast<u32>(ReadValue<u8>(registers.r12, 0xED));
            out << ",\"candidate_stale_count\":"
                << static_cast<u32>(ReadValue<u8>(registers.r12, 0xEE));
        }
        out << ",\"local_identity_pointer\":";
        WriteHex(out, local_identity);
        if (local_identity >= 0x10000) {
            out << ",\"local_identity\":[";
            WriteHex(out, ReadValue<u64>(local_identity, 0));
            out << ',';
            WriteHex(out, ReadValue<u64>(local_identity, sizeof(u64)));
            out << ']';
        }
        out << ",\"manager_root\":";
        WriteHex(out, root);
        if (root >= 0x10000) {
            out << ",\"manager_identity\":[";
            WriteHex(out, ReadValue<u64>(root, 0xA68));
            out << ',';
            WriteHex(out, ReadValue<u64>(root, 0xA70));
            out << ']';
            out << ",\"manager_identity_state\":" << ReadValue<s32>(root, 0xA78);
            out << ",\"manager_area\":" << ReadValue<s32>(root, 0xA80);
            out << ",\"manager_gate_ac0\":" << ReadValue<s32>(root, 0xAC0);
            out << ",\"manager_flags_b14\":";
            WriteHex(out, ReadValue<u8>(root, 0xB14));
        }
    } else if (is_filter_progress) {
        out << ",\"constructed_candidate\":";
        WriteHex(out, registers.r12);
    } else if (is_pending_inserted) {
        const u64 pending_head =
            registers.r14 >= 0x10000 ? ReadValue<u64>(registers.r14, 0x1A8) : 0;
        const u64 pending_node =
            pending_head >= 0x10000 ? ReadValue<u64>(pending_head, sizeof(u64)) : 0;
        const u64 pending_wrapper =
            pending_node >= 0x10000 ? ReadValue<u64>(pending_node, 0x10) : 0;
        out << ",\"pending_node\":";
        WriteHex(out, pending_node);
        out << ",\"pending_wrapper\":";
        WriteHex(out, pending_wrapper);
        out << ",\"constructed_candidate\":";
        WriteHex(out, pending_wrapper >= 0x10000 ? ReadValue<u64>(pending_wrapper, 0) : 0);
        if (registers.r14 >= 0x10000) {
            out << ",\"pending_count\":" << ReadValue<u64>(registers.r14, 0x1B0);
        }
    } else if (is_inserted) {
        out << ",\"constructed_candidate\":";
        WriteHex(out, registers.r15);
        if (registers.r15 >= 0x10000) {
            out << ",\"candidate_observer_before_registration\":";
            WriteHex(out, ReadValue<u64>(registers.r15, 0xF0));
        }
    } else if (is_observer_result) {
        out << ",\"constructed_candidate\":";
        WriteHex(out, registers.r14);
        out << ",\"interface\":";
        WriteHex(out, registers.r15);
        out << ",\"observer\":";
        WriteHex(out, registers.r12);
        if (registers.r12 >= 0x10000) {
            out << ",\"observer_callback\":";
            WriteHex(out, ReadValue<u64>(registers.r12, 0x58));
        }
        out << ",\"observer_registered\":" << static_cast<u32>(static_cast<u8>(registers.rax));
    } else if (is_submit) {
        out << ",\"constructed_candidate\":";
        WriteHex(out, registers.r14);
    }
    out << '}';

    if (!is_entry || image_base == 0 || MemoryPatcher::g_eboot_image_size < sizeof(u64) ||
        CandidateLocalStatePointerOffset > MemoryPatcher::g_eboot_image_size - sizeof(u64)) {
        return;
    }

    const u64 root = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = root >= 0x10000 ? ReadValue<u64>(root, 0x60) : 0;
    out << ",\"candidate_local_state\":{";
    out << "\"root\":";
    WriteHex(out, root);
    out << ",\"player\":";
    WriteHex(out, player);
    if (player >= 0x10000) {
        const u64 effects = ReadValue<u64>(player, 0x1C8);
        out << ",\"effects\":";
        WriteHex(out, effects);
        if (effects >= 0x10000) {
            out << ",\"effect_mask\":";
            WriteHex(out, ReadValue<u32>(effects, 0x20));
        }
    }
    out << '}';
    WriteMultiplayerEffects(out, player);
}

void WriteCharacterWarp(std::ostream& out, const TraceSite& site,
                        const GuestRegisterSnapshot& registers) {
    out << ",\"character_warp\":{";
    out << "\"operation\":\"" << site.name << "\"";
    const u64 return_address = ReadValue<u64>(registers.rsp, 0);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '\"';
    }
    if (site.offset == 0x013CB3D0) {
        out << ",\"source_entity_id\":" << static_cast<s32>(registers.rdi);
        out << ",\"target_area_entity_id\":" << static_cast<s32>(registers.rsi);
        out << ",\"command_variant_argument\":" << static_cast<s32>(registers.rdx);
    } else if (site.offset == 0x013CB870) {
        out << ",\"source_entity_id\":" << static_cast<s32>(registers.rdi);
        out << ",\"target_object_entity_id\":" << static_cast<s32>(registers.rsi);
        out << ",\"target_dummypoly_id\":" << static_cast<s32>(registers.rdx);
        out << ",\"command_variant_argument\":" << static_cast<s32>(registers.rcx);
    } else if (site.offset == 0x013CCBD0) {
        out << ",\"source_entity_id\":" << static_cast<s32>(registers.rdx);
        out << ",\"target_character_entity_id\":" << static_cast<s32>(registers.rdi);
        out << ",\"target_dummypoly_id\":" << static_cast<s32>(registers.rsi);
    } else {
        out << ",\"character_object\":";
        WriteHex(out, registers.rdi);
        out << ",\"floor_object\":";
        WriteHex(out, registers.rsi);
    }
    out << '}';
}

void WriteSummonSelection(std::ostream& out, const TraceSite& site,
                          const GuestRegisterSnapshot& registers) {
    const bool is_scan = site.offset == 0x01873269;
    const bool is_match = site.offset == 0x018732DD;
    const bool is_queue_insert = site.offset == 0x01875320;
    const bool is_queued = site.offset == 0x01873605;
    const bool is_start = site.offset == 0x014BAEC0;
    const bool is_allocated = site.offset == 0x014BAFE3;
    const u64 manager =
        is_scan ? registers.rax : (is_allocated ? registers.r12 : GetSummonManager());
    const u64 status =
        is_scan && registers.rbp >= 0x6F0 + 0x10000
            ? ReadValue<u64>(registers.rbp - 0x6F0, 0)
            : ((is_match ? registers.r15
                         : (is_queue_insert ? registers.rdi : (is_queued ? registers.rbx : 0))));

    out << ",\"summon_selection\":{";
    out << "\"stage\":\"" << site.name << "\"";
    out << ",\"manager\":";
    WriteHex(out, manager);
    out << ",\"status\":";
    WriteHex(out, status);

    const u64 matching_state = GetMatchingState();
    out << ",\"matching_state\":";
    WriteHex(out, matching_state);
    if (matching_state >= 0x10000) {
        out << ",\"matching_state_124\":" << ReadValue<s32>(matching_state, 0x124);
    }

    if (is_scan || is_match) {
        out << ",\"selection_mask\":";
        WriteHex(out, static_cast<u32>(registers.r13));
    }
    if (manager >= 0x10000) {
        const u64 candidate_head = ReadValue<u64>(manager, 0x30);
        out << ",\"candidate_head\":";
        WriteHex(out, candidate_head);
        out << ",\"candidate_count\":" << ReadValue<u64>(manager, 0x38);
        const u64 pending_head = ReadValue<u64>(manager, 0x1A8);
        out << ",\"pending_head\":";
        WriteHex(out, pending_head);
        out << ",\"pending_count\":" << ReadValue<u64>(manager, 0x1B0);
        out << ",\"request_handle\":";
        WriteHex(out, ReadValue<u64>(manager, 0x138));
        if (pending_head >= 0x10000) {
            const u64 node = ReadValue<u64>(pending_head, 0);
            out << ",\"pending_first\":";
            WriteHex(out, node == pending_head ? 0 : node);
            if (node >= 0x10000 && node != pending_head) {
                const u64 item = ReadValue<u64>(node, 0x10);
                out << ",\"pending_item\":";
                WriteHex(out, item);
                if (item >= 0x10000) {
                    const u64 candidate = ReadValue<u64>(item, 0);
                    const u64 prepared = ReadValue<u64>(item, 0x08);
                    out << ",\"pending_state\":" << static_cast<u32>(ReadValue<u8>(item, 0x10));
                    out << ",\"pending_candidate\":";
                    WriteHex(out, candidate);
                    out << ",\"pending_prepared\":";
                    WriteHex(out, prepared);
                    if (candidate >= 0x10000) {
                        const s32 role_code = ReadValue<s8>(candidate, 0x08);
                        out << ",\"pending_role_code\":" << role_code;
                        WriteRoleSelectionMask(out, "pending_role_selection_mask", role_code);
                    }
                    if (prepared >= 0x10000) {
                        out << ",\"pending_prepared_role_code\":"
                            << static_cast<u32>(ReadValue<u8>(prepared, 0x22));
                    }
                }
            }
        }
    }
    if (is_match && registers.rsi >= 0x10000) {
        const u64 candidate = ReadValue<u64>(registers.rsi, 0);
        const u64 prepared = ReadValue<u64>(registers.rsi, 0x08);
        out << ",\"matched_item\":";
        WriteHex(out, registers.rsi);
        out << ",\"matched_candidate\":";
        WriteHex(out, candidate);
        out << ",\"matched_prepared\":";
        WriteHex(out, prepared);
        if (candidate >= 0x10000) {
            const s32 role_code = ReadValue<s8>(candidate, 0x08);
            out << ",\"matched_role_code\":" << role_code;
            WriteRoleSelectionMask(out, "matched_role_selection_mask", role_code);
        }
        if (prepared >= 0x10000) {
            out << ",\"matched_prepared_role_code\":"
                << static_cast<u32>(ReadValue<u8>(prepared, 0x22));
        }
    }
    if (status >= 0x10000) {
        out << ",\"queued_count\":" << ReadValue<u64>(status, 0x78);
        out << ",\"request_state_88\":" << ReadValue<s32>(status, 0x88);
        out << ",\"active_request_id\":" << ReadValue<s32>(status, 0x90);
    }
    if (is_start) {
        out << ",\"target_id\":" << static_cast<u32>(registers.rdi);
        out << ",\"session_id\":" << static_cast<u32>(registers.rsi);
        out << ",\"request_context\":";
        WriteHex(out, registers.rdx);
    } else if (is_allocated) {
        out << ",\"allocated_handle\":";
        WriteHex(out, registers.rax);
    } else if (is_queue_insert) {
        out << ",\"queue_object\":";
        WriteHex(out, registers.rsi);
    }
    out << '}';
}

void WriteSummonBuild(std::ostream& out, const TraceSite& site,
                      const GuestRegisterSnapshot& registers) {
    u64 prepared{};
    switch (site.offset) {
    case SummonBuildEntryOffset:
        prepared = registers.rsi;
        break;
    case 0x01874C94:
        prepared = registers.r15;
        break;
    case 0x01874E5A:
        prepared = registers.rbx;
        break;
    case 0x018750F7:
        prepared = registers.rbx;
        break;
    default:
        prepared = registers.r8;
        break;
    }

    out << ",\"summon_build\":{";
    out << "\"stage\":\"" << site.name << "\"";
    out << ",\"prepared\":";
    WriteHex(out, prepared);
    if (site.offset == SummonBuildEntryOffset) {
        if (host_placement_rewrite.prepared == prepared) {
            const auto write_placement = [&out](std::string_view name,
                                                const SummonPlacementDescriptor& placement) {
                out << ",\"" << name << "\":{";
                out << "\"packed_region\":";
                WriteHex(out, placement.packed_region);
                out << ",\"area\":" << placement.area;
                out << ",\"x\":" << placement.x;
                out << ",\"y\":" << placement.y;
                out << ",\"z\":" << placement.z;
                out << ",\"heading\":" << placement.heading << '}';
            };
            out << ",\"host_placement_rewrite\":{\"result\":\"" << host_placement_rewrite.result
                << '\"';
            write_placement("candidate", host_placement_rewrite.candidate);
            write_placement("host", host_placement_rewrite.host);
            out << '}';
        }
        out << ",\"status\":";
        WriteHex(out, registers.rdi);
        out << ",\"destructive_reject\":" << static_cast<u32>(registers.rdx & 0xFF);
        if (registers.rdi >= 0x10000) {
            out << ",\"status_extra_players\":" << ReadValue<s32>(registers.rdi, 0x1C8);
        }

        const u64 matching_state = GetMatchingState();
        out << ",\"matching_state\":";
        WriteHex(out, matching_state);
        if (matching_state >= 0x10000) {
            out << ",\"matching_state_124\":" << ReadValue<s32>(matching_state, 0x124);
        }

        const u64 session_rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
        out << ",\"session_rules\":";
        WriteHex(out, session_rules);
        if (session_rules >= 0x10000) {
            out << ",\"session_capacity\":" << ReadValue<s32>(session_rules, 0x0C);
        }

        const u64 global_state = ReadValue<u64>(image_base + GlobalStatePointerOffset, 0);
        const u64 population = global_state >= 0x10000 ? ReadValue<u64>(global_state, 0x16F8) : 0;
        out << ",\"global_state\":";
        WriteHex(out, global_state);
        out << ",\"online_ready\":" << static_cast<u32>(ReadValue<u8>(global_state, 0x1592));
        out << ",\"population\":";
        WriteHex(out, population);
        if (population >= 0x10000) {
            out << ",\"population_08\":" << ReadValue<s32>(population, 0x08);
            out << ",\"population_0c\":" << ReadValue<s32>(population, 0x0C);
            out << ",\"population_10\":" << ReadValue<s32>(population, 0x10);
            out << ",\"population_14\":" << ReadValue<s32>(population, 0x14);
            out << ",\"population_request_98\":" << ReadValue<s32>(population, 0x98);
        }

        const u64 local_state = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        const u64 local_world = local_state >= 0x10000 ? ReadValue<u64>(local_state, 0x60) : 0;
        out << ",\"local_state\":";
        WriteHex(out, local_state);
        out << ",\"local_world\":";
        WriteHex(out, local_world);
        if (local_world >= 0x10000) {
            out << ",\"local_world_state\":" << ReadValue<s32>(local_world, 0x78);
        }

        const u64 manager_root = GetSummonManagerRoot();
        out << ",\"manager_root\":";
        WriteHex(out, manager_root);
        if (manager_root >= 0x10000) {
            out << ",\"manager_area\":" << ReadValue<s32>(manager_root, 0xA80);
            out << ",\"manager_flags_b14\":";
            WriteHex(out, ReadValue<u8>(manager_root, 0xB14));
        }
    }
    if (site.offset == 0x018750F7) {
        out << ",\"result\":";
        WriteHex(out, registers.rbx);
    }

    if (prepared >= 0x10000) {
        const u32 role_code = ReadValue<u8>(prepared, 0x22);
        out << ",\"role_code\":" << role_code;
        out << ",\"packed_region\":";
        WriteHex(out, ReadValue<u32>(prepared, 0x04));
        out << ",\"area\":" << ReadValue<s32>(prepared, 0x18);
        out << ",\"request_id\":" << ReadValue<s32>(prepared, 0x88);

        if (role_code < RoleMetadataCount) {
            const u64 metadata = image_base + RoleMetadataTableOffset +
                                 static_cast<u64>(role_code) * RoleMetadataStride;
            const u64 build_role =
                image_base + SummonBuildRoleTableOffset + static_cast<u64>(role_code) * 12;
            out << ",\"selection_mask\":";
            WriteHex(out, ReadValue<u32>(metadata, 0x04));
            out << ",\"role_flags\":";
            WriteHex(out, ReadValue<u8>(metadata, 0x08));
            out << ",\"capacity_class\":" << ReadValue<s32>(metadata, 0x10);
            out << ",\"role_rule\":" << static_cast<u32>(ReadValue<u8>(metadata, 0x14));
            out << ",\"delay_enabled\":" << static_cast<u32>(ReadValue<u8>(build_role, 0));
            out << ",\"delay_seconds\":" << ReadValue<float>(build_role, 0x04);
        }
    }
    out << '}';
}

void WriteMessengerState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"messenger\":{";
    out << "\"request\":";
    WriteHex(out, ReadValue<u64>(object, 0xE0));
    out << ",\"waiting\":" << static_cast<u32>(ReadValue<u8>(object, 0xF0));
    out << ",\"flags_144\":";
    WriteHex(out, ReadValue<u8>(object, 0x144));
    out << '}';
}

bool HasActionTransition(u64 object) {
    if (object < 0x10000) {
        return false;
    }
    const u64 current = ReadValue<u64>(object, 0x10);
    const u64 previous = ReadValue<u64>(object, 0x18);
    return current != previous;
}

void WriteActionFlags(std::ostream& out, u64 object, u64 stack_pointer) {
    if (object < 0x10000 || stack_pointer < 0x10000) {
        return;
    }
    const u64 current = ReadValue<u64>(object, 0x10);
    const u64 previous = ReadValue<u64>(object, 0x18);
    const u64 return_address = ReadValue<u64>(stack_pointer, 0);
    out << ",\"action_flags\":{";
    out << "\"module\":";
    WriteHex(out, object);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '\"';
    }
    out << ",\"current\":";
    WriteHex(out, current);
    out << ",\"previous\":";
    WriteHex(out, previous);
    out << ",\"pressed\":";
    WriteHex(out, current & ~previous);
    out << ",\"released\":";
    WriteHex(out, previous & ~current);
    out << ",\"field_30\":";
    WriteHex(out, ReadValue<u64>(object, 0x30));
    out << ",\"field_38\":";
    WriteHex(out, ReadValue<u64>(object, 0x38));
    out << ",\"field_40\":";
    WriteHex(out, ReadValue<u64>(object, 0x40));
    out << ",\"field_48\":";
    WriteHex(out, ReadValue<u64>(object, 0x48));
    out << ",\"state_90\":";
    WriteHex(out, ReadValue<u32>(object, 0x90));
    out << ",\"state_94\":";
    WriteHex(out, ReadValue<u32>(object, 0x94));
    out << ",\"flags_9c\":";
    WriteHex(out, ReadValue<u8>(object, 0x9C));
    out << '}';
}

void WritePlayerItemState(std::ostream& out, u64 player) {
    out << "\"player\":";
    WriteHex(out, player);
    if (player >= 0x10000) {
        const u64 vtable = ReadValue<u64>(player, 0);
        out << ",\"vtable\":";
        WriteHex(out, vtable);
        if (vtable >= 0x10000) {
            out << ",\"vfunc_1c0\":";
            WriteHex(out, ReadValue<u64>(vtable, 0x1C0));
            out << ",\"vfunc_1c8\":";
            WriteHex(out, ReadValue<u64>(vtable, 0x1C8));
        }
        out << ",\"entity_60\":";
        WriteHex(out, ReadValue<u64>(player, 0x60));
        out << ",\"handle_90\":";
        WriteHex(out, ReadValue<u32>(player, 0x90));
        out << ",\"active_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x1C0));
        out << ",\"resolved_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x478));
        out << ",\"override_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x47C));
        out << ",\"queued_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x488));
        out << ",\"item_state\":";
        WriteHex(out, ReadValue<u32>(player, 0x490));
        WriteMultiplayerEffects(out, player);
    }
}

void WriteUseItemState(std::ostream& out, const TraceSite& site,
                       const GuestRegisterSnapshot& registers) {
    out << ",\"use_item\":{";
    WritePlayerItemState(out, registers.r13);
    out << ",\"rax\":";
    WriteHex(out, registers.rax);
    out << ",\"eax_signed\":" << static_cast<s64>(static_cast<s32>(registers.rax));
    if (site.offset == 0x018F751C && registers.rax >= 0x10000) {
        const u64 current = ReadValue<u64>(registers.rax, 0x10);
        const u64 previous = ReadValue<u64>(registers.rax, 0x18);
        out << ",\"action_current\":";
        WriteHex(out, current);
        out << ",\"action_previous\":";
        WriteHex(out, previous);
        out << ",\"action_pressed\":";
        WriteHex(out, ReadValue<u64>(registers.rax, 0x20));
    }
    if (site.offset == 0x018F7529 && registers.rax >= 0x10000) {
        out << ",\"inventory\":";
        WriteHex(out, registers.rax);
        out << ",\"inventory_selection\":[";
        for (size_t index = 0; index < 8; ++index) {
            if (index != 0) {
                out << ',';
            }
            WriteHex(out, ReadValue<u64>(registers.rax, 0x1D0 + index * sizeof(u64)));
        }
        out << ']';
    }
    out << '}';
}

bool IsTrackedGoods(s32 goods_id) {
    constexpr std::array TrackedGoods{100, 111, 200, 201, 205, 206, 225, 226};
    return std::ranges::find(TrackedGoods, goods_id) != TrackedGoods.end();
}

s32 GetUseItemExecutionGoods(const TraceSite& site, const GuestRegisterSnapshot& registers) {
    if (site.offset == 0x018C9364) {
        return registers.rbp >= 0xD8 ? ReadValue<s32>(registers.rbp - 0xD8, 0) : -1;
    }
    return static_cast<s32>(registers.rsi);
}

void WriteUseItemExecution(std::ostream& out, const TraceSite& site,
                           const GuestRegisterSnapshot& registers) {
    const bool is_result = site.offset == 0x018C9364;
    const u64 player = is_result ? registers.r14 : registers.rdi;
    const s32 goods_id = GetUseItemExecutionGoods(site, registers);
    const s32 argument = static_cast<s32>(is_result ? registers.rbx : registers.rdx);
    out << ",\"use_item_execution\":{";
    WritePlayerItemState(out, player);
    out << ",\"goods_id\":" << goods_id;
    out << ",\"argument\":" << argument;
    if (is_result) {
        out << ",\"result\":" << static_cast<u32>(static_cast<u8>(registers.r12));
    }
    out << '}';
}

void WriteGoodsActionSubmit(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"goods_action\":{";
    out << "\"player\":";
    WriteHex(out, registers.rdi);
    out << ",\"goods_id\":" << static_cast<s32>(registers.rsi);
    out << ",\"argument\":" << static_cast<s32>(registers.rdx);
    out << ",\"sp_effect_id\":" << static_cast<s32>(registers.rcx);
    out << ",\"executor_allowed\":" << static_cast<u32>(static_cast<u8>(registers.r8));
    WriteMultiplayerEffects(out, registers.rdi);
    out << '}';
}

void WriteGoodsParamLookup(std::ostream& out, const GuestRegisterSnapshot& registers) {
    const s32 goods_id = static_cast<s32>(registers.rsi);
    const u64 return_address = ReadValue<u64>(registers.rsp, 0);
    out << ",\"goods_lookup\":{";
    out << "\"goods_id\":" << goods_id;
    out << ",\"sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
    out << ",\"result_storage\":";
    WriteHex(out, registers.rdi);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '\"';
    }
    out << '}';
}

void WriteBellAvailability(std::ostream& out, const TraceSite& site,
                           const GuestRegisterSnapshot& registers) {
    const bool is_native_result = site.offset == 0x0157F962;
    const u64 selected_area = static_cast<u8>(registers.rax) != 0 ? registers.rcx : registers.rbx;
    const s32 area =
        is_native_result ? static_cast<s32>(registers.rbx) : ReadValue<s32>(selected_area, 0);
    out << ",\"bell_availability\":{";
    out << "\"area\":" << area;
    out << ",\"observed_sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
    out << ",\"player\":";
    WriteHex(out, registers.r14);
    if (is_native_result) {
        out << ",\"native_blocked\":" << static_cast<u32>(static_cast<u8>(registers.rax));
    }
    WriteMultiplayerEffects(out, registers.r14);
    out << '}';
}

void WriteResponderBellAvailability(std::ostream& out, const TraceSite& site,
                                    const GuestRegisterSnapshot& registers, s32 goods_id) {
    out << ",\"responder_bell_availability\":{";
    out << "\"stage\":\"" << site.name << "\"";
    out << ",\"goods_id\":" << goods_id;
    out << ",\"player\":";
    WriteHex(out, registers.r14);
    out << ",\"original_goods_argument\":" << static_cast<s32>(registers.rbx);
    out << ",\"resolved_goods_id\":" << static_cast<s32>(registers.r12);
    if (site.offset == 0x015800D1) {
        out << ",\"final_result\":" << static_cast<u32>(static_cast<u8>(registers.rcx));
    }
    out << '}';
    if (site.offset == 0x0157F686) {
        WriteMultiplayerEffects(out, registers.r14);
    }
}

void WriteQwords(std::ostream& out, std::string_view key, u64 address, size_t count) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":[";
    for (size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << ',';
        }
        WriteHex(out, ReadValue<u64>(address, index * sizeof(u64)));
    }
    out << ']';
}

void WriteFloatVector(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":[";
    for (size_t index = 0; index < 4; ++index) {
        if (index != 0) {
            out << ',';
        }
        const float value = ReadValue<float>(address, index * sizeof(float));
        if (std::isfinite(value)) {
            out << std::setprecision(9) << value;
        } else {
            out << "null";
        }
    }
    out << ']';
}

void WriteMapId(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    const u32 map_id = ReadValue<u32>(address, 0);
    out << ",\"" << key << "\":";
    WriteHex(out, map_id);
    out << ",\"" << key << "_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF)
        << ',' << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
}

void WriteU32(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":";
    WriteHex(out, ReadValue<u32>(address, 0));
}

void WritePlacementState(std::ostream& out, u64 state) {
    WriteQwords(out, "global_qwords", state, 6);
    WriteQwords(out, "sos_placement_qwords", state + 0x14A0, 5);
    WriteQwords(out, "summoned_placement_qwords", state + 0x14D0, 5);
    WriteQwords(out, "warp_qwords", state + 0x1518, 3);
    WriteQwords(out, "received_placement_qwords", state + 0x1620, 6);
}

void WriteStageWarpState(std::ostream& out, u64 state) {
    if (state < 0x10000) {
        return;
    }
    out << ",\"stage_warp_state\":{";
    out << "\"state\":";
    WriteHex(out, state);
    out << ",\"request_active\":" << static_cast<u32>(ReadValue<u8>(state, 0x08));
    const u32 map_id = ReadValue<u32>(state, 0x0C);
    out << ",\"map_id\":";
    WriteHex(out, map_id);
    out << ",\"map_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF) << ','
        << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
    out << ",\"warp_info_id\":" << ReadValue<s32>(state, 0x10);
    out << ",\"mode_1538\":" << ReadValue<s32>(state, 0x1538);
    out << ",\"respawn_point_id_153c\":" << ReadValue<s32>(state, 0x153C);
    out << ",\"auxiliary_id_1540\":" << ReadValue<s32>(state, 0x1540);
    out << ",\"warp_fields_153c\":";
    WriteHex(out, ReadValue<u64>(state, 0x153C));
    out << ",\"state_1592\":" << static_cast<u32>(ReadValue<u8>(state, 0x1592));
    out << ",\"transition_16f8\":";
    WriteHex(out, ReadValue<u64>(state, 0x16F8));
    out << '}';
}

void WriteStageWarp(std::ostream& out, const TraceSite& site,
                    const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp\":{";
    if (site.offset == 0x0132E010) {
        const u32 map_id = (static_cast<u32>(registers.rsi) & 0xFF) << 24 |
                           (static_cast<u32>(registers.rdx) & 0xFF) << 16 |
                           (static_cast<u32>(registers.rcx) & 0xFF) << 8 |
                           (static_cast<u32>(registers.r8) & 0xFF);
        out << "\"source\":\"event\",\"map_id\":";
        WriteHex(out, map_id);
        out << ",\"map_parts\":[" << (registers.rsi & 0xFF) << ',' << (registers.rdx & 0xFF) << ','
            << (registers.rcx & 0xFF) << ',' << (registers.r8 & 0xFF) << ']';
        out << ",\"warp_info_id\":" << static_cast<s32>(registers.r9);
    } else if (site.offset == 0x0132E050) {
        const s32 warp_info_id = static_cast<s32>(registers.rsi);
        const s32 area = warp_info_id / 100000;
        const s32 block = (warp_info_id / 10000) % 10;
        out << "\"source\":\"bonfire_event\",\"warp_info_id\":" << warp_info_id;
        out << ",\"derived_map_parts\":[" << area << ',' << block << ",0,0]";
    } else {
        out << "\"source\":\"transition\"";
    }
    const u64 player = GetLocalPlayer();
    out << ",\"current_map\":";
    WriteHex(out, GetCurrentPackedMap());
    out << ",\"player\":";
    WriteHex(out, player);
    out << ",\"host_effect_9001\":" << (HasPlayerEffect(player, 9001) ? "true" : "false");
    out << '}';
    WriteStageWarpState(out, GetGlobalState());
}

void WriteStageWarpDescriptor(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp_descriptor\":{";
    out << "\"descriptor\":";
    WriteHex(out, registers.r9);
    out << ",\"mode\":" << static_cast<s32>(registers.r13);
    if (registers.r9 >= 0x10000) {
        out << ",\"request_type\":" << ReadValue<s32>(registers.r9, 0x54);
        out << ",\"request_subtype\":" << ReadValue<s32>(registers.r9, 0x58);
        out << ",\"descriptor_mode\":" << ReadValue<s32>(registers.r9, 0x98);
        out << ",\"previous_respawn_point\":" << ReadValue<s32>(registers.r9, 0x80);
        out << ",\"previous_auxiliary_id\":" << ReadValue<s32>(registers.r9, 0x84);
    }
    out << '}';
    WriteStageWarpState(out, registers.rax);
}

void WriteHealingFountainRegistration(std::ostream& out, const GuestRegisterSnapshot& registers) {
    float reaction_distance{};
    float reaction_angle{};
    std::memcpy(&reaction_distance, registers.ymm[0].data(), sizeof(reaction_distance));
    std::memcpy(&reaction_angle, registers.ymm[1].data(), sizeof(reaction_angle));

    out << ",\"healing_fountain_registration\":{";
    out << "\"manager\":";
    WriteHex(out, registers.rdi);
    out << ",\"event_flag\":" << static_cast<s32>(registers.rsi);
    out << ",\"entity_id\":" << static_cast<s32>(registers.rdx);
    out << ",\"reaction_distance\":";
    if (std::isfinite(reaction_distance)) {
        out << std::setprecision(9) << reaction_distance;
    } else {
        out << "null";
    }
    out << ",\"reaction_angle\":";
    if (std::isfinite(reaction_angle)) {
        out << std::setprecision(9) << reaction_angle;
    } else {
        out << "null";
    }
    out << ",\"initial_sword_number\":" << static_cast<s32>(registers.r8);
    out << ",\"sword_level\":" << static_cast<s32>(registers.rcx);
    out << '}';
}

void WriteHealingFountainAvailability(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"healing_fountain_availability\":{";
    out << "\"result\":\"" << healing_fountain_availability.result << '\"';
    out << ",\"object\":";
    WriteHex(out, healing_fountain_availability.object);
    out << ",\"player\":";
    WriteHex(out, healing_fountain_availability.player);
    out << ",\"host_effect_9001\":"
        << (healing_fountain_availability.host_effect ? "true" : "false");
    out << ",\"blocked_before\":"
        << (healing_fountain_availability.blocked_before ? "true" : "false");
    out << ",\"blocked_after\":"
        << (healing_fountain_availability.blocked_after ? "true" : "false");
    if (registers.r13 >= 0x10000 && HasMemoryAccess(registers.r13, 0x4C, MemoryProt::CpuRead)) {
        out << ",\"special_map_48\":" << static_cast<u32>(ReadValue<u8>(registers.r13, 0x48));
        out << ",\"multiplayer_49\":" << static_cast<u32>(ReadValue<u8>(registers.r13, 0x49));
        out << ",\"entity_missing_4a\":" << static_cast<u32>(ReadValue<u8>(registers.r13, 0x4A));
        out << ",\"native_gate_4b\":" << static_cast<u32>(ReadValue<u8>(registers.r13, 0x4B));
    }
    out << '}';
}

void WriteChairRespawnNotification(std::ostream& out, const GuestRegisterSnapshot& registers) {
    const u64 task = registers.rdi;
    const u64 manager = GetSummonManagerRoot();
    const u64 online_state =
        image_base != 0 ? ReadValue<u64>(image_base + OnlineStatePointerOffset, 0) : 0;

    out << ",\"chair_messenger_respawn_notify\":{";
    out << "\"task\":";
    WriteHex(out, task);
    if (task >= 0x10000 && HasMemoryAccess(task, 0xDC, MemoryProt::CpuRead)) {
        out << ",\"state_50\":" << ReadValue<s32>(task, 0x50);
        out << ",\"step_54\":" << ReadValue<s32>(task, 0x54);
        out << ",\"wait_active_c8\":" << static_cast<u32>(ReadValue<u8>(task, 0xC8));
        out << ",\"selected_respawn_point_cc\":" << ReadValue<s32>(task, 0xCC);
        const float timer = ReadValue<float>(task, 0xD8);
        out << ",\"timer_d8\":";
        if (std::isfinite(timer)) {
            out << std::setprecision(9) << timer;
        } else {
            out << "null";
        }
    }

    out << ",\"manager\":";
    WriteHex(out, manager);
    if (manager >= 0x10000 && HasMemoryAccess(manager, 0xA84, MemoryProt::CpuRead)) {
        out << ",\"active_08\":" << static_cast<u32>(ReadValue<u8>(manager, 0x08));
        out << ",\"state_9f4\":";
        WriteHex(out, ReadValue<u64>(manager, 0x9F4));
        out << ",\"lan_connected_9f5\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9F5));
        out << ",\"lan_disconnected_9f6\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9F6));
        out << ",\"signed_in_9f7\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9F7));
        out << ",\"signed_out_9f8\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9F8));
        out << ",\"state_9f9\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9F9));
        out << ",\"state_9fa\":" << static_cast<u32>(ReadValue<u8>(manager, 0x9FA));
        out << ",\"network_flags_a05\":";
        WriteHex(out, ReadValue<u8>(manager, 0xA05));
        out << ",\"gate_a50\":" << static_cast<u32>(ReadValue<u8>(manager, 0xA50));
        out << ",\"map_a78\":";
        WriteHex(out, ReadValue<u32>(manager, 0xA78));
        out << ",\"area_a80\":" << ReadValue<s32>(manager, 0xA80);
        out << ",\"latch_d30\":" << static_cast<u32>(ReadValue<u8>(manager, 0xD30));
        out << ",\"latch_d31\":" << static_cast<u32>(ReadValue<u8>(manager, 0xD31));
    }
    out << ",\"online_state\":";
    WriteHex(out, online_state);
    if (online_state >= 0x10000 && HasMemoryAccess(online_state, 0x130, MemoryProt::CpuRead)) {
        out << ",\"online_d4\":" << ReadValue<s32>(online_state, 0xD4);
        out << ",\"online_d8\":" << static_cast<u32>(ReadValue<u8>(online_state, 0xD8));
        out << ",\"online_e0\":" << static_cast<u32>(ReadValue<u8>(online_state, 0xE0));
        out << ",\"online_120\":" << ReadValue<s32>(online_state, 0x120);
        out << ",\"online_124\":" << ReadValue<s32>(online_state, 0x124);
        out << ",\"online_128\":" << ReadValue<s32>(online_state, 0x128);
        out << ",\"online_12c\":" << ReadValue<s32>(online_state, 0x12C);
    }
    out << '}';
}

void WriteStageWarpAcknowledge(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp_acknowledge\":{";
    out << "\"state\":";
    WriteHex(out, registers.rcx);
    const bool frame_valid = registers.rbp >= 0x198 + 0x10000;
    if (frame_valid) {
        const bool will_clear_request = ReadValue<u8>(registers.rbp - 0x198, 0) != 0;
        out << ",\"will_clear_request\":" << (will_clear_request ? "true" : "false");
    }
    out << '}';
    WriteStageWarpState(out, registers.rcx);
}

void WriteStageWarpTask(std::ostream& out, u64 task, bool include_resolved_transform) {
    if (task < 0x10000) {
        return;
    }

    const u32 map_id = ReadValue<u32>(task, 0x58);
    out << ",\"stage_warp_task\":{";
    out << "\"task\":";
    WriteHex(out, task);
    out << ",\"map_id\":";
    WriteHex(out, map_id);
    out << ",\"map_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF) << ','
        << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
    out << ",\"request_type\":" << ReadValue<s32>(task, 0xA4);
    out << ",\"request_subtype\":" << ReadValue<s32>(task, 0xA8);
    out << ",\"respawn_point_id\":" << ReadValue<s32>(task, 0xD0);
    out << ",\"auxiliary_id\":" << ReadValue<s32>(task, 0xD4);
    out << ",\"mode\":" << ReadValue<s32>(task, 0xE8);
    out << '}';
    if (include_resolved_transform) {
        WriteFloatVector(out, "resolved_position", task + 0xF0);
        const float facing = ReadValue<float>(task, 0x100);
        out << ",\"resolved_facing\":";
        if (std::isfinite(facing)) {
            out << std::setprecision(9) << facing;
        } else {
            out << "null";
        }
    }
}

void WriteRespawnTransformResolve(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"respawn_transform_resolve\":{";
    out << "\"map_resource\":";
    WriteHex(out, registers.rdi);
    out << ",\"position_output\":";
    WriteHex(out, registers.rsi);
    out << ",\"orientation_output\":";
    WriteHex(out, registers.rdx);
    out << ",\"respawn_point_id\":" << static_cast<s32>(registers.rcx);
    out << '}';
}

u64 GetGlobalState() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        GlobalStatePointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + GlobalStatePointerOffset, 0);
}

// Lowest address the param / code scans search. The shadp2p fork keeps all game memory above 4 GB;
// the official shadPS4 nightly (2026-10-09) loads the eboot at 0x9B00000 and its modules low too.
u64 BbscScanLow() {
    return image_base != 0 && image_base < 0x100000000ULL ? 0x1000000ULL : 0x100000000ULL;
}

bool IsEbootCodeAddress(u64 address) {
    return image_base != 0 && address > image_base &&
           address - image_base < MemoryPatcher::g_eboot_image_size;
}

// Printable ASCII prefix of a guest C string, or empty when unreadable.
std::string ReadGuestAscii(u64 address, size_t max_length) {
    std::string text;
    if (address < 0x10000) {
        return text;
    }
    for (size_t i = 0; i < max_length; ++i) {
        if (!HasMemoryAccess(address + i, 1, MemoryProt::CpuRead)) {
            break;
        }
        const char c = static_cast<char>(ReadValue<u8>(address, i));
        if (c == '\0') {
            break;
        }
        text.push_back(c >= 0x20 && c < 0x7F && c != '"' && c != '\\' ? c : '?');
    }
    return text;
}

// Engine.Panic: rbp frame chain (Bloodborne functions keep push rbp; mov rbp,rsp frames) plus a
// raw scan of the stack for eboot return addresses, in case a frame omits rbp.
void WriteEnginePanicStack(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"engine_panic\":{\"file\":\"" << ReadGuestAscii(registers.rdi, 96)
        << "\",\"line\":" << static_cast<s32>(registers.rsi) << ",\"format\":\""
        << ReadGuestAscii(registers.rdx, 160) << "\",\"frames\":[";
    bool first = true;
    auto write_offset = [&](u64 address) {
        out << (first ? "" : ",") << "\"0x" << std::hex << address - image_base << std::dec
            << '\"';
        first = false;
    };
    if (HasMemoryAccess(registers.rsp, sizeof(u64), MemoryProt::CpuRead)) {
        const u64 return_address = ReadValue<u64>(registers.rsp, 0);
        if (IsEbootCodeAddress(return_address)) {
            write_offset(return_address);
        }
    }
    u64 frame = registers.rbp;
    for (int depth = 0; depth < 32; ++depth) {
        if (frame < 0x10000 || !HasMemoryAccess(frame, 2 * sizeof(u64), MemoryProt::CpuRead)) {
            break;
        }
        const u64 return_address = ReadValue<u64>(frame, 8);
        if (!IsEbootCodeAddress(return_address)) {
            break;
        }
        write_offset(return_address);
        const u64 next = ReadValue<u64>(frame, 0);
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    out << "],\"stack_scan\":[";
    first = true;
    int found = 0;
    for (u64 slot = 0; slot < 0x1000 && found < 64; slot += sizeof(u64)) {
        if (!HasMemoryAccess(registers.rsp + slot, sizeof(u64), MemoryProt::CpuRead)) {
            break;
        }
        const u64 value = ReadValue<u64>(registers.rsp, slot);
        if (IsEbootCodeAddress(value)) {
            write_offset(value);
            ++found;
        }
    }
    out << "]}";
}

// The guest's loads under the death curtain's fade, see "Load cover".
void StartBbscLoadCover(std::string_view why);
void EndBbscLoadCover(std::string_view how);
// The host's world state for a follow load, see "World state when warping together".
bool BbscHostWorldFlagsReady(u32 packed_map);
bool ApplyBbscHostWorldFlags(u32 packed_map, std::string_view why);
void LogBbscHostWorldFlagsDiff(u32 packed_map, std::string_view why);
void ApplyBbscGuestLootView(u32 packed_map, std::string_view why); // see "Guest loot view"
void KeepBbscOwnWorldGroup(u32 key, const u8* live); // see "Own world journal"
void CaptureBbscOwnWorldBeforeJoin();
void WriteBbscOwnWorldFile();
void NoteBbscOwnDreamRestored(u32 key, const u8* live);
s32 GetSessionState();
u32 GetCurrentPackedMap();
// The map as of the last game tick, for code on other threads (the relay's placement packets):
// the map list is rebuilt during loads, so walking it off the game thread can read freed memory.
std::atomic<u32> bbsc_tick_map{0};

void BbscLeaveItemUsed(std::string_view event); // see "Silencing Blank" below
void SuspendBbscPersistenceOnce(std::string_view why); // see "quit to title" below

void WriteLuaSessionEvent(std::ostream& out, const GuestRegisterSnapshot& registers) {
    const std::string name = ReadGuestAscii(registers.rsi, 64);
    const u64 return_address = ReadValue<u64>(registers.rsp, 0);
    out << ",\"lua_event\":{\"name\":\"" << name << "\",\"caller_offset\":\"0x" << std::hex
        << (return_address >= image_base ? return_address - image_base : 0) << std::dec << "\"}";
    LOG_INFO(Debug, "BBSC lua event: {} (caller {:#x})", name,
             return_address >= image_base ? return_address - image_base : 0);
    // The guest accepted a summon: its summon load is next (see "Load cover").
    if (name == "JoinSession_White" && GetSessionState() != 3) {
        CaptureBbscOwnWorldBeforeJoin(); // first: the load resets the map's item flags
        StartBbscLoadCover("summon");
    }
    if (name == "OnLeaveMagic" || name == "OnLeaveMenu_Yes") {
        BbscLeaveItemUsed(name);
    }
}

void WriteWorldStateValidation(std::ostream& out, const TraceSite& site,
                               const GuestRegisterSnapshot& registers) {
    s32 area = -1;
    switch (site.offset) {
    case 0x018C5365:
        area = ReadValue<s32>(registers.rbp - 0x38, 0);
        break;
    case 0x018CF97A:
        area = ReadValue<s32>(registers.rbp - 0x1A8, 0);
        break;
    case 0x0191A980:
    case 0x0191AA71:
        area = static_cast<s32>(registers.rbx);
        break;
    case 0x01AC6557:
    case 0x01AC661E:
        area = ReadValue<s32>(registers.rbx, 0);
        break;
    case 0x01FEB611:
    case 0x01FEBD01:
    case 0x01FEC381:
        area = ReadValue<s32>(registers.rbp - 0x1B8, 0);
        break;
    }

    const u64 return_address = ReadValue<u64>(registers.rbp, sizeof(u64));
    out << ",\"world_state_validation\":{";
    out << "\"call_offset\":\"0x" << std::hex << site.offset << std::dec << '"';
    out << ",\"result_low8\":" << static_cast<u32>(static_cast<u8>(registers.rax));
    out << ",\"area\":" << area;
    out << ",\"manager_area\":";
    const u64 root = GetSummonManagerRoot();
    out << (root >= 0x10000 ? ReadValue<s32>(root, 0xA80) : -1);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '"';
    }
    out << '}';
}

// BBSC experimental re-insertion (docs/RESEARCH-NOTES.md, Phase 2). A kept structural session end
// (BBSC.Persist.SkippedCleanup) arms a one-shot replay of the room members once the stage request is
// consumed and SHADPS4_BBSC_REINSERT_DELAY_MS has passed. Mode: SHADPS4_BBSC_REINSERT=joined|leftjoined.
constexpr u64 BbscPersistNoOpOffset = 0x013001E0;
std::atomic<s64> bbsc_reinsert_armed_ms{0};

s64 SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void MaybeRunBbscReinsert();
std::atomic<bool> bbsc_reinsert_timer_running{false};

void ArmBbscReinsert() {
    const char* mode = std::getenv("SHADPS4_BBSC_REINSERT");
    if (mode == nullptr || mode[0] == '\0' || std::string_view{mode} == "0") {
        return;
    }
    bbsc_reinsert_armed_ms.store(SteadyNowMs(), std::memory_order_relaxed);
    LOG_INFO(Debug, "BBSC reinsert armed after kept session end (mode={})", mode);

    // Own timer thread: SosStatus.Update mostly runs while a bell is active, and death silences it.
    bool expected = false;
    if (!bbsc_reinsert_timer_running.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread([] {
        const s64 give_up = SteadyNowMs() + 60'000;
        while (bbsc_reinsert_armed_ms.load(std::memory_order_relaxed) != 0 && SteadyNowMs() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            MaybeRunBbscReinsert();
        }
        if (bbsc_reinsert_armed_ms.exchange(0) != 0) {
            LOG_WARNING(Debug, "BBSC reinsert gave up: stage request never cleared within 60 s");
        }
        bbsc_reinsert_timer_running.store(false);
    }).detach();
}

void MaybeRunBbscReinsert() {
    const s64 armed = bbsc_reinsert_armed_ms.load(std::memory_order_relaxed);
    if (armed == 0) {
        return;
    }
    s64 delay_ms = 6000;
    if (const char* delay = std::getenv("SHADPS4_BBSC_REINSERT_DELAY_MS")) {
        std::from_chars(delay, delay + std::strlen(delay), delay_ms);
    }
    if (SteadyNowMs() - armed < delay_ms) {
        return;
    }
    const u64 state = GetGlobalState();
    if (state < 0x10000 || ReadValue<u8>(state, 0x08) != 0) {
        return; // stage request still pending: wait for the load to be consumed
    }
    s64 expected = armed;
    if (!bbsc_reinsert_armed_ms.compare_exchange_strong(expected, 0)) {
        return;
    }
    const char* mode = std::getenv("SHADPS4_BBSC_REINSERT");
    const bool announce_left = mode != nullptr && std::string_view{mode} == "leftjoined";
    const int replayed = ::Libraries::Np::NpMatching2::BbscReplayRoomMembers(announce_left);
    LOG_INFO(Debug, "BBSC reinsert: replayed {} member(s) (announce_left={}, map={:#x})", replayed,
             announce_left, GetCurrentPackedMap());
}

// BBSC experimental guest rejoin after death (Phase 3/6), v3 (see the v3 note in ArmBbscRejoin;
// the description below is v2's design, whose forced-warp step was removed after a second panic).
// Live tests 2026-09-15:
//  * without rejoin, a kept guest death reloads the guest's OWN world (host sees it labeled
//    "Beckoner" with no collision; guest sees no host);
//  * v1 started a second StageTransition from a game tick after that load and the engine panicked
//    ("Dantelion2 Panic: SprjLuaEventMan: accessed an uninitialized singleton").
// v2 never starts a load. The kept end is observed inside the game's own death StageTransition
// (request byte armed, load descriptor not built yet). For a joined guest (CSMultiPlayMan state 6)
// the host placement received with the claim is written to the forced summon placement, the
// forced-warp selector picks the host map, and only the descriptor-finalize reload request is
// armed. The game's own respawn load then runs SummonedMapReload into the host world at the same
// point the fork's working in-room handoff uses. The request is cleared if it is not consumed.
constexpr u64 BbscGameTickOffset = 0x01308DC0;
constexpr s64 BbscRejoinDescriptorTimeoutMs = 20'000;
std::recursive_mutex bbsc_rejoin_mutex; // hooks may run on different guest threads
constexpr s64 BbscRejoinDeferredTimeoutMs = 90'000;
s64 bbsc_rejoin_armed_ms = 0;
u32 bbsc_rejoin_armed_map = 0;
bool bbsc_rejoin_deferred = false; // request moved to the in-world deferred reload (v5)

bool BbscRejoinEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_REJOIN");
    return enabled;
}

// v7 (default; SHADPS4_BBSC_REJOIN_V6=1 restores the deferred path): do what the game's own
// SummonReloadStart does, before the stage unloads. The reload native 0x0131E5C0 (called directly;
// only its Lua wrapper 0x01336B90 needs SprjLuaEventMan) snapshots characters/decals into
// global+0x1698/+0x16B8/+0x16D8 and sets summon reload phase [[0x0553D6D0]+0x84] = 3 while
// CSMultiPlayMan is in state 6. The game then (all CONFIRMED statically):
//   - stage task destructor 0x019470F0 keeps the session when phase == 3 (no Stop / 0x01E54830),
//   - load step 0x019297E0 warps to the summoned placement map global+0x14C4 when phase == 3,
//   - StageFlow player create 0x01939470 moves phase 3 -> 4 (reload finished).
bool BbscRejoinV7Enabled() {
    static const bool v6 = EnvFlagEnabled("SHADPS4_BBSC_REJOIN_V6");
    return !v6;
}

// ---- v8: keep the guest a summoned client and re-run remote-player creation -------------------
// All CONFIRMED statically (Ghidra):
//   [0x0553B130]      player-slot manager: +0x08 local slot data (+0xA4 = player type),
//                     +0x10 remote slot array (0x6A0 each), +0x8C pending type override that
//                     StageFlow player create 0x01939470 applies to +0x08->+0xA4 (then sets -1).
//   [0x05540230]      CSMultiPlayMan: +0x10/+0x18 vector of CSMultiPlayerIns* (one per member).
//   CSMultiPlayerIns  ctor 0x01E4F9F0; +0x50 current state, +0x54 next state, +0xC8 member id,
//                     +0xE8 mode (0 self-guest, 1 remote guest, 2 remote host, 3 self-host),
//                     +0xED flags (bit0 first sync ready, bit1 exit). SetState = 0x01E52560.
//                     States: 1 Init, 2 StartNotifyWait, 3 StartWait, 4 FirstSyncWait (creates the
//                     remote PlayerIns from its slot when bit0 is set), 5 Create, 6 CreateWait,
//                     7 Update, 8 WaitExit, 9 Delete, 10 DeleteWait.
//   WorldChrMan       [0x0553E878]+0x30 count, +0x38 entries (0x38 each, +0 PlayerIns*),
//                     PlayerIns+0x3D8 member id. Member-id compare = 0x00CA1C70.
constexpr u64 PlayerSlotManagerPointerOffset = 0x0553B130;
constexpr u64 MultiPlayManPointerOffset = 0x05540230;
constexpr u64 MultiPlayerInsSetStateOffset = 0x01E52560;
// Ghidra name FUN_01ca1c70 = offset 0x00CA1C70 (Ghidra base 0x01000000). 0x01CA1C70 is mid-function.
constexpr u64 MemberIdEqualsOffset = 0x00CA1C70;
constexpr s64 BbscReinsertSettleMs = 2'500;
constexpr s64 BbscReinsertTimeoutMs = 120'000;

s32 bbsc_v8_saved_type = -1;
s32 bbsc_v8_original_type = -1;     // [[0x0553B188]+4] before we swap it
bool bbsc_v8_original_swapped = false;
s32 bbsc_v8_saved_sign_type = 0;    // global+0x16F0 (summon sign type, zeroed by death handlers)
u8 bbsc_v8_saved_sign_extra = 0;    // global+0x16F4
s64 bbsc_v8_death_ms = 0;
s64 bbsc_v8_ready_since_ms = 0;
bool bbsc_v8_pending = false;

bool ReadablePointer(u64 pointer, size_t size) {
    return pointer >= 0x10000 && HasMemoryAccess(pointer, size, MemoryProt::CpuRead);
}

s32 GetLocalPlayerType() {
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    if (!ReadablePointer(slots, 0x90)) {
        return -1;
    }
    const u64 local = ReadValue<u64>(slots, 0x08);
    return ReadablePointer(local, 0xA8) ? ReadValue<s32>(local, 0xA4) : -1;
}

// Live local PlayerIns type: [[0x0553E878]+0x60]+0x78 (read by revive 0x01389D20: 1/2 phantoms).
s32 GetLocalCharacterType() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = ReadablePointer(world_chr, 0x68) ? ReadValue<u64>(world_chr, 0x60) : 0;
    return ReadablePointer(player, 0x80) ? ReadValue<s32>(player, 0x78) : -1;
}

// [[0x0553B188]+4] is the type the game restores when a phantom returns to its own world
// (0x015C9940 and 0x01941C20 both write it back over types 1/2/12/13). During a kept death the
// guest is NOT going home, so that restore is what turned the respawned guest back into type 0.
// Swapping this value makes the game's own code rebuild the character as the phantom type.
bool SetOwnWorldRestoreType(s32 type, s32* previous) {
    const u64 record = ReadValue<u64>(image_base + 0x0553B188, 0);
    if (!ReadablePointer(record, 0x08) || !HasMemoryAccess(record + 4, 4, MemoryProt::CpuWrite)) {
        return false;
    }
    if (previous != nullptr) {
        *previous = ReadValue<s32>(record, 4);
    }
    WriteValue<s32>(record, 4, type);
    return true;
}

// Remembers the summon sign code while it is set (join), for the respawn after a kept death.
void TrackBbscSignType() {
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x16F8)) {
        return;
    }
    if (const s32 sign = ReadValue<s32>(global, 0x16F0); sign != 0) {
        bbsc_v8_saved_sign_type = sign;
        bbsc_v8_saved_sign_extra = ReadValue<u8>(global, 0x16F4);
    }
}

// StageFlow player create applies these to a phantom's own character (types 1/2/12, see
// 0x019394D5..0x019394F3): clear bit 12 of the 5-byte field at +0x1E1, set bit 14. A character
// created as type 0 and re-typed afterwards lacks them.
void ApplyPhantomCharacterFlags(u64 player) {
    if (!ReadablePointer(player, 0x1E8) ||
        !HasMemoryAccess(player + 0x1E1, 5, MemoryProt::CpuWrite)) {
        return;
    }
    const u64 before = ReadValue<u64>(player, 0x1E1) & 0xFF'FFFF'FFFFull;
    const u64 after = (before & 0xFF'FFFF'AFFFull) + 0x4000;
    WriteValue<u32>(player, 0x1E1, static_cast<u32>(after));
    WriteValue<u8>(player, 0x1E5, static_cast<u8>(after >> 32));
    LOG_INFO(Debug, "BBSC reinsert v8: phantom character flags {:#x} -> {:#x}", before, after);
}

// Called from the BBSC.Type.ReloadFinish hook (0x0178D9C0 entry, before ApplySummonType runs):
// put back the summon sign type the death handlers cleared, so the respawn gets the phantom type.
void RestoreBbscSignTypeBeforeReloadFinish() {
    if (!bbsc_v8_pending || bbsc_v8_saved_type <= 0) {
        return;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x16F8) ||
        !HasMemoryAccess(global + 0x16F0, 5, MemoryProt::CpuWrite)) {
        return;
    }
    const s32 sign_now = ReadValue<s32>(global, 0x16F0);
    if (sign_now != bbsc_v8_saved_sign_type) {
        WriteValue<s32>(global, 0x16F0, bbsc_v8_saved_sign_type);
        WriteValue<u8>(global, 0x16F4, bbsc_v8_saved_sign_extra);
    }
    LOG_INFO(Debug,
             "BBSC reinsert v8: reload finish at respawn, sign type {} -> {} (slot type {}, chr {})",
             sign_now, bbsc_v8_saved_sign_type, GetLocalPlayerType(), GetLocalCharacterType());
}

std::vector<u64> GetMultiPlayerInsTasks() {
    std::vector<u64> tasks;
    const u64 man = ReadValue<u64>(image_base + MultiPlayManPointerOffset, 0);
    if (!ReadablePointer(man, 0x20)) {
        return tasks;
    }
    const u64 begin = ReadValue<u64>(man, 0x10);
    const u64 end = ReadValue<u64>(man, 0x18);
    if (begin == 0 || end < begin || end - begin > 16 * sizeof(u64) ||
        !ReadablePointer(begin, end - begin == 0 ? sizeof(u64) : end - begin)) {
        return tasks;
    }
    for (u64 slot = begin; slot < end; slot += sizeof(u64)) {
        const u64 task = ReadValue<u64>(slot, 0);
        if (ReadablePointer(task, 0x100)) {
            tasks.push_back(task);
        }
    }
    return tasks;
}

// The compare at 0x00CA1C70 reads [id+0x08] and calls through that object's vtable, so a bogus
// id pointer crashes inside it. Everything it touches is checked first.
bool IsValidMemberId(u64 id) {
    if (!ReadablePointer(id, 0x20)) {
        return false;
    }
    const u64 lock = ReadValue<u64>(id, 0x08);
    if (!ReadablePointer(lock, 0x08)) {
        return false;
    }
    const u64 vtable = ReadValue<u64>(lock, 0);
    return ReadablePointer(vtable, 0x30);
}

bool RemotePlayerExists(u64 task) {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x40)) {
        return false;
    }
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return false;
    }
    using MemberIdEquals = bool PS4_SYSV_ABI (*)(u64 a, u64 b);
    const auto equals = reinterpret_cast<MemberIdEquals>(image_base + MemberIdEqualsOffset);
    for (s32 i = 0; i < count; ++i) {
        const u64 player = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (ReadablePointer(player, 0x400) && IsValidMemberId(player + 0x3D8) &&
            IsValidMemberId(task + 0xC8) && BbGuestCall(equals, player + 0x3D8, task + 0xC8)) {
            return true;
        }
    }
    return false;
}

// Member slot of an insert task: the summon reload rules keep a member list at +0x68 whose
// entries hold the member id at +0x10 and the slot index at +0x30 (FirstSyncWait 0x01E50090 reads
// it). Remote slot data lives at [0x0553B130]+0x10, 0x6A0 per slot: type +0xA4, role byte +0x5D8.
struct MemberSlotInfo {
    s32 index{-1};
    s32 type{-1};
    s32 role{-1};
};

MemberSlotInfo GetMemberSlotInfo(u64 task) {
    MemberSlotInfo info{};
    const u64 rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (!ReadablePointer(rules, 0x70)) {
        return info;
    }
    using MemberIdEquals = bool PS4_SYSV_ABI (*)(u64 a, u64 b);
    const auto equals = reinterpret_cast<MemberIdEquals>(image_base + MemberIdEqualsOffset);
    const u64 head = ReadValue<u64>(rules, 0x68);
    if (!ReadablePointer(head, 0x38) || !IsValidMemberId(task + 0xC8)) {
        return info;
    }
    u64 entry = ReadValue<u64>(head, 0);
    for (int guard = 0; guard < 16 && ReadablePointer(entry, 0x38) && entry != head; ++guard) {
        if (IsValidMemberId(entry + 0x10) && BbGuestCall(equals, entry + 0x10, task + 0xC8)) {
            info.index = ReadValue<s32>(entry, 0x30);
            break;
        }
        entry = ReadValue<u64>(entry, 0);
    }
    if (info.index < 0 || info.index > 3) {
        return info;
    }
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    const u64 table = ReadablePointer(slots, 0x18) ? ReadValue<u64>(slots, 0x10) : 0;
    const u64 slot = table + static_cast<u64>(info.index) * 0x6A0;
    if (ReadablePointer(slot, 0x5E0)) {
        info.type = ReadValue<s32>(slot, 0xA4);
        info.role = ReadValue<u8>(slot, 0x5D8);
    }
    return info;
}

// Logs local type, session state, phase and every insert task whenever any of them changes.
void LogBbscSessionShape(std::string_view reason, bool check_characters = false) {
    static std::string last;
    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    const s32 session_state =
        ReadablePointer(multi_play, 0x128) ? ReadValue<s32>(multi_play, 0x124) : -1;
    const u64 rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const s32 phase = ReadablePointer(rules, 0x88) ? ReadValue<s32>(rules, 0x84) : -1;
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    const s32 pending_type = ReadablePointer(slots, 0x90) ? ReadValue<s32>(slots, 0x8C) : -1;
    const u64 global = GetGlobalState();
    const s32 own_world = ReadablePointer(global, 0x1593) ? ReadValue<u8>(global, 0x1592) : -1;
    const s32 sign_type = ReadablePointer(global, 0x16F8) ? ReadValue<s32>(global, 0x16F0) : -99;
    std::string shape =
        fmt::format("state={} phase={} type={} chr_type={} pending_type={} sign={} own_world={} ins=[",
                    session_state, phase, GetLocalPlayerType(), GetLocalCharacterType(),
                    pending_type, sign_type, own_world);
    for (const u64 task : GetMultiPlayerInsTasks()) {
        const MemberSlotInfo slot = check_characters ? GetMemberSlotInfo(task) : MemberSlotInfo{};
        shape += fmt::format("(mode={} cur={} next={} flags={:#x} chr={} slot={} slot_type={} role={})",
                             ReadValue<s32>(task, 0xE8), ReadValue<s32>(task, 0x50),
                             ReadValue<s32>(task, 0x54), ReadValue<u8>(task, 0xED),
                             check_characters ? (RemotePlayerExists(task) ? 1 : 0) : -1, slot.index,
                             slot.type, slot.role);
    }
    shape += ']';
    if (shape != last) {
        last = shape;
        LOG_INFO(Debug, "BBSC session shape ({}): {}", reason, shape);
    }
}

// At the kept death (guest, state 6): queue the current player type as the respawn override.
void ArmBbscReinsertV8() {
    const s32 type = GetLocalPlayerType();
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    bbsc_v8_saved_type = type;
    // The sign code is cleared by the time the death handlers run, so the value remembered
    // during the join (TrackBbscSignType) is used instead.
    if (const u64 global = GetGlobalState();
        ReadablePointer(global, 0x16F8) && ReadValue<s32>(global, 0x16F0) != 0) {
        bbsc_v8_saved_sign_type = ReadValue<s32>(global, 0x16F0);
        bbsc_v8_saved_sign_extra = ReadValue<u8>(global, 0x16F4);
    }
    bbsc_v8_death_ms = SteadyNowMs();
    bbsc_v8_ready_since_ms = 0;
    bbsc_v8_pending = true;
    if (type > 0 && ReadablePointer(slots, 0x90) &&
        HasMemoryAccess(slots + 0x8C, 4, MemoryProt::CpuWrite)) {
        WriteValue<s32>(slots, 0x8C, type);
    }
    bbsc_v8_original_swapped = false;
    if (type > 0) {
        bbsc_v8_original_swapped = SetOwnWorldRestoreType(type, &bbsc_v8_original_type);
    }
    LOG_INFO(Debug,
             "BBSC reinsert v8: kept death, player type {} queued for respawn (sign type {}, "
             "extra {}, own-world restore type {} -> {})",
             type, bbsc_v8_saved_sign_type, bbsc_v8_saved_sign_extra,
             bbsc_v8_original_swapped ? bbsc_v8_original_type : -1,
             bbsc_v8_original_swapped ? type : -1);
    LogBbscSessionShape("v8 death");
}

// Game-thread tick after the kept death: once the respawned player is alive and the load is over,
// restore the player type if the respawn reset it and restart remote-player creation.
void TickBbscReinsertV8() {
    if (!bbsc_v8_pending) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - bbsc_v8_death_ms > BbscReinsertTimeoutMs) {
        bbsc_v8_pending = false;
        if (bbsc_v8_original_swapped) {
            SetOwnWorldRestoreType(bbsc_v8_original_type, nullptr);
            bbsc_v8_original_swapped = false;
        }
        LOG_WARNING(Debug, "BBSC reinsert v8: timed out waiting for the respawned player");
        LogBbscSessionShape("v8 timeout");
        return;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1521) || ReadValue<u8>(global, 0x08) != 0 ||
        GetBbscLocalPlayerHp() <= 0 || GetLocalPlayerType() < 0) {
        bbsc_v8_ready_since_ms = 0;
        return;
    }
    if (bbsc_v8_ready_since_ms == 0) {
        bbsc_v8_ready_since_ms = now;
    }
    if (now - bbsc_v8_ready_since_ms < BbscReinsertSettleMs) {
        return;
    }
    bbsc_v8_pending = false;
    LogBbscSessionShape("v8 respawned", true);

    const s32 type_now = GetLocalPlayerType();
    const s32 chr_type_now = GetLocalCharacterType();
    if (bbsc_v8_saved_type > 0 && (type_now != bbsc_v8_saved_type || chr_type_now != bbsc_v8_saved_type)) {
        const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
        const u64 local = ReadablePointer(slots, 0x10) ? ReadValue<u64>(slots, 0x08) : 0;
        if (ReadablePointer(local, 0xA8) && HasMemoryAccess(local + 0xA4, 4, MemoryProt::CpuWrite)) {
            WriteValue<s32>(local, 0xA4, bbsc_v8_saved_type);
        }
        // The live character's +0x78 is the authority (0x01947310 copies it into the slot, and
        // StageTransition 0x013CDE30 does too), so set it as well.
        const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        const u64 player = ReadablePointer(world_chr, 0x68) ? ReadValue<u64>(world_chr, 0x60) : 0;
        if (ReadablePointer(player, 0x80) && HasMemoryAccess(player + 0x78, 4, MemoryProt::CpuWrite)) {
            WriteValue<s32>(player, 0x78, bbsc_v8_saved_type);
            // The creation flags are NOT applied by default: writing them onto an already built
            // character made the guest invisible and immune to damage (run 2026-09-16,
            // 0x1001098 -> 0x1004098). SHADPS4_BBSC_PHANTOM_FLAGS=1 re-enables the experiment.
            static const bool phantom_flags = EnvFlagEnabled("SHADPS4_BBSC_PHANTOM_FLAGS");
            if (phantom_flags &&
                (bbsc_v8_saved_type == 1 || bbsc_v8_saved_type == 2 || bbsc_v8_saved_type == 12)) {
                ApplyPhantomCharacterFlags(player);
            }
        }
        LOG_INFO(Debug, "BBSC reinsert v8: player type restored (slot {} chr {}) -> {}", type_now,
                 chr_type_now, bbsc_v8_saved_type);
    }

    using SetState = void PS4_SYSV_ABI (*)(u64 task, s32 state);
    const auto set_state = reinterpret_cast<SetState>(image_base + MultiPlayerInsSetStateOffset);
    int restarted = 0;
    for (const u64 task : GetMultiPlayerInsTasks()) {
        const s32 mode = ReadValue<s32>(task, 0xE8);
        const s32 current = ReadValue<s32>(task, 0x50);
        if ((mode != 1 && mode != 2) || current >= 9 || RemotePlayerExists(task) ||
            !HasMemoryAccess(task + 0xED, 1, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue<u8>(task, 0xED, static_cast<u8>(ReadValue<u8>(task, 0xED) | 0x01));
        BbGuestCall(set_state, task, 4);
        ++restarted;
    }
    if (bbsc_v8_original_swapped) {
        SetOwnWorldRestoreType(bbsc_v8_original_type, nullptr);
        bbsc_v8_original_swapped = false;
        LOG_INFO(Debug, "BBSC reinsert v8: own-world restore type put back to {}",
                 bbsc_v8_original_type);
    }
    LOG_INFO(Debug, "BBSC reinsert v8: restarted {} remote player insert task(s) at FirstSyncWait",
             restarted);
    LogBbscSessionShape("v8 restarted", true);
}

// Self-heal (host and guest): a member task that reached CreateWait (6) but whose character is gone
// for BbscSelfHealMissingMs while no load is running gets its create step re-run. This is what
// brings the guest back on the host's screen after a HOST death (the host's load destroys the
// guest's character; with the Tasks patch group the task itself survives).
constexpr s64 BbscSelfHealMissingMs = 4'000;

void TickBbscSelfHeal() {
    struct MissingSince {
        u64 task;
        s64 since_ms;
    };
    static std::array<MissingSince, 8> missing{};
    static s64 last_check_ms = 0;

    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 1'000) {
        return;
    }
    last_check_ms = now;

    const u64 global = GetGlobalState();
    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    const s32 session_state =
        ReadablePointer(multi_play, 0x128) ? ReadValue<s32>(multi_play, 0x124) : -1;
    if (bbsc_v8_pending || (session_state != 3 && session_state != 6) ||
        !ReadablePointer(global, 0x1521) || ReadValue<u8>(global, 0x08) != 0 ||
        GetBbscLocalPlayerHp() <= 0) {
        missing = {};
        return;
    }

    using SetState = void PS4_SYSV_ABI (*)(u64 task, s32 state);
    const auto set_state = reinterpret_cast<SetState>(image_base + MultiPlayerInsSetStateOffset);
    const auto tasks = GetMultiPlayerInsTasks();
    for (auto& slot : missing) {
        if (slot.task != 0 && std::ranges::find(tasks, slot.task) == tasks.end()) {
            slot = {};
        }
    }
    for (const u64 task : tasks) {
        const s32 mode = ReadValue<s32>(task, 0xE8);
        if ((mode != 1 && mode != 2) || ReadValue<s32>(task, 0x50) != 6 ||
            ReadValue<s32>(task, 0x54) != 6) {
            continue;
        }
        auto it = std::ranges::find_if(missing, [task](const MissingSince& m) { return m.task == task; });
        if (RemotePlayerExists(task)) {
            if (it != missing.end()) {
                *it = {};
            }
            continue;
        }
        if (it == missing.end()) {
            it = std::ranges::find_if(missing, [](const MissingSince& m) { return m.task == 0; });
            if (it != missing.end()) {
                *it = {task, now};
            }
            continue;
        }
        if (now - it->since_ms < BbscSelfHealMissingMs ||
            !HasMemoryAccess(task + 0xED, 1, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue<u8>(task, 0xED, static_cast<u8>(ReadValue<u8>(task, 0xED) | 0x01));
        BbGuestCall(set_state, task, 4);
        *it = {};
        LOG_INFO(Debug,
                 "BBSC self-heal: member character missing for {} ms (mode {}, session state {}); "
                 "insert task restarted at FirstSyncWait",
                 BbscSelfHealMissingMs, mode, session_state);
        LogBbscSessionShape("self-heal", true);
    }
}

// ---- Spectate on death (ERSC-style) ------------------------------------------------------------
// Bloodborne's death flow is a Lua state machine: SoloPlayDeath (host / own world) and
// PartyGhostDeath (summoned guest) show "YOU DIED" and schedule their final step on a timer through
// 0x0132B1C0(delay, event, 4000, "<name>_2", ...). That final step frees the own-world save
// snapshot, picks the respawn point (0x01332BC0), and starts the reload (StageTransition
// 0x013CDE30). It runs once, with the event object in rdi (CONFIRMED statically).
//
// While a partner is still alive the step still runs, but its StageTransition call is retargeted
// to a no-op (see Group::Spectate in bbsc_persistence.cpp), so the world is NOT rebuilt and the
// partner keeps playing in the same world. The step's own bookkeeping - releasing the death UI
// objects at global+0x1690/+0x1698/+0x16B0/+0x16B8 and dropping the blood echoes - still happens,
// which is what a second death needs in order to run at all (run 2026-09-19: with the whole step
// skipped, the second death never reached it and the player was stuck with only camera control).
// This hook then decides what happens next: revive on the spot (TickBbscSpectate ->
// BbscRespawnInPlace) or, during a boss fight, wait it out first. When nobody is left alive or the
// session ends, the reload the step asked for is requested for real instead.
constexpr u64 SoloPlayDeathFinalOffset = 0x01381DE0;        // Lua "SoloPlayDeath_2"
constexpr u64 PartyGhostDeathFinalOffset = 0x013824C0;      // Lua "PartyGhostDeath_2"
constexpr s64 BbscSpectateReleaseDebounceMs = 1'500;
constexpr s64 BbscSpectateSafetyMs = 15 * 60 * 1'000;

bool bbsc_spectate_active = false;
bool bbsc_spectate_releasing = false;
std::string_view bbsc_spectate_name;
// The death step's own StageTransition call is retargeted to a no-op (Group::Spectate), so BBSC
// asks for the reload itself whenever a death has to end the way the game intends.
constexpr u64 DeathReloadRequestOffset = 0x013CDE30;
bool bbsc_request_death_reload = false;

void RequestBbscDeathReload(std::string_view why) {
    if (image_base == 0) {
        return;
    }
    // 0x01332BC0 already picked the destination (global+0xC map, global+0x10 respawn point) while
    // the death step ran, so the request alone reproduces the vanilla death reload.
    using ReloadRequest = u64 PS4_SYSV_ABI (*)();
    const u64 result = BbGuestCall(reinterpret_cast<ReloadRequest>(image_base + DeathReloadRequestOffset));
    LOG_INFO(Debug, "BBSC death: reload requested ({}), result {}", why, result);
}
s64 bbsc_spectate_since_ms = 0;
// The local character when the spectate started. A load builds a new one: someone reloaded (the
// host after its own death) and every boss is back to full health, so nobody is going to kill the
// boss this death was waiting for. Test 2026-10-01: the guest stayed dead after the host's reload
// until the host died again.
u64 bbsc_spectate_chr = 0;
s64 bbsc_spectate_alone_since_ms = 0;
// Test 2026-10-04 (mod 11, boss fight): the guest died first and spectated; when the host died the
// guest saw nobody alive and reloaded on its own - into its OWN respawn point (the Hunter's Dream),
// still in the session. While it loaded it stopped reporting "dead", the host's death step saw the
// guest's copy alive, spectated, and stayed a corpse. A summoned guest therefore never reloads
// itself while the session lives: it keeps spectating until the host's reload rebuilds the guest's
// character ("a load ended the boss fight" -> respawn in place in the host's reset world). Only if
// no such load comes within BbscGuestWaitHostMs does it fall back to its own reload.
constexpr s64 BbscGuestWaitHostMs = 45'000;
bool bbsc_spectate_logged_wait = false;
// Test 2026-10-04 (mod 15, CLS_AMG host, guest): the wait worked, the host reloaded and came
// back alive - but the host's reload does NOT reload a summoned guest, so the guest's character
// never changed, "a load ended the boss fight" never fired and the guest stayed dead waiting for a
// boss kill. So once a guest has seen nobody alive (a wipe), the host coming back alive means the
// host reloaded: the guest then does its own full reload into the host's map at the host's
// position (the join warp, RequestBbscSessionWarp), which resets its copy of the world too; the
// new character after that load is revived by the "a load ended the boss fight" path.
bool bbsc_spectate_saw_wipe = false;
bool bbsc_spectate_wipe_warped = false;
s64 bbsc_spectate_host_back_ms = 0;
constexpr s64 BbscWipeHostSettleMs = 2'000; // let the host's synced position settle first
// bbsc_peer_revivals when this death started. A guest whose partner said "dead" and then "alive"
// while the guest was down saw a wipe and a reload, even if it never saw "nobody alive" itself
// (test 2026-10-06: the guest died while the dead host was already reloading).
u32 bbsc_spectate_revivals_at_death = 0;

bool BbscSpectateEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_SPECTATE");
    return enabled;
}

// HP through the same path SummonReloadStart uses for the local player: [[chr+0x3B0]+0x20]+0xF8.
s32 GetCharacterHp(u64 chr) {
    if (!ReadablePointer(chr, 0x3B8)) {
        return -1;
    }
    const u64 modules = ReadValue<u64>(chr, 0x3B0);
    if (!ReadablePointer(modules, 0x28)) {
        return -1;
    }
    const u64 data = ReadValue<u64>(modules, 0x20);
    return ReadablePointer(data, 0xFC) ? ReadValue<s32>(data, 0xF8) : -1;
}

// ---- Life state over the relay (2026-10-04) -------------------------------------------------------
// Report 2026-10-04: in a boss fight both players died; the first respawned, the second stayed dead
// and the world was not reset. A dead player's copy on the other machine keeps HP above 0 (only the
// dead flag [chr+0x58]+0x114 bit 0 is set), and TickBbscRemoteRevive even revives such a copy,
// because the same picture also means "the partner respawned in place and its copy is stale". So
// the second to die saw the first one alive, spectated and waited for a boss kill that never came.
// Every player now says twice a second whether it is dead (dying or spectating) or alive, over the
// BBSC relay (packet "SHAD" 0x23); the partner checks below use that whenever it is fresh.
constexpr s64 BbscLifeSendMs = 500;
constexpr s64 BbscLifeFreshMs = 3'000;
// Test 2026-10-06 (host and guest died in a boss fight, the host had to die AGAIN before the guest
// respawned): the host died first, then released its death and reloaded. A player sends nothing
// while it loads (no session state), so 3 s later its "dead" report was stale; the guest died in
// that window, fell back to the host's copy in its own world - which TickBbscRemoteRevive had just
// stood up - saw a living partner and spectated, waiting for a boss kill. A "dead" report now holds
// until the same player says "alive" (or for this long without a word: a dead player who left).
constexpr s64 BbscLifeDeadHoldMs = 60'000;
struct BbscPeerLife {
    u8 dead = 0;
    u32 sequence = 0;
    s64 received_ms = 0;
};
std::mutex bbsc_life_mutex;
std::map<u32, BbscPeerLife> bbsc_peer_life;
std::atomic<u32> bbsc_life_own_id{0}; // ours, so an echo of our own packet is ignored
// Counts "dead -> alive" reports: a partner that came back by reloading (see TickBbscSpectate).
std::atomic<u32> bbsc_peer_revivals{0};

// {fresh reports, of which dead}
std::pair<u32, u32> BbscPeerLifeCounts() {
    std::scoped_lock lock{bbsc_life_mutex};
    const s64 now = SteadyNowMs();
    u32 fresh = 0;
    u32 dead = 0;
    for (const auto& [player, life] : bbsc_peer_life) {
        const s64 age = now - life.received_ms;
        if (age <= BbscLifeFreshMs || (life.dead != 0 && age <= BbscLifeDeadHoldMs)) {
            ++fresh;
            dead += life.dead != 0 ? 1 : 0;
        }
    }
    return {fresh, dead};
}

// Every partner that reports says it is dead.
bool BbscAllPartnersReportDead() {
    const auto [fresh, dead] = BbscPeerLifeCounts();
    return fresh != 0 && dead == fresh;
}

// Another player character in WorldChrMan ([0x0553E878]+0x30 count, +0x38 entries of 0x38,
// +0 PlayerIns*) that is not the local player (+0x60) and still has HP.
// Character type at chr+0x78: 0 host / own world, 1 co-op phantom, 2 and 12 invaders.
s32 GetCharacterType(u64 chr) {
    return ReadablePointer(chr, 0x80) ? ReadValue<s32>(chr, 0x78) : -1;
}

bool IsInvaderType(s32 type) {
    return type == 2 || type == 12;
}

// Invasions (2026-10-05): an invader is never a "partner". Their death and the host's follow the
// vanilla rules - the invader goes home whether they win or lose - so the kept-session death
// rules only count co-op players.
bool LivingPartnerExists() {
    if (BbscAllPartnersReportDead()) {
        return false; // their copies may still show HP; they said they are dead
    }
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return false;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return false;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 player = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (player != 0 && player != local && GetCharacterHp(player) > 0 &&
            !IsInvaderType(GetCharacterType(player))) {
            return true;
        }
    }
    return false;
}

// Remote player characters of an invader type (seen from this world), and the host's character
// as seen by a phantom (type 0 among the remote players).
int RemoteInvaderCount() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return 0;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return 0;
    }
    int invaders = 0;
    for (s32 i = 0; i < count; ++i) {
        const u64 player = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (player != 0 && player != local && IsInvaderType(GetCharacterType(player))) {
            ++invaders;
        }
    }
    return invaders;
}

u64 FindRemoteHostCharacter() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return 0;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return 0;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 player = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (player != 0 && player != local && GetCharacterType(player) == 0) {
            return player;
        }
    }
    return 0;
}

s32 GetSessionState() {
    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    return ReadablePointer(multi_play, 0x128) ? ReadValue<s32>(multi_play, 0x124) : -1;
}

// ---- Seamless Co-op death rules (ER / DS3 / DSR) ------------------------------------------------
//  * Boss fight (a boss health bar is up) + partner alive: spectate until the boss dies (then
//    respawn in place) or nobody is left alive (then the normal reload for everyone).
//  * No boss fight + partner alive: respawn at your last lamp WITHOUT a reload; the world is not
//    reset and the partner keeps playing.
//  * Nobody left alive: the game's normal death reload.
// Pieces (CONFIRMED statically):
//  * Boss gauge: SetBossGauge 0x01336C80 stores up to 3 bosses at [0x05562878]+0x1658+i*0x14
//    (+0x165C = boss character handle); ClearBossGauge 0x01336D00 resets them to -1.
//  * Revive + teleport without load: see BbscRespawnInPlace (a copy of the local-player branch of
//    the game's event revive 0x013CBAD0 without its CSMultiPlayMan::Stop call).
//  * Position: [[[[chr+0x58]+0x08]+0x3B0]+0x68]+0x1E0 (vec4), rotation at +0x1D0, the same block
//    the event warp 0x013CB3D0 reads (STRONGLY INDICATED; captured values match host placements).
//  * Death globals: the death handler 0x018C49F0 sets global+0x1522 (local player died) and
//    global+0x1523 (fell); the skipped reload is what normally clears them.
constexpr u64 BossGaugePointerOffset = 0x05562878;
constexpr s64 BbscBossClearedSettleMs = 2'000;

struct BbscTransform {
    std::array<float, 4> position{};
    std::array<float, 4> rotation{};
};

enum class BbscDeathMode : u8 { Spectate, RespawnNow };

BbscDeathMode bbsc_death_mode = BbscDeathMode::Spectate;
s64 bbsc_boss_cleared_since_ms = 0;

bool BossFightActive() {
    const u64 gauge = ReadValue<u64>(image_base + BossGaugePointerOffset, 0);
    if (!ReadablePointer(gauge, 0x1690)) {
        return false;
    }
    for (int slot = 0; slot < 3; ++slot) {
        if (ReadValue<s32>(gauge, 0x165C + static_cast<size_t>(slot) * 0x14) != -1) {
            return true;
        }
    }
    return false;
}

// The boss bar is gone by the time the death step runs: the game's "died in the boss area" script
// Failed_BossAreaMission (0x0138B2D0, Lua) wipes all three gauge slots (0x0138B302..0x0138B38F)
// and sets event flags 4047/4000 first (CONFIRMED statically 2026-09-30; test 2026-09-29: every
// boss death was logged "no boss fight"). So the bar is remembered: every tick and right before
// that script, and a boss kill (BlockClear2_1) ends it.
constexpr s64 BbscBossBarMemoryMs = 10'000;
s64 bbsc_boss_bar_seen_ms = 0;
s64 bbsc_boss_killed_ms = 0;

void NoteBbscBossBar() {
    if (BossFightActive()) {
        bbsc_boss_bar_seen_ms = SteadyNowMs();
    }
}

// A boss fight was on when the player died: a bar is up now, or was up moments ago and no boss
// has been killed since.
bool BbscBossFightAtDeath() {
    if (BossFightActive()) {
        return true;
    }
    return bbsc_boss_bar_seen_ms != 0 && bbsc_boss_killed_ms < bbsc_boss_bar_seen_ms &&
           SteadyNowMs() - bbsc_boss_bar_seen_ms <= BbscBossBarMemoryMs;
}

constexpr u64 FailedBossAreaMissionOffset = 0x0138B2D0; // Lua "Failed_BossAreaMission"

void PS4_SYSV_ABI BbscFailedBossAreaHook(u64 tag, const GuestRegisterSnapshot* registers) {
    (void)tag;
    if (registers == nullptr) {
        return;
    }
    const bool bar = BossFightActive();
    NoteBbscBossBar();
    LOG_INFO(Debug, "BBSC boss fight: Failed_BossAreaMission (died in the boss area), boss bar {}",
             bar ? "up - remembered for the death step" : "not up");
}

u64 GetCharacterTransformBlock(u64 chr) {
    if (!ReadablePointer(chr, 0x60)) {
        return 0;
    }
    u64 pointer = ReadValue<u64>(chr, 0x58);
    for (const u64 offset : {u64{0x08}, u64{0x3B0}, u64{0x68}}) {
        if (!ReadablePointer(pointer, offset + sizeof(u64))) {
            return 0;
        }
        pointer = ReadValue<u64>(pointer, offset);
    }
    return ReadablePointer(pointer, 0x1F0) ? pointer : 0;
}

std::optional<BbscTransform> ReadCharacterTransform(u64 chr) {
    const u64 block = GetCharacterTransformBlock(chr);
    if (block == 0) {
        return std::nullopt;
    }
    BbscTransform transform{};
    for (size_t i = 0; i < 4; ++i) {
        transform.position[i] = ReadValue<float>(block, 0x1E0 + i * 4);
        transform.rotation[i] = ReadValue<float>(block, 0x1D0 + i * 4);
    }
    bool all_zero = true;
    for (size_t i = 0; i < 3; ++i) {
        const float value = transform.position[i];
        if (!std::isfinite(value) || std::abs(value) > 20000.0f) {
            return std::nullopt; // not a world position: never warp there
        }
        all_zero = all_zero && value == 0.0f;
    }
    if (all_zero) {
        return std::nullopt;
    }
    return transform;
}

u64 GetLocalPlayerCharacter() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    return ReadablePointer(world_chr, 0x68) ? ReadValue<u64>(world_chr, 0x60) : 0;
}

u64 FindLivingPartnerCharacter() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return 0;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return 0;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 player = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (player != 0 && player != local && GetCharacterHp(player) > 0) {
            return player;
        }
    }
    return 0;
}

// ---- Relay sessions (mod, 2026-10-06) ------------------------------------------------------------
// The game server's relay hands every frame to EVERY player connected to it, not only to the ones
// in your session. 4-player test 2026-10-06: several players rang their own Beckoning Bell (each
// one hosting, session state 3) while others were guests, so a guest got the follow packets, world
// flags and scaling of every host on the server, and the life reports of strangers. The follow
// code took another host's position as "the host travelled" (a guest pulled to someone else's map
// behind a black screen). Every frame now carries its sender and the sender's session (the host's
// sender id; bbsc_relay.cpp), and a guest locks onto its own host's sender:
//   - the only player on the server who is hosting, or
//   - with several, the host whose follow packet puts it in our map within BbscRelayLockMetres of
//     the host character in our world (FindRemoteHostCharacter).
// The lock is made here on the game tick (game memory is not read on the relay thread) and dropped
// once we have not been a guest for BbscRelayUnlockMs.
constexpr s64 BbscRelayHostFreshMs = 5'000;
constexpr s64 BbscRelayUnlockMs = 5'000;
constexpr float BbscRelayLockMetres = 15.0f;
struct BbscRelayHostSeen {
    u32 map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    s64 ms = 0;
};
std::mutex bbsc_relay_mutex;
std::map<u32, BbscRelayHostSeen> bbsc_relay_hosts; // hosting senders -> their last follow packet
std::atomic<u32> bbsc_relay_locked_host{0};
std::atomic<s32> bbsc_relay_session_state{-1}; // GetSessionState() as of the last game tick
// Drops every world-flag snapshot received so far: they belong to the host we just let go (see
// "World state when warping together"; a neighbour block's snapshot was used for up to 2 minutes).
void ClearBbscHostWorldSnapshots();

// The host whose packets a guest takes: the locked one, else the only one hosting, else 0.
u32 BbscRelayHostForPackets() {
    if (const u32 locked = bbsc_relay_locked_host.load(); locked != 0) {
        return locked;
    }
    std::scoped_lock lock{bbsc_relay_mutex};
    const s64 now = SteadyNowMs();
    u32 only = 0;
    for (const auto& [sender, seen] : bbsc_relay_hosts) {
        if (now - seen.ms <= BbscRelayHostFreshMs) {
            if (only != 0) {
                return 0; // more than one host on the server: wait for the position lock
            }
            only = sender;
        }
    }
    return only;
}

void TickBbscRelayHostLock() {
    static s64 last_ms = 0;
    static s64 not_guest_since_ms = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 250) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    bbsc_relay_session_state = state;
    const u32 locked = bbsc_relay_locked_host.load();
    if (state != 6 && state != 4) {
        if (not_guest_since_ms == 0) {
            not_guest_since_ms = now;
        }
        if (locked != 0 && now - not_guest_since_ms >= BbscRelayUnlockMs) {
            bbsc_relay_locked_host = 0;
            ClearBbscHostWorldSnapshots();
            LOG_INFO(Debug, "BBSC relay: no longer a guest (session state {}) - host {:08x} let go",
                     state, locked);
        }
        return;
    }
    not_guest_since_ms = 0;
    if (locked != 0 || state != 6) {
        return;
    }
    std::vector<std::pair<u32, BbscRelayHostSeen>> hosts;
    {
        std::scoped_lock lock{bbsc_relay_mutex};
        for (const auto& [sender, seen] : bbsc_relay_hosts) {
            if (now - seen.ms <= BbscRelayHostFreshMs) {
                hosts.emplace_back(sender, seen);
            }
        }
    }
    if (hosts.empty()) {
        return;
    }
    if (hosts.size() == 1) {
        bbsc_relay_locked_host = hosts[0].first;
        LOG_INFO(Debug, "BBSC relay: our host is player {:08x} (the only one hosting)",
                 hosts[0].first);
        return;
    }
    const auto host = ReadCharacterTransform(FindRemoteHostCharacter());
    const u32 map = GetCurrentPackedMap();
    if (!host.has_value() || map == 0) {
        return;
    }
    u32 best = 0;
    float best_d2 = BbscRelayLockMetres * BbscRelayLockMetres;
    for (const auto& [sender, seen] : hosts) {
        const float dx = seen.x - host->position[0];
        const float dy = seen.y - host->position[1];
        const float dz = seen.z - host->position[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (seen.map == map && d2 < best_d2) {
            best = sender;
            best_d2 = d2;
        }
    }
    if (best != 0) {
        bbsc_relay_locked_host = best;
        LOG_INFO(Debug, "BBSC relay: our host is player {:08x} ({} hosting on the server, it stands "
                        "{:.1f} m from the host in our world)",
                 best, hosts.size(), std::sqrt(best_d2));
    }
}

// Guest joins: teleport next to the host once, so a guest summoned from elsewhere starts beside
// the host. Guest = session state 6 in the host's world (global+0x1592 == 0). Waits until nothing
// is loading and the host character's position has been stable (synced) for
// BbscJoinTeleportSettleMs. Uses the in-map event warp of 0x013CB3D0 (CONFIRMED statically):
// chr->vtable[0x5F8](chr, &pos, &rot, 1), then chr+0x1E1 |= 2. Re-armed when the session ends.
constexpr s64 BbscJoinTeleportSettleMs = 2'000;
// Set once the initial join has placed the guest. Warping together (below) waits for it,
// so the two never move the same character at the same time.
bool bbsc_join_teleport_done = false;

// Ask the game to warp us properly instead of sliding the guest across the floor.
//
// The plain teleport is the game's own in-map event warp (0x013CB3D0 does exactly the same two
// operations), which is meant for short hops inside an already streamed area. Used to arrive in
// the host's world it moves the player without the engine ever running a real transition, which
// is the suspected cause of the cross-map texture mess.
//
// The reload destination selector (0x01332BC0) has a forced-warp branch built for this:
//   global+0x1520 = 1        ignore the last lamp, use my destination
//   global+0x14f0            the map to load
//   global+0x10              the point in it (kept, then corrected by the teleport afterwards)
// and 0x013CDE30 performs the reload - loading screen, teardown, streaming, all of it. That is
// the same call the death respawn already uses, so this is machinery we exercise every session.
//
// Sequence: request the reload once, wait for it to finish, then teleport to the host's exact
// position as before. The teleport is a few metres inside a freshly loaded map, which is what the
// in-map warp is actually for.
bool BbscWarpJoinEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_WARP_JOIN");
    return enabled;
}

// A real reload for a guest in a session, to `to` (map + spot).
//
// Uses the game's own forced summon placement, in exactly the order the fork's cross-map join
// handoff uses it (ApplyCrossMapSummonGuestPlacement): the summoned placement block (+0x14C4 map,
// +0x14D0 position, +0x14E0 rotation), then the native setters SetForcedSummonPosition /
// Orientation / Map / Warp, which copy it into the forced block (SetForcedSummonMap is
// `mov [global+0x14F0], ecx`) and raise the forced-warp byte +0x1520, then the selector
// 0x01332BC0 (which reads +0x14F0 because +0x1520 is set) and StageTransition.
//
// Code review 2026-09-23 A2 had removed the +0x14F0/+0x1520 writes, taking +0x14F0 for the guest's
// home map. That was wrong: they are the forced placement the game spawns a summoned player from,
// and without them the reload used the map's default spawn - the guest appeared in Iosefka's
// Clinic for a moment before the join teleport moved her (a tester, 2026-09-23, build 732ABF1E).
bool RequestBbscSessionWarp(u64 global, const SummonPlacementDescriptor& to, std::string_view why) {
    if (!ReadablePointer(global, 0x1598) || !IsUsablePackedMap(to.packed_region)) {
        return false;
    }
    if (!WriteTransportedSummonPlacement(global, to)) {
        LOG_WARNING(Debug, "BBSC {} warp: summoned placement block not writable", why);
        return false;
    }
    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = u64 PS4_SYSV_ABI (*)();
    BbGuestCall(reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset), 
        reinterpret_cast<const void*>(global + 0x14D0));
    BbGuestCall(reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset), 
        reinterpret_cast<const void*>(global + 0x14E0));
    BbGuestCall(reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset), 
        reinterpret_cast<const void*>(global + 0x14C4));
    BbGuestCall(reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset));
    const bool selected =
        BbGuestCall(reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset), 0, -1);
    const u32 selected_map = ReadValue<u32>(global, 0x0C);
    if (!selected || selected_map != to.packed_region) {
        LOG_WARNING(Debug, "BBSC {} warp: selector picked {:#x}, wanted {:#x}; not loading", why,
                    selected_map, to.packed_region);
        return false;
    }
    const u64 rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (ReadablePointer(rules, 0x88) && HasMemoryAccess(rules + 0x84, 4, MemoryProt::CpuWrite)) {
        WriteValue<s32>(rules, 0x84, 3);
    }
    StartBbscLoadCover(why); // already on for the summon when this is the cross-map second reload
    const u64 result = BbGuestCall(reinterpret_cast<StageTransition>(image_base + StageTransitionOffset));
    LOG_INFO(Debug,
             "BBSC {} warp: full reload requested into map {:#x} at ({:.2f}, {:.2f}, {:.2f}), "
             "result {}",
             why, to.packed_region, to.x, to.y, to.z, result);
    return true;
}

void TickBbscJoinTeleport() {
    static bool done = false;
    static bool warp_requested = false;
    static s64 warp_requested_ms = 0;
    static s64 stable_since_ms = 0;
    static std::array<float, 3> last_host{};
    static s64 last_check_ms = 0;
    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 250) {
        return;
    }
    last_check_ms = now;

    const s32 session_state = GetSessionState();
    const u64 global = GetGlobalState();
    if (session_state != 6) {
        done = false;
        bbsc_join_teleport_done = false;
        bbsc_cross_map_join.store(false, std::memory_order_release);
        warp_requested = false;
        warp_requested_ms = 0;
        stable_since_ms = 0;
        return;
    }
    if (done || !ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x08) != 0 ||
        ReadValue<u8>(global, 0x1592) != 0) {
        stable_since_ms = 0;
        return;
    }
    const u64 player = GetLocalPlayerCharacter();
    // Test 2026-10-08 (a tester invading): "the invader got 2 loading screens". An invader is in
    // session state 6 too; the join warp/teleport to the host is for co-op guests only (player
    // type 1) - an invader arrives where the game puts it.
    if (GetLocalPlayerType() == 2) {
        done = true;
        bbsc_join_teleport_done = true;
        return;
    }
    // The host, not just any living partner: with 3+ hunters the first one found can be another
    // guest, who may be anywhere (2026-10-06).
    const u64 real_host = FindRemoteHostCharacter();
    const u64 host = real_host != 0 && GetCharacterHp(real_host) > 0 ? real_host
                                                                     : FindLivingPartnerCharacter();
    if (!ReadablePointer(player, 0x410) || GetCharacterHp(player) <= 0 || host == 0) {
        stable_since_ms = 0;
        return;
    }
    const auto host_transform = ReadCharacterTransform(host);
    if (!host_transform.has_value()) {
        stable_since_ms = 0;
        return;
    }
    const std::array<float, 3> host_pos{host_transform->position[0], host_transform->position[1],
                                        host_transform->position[2]};
    float moved = 0.0f;
    for (size_t i = 0; i < 3; ++i) {
        moved = std::max(moved, std::abs(host_pos[i] - last_host[i]));
    }
    last_host = host_pos;
    if (stable_since_ms == 0 || moved > 5.0f) {
        stable_since_ms = now; // first sighting or the synced position jumped: wait again
        return;
    }
    if (now - stable_since_ms < BbscJoinTeleportSettleMs) {
        return;
    }

    // Let the engine do the transition first, once, then position inside the loaded map.
    // Only after a cross-map join: the one summon load into the host's map left the lighting
    // flickering (lights switching on and off, 2026-09-24) and this second reload fixed it. A
    // same-map join does not need it, and the reload after every summon was what turned the guest
    // into "Beckoner" (a tester, 2026-09-23).
    if (BbscWarpJoinEnabled() && bbsc_cross_map_join.load(std::memory_order_acquire) &&
        !warp_requested) {
        SummonPlacementDescriptor to{};
        to.packed_region = GetCurrentPackedMap();
        to.x = host_transform->position[0] + 1.0f;
        to.y = host_transform->position[1];
        to.z = host_transform->position[2];
        to.heading = host_transform->rotation[1];
        if (RequestBbscSessionWarp(global, to, "join")) {
            warp_requested = true;
            warp_requested_ms = now;
            stable_since_ms = 0; // the host's synced position has to settle again after the load
            return;
        }
        // Could not ask for it - fall through and use the plain teleport rather than strand the
        // guest wherever the load happened to drop them.
        LOG_WARNING(Debug, "BBSC join warp: could not request the reload, teleporting instead");
        warp_requested = true;
    }
    // Give the load a moment to finish before moving the character it just placed.
    if (warp_requested && now - warp_requested_ms < BbscJoinTeleportSettleMs) {
        return;
    }
    done = true;
    bbsc_join_teleport_done = true; // from here on TickBbscFollowHost may move the guest
    LogBbscHostWorldFlagsDiff(GetCurrentPackedMap(), "after joining");

    const u64 vtable = ReadValue<u64>(player, 0);
    const u64 warp = ReadablePointer(vtable, 0x600) ? ReadValue<u64>(vtable, 0x5F8) : 0;
    if (!IsEbootCodeAddress(warp)) {
        LOG_WARNING(Debug, "BBSC join teleport: character vtable unexpected");
        EndBbscLoadCover("joined, not placed");
        return;
    }
    const auto own = ReadCharacterTransform(player);
    alignas(16) std::array<float, 4> position = host_transform->position;
    alignas(16) std::array<float, 4> rotation = host_transform->rotation;
    position[0] += 1.0f; // beside the host, not inside
    position[3] = 1.0f;
    rotation[3] = 0.0f;
    using Warp = void PS4_SYSV_ABI (*)(u64 chr, float* position, float* rotation, u32 flag);
    BbGuestCall(reinterpret_cast<Warp>(warp), player, position.data(), rotation.data(), 1);
    WriteValue<u8>(player, 0x1E1, static_cast<u8>(ReadValue<u8>(player, 0x1E1) | 0x02));
    LOG_INFO(Debug,
             "BBSC join teleport: guest moved from ({:.2f}, {:.2f}, {:.2f}) to the host at "
             "({:.2f}, {:.2f}, {:.2f})",
             own ? own->position[0] : 0.0f, own ? own->position[1] : 0.0f,
             own ? own->position[2] : 0.0f, position[0], position[1], position[2]);
    EndBbscLoadCover("guest placed beside the host");
}

// ---- Warping together --------------------------------------------------------------------------
// When the host takes a lamp to another area, vanilla Bloodborne ends the session: a client cannot
// follow a host across a stage load, so everyone is sent home. The persistence patches keep the
// session alive through that load, which leaves the guest standing in a world the host has left.
//
// Nothing the game sends tells a guest where the host went, because vanilla never needs to know -
// it only needs to know that the stage changed, so it can end things. The host placement BBSC
// already transports (the X-header on the summon exchange, see GetSeamlessHostPlacementHeader) is
// a handshake-time channel and is not refreshed during a session either.
//
// So BBSC sends it: SignalingBbscFollow, a 30-byte packet on the np_signaling UDP socket the two
// peers already keep alive with echo pings. The host broadcasts its map and position twice a
// second; the guest follows any map that is not the one it is standing in, using exactly the
// sequence the cross-map join uses and that is already confirmed working - a real reload through
// 0x013CDE30 with the forced-warp destination, then the in-map warp onto the host's spot.
//
// The position comes over the wire rather than being read from the host's character, because after
// the host's load that character no longer exists in the guest's world - which is the whole
// problem being solved.
constexpr s64 BbscFollowBroadcastMs = 500;
constexpr s64 BbscFollowStaleMs = 15000;
constexpr s64 BbscFollowSettleMs = 2000;

struct BbscFollowMessage {
    u32 packed_map{};
    float x{};
    float y{};
    float z{};
    float heading{};
    u32 sequence{};
    s32 lamp_point{-1}; // the host's last lamp respawn point (global+0x1528)
    s64 received_ms{};
    // Where the host came out of its last load (a lamp, a respawn): see OnBbscHostFollowPacket.
    u32 arrival_map{};
    float arrival_x{};
    float arrival_y{};
    float arrival_z{};
    float arrival_heading{};
    s64 arrival_ms{};
    bool arrival_by_load{}; // the host was silent (loading) before it: a lamp, a reload
};

std::mutex bbsc_follow_mutex;
BbscFollowMessage bbsc_follow_latest{};

bool BbscWarpTogetherEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_WARP_TOGETHER");
    return enabled;
}

// BBSC's own on-screen notices (the game's co-op banner), see BbscAnnounce.
void BbscAnnounce(const char16_t* text, std::string_view what);
// The co-op lines while warping together, see TickBbscRejoinLines.
void ArmBbscRejoinLines(bool guest, u32 target_map, s64 for_ms);
constexpr s64 BbscHostRejoinLinesMs = 90'000;  // host: until the guest has caught up
constexpr s64 BbscGuestRejoinGraceMs = 20'000; // guest: after arriving
bool BbscDreamEnabled(); // the Hunter's Dream together, see BbscDreamTalkHook
constexpr s64 BbscWorldFlagsWaitMs = 4'000; // how long a follow waits for the host's flags
// The host's Hunter's Dream for a guest, see "The host's Hunter's Dream".
constexpr u32 BbscDreamStateMap = 0x15000000;
bool BbscHostDreamStateReady();
void ApplyBbscHostDreamState(std::string_view why);
void RestoreBbscOwnDreamState(std::string_view why);

// Host side: say where we are, so a guest that is somewhere else can come to us.
void TickBbscFollowBroadcast() {
    static s64 last_ms = 0;
    static u32 sequence = 0;
    static u32 announced_map = 0;
    if (!BbscWarpTogetherEnabled() || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_ms < BbscFollowBroadcastMs) {
        return;
    }
    last_ms = now;

    if (GetSessionState() != 3) {
        announced_map = 0; // not hosting a session with anyone in it
        return;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x1592) == 0 ||
        ReadValue<u8>(global, 0x08) != 0) {
        return; // not in our own world, or a load is in flight and the map is not settled
    }
    const u32 map = GetCurrentPackedMap();
    if (!IsUsablePackedMap(map)) {
        return;
    }
    const auto here = ReadCharacterTransform(GetLocalPlayerCharacter());
    if (!here.has_value()) {
        return;
    }
    ++sequence;
    // The host's last lamp goes along, so a dead guest respawns where the host would.
    const s32 lamp_point = ReadablePointer(global, 0x1530) ? ReadValue<s32>(global, 0x1528) : -1;
    const u32 peers = Libraries::Np::NpSignaling::Helpers::SendBbscFollowToPeers(
        map, here->position[0], here->position[1], here->position[2], here->rotation[1], sequence,
        lamp_point);
    if (map != announced_map) {
        const bool moved = announced_map != 0; // not the first report of this session
        announced_map = map;
        LOG_INFO(Debug,
                 "BBSC warp together: host is now in map {:#x} at ({:.2f}, {:.2f}, {:.2f}), told "
                 "{} peer(s)",
                 map, here->position[0], here->position[1], here->position[2], peers);
        if (moved && peers != 0) {
            ArmBbscRejoinLines(false, map, BbscHostRejoinLinesMs);
        }
    }
}

// A follow load that has not arrived after this long is treated as failed and retried.
constexpr s64 BbscFollowArriveTimeoutMs = 90'000;
constexpr int BbscFollowMaxAttempts = 3;
// No follow load while the host is this close in the same area (shared coordinates, see below).
constexpr float BbscFollowNearMetres = 120.0f;
// ... and only once the host's map has differed from ours this long.
constexpr s64 BbscFollowMismatchMs = 2'500;

// True when that map is loaded in this game (the list TickBbscMapTrace walks): the host walked into a
// neighbouring area the guest's game streams too, so the guest can simply walk there.
// Fixed 2026-10-06 (a tester test: a loading screen at every crossing between Cathedral Ward 0x18000000
// and the next area 0x17000000, the guest pulled back and forth): +0x20 of the array object is a
// POINTER to the entries, as TickBbscMapTrace reads it. This read the entries inline at +0x20, found
// nothing and answered "not loaded" every time, so every cross-area walk became a follow load.
bool BbscMapResident(u32 packed_map) {
    const u64 list = image_base != 0 ? ReadValue<u64>(image_base + CurrentMapListPointerOffset, 0) : 0;
    if (!ReadablePointer(list, 0x18)) {
        return false;
    }
    const u64 array = ReadValue<u64>(list, 0x10);
    if (!ReadablePointer(array, 0x28)) {
        return false;
    }
    const s32 count = ReadValue<s32>(array, 0x18);
    const u64 entries = ReadValue<u64>(array, 0x20);
    if (count <= 0 || count > 64 || entries < 0x10000) {
        return false;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 entry = entries + static_cast<u64>(i) * 0xA0;
        if (!ReadablePointer(entry, 0x10)) {
            return false;
        }
        if (ReadValue<u32>(entry, 0x08) == packed_map) {
            return true;
        }
    }
    return false;
}

// The loaded maps as text ("m24_00 m23_00"), for the follow decision's log lines.
std::string BbscResidentMapsText() {
    const u64 list = image_base != 0 ? ReadValue<u64>(image_base + CurrentMapListPointerOffset, 0) : 0;
    if (!ReadablePointer(list, 0x18)) {
        return "?";
    }
    const u64 array = ReadValue<u64>(list, 0x10);
    if (!ReadablePointer(array, 0x28)) {
        return "?";
    }
    const s32 count = ReadValue<s32>(array, 0x18);
    const u64 entries = ReadValue<u64>(array, 0x20);
    if (count <= 0 || count > 64 || entries < 0x10000) {
        return "?";
    }
    std::string text;
    for (s32 i = 0; i < count; ++i) {
        const u64 entry = entries + static_cast<u64>(i) * 0xA0;
        if (!ReadablePointer(entry, 0x10)) {
            break;
        }
        const u32 packed = ReadValue<u32>(entry, 0x08);
        text += fmt::format("{}m{:02d}_{:02d}", text.empty() ? "" : " ", (packed >> 24) & 0xFF,
                            (packed >> 16) & 0xFF);
    }
    return text;
}

// Where a following guest is put: the spot the host arrived at in its current map (the lamp), or
// the host itself when that is unknown.
struct BbscFollowSpot {
    float x, y, z, heading;
    const char* what;
};
BbscFollowSpot BbscFollowSpotOf(const BbscFollowMessage& host) {
    if (host.arrival_map == host.packed_map) {
        return {host.arrival_x, host.arrival_y, host.arrival_z, host.arrival_heading,
                "arrival point (lamp)"};
    }
    return {host.x, host.y, host.z, host.heading, "side"};
}

// Guest side: go wherever the host says it is.
constexpr s64 BbscFollowAfterLoadMs = 5'000;

void TickBbscFollowHost() {
    static s64 last_check_ms = 0;
    static bool warping = false;
    static s64 warp_requested_ms = 0;
    static u32 target_map = 0;
    static u32 attempts_map = 0;
    static int attempts = 0;
    if (!BbscWarpTogetherEnabled() || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 250) {
        return;
    }
    last_check_ms = now;

    const u64 global = GetGlobalState();
    if (GetSessionState() != 6 || !ReadablePointer(global, 0x1598) ||
        ReadValue<u8>(global, 0x1592) != 0 || GetLocalPlayerType() == 2) {
        warping = false;
        target_map = 0;
        return; // not a co-op guest in someone else's world (an invader does not follow)
    }
    // Test 2026-10-08 (cls_amg, guest): the wipe reload finished and in the same tick the follow
    // asked for a load into the host's Dream - the game closed right there. A new load only once the
    // last one has been over for a while and our character stands in the map.
    static s64 last_load_ms = 0;
    if (ReadValue<u8>(global, 0x08) != 0) {
        last_load_ms = now;
        return; // a load is running: let it finish, whoever asked for it
    }
    const bool settled = now - last_load_ms >= BbscFollowAfterLoadMs;

    BbscFollowMessage host{};
    {
        std::scoped_lock lock{bbsc_follow_mutex};
        host = bbsc_follow_latest;
    }
    if (host.received_ms == 0 || now - host.received_ms > BbscFollowStaleMs ||
        !IsUsablePackedMap(host.packed_map)) {
        return; // the host is not telling us anything we can act on
    }

    if (warping) {
        if (GetCurrentPackedMap() != target_map) {
            if (now - warp_requested_ms > BbscFollowArriveTimeoutMs) {
                // Never waiting forever again (code review A3): the load went somewhere else.
                warping = false;
                LOG_WARNING(Debug,
                            "BBSC warp together: no arrival in map {:#x} after {} s (now in "
                            "{:#x}), attempt {} of {}",
                            target_map, BbscFollowArriveTimeoutMs / 1000, GetCurrentPackedMap(),
                            attempts, BbscFollowMaxAttempts);
                EndBbscLoadCover("follow load did not arrive");
            }
            return; // still on the way
        }
        if (now - warp_requested_ms < BbscFollowSettleMs) {
            return; // arrived, let the map settle before placing the character
        }
        warping = false;
        const u64 player = GetLocalPlayerCharacter();
        const u64 vtable = ReadablePointer(player, 0x08) ? ReadValue<u64>(player, 0) : 0;
        const u64 warp = ReadablePointer(vtable, 0x600) ? ReadValue<u64>(vtable, 0x5F8) : 0;
        if (!IsEbootCodeAddress(warp)) {
            LOG_WARNING(Debug,
                        "BBSC warp together: arrived in map {:#x} but the character vtable is "
                        "unexpected, staying where the load left us",
                        target_map);
            EndBbscLoadCover("arrived, not placed");
            return;
        }
        if (host.packed_map != target_map) {
            // 2026-10-12 (a tester, guest, 21:16:42): the host loaded again (into the next block,
            // 0x18010000) while our load into 0x18000000 ran; its position, in the other block's
            // coordinates, put us under the ground and we fell through the map. Positions are per
            // map block: stay where the load put us (the host's spot when it was asked for).
            LOG_INFO(Debug,
                     "BBSC warp together: arrived in map {:#x}, but the host has moved on to "
                     "{:#x} since - staying where the load put us (positions are per map)",
                     target_map, host.packed_map);
            ArmBbscRejoinLines(true, target_map, BbscGuestRejoinGraceMs);
            EndBbscLoadCover("arrived, host moved on");
            return;
        }
        const BbscFollowSpot spot = BbscFollowSpotOf(host);
        alignas(16) std::array<float, 4> position{spot.x + 1.0f, spot.y, spot.z, 1.0f};
        alignas(16) std::array<float, 4> rotation{0.0f, spot.heading, 0.0f, 0.0f};
        using Warp = void PS4_SYSV_ABI (*)(u64 chr, float* position, float* rotation, u32 flag);
        BbGuestCall(reinterpret_cast<Warp>(warp), player, position.data(), rotation.data(), 1);
        WriteValue<u8>(player, 0x1E1, static_cast<u8>(ReadValue<u8>(player, 0x1E1) | 0x02));
        LOG_INFO(Debug,
                 "BBSC warp together: guest arrived in map {:#x} at the host's {} ({:.2f}, "
                 "{:.2f}, {:.2f})",
                 target_map, spot.what, position[0], position[1], position[2]);
        // The game's own summon line after the rejoin says so ("You have caught up with ..."),
        // see TickBbscRejoinLines; keep it pointed there a little longer.
        ArmBbscRejoinLines(true, target_map, BbscGuestRejoinGraceMs);
        LogBbscHostWorldFlagsDiff(target_map, "after arriving");
        EndBbscLoadCover("arrived beside the host");
        return;
    }

    if (!bbsc_join_teleport_done) {
        return; // the initial join still owns the guest
    }
    const u32 here = GetCurrentPackedMap();
    static s64 mismatch_since_ms = 0;
    // The host's map when both were last in the same one: a different map later means the HOST
    // went somewhere. If only the guest walked off, the host's map is still this one.
    static u32 shared_host_map = 0;
    if (!IsUsablePackedMap(here) || here == host.packed_map) {
        mismatch_since_ms = 0;
        shared_host_map = host.packed_map;
        return; // same map as the host: nothing to follow
    }
    // Report 2026-09-28: a loading screen every time the host walked into the next area. When the
    // host's map is loaded in our game too and the host did not get there by a load (lamp, death
    // reload), the host walked across - so do we, no reload.
    const bool host_loaded_there = host.arrival_by_load && host.arrival_map == host.packed_map &&
                                   now - host.arrival_ms < 90'000;
    // The maps of one area (Cathedral Ward 0x18000000, Central Yharnam 0x18010000, ...) share one
    // coordinate frame and join without a load. Report 2026-09-27: border crossings near the
    // Central Yharnam lamp reloaded the guest. Report 2026-09-29: the guest walked from Cathedral
    // Ward down to Central Yharnam while the host stayed, and was pulled back to the host (the old
    // rule followed any host more than 120 m away). Walking is free now: in the same area the guest
    // follows only a host who got there by a load (lamp, death reload); in another area only a
    // host who changed map since they last shared one.
    const bool host_moved = shared_host_map != 0 && host.packed_map != shared_host_map;
    const bool same_area = (here >> 24) == (host.packed_map >> 24);
    if (!host_loaded_there && (same_area || !host_moved)) {
        static u32 apart_logged = 0;
        if (apart_logged != (here ^ host.packed_map)) {
            apart_logged = here ^ host.packed_map;
            LOG_INFO(Debug,
                     "BBSC warp together: we are in map {:#x}, the host in {:#x} ({}) - no reload",
                     here, host.packed_map,
                     same_area ? "same area, host walked or we walked" : "we walked away");
        }
        mismatch_since_ms = 0;
        return;
    }
    if (!host_loaded_there && BbscMapResident(host.packed_map)) {
        static u32 walk_logged = 0;
        if (walk_logged != host.packed_map) {
            walk_logged = host.packed_map;
            LOG_INFO(Debug,
                     "BBSC warp together: host walked into map {:#x} (loaded here too) - no reload",
                     host.packed_map);
        }
        mismatch_since_ms = 0;
        return;
    }
    // A host standing on a border flips between the two ids; act only on a lasting difference.
    if (mismatch_since_ms == 0) {
        mismatch_since_ms = now;
    }
    if (now - mismatch_since_ms < BbscFollowMismatchMs) {
        return;
    }
    if (!settled || GetCharacterHp(GetLocalPlayerCharacter()) <= 0) {
        return; // just out of a load, or dead: the follow waits (the wipe/respawn logic owns this)
    }
    if (attempts_map != host.packed_map) {
        attempts_map = host.packed_map;
        attempts = 0;
    }
    if (attempts >= BbscFollowMaxAttempts) {
        return; // gave up on this map; a new host map starts a fresh count
    }
    // Load the host's world, not our own: its flags for that map first (sent every second).
    static u32 world_wait_map = 0;
    static s64 world_wait_since_ms = 0;
    if (world_wait_map != host.packed_map) {
        world_wait_map = host.packed_map;
        world_wait_since_ms = now;
    }
    const bool dream_wait = host.packed_map == BbscDreamStateMap && BbscDreamEnabled() &&
                            !BbscHostDreamStateReady() &&
                            now - world_wait_since_ms < BbscWorldFlagsWaitMs;
    if (dream_wait) {
        return; // the host's Dream state comes with its flags, a moment more
    }
    if (!BbscHostWorldFlagsReady(host.packed_map)) {
        if (now - world_wait_since_ms < BbscWorldFlagsWaitMs) {
            return; // give the host's next snapshot a moment to arrive
        }
        LOG_WARNING(Debug,
                    "BBSC world state: no flags from the host for map {:#x} after {} s (older "
                    "build?) - following with our own",
                    host.packed_map, BbscWorldFlagsWaitMs / 1000);
    } else {
        ApplyBbscHostWorldFlags(host.packed_map, "before the follow load");
    }
    ApplyBbscGuestLootView(host.packed_map, "before the follow load");
    if (host.packed_map == BbscDreamStateMap) {
        ApplyBbscHostDreamState("before the follow load");
    } else {
        RestoreBbscOwnDreamState("following the host out of the Dream");
    }
    world_wait_map = 0;
    ++attempts;
    LOG_INFO(Debug,
             "BBSC warp together: host moved to map {:#x} (we are in {:#x}), following "
             "(attempt {} of {}; {}; maps loaded here: {})",
             host.packed_map, here, attempts, BbscFollowMaxAttempts,
             host_loaded_there ? "host arrived by a load" : "host walked", BbscResidentMapsText());
    const BbscFollowSpot spot = BbscFollowSpotOf(host);
    SummonPlacementDescriptor to{};
    to.packed_region = host.packed_map;
    to.x = spot.x + 1.0f;
    to.y = spot.y;
    to.z = spot.z;
    to.heading = spot.heading;
    if (!RequestBbscSessionWarp(global, to, "follow")) {
        LOG_WARNING(Debug, "BBSC warp together: could not ask for the reload into {:#x}",
                    host.packed_map);
        return;
    }
    warping = true;
    target_map = host.packed_map;
    warp_requested_ms = now;
    ArmBbscRejoinLines(true, host.packed_map, BbscFollowArriveTimeoutMs);
}

// ---- World state when warping together (2026-09-25) --------------------------------------------
// A guest that followed the host into another map loaded it with the wrong event flags: in Central
// Yharnam the opening cutscene played (event 12410000 ends only on its own flag), doors and
// shortcuts were shut, and the boss the host had killed stood there until the host's state caught
// up and killed it in front of the guest - echoes and all (log 2026-09-25: kill reward 3466 right
// after the guest's member task restarted at FirstSyncWait). Nothing in the game hands a guest the
// host's flags for a map it enters later, because vanilla ends the session at every stage change.
//
// The game keeps every event flag in SprjEventFlagMan [0x0553B100] (CONFIRMED statically:
// constructor 0x013CF0E0 via 0x013B5910, read 0x013CFD80, write 0x013CFCC0):
//   +0x1C flags per group (1000), +0x20 bytes per group (125), +0x24 group count (1200),
//   +0x28 group storage, +0x38 std::map group key -> node {+0x20 key, +0x28 kind (1: storage
//   index at +0x30, 2: pointer at +0x30)}. Group key = flag / 1000; flag % 1000 is a bit counted
//   from the top bit of each byte.
// A map block (area AA, block B) owns the keys kind * 10000 + AA * 100 + B * 10 + 0..9 (0x013B6540):
//   kind 1  1AABxxxx  its events: bosses, doors, shortcuts, cutscenes, lamps lit
//   kind 5  5AABxxxx  item pickups (ItemLotParam getItemFlagId) - NOT copied: loot stays personal
//   kind 6, 7         its other state (NPC and object states, e.g. the Doll's 72100100-72100102)
// The host sends the kind 1/6/7 groups of the block it stands in (30 x 125 bytes) every second, and
// its kind 5 groups after them (10 more, 5 chunks of SignalingBbscWorldFlags in all). Kind 5 is never
// written: the guest only compares it with its own to log which items it cannot see because its own
// world has them taken (2026-09-26, "guest can't see items"). The guest writes them into its own table right before it
// asks for the follow load and again when that load creates its character (0x0178D9C0 hook), which
// is before the new map's events first run. Global groups (flags below 100000: story, NPC quests)
// are left alone. Only the live table of the host's world changes: while a guest, the save is
// written from the own-world snapshot (0x0156AA50, global+0x1592 == 0), as in vanilla co-op.
constexpr u64 WorldFlagManPointerOffset = 0x0553B100;
constexpr std::array<u32, 4> BbscWorldFlagKinds{1, 6, 7, 5};
constexpr size_t BbscWorldGroupCount = BbscWorldFlagKinds.size() * 10;
constexpr size_t BbscWorldAppliedGroups = 30; // kinds 1, 6, 7; the item pickups (kind 5) after them
constexpr size_t BbscWorldGroupBytes = Libraries::Np::NpSignaling::kBbscWorldFlagGroupBytes;
constexpr size_t BbscWorldChunkGroups = Libraries::Np::NpSignaling::kBbscWorldFlagGroupsPerChunk;
constexpr size_t BbscWorldChunkCount =
    (BbscWorldGroupCount + BbscWorldChunkGroups - 1) / BbscWorldChunkGroups;
constexpr s64 BbscWorldFlagsSendMs = 1'000;
constexpr s64 BbscWorldFlagsFreshMs = 6'000;
static_assert(BbscWorldGroupCount <= 64 && BbscWorldChunkCount <= 8);

// ---- The host's Hunter's Dream (2026-09-27) ----------------------------------------------------
// Report 2026-09-27: in the host's Dream the guest saw its OWN Dream - a Bath Messenger in another
// place, the Memory Altar prompt at the mirror, the Insight messengers closed. User: "the guest
// should see my copy of the hunters dream ... if i have bath messanger locked then his is locked as
// well". The Dream (m21_00) is mostly driven by flags outside its own block: story (1xxx), the
// Dream's progress and the messengers (5xxx, 6xxx, 7xxx, 9xxx), the lamps lit in every other map
// (1AAB7810.., the headstones), key items (5000xxxx...) and the chalice dungeons (94x05xxx). The
// list below is every flag group that m21_00_00_00.emevd tests or sets, and that the Dream's talk
// scripts read, outside m21_00's own groups (those come with the map snapshot above); group 0
// (system flags) is left out. Found with tools/gamedata (emevd.py, esd_dump.py).
// While the host stands in the Dream it sends these too (version 2 of SignalingBbscWorldFlags,
// ignored by older builds). A guest writes them into its live table when it loads into the Dream,
// after keeping its own copy, and puts its own copy back when it leaves the Dream or the session -
// keeping every bit the game itself changed meanwhile (a purchase, a message read). The save is
// written from the own-world snapshot while the guest is away (0x0156AA50, global+0x1592 == 0).
constexpr std::array<u32, 57> BbscDreamStateKeys{
    1,     2,     5,     6,     7,     9,     2100,  12117, 12207, 12301, 12307, 12400,
    12407, 12411, 12417, 12427, 12507, 12601, 12607, 12707, 12801, 12807, 12907, 12910,
    13207, 13307, 13407, 13501, 13507, 13601, 13607, 50000, 50001, 50002, 50003, 52200,
    52400, 52410, 52420, 52600, 52605, 52700, 52800, 53200, 53300, 70000, 70009, 72000,
    72410, 79900, 94005, 94105, 94205, 94305, 94405, 94505, 94605,
};
constexpr size_t BbscDreamGroupCount = BbscDreamStateKeys.size();
constexpr size_t BbscDreamChunkCount =
    (BbscDreamGroupCount + BbscWorldChunkGroups - 1) / BbscWorldChunkGroups;
constexpr size_t BbscSnapshotMaxGroups = 64;
static_assert(BbscDreamGroupCount <= BbscSnapshotMaxGroups && BbscDreamChunkCount <= 8);

bool BbscWorldSyncEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_WORLD_SYNC");
    return enabled;
}

// Flag group key of entry `index` of map `packed_map`'s snapshot; 0 for maps outside the scheme.
u32 BbscWorldGroupKey(u32 packed_map, size_t index) {
    const u32 area = (packed_map >> 24) & 0xFF;
    const u32 block = (packed_map >> 16) & 0xFF;
    if (area == 0 || area > 99 || block > 9 || index >= BbscWorldGroupCount) {
        return 0;
    }
    return BbscWorldFlagKinds[index / 10] * 10'000 + area * 100 + block * 10 +
           static_cast<u32>(index % 10);
}

// The 125 bytes of flag group `key` in the live table; 0 when the table or the group is missing.
u64 BbscFlagGroupStorage(u32 key) {
    const u64 man = ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0);
    if (!ReadablePointer(man, 0x40) || ReadValue<u32>(man, 0x1C) != 1000 ||
        ReadValue<u32>(man, 0x20) != BbscWorldGroupBytes) {
        return 0;
    }
    const u64 head = ReadValue<u64>(man, 0x38);
    if (!ReadablePointer(head, 0x20)) {
        return 0;
    }
    // MSVC std::map nodes: +0x00 left, +0x08 parent (the root, in the head), +0x10 right,
    // +0x19 is-nil. Same lower_bound walk as 0x013CFD80.
    u64 node = ReadValue<u64>(head, 0x08);
    u64 found = head;
    for (int depth = 0; depth < 64 && ReadablePointer(node, 0x38) && ReadValue<u8>(node, 0x19) == 0;
         ++depth) {
        if (key <= ReadValue<u32>(node, 0x20)) {
            found = node;
            node = ReadValue<u64>(node, 0x00);
        } else {
            node = ReadValue<u64>(node, 0x10);
        }
    }
    if (found == head || ReadValue<u32>(found, 0x20) != key) {
        return 0;
    }
    u64 storage = 0;
    switch (ReadValue<s32>(found, 0x28)) {
    case 1: {
        const u32 slot = ReadValue<u32>(found, 0x30);
        if (slot >= ReadValue<u32>(man, 0x24)) {
            return 0;
        }
        storage = ReadValue<u64>(man, 0x28) + static_cast<u64>(slot) * BbscWorldGroupBytes;
        break;
    }
    case 2:
        storage = ReadValue<u64>(found, 0x30);
        break;
    default:
        return 0;
    }
    return storage >= 0x10000 &&
                   HasMemoryAccess(storage, BbscWorldGroupBytes, MemoryProt::CpuWrite)
               ? storage
               : 0;
}

struct BbscWorldSnapshot {
    u32 map = 0;
    u32 snapshot = 0;
    u64 present = 0; // bit per group
    u32 chunks = 0;  // bit per received chunk
    std::array<std::array<u8, BbscWorldGroupBytes>, BbscSnapshotMaxGroups> groups{};
    s64 complete_ms = 0; // 0 while chunks are missing
};

std::mutex bbsc_world_mutex;
BbscWorldSnapshot bbsc_world_assembling; // guest: the snapshot being received
BbscWorldSnapshot bbsc_world_ready;      // guest: the newest complete one
// Guest: the newest complete snapshot of every block of the host's area (2026-09-28). The blocks of
// one area join without a load (Central Yharnam 0x18000000 / Cathedral Ward 0x18010000 / ...), so a
// guest who joined in one block and walked into the next had that block in its OWN state: the
// Oedon Chapel lamp unlit, chests shut or open as in its own world (report 2026-09-28). The host
// now sends every block of its area in turn, and a load writes all of them.
std::map<u32, BbscWorldSnapshot> bbsc_world_blocks;
constexpr s64 BbscWorldBlockFreshMs = 120'000; // other blocks: a turn takes a few seconds
BbscWorldSnapshot bbsc_dream_assembling; // guest: the host's Dream state being received
BbscWorldSnapshot bbsc_dream_ready;      // guest: the newest complete one

void ClearBbscHostWorldSnapshots() {
    std::scoped_lock lock{bbsc_world_mutex};
    bbsc_world_assembling = {};
    bbsc_world_ready = {};
    bbsc_world_blocks.clear();
    bbsc_dream_assembling = {};
    bbsc_dream_ready = {};
}

// Host side: one snapshot of `count` groups (group n = key_of(map, n)) in chunks of 8. Returns the
// peers reached; `present` gets a bit per group that exists here.
u32 SendBbscFlagSnapshot(u8 version, u32 map, u32 snapshot, size_t count,
                         u32 (*key_of)(u32 map, size_t index), u64& present) {
    const size_t chunks = (count + BbscWorldChunkGroups - 1) / BbscWorldChunkGroups;
    u32 sent = 0;
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
        Libraries::Np::NpSignaling::SignalingBbscWorldFlags packet{};
        packet.version = version;
        packet.packed_map = map;
        packet.snapshot = snapshot;
        packet.chunk = static_cast<u8>(chunk);
        packet.chunk_count = static_cast<u8>(chunks);
        const size_t first = chunk * BbscWorldChunkGroups;
        const size_t here = std::min(BbscWorldChunkGroups, count - first);
        packet.group_count = static_cast<u8>(here);
        for (size_t i = 0; i < here; ++i) {
            const u64 storage = BbscFlagGroupStorage(key_of(map, first + i));
            if (storage == 0) {
                continue;
            }
            std::memcpy(packet.groups[i], reinterpret_cast<const void*>(storage),
                        BbscWorldGroupBytes);
            packet.present = static_cast<u8>(packet.present | (1u << i));
            present |= u64{1} << (first + i);
        }
        sent = Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet,
                                                                              sizeof(packet));
    }
    return sent;
}

u32 BbscDreamGroupKey(u32 /*map*/, size_t index) {
    return index < BbscDreamGroupCount ? BbscDreamStateKeys[index] : 0;
}

// Host side: send the flags of the block we stand in to the guest(s), once a second.
void TickBbscWorldFlagsBroadcast() {
    static s64 last_ms = 0;
    static u32 snapshot = 0;
    static u32 logged_map = 0;
    if (!BbscWorldSyncEnabled() || !BbscWarpTogetherEnabled() || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_ms < BbscWorldFlagsSendMs) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    if (GetSessionState() != 3 || !ReadablePointer(global, 0x1598) ||
        ReadValue<u8>(global, 0x1592) == 0 || ReadValue<u8>(global, 0x08) != 0) {
        logged_map = 0;
        return; // not hosting in our own world, or a load is running
    }
    const u32 map = GetCurrentPackedMap();
    if (!IsUsablePackedMap(map) || BbscWorldGroupKey(map, 0) == 0) {
        return;
    }
    // Every other second: another block of this area (one that exists in our flag table), so the
    // guest has them all before it loads in or walks across.
    static u32 turn = 0;
    static u32 other_block = 0;
    ++turn;
    if ((turn & 1) == 0) {
        for (u32 k = 1; k <= 10; ++k) {
            const u32 b = (other_block + k) % 10;
            const u32 candidate = (map & 0xFF000000u) | (b << 16);
            if (candidate != map && BbscFlagGroupStorage(BbscWorldGroupKey(candidate, 0)) != 0) {
                other_block = b;
                ++snapshot;
                u64 other_present = 0;
                SendBbscFlagSnapshot(1, candidate, snapshot, BbscWorldGroupCount, BbscWorldGroupKey,
                                     other_present);
                break;
            }
        }
    }
    ++snapshot;
    u64 present = 0;
    const u32 sent = SendBbscFlagSnapshot(1, map, snapshot, BbscWorldGroupCount,
                                          BbscWorldGroupKey, present);
    u64 dream_present = 0;
    const bool dream = map == BbscDreamStateMap && BbscDreamEnabled();
    if (dream) {
        SendBbscFlagSnapshot(2, map, snapshot, BbscDreamGroupCount, BbscDreamGroupKey,
                             dream_present);
    }
    if (map != logged_map && sent != 0) {
        logged_map = map;
        LOG_INFO(Debug,
                 "BBSC world state: sending our flags for map {:#x} to {} peer(s) ({} of {} groups "
                 "exist; item pickups only for comparison){}",
                 map, sent, std::popcount(present), BbscWorldGroupCount,
                 dream ? fmt::format(", and our Hunter's Dream state ({} of {} groups)",
                                     std::popcount(dream_present), BbscDreamGroupCount)
                       : std::string{});
    }
}

// Guest side: true when a complete, recent snapshot of the host's flags for that map is in.
bool BbscHostWorldFlagsReady(u32 packed_map) {
    if (!BbscWorldSyncEnabled()) {
        return true; // nothing to wait for
    }
    std::scoped_lock lock{bbsc_world_mutex};
    const auto it = bbsc_world_blocks.find(packed_map);
    return it != bbsc_world_blocks.end() && it->second.complete_ms != 0 &&
           SteadyNowMs() - it->second.complete_ms < BbscWorldFlagsFreshMs;
}

// Guest side: the newest snapshot of that block (false if none).
bool BbscHostBlockSnapshot(u32 packed_map, BbscWorldSnapshot& out) {
    std::scoped_lock lock{bbsc_world_mutex};
    const auto it = bbsc_world_blocks.find(packed_map);
    if (it == bbsc_world_blocks.end() || it->second.complete_ms == 0) {
        return false;
    }
    out = it->second;
    return true;
}

// Guest side: puts the host's flags for that map into our live table. Game thread.
bool ApplyBbscHostWorldFlags(u32 packed_map, std::string_view why) {
    if (!BbscWorldSyncEnabled() || image_base == 0) {
        return false;
    }
    BbscWorldSnapshot host;
    if (!BbscHostBlockSnapshot(packed_map, host)) {
        LOG_WARNING(Debug, "BBSC world state ({}): no host flags for map {:#x}", why, packed_map);
        return false;
    }
    // The Dream's messengers (2026-10-08, 3-player test in a tester's Dream: "the guest cannot take
    // the weapons if the host did"). Events 12101020-12101028 hold out the starter trick weapon /
    // firearm, the Notebook, the Beckoning Bell, the Old Hunter Bell and the Eye of a Blood-drunk
    // Hunter; they end on their own done flags (group 12101, bits 20-29), play through 12105050-
    // 12105069 and keep the messengers' item states in group 72101. Written from the host, a host who
    // had taken them left nothing for the guest. Those stay the guest's own.
    const auto keep_own_dream_messengers = [](u32 key, const u8* before, u8* live) {
        const auto keep_bits = [&](u32 first, u32 last) {
            for (u32 bit = first; bit <= last && bit / 8 < BbscWorldGroupBytes; ++bit) {
                const u8 mask = static_cast<u8>(0x80u >> (bit % 8));
                live[bit / 8] = static_cast<u8>((live[bit / 8] & ~mask) | (before[bit / 8] & mask));
            }
        };
        if (key == 72101) {
            std::memcpy(live, before, BbscWorldGroupBytes);
        } else if (key == 12101) {
            keep_bits(20, 29);
        } else if (key == 12105) {
            keep_bits(50, 69);
        }
    };
    const auto write_block = [&](const BbscWorldSnapshot& snap, size_t& changed, size_t& missing) {
        size_t written = 0;
        for (size_t i = 0; i < BbscWorldAppliedGroups; ++i) {
            if ((snap.present & (u64{1} << i)) == 0) {
                continue;
            }
            const u32 key = BbscWorldGroupKey(snap.map, i);
            const u64 storage = BbscFlagGroupStorage(key);
            if (storage == 0) {
                ++missing;
                continue;
            }
            auto* live = reinterpret_cast<u8*>(storage);
            KeepBbscOwnWorldGroup(key, live);
            std::array<u8, BbscWorldGroupBytes> before{};
            std::memcpy(before.data(), live, BbscWorldGroupBytes);
            for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                changed += live[b] != snap.groups[i][b] ? 1 : 0;
            }
            std::memcpy(live, snap.groups[i].data(), BbscWorldGroupBytes);
            if (snap.map == BbscDreamStateMap) {
                keep_own_dream_messengers(key, before.data(), live);
            }
            ++written;
        }
        return written;
    };
    size_t changed_bytes = 0;
    size_t missing = 0;
    const size_t written = write_block(host, changed_bytes, missing);
    LOG_INFO(Debug,
             "BBSC world state ({}): host's flags for map {:#x} written ({} groups, {} bytes "
             "differed, {} groups missing here, snapshot {} is {} ms old)",
             why, packed_map, written, changed_bytes, missing, host.snapshot,
             SteadyNowMs() - host.complete_ms);
    // The rest of the area: the blocks the guest can walk into without a load.
    std::vector<BbscWorldSnapshot> others;
    {
        std::scoped_lock lock{bbsc_world_mutex};
        const s64 now = SteadyNowMs();
        for (const auto& [map, snap] : bbsc_world_blocks) {
            if (map != packed_map && (map >> 24) == (packed_map >> 24) && snap.complete_ms != 0 &&
                now - snap.complete_ms < BbscWorldBlockFreshMs) {
                others.push_back(snap);
            }
        }
    }
    for (const BbscWorldSnapshot& snap : others) {
        size_t other_changed = 0;
        size_t other_missing = 0;
        const size_t n = write_block(snap, other_changed, other_missing);
        LOG_INFO(Debug,
                 "BBSC world state ({}): host's flags for the neighbouring block {:#x} written ({} "
                 "groups, {} bytes differed, {} groups missing here)",
                 why, snap.map, n, other_changed, other_missing);
    }
    WriteBbscOwnWorldFile();
    return written != 0;
}

// Guest side, diagnostics only: how far our live flags for that map are from the host's.
void LogBbscHostWorldFlagsDiff(u32 packed_map, std::string_view why) {
    if (!BbscWorldSyncEnabled() || image_base == 0) {
        return;
    }
    BbscWorldSnapshot host;
    if (!BbscHostBlockSnapshot(packed_map, host)) {
        return;
    }
    size_t groups = 0;
    size_t bytes = 0;
    size_t hidden_for_us = 0;   // item flags set in our table only: taken in our own world
    size_t taken_by_host = 0;   // set in the host's only: gone for the host, still here for us
    for (size_t i = 0; i < BbscWorldGroupCount; ++i) {
        const u64 storage = (host.present & (u64{1} << i)) != 0
                                ? BbscFlagGroupStorage(BbscWorldGroupKey(packed_map, i))
                                : 0;
        if (storage == 0) {
            continue;
        }
        const auto* live = reinterpret_cast<const u8*>(storage);
        if (i >= BbscWorldAppliedGroups) {
            for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                hidden_for_us += std::popcount(static_cast<u8>(live[b] & ~host.groups[i][b]));
                taken_by_host += std::popcount(static_cast<u8>(host.groups[i][b] & ~live[b]));
            }
            continue;
        }
        size_t differ = 0;
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            differ += live[b] != host.groups[i][b] ? 1 : 0;
        }
        groups += differ != 0 ? 1 : 0;
        bytes += differ;
    }
    LOG_INFO(Debug,
             "BBSC world state ({}): map {:#x} differs from the host's in {} groups / {} bytes; "
             "items: {} taken in our own world (hidden for us only), {} taken by the host (still "
             "here for us)",
             why, packed_map, groups, bytes, hidden_for_us, taken_by_host);
}

bool BbscGuestLootActive(); // see "Guest loot view"

// Guest side: our own values of the Dream groups while the host's are in the live table.
struct BbscDreamOwnState {
    u64 held = 0; // bit per BbscDreamStateKeys entry: own copy kept, host's written
    std::array<std::array<u8, BbscWorldGroupBytes>, BbscDreamGroupCount> own{};
    std::array<std::array<u8, BbscWorldGroupBytes>, BbscDreamGroupCount> written{};
    s64 applied_ms = 0;
};
BbscDreamOwnState bbsc_dream_own; // game thread only

// The Dream's shops in the host's Dream (2026-10-07). Report: "I could buy anything from the Bath
// Messenger" (guest, only the Cleric Beast badge). A ShopLineupParam row is shown when its qwcId
// flag is on (-1 = always) and counts its sales in 8 bits from eventFlag; the Dream's shops are
// rows 2000.. (starter weapons), 100000-149999 (Bath Messengers, t210691 c1_22) and 200000-200099
// (Insight messengers, t210698 c1_86). Those flags sit in groups 5, 6 and 70009, which the host's
// Dream state wrote over the guest's, so the guest shopped with the host's unlocks and stock. The
// shop flags keep the guest's own values: group 70009 is nothing but sold counts (all of it), and
// these are the unlock flags in groups 5 and 6 (tools/gamedata, ShopLineupParam qwcId/eventFlag
// of those rows). The rest of the Dream still looks like the host's.
constexpr u32 BbscDreamSoldGroup = 70009;
// Plus the Dream NPCs' "already met" flags (2026-10-08, a tester's guest: "bath messengers keep
// resetting and laughing"): the talk scripts play the first meeting while their flag is off and set
// it after (t210691 6710, t210692 6702-6704/6719, t210693 6701, t210694 6713, t210695 6714, t210698
// 6711, t210720 6716/6722). From a host who never met them the guest got the intro every visit.
constexpr std::array<u16, 130> BbscDreamShopUnlockFlags{
    6701, 6702, 6703, 6704, 6710, 6711, 6713, 6714, 6716, 6719, 6722,
    5000, 5001, 5003, 5004, 5005, 5006, 5007, 5009, 5053, 5090, 5091, 5100, 5120, 5121, 5140,
    5160, 5161, 5162, 5163, 5180, 5200, 5201, 5202, 5203, 5220, 5221, 5222, 5223, 5240, 5241,
    5242, 5243, 5244, 5260, 5261, 5262, 5263, 5264, 5280, 5281, 5282, 5500, 5501, 5502, 5503,
    5504, 5505, 5506, 5507, 5508, 5509, 5510, 5511, 5512, 5513, 5514, 5515, 5516, 5517, 5518,
    5519, 5520, 5521, 5901, 5902, 5903, 5904, 5910, 5911, 5912, 5913, 5914, 6192, 6603, 6610,
    6611, 6612, 6613, 6614, 6615, 6616, 6617, 6622, 6650, 6651, 6652, 6653, 6654, 6655, 6656,
    6657, 6660, 6661, 6662, 6663, 6664, 6665, 6666, 6667, 6671, 6675, 6680, 6681, 6682, 6683,
    6684, 6685, 6686, 6687, 6688, 6689, 6690, 6691, 6692, 6693, 6694, 6695, 6696, 6697,
};

// Puts the guest's own shop flags of Dream group `index` back into `live`; returns bits changed.
size_t KeepBbscOwnDreamShopFlags(size_t index, u8* live) {
    const u32 key = BbscDreamStateKeys[index];
    const auto& own = bbsc_dream_own.own[index];
    size_t changed = 0;
    if (key == BbscDreamSoldGroup) {
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            changed += std::popcount(static_cast<u8>(live[b] ^ own[b]));
            live[b] = own[b];
        }
        return changed;
    }
    for (const u16 flag : BbscDreamShopUnlockFlags) {
        if (flag / 1000 != key) {
            continue;
        }
        const u32 bit = flag % 1000;
        const u8 mask = static_cast<u8>(0x80u >> (bit % 8));
        if (bit / 8 >= BbscWorldGroupBytes) {
            continue;
        }
        u8& byte = live[bit / 8];
        const u8 want = static_cast<u8>(own[bit / 8] & mask);
        if ((byte & mask) != want) {
            byte = static_cast<u8>((byte & ~mask) | want);
            ++changed;
        }
    }
    return changed;
}

bool BbscHostDreamStateReady() {
    std::scoped_lock lock{bbsc_world_mutex};
    return bbsc_dream_ready.map == BbscDreamStateMap && bbsc_dream_ready.complete_ms != 0 &&
           SteadyNowMs() - bbsc_dream_ready.complete_ms < BbscWorldFlagsFreshMs;
}

// Guest side: the host's Dream state into our live table (see "The host's Hunter's Dream").
void ApplyBbscHostDreamState(std::string_view why) {
    if (!BbscWorldSyncEnabled() || !BbscDreamEnabled() || image_base == 0) {
        return;
    }
    BbscWorldSnapshot host;
    {
        std::scoped_lock lock{bbsc_world_mutex};
        host = bbsc_dream_ready;
    }
    if (host.map != BbscDreamStateMap || host.complete_ms == 0) {
        LOG_WARNING(Debug, "BBSC Dream state ({}): nothing from the host yet (older build?)", why);
        return;
    }
    size_t written = 0;
    size_t changed_bytes = 0;
    size_t shop_bits = 0;
    for (size_t i = 0; i < BbscDreamGroupCount; ++i) {
        if ((host.present & (u64{1} << i)) == 0) {
            continue;
        }
        const u64 storage = BbscFlagGroupStorage(BbscDreamStateKeys[i]);
        if (storage == 0) {
            continue;
        }
        auto* live = reinterpret_cast<u8*>(storage);
        KeepBbscOwnWorldGroup(BbscDreamStateKeys[i], live);
        if ((bbsc_dream_own.held & (u64{1} << i)) == 0) {
            std::memcpy(bbsc_dream_own.own[i].data(), live, BbscWorldGroupBytes);
            bbsc_dream_own.held |= u64{1} << i;
        }
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            changed_bytes += live[b] != host.groups[i][b] ? 1 : 0;
        }
        std::memcpy(live, host.groups[i].data(), BbscWorldGroupBytes);
        shop_bits += KeepBbscOwnDreamShopFlags(i, live);
        // What is in the table now, so the restore keeps only what the game changes after this.
        std::memcpy(bbsc_dream_own.written[i].data(), live, BbscWorldGroupBytes);
        ++written;
    }
    bbsc_dream_own.applied_ms = SteadyNowMs();
    WriteBbscOwnWorldFile();
    LOG_INFO(Debug,
             "BBSC Dream state ({}): the host's Hunter's Dream - {} groups written, {} bytes "
             "differed from ours (snapshot {} ms old); shops keep our own unlocks and stock ({} "
             "flags differed from the host's)",
             why, written, changed_bytes, SteadyNowMs() - host.complete_ms, shop_bits);
}

// Guest side: our own Dream groups back, keeping whatever the game changed since we wrote.
void RestoreBbscOwnDreamState(std::string_view why) {
    if (bbsc_dream_own.held == 0) {
        return;
    }
    size_t restored = 0;
    size_t kept_bits = 0;
    for (size_t i = 0; i < BbscDreamGroupCount; ++i) {
        if ((bbsc_dream_own.held & (u64{1} << i)) == 0) {
            continue;
        }
        const u64 storage = BbscFlagGroupStorage(BbscDreamStateKeys[i]);
        if (storage == 0) {
            continue;
        }
        auto* live = reinterpret_cast<u8*>(storage);
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            const u8 changed = static_cast<u8>(live[b] ^ bbsc_dream_own.written[i][b]);
            kept_bits += std::popcount(changed);
            live[b] = static_cast<u8>((bbsc_dream_own.own[i][b] & ~changed) | (live[b] & changed));
        }
        // 2026-10-11 squad test: the Dream's own scripts, run for the guest in the host's Dream,
        // switched on Bath Messenger unlocks (flags 5500-5515) and this "changed meanwhile" rule
        // carried them into the guest's own save. Unlock and "met" flags always get our own values
        // back; only the sold counts (group 70009, a purchase) keep what changed.
        if (BbscDreamStateKeys[i] != BbscDreamSoldGroup) {
            kept_bits -= std::min(kept_bits, KeepBbscOwnDreamShopFlags(i, live));
        }
        NoteBbscOwnDreamRestored(BbscDreamStateKeys[i], live);
        ++restored;
    }
    bbsc_dream_own.held = 0;
    LOG_INFO(Debug,
             "BBSC Dream state ({}): our own Dream flags are back ({} groups, {} flags the game "
             "changed meanwhile kept)",
             why, restored, kept_bits);
}

// Guest side: puts our own Dream state back once we are out of the host's Dream for good.
void TickBbscDreamStateRestore() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (bbsc_dream_own.held == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    const bool guest = BbscGuestLootActive();
    if (guest && (GetCurrentPackedMap() == BbscDreamStateMap || !ReadablePointer(global, 0x1598) ||
                  ReadValue<u8>(global, 0x08) != 0 ||
                  now - bbsc_dream_own.applied_ms < 30'000)) {
        return; // in the Dream, loading, or on the way in
    }
    RestoreBbscOwnDreamState(guest ? "out of the Dream" : "no longer a guest");
}

// Guest side: the load into the host's map has created our character (0x0178D9C0 entry). The map's
// events have not run yet; make sure they see the host's flags.
void ApplyBbscHostWorldFlagsAtLoad() {
    if (!BbscWorldSyncEnabled() || image_base == 0 || GetSessionState() != 6) {
        return;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x1592) != 0) {
        return;
    }
    // The load's destination: the summoned placement map this reload goes to (see
    // RequestBbscSessionWarp); the current map may still be the one we left.
    const u32 map = ReadValue<u32>(global, 0x14C4);
    if (!IsUsablePackedMap(map)) {
        return;
    }
    LogBbscHostWorldFlagsDiff(map, "load created the character, before writing");
    ApplyBbscHostWorldFlags(map, "load created the character");
    if (map == BbscDreamStateMap) {
        ApplyBbscHostDreamState("load created the character");
    }
}

// The character being played (also used by shared progress on disk, below).
struct BbscSaveSlot {
    std::string user;
    int slot = -1;
};
const auto bbsc_run_started = std::filesystem::file_time_type::clock::now();

#ifdef _WIN32
extern "C" __declspec(dllimport) wchar_t* __stdcall GetCommandLineW();
#endif

// The local user this process plays as: --user-id N on the emulator's command line, empty when
// not given (one player per PC: the newest save is then ours). 2026-10-12 two-window test: both
// windows share %APPDATA%\shadPS4, the guest took the host's newer save (user 1002) for its own,
// wrote its own-world journal and pending shared progress under it, and the HOST merged them
// ("own world restored after a crash" + shared progress notice at every join).
const std::string& BbscProcessUserId() {
    static const std::string id = [] {
        std::string out;
#ifdef _WIN32
        const std::wstring line = GetCommandLineW();
        const std::wstring flag = L"--user-id";
        const size_t at = line.find(flag);
        if (at != std::wstring::npos) {
            size_t i = at + flag.size();
            while (i < line.size() && (line[i] == L' ' || line[i] == L'=' || line[i] == L'"')) {
                ++i;
            }
            while (i < line.size() && line[i] >= L'0' && line[i] <= L'9') {
                out += static_cast<char>(line[i++]);
            }
        }
#endif
        if (!out.empty()) {
            LOG_INFO(Debug, "BBSC save slot: this window plays as local user {} - only its saves "
                            "count as ours",
                     out);
        }
        return out;
    }();
    return id;
}

// The character being played: the save slot file the game wrote last, during this run.
std::optional<BbscSaveSlot> BbscActiveSaveSlotUnchecked();
// Runs inside game hooks (the join load): a filesystem error must never become an exception there.
std::optional<BbscSaveSlot> BbscActiveSaveSlot() {
    try {
        return BbscActiveSaveSlotUnchecked();
    } catch (const std::exception& e) {
        LOG_WARNING(Debug, "BBSC save slot: could not read the save folder ({})", e.what());
        return std::nullopt;
    }
}

std::optional<BbscSaveSlot> BbscActiveSaveSlotUnchecked() {
    namespace fs = std::filesystem;
    std::error_code ec;
    // As shadPS4 picks its user folder: <working folder>\user when it exists (portable install),
    // else %APPDATA%\shadPS4 - never both (another window may play from the other one).
    std::vector<fs::path> bases{fs::current_path(ec) / "user"};
#ifdef _WIN32
    if (!fs::is_directory(bases.front() / "home", ec) &&
        !fs::is_directory(bases.front() / "savedata", ec)) {
        bases.clear();
        if (const char* appdata = std::getenv("APPDATA")) {
            bases.push_back(fs::path(appdata) / "shadPS4");
        }
    }
    ec.clear();
#endif
    std::optional<BbscSaveSlot> best;
    fs::file_time_type best_time = bbsc_run_started;
    // BBSC_SAVE_DIR (2026-10-12): the folder holding userdata0000.. on hosts that keep saves
    // elsewhere (bbhost: <data>\saves\SPRJ0005); its slots count as user "save".
    if (const char* save_dir = std::getenv("BBSC_SAVE_DIR"); save_dir != nullptr && save_dir[0]) {
        for (int slot = 0; slot < 10; ++slot) {
            const fs::path file = fs::path(save_dir) / fmt::format("userdata{:04}", slot);
            const auto written = fs::last_write_time(file, ec);
            if (!ec && written > best_time) {
                best_time = written;
                best = BbscSaveSlot{"save", slot};
            }
            ec.clear();
        }
    }
    for (const fs::path& base : bases) {
        for (const auto& user : fs::directory_iterator(base / "home", ec)) {
            const std::string name = user.path().filename().string();
            if (name == "1004") {
                continue; // the user's rule: never touch user 1004
            }
            if (!BbscProcessUserId().empty() && name != BbscProcessUserId()) {
                continue; // another window's character on this PC
            }
            const fs::path dir = user.path() / "savedata" / "CUSA00207" / "SPRJ0005";
            for (int slot = 0; slot < 10; ++slot) {
                const fs::path file = dir / fmt::format("userdata{:04}", slot);
                const auto written = fs::last_write_time(file, ec);
                if (!ec && written > best_time) {
                    best_time = written;
                    best = BbscSaveSlot{name, slot};
                }
                ec.clear();
            }
        }
        ec.clear();
        // The older layout <base>\savedata\<user>\CUSA00207 (BBport for Windows keeps its saves
        // there, 2026-10-12); its users are named "sd<user>" so they never share a shadPS4 user's files.
        for (const auto& user : fs::directory_iterator(base / "savedata", ec)) {
            const std::string name = user.path().filename().string();
            if (name == "1004") {
                continue;
            }
            const fs::path dir = user.path() / "CUSA00207" / "SPRJ0005";
            for (int slot = 0; slot < 10; ++slot) {
                const fs::path file = dir / fmt::format("userdata{:04}", slot);
                const auto written = fs::last_write_time(file, ec);
                if (!ec && written > best_time) {
                    best_time = written;
                    best = BbscSaveSlot{"sd" + name, slot};
                }
                ec.clear();
            }
        }
        ec.clear();
    }
    return best;
}


// ---- Own world journal (2026-10-08) --------------------------------------------------------------
// Test 2026-10-08 (cls_amg guest in a tester's world): the guest's game crashed during a follow load
// and came back stuck in a Hunter's Dream that was the HOST's (a new save: hardly any headstones).
// The save writes the event flags from the LIVE table (0x01399DE0, see "Shared progress on disk"),
// and while a guest that table holds what BBSC wrote in: the host's map flags (world sync, kinds
// 1/6/7), the item view (kind 5) and the host's Dream. The Dream went back in memory when the guest
// left it; the map groups never did (a door or boss of the guest's own world came back as the host
// has it), and nothing survived a crash. Now the first time a group is overwritten while a guest,
// our own copy is kept - in memory and in <mod>/own-world/own-<user>-<slot>.bin - and put back once
// we are home, before shared progress adds the host's progress. A file left by a crash is put back
// the next time that character is home. The Dream's own restore (which keeps what the game changed
// meanwhile, e.g. a purchase) updates the kept copy of the Dream-only groups.
constexpr u32 BbscOwnWorldMagic = 0x574F4242; // "BBOW"
constexpr s64 BbscOwnWorldRecoverMs = 15'000;
std::map<u32, std::array<u8, BbscWorldGroupBytes>> bbsc_own_world; // game thread only
std::optional<BbscSaveSlot> bbsc_own_world_slot;
bool bbsc_own_world_dirty = false;

std::filesystem::path BbscOwnWorldPath(const BbscSaveSlot& slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::CapturesDir).parent_path() / "own-world" /
           fmt::format("own-{}-{}.bin", slot.user, slot.slot);
}

// A group of a map's own flags (world sync / item view) as opposed to the Dream's other groups.
bool BbscIsMapFlagGroup(u32 key) {
    const u32 kind = key / 10'000;
    const u32 area = (key % 10'000) / 100;
    return (kind == 1 || kind == 5 || kind == 6 || kind == 7) && area >= 21 && area <= 99;
}

void KeepBbscOwnWorldGroup(u32 key, const u8* live) {
    if (key == 0 || live == nullptr || bbsc_own_world.contains(key)) {
        return;
    }
    std::memcpy(bbsc_own_world[key].data(), live, BbscWorldGroupBytes);
    bbsc_own_world_dirty = true;
}

// 2026-10-12 (a tester, guest, 21:11:20): the summon load resets the item flags of the map it
// loads, and the journal kept its first copy at "load created the character" - after the reset:
// "0 taken in our own world", every item she had taken at home could be taken again in the host's
// world, and the empty copy went back into her own world at home (taken items back there too).
// After a later warp the game re-read her real flags (69 taken) and BBSC switched the items off.
// Now the whole own world is kept when the guest accepts the summon (JoinSession_White, on the
// game thread, before the load): every map group of kinds 1/5/6/7 in the flag table.
void CaptureBbscOwnWorldBeforeJoin() {
    if (image_base == 0 || !bbsc_own_world.empty()) {
        return; // a copy is kept already (an earlier session that has not got home yet)
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x1592) == 0) {
        return; // not in our own world
    }
    const u64 man = ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0);
    if (!ReadablePointer(man, 0x40)) {
        return;
    }
    const u64 head = ReadValue<u64>(man, 0x38);
    if (!ReadablePointer(head, 0x20)) {
        return;
    }
    // In-order walk of the game's std::map (see BbscFlagGroupStorage for the node layout).
    std::vector<u32> keys;
    std::vector<u64> stack;
    u64 node = ReadValue<u64>(head, 0x08);
    const auto usable = [](u64 n) { return ReadablePointer(n, 0x38) && ReadValue<u8>(n, 0x19) == 0; };
    size_t visited = 0;
    while ((usable(node) || !stack.empty()) && visited < 50'000) {
        while (usable(node) && stack.size() < 128) {
            stack.push_back(node);
            node = ReadValue<u64>(node, 0x00);
        }
        if (stack.empty()) {
            break;
        }
        node = stack.back();
        stack.pop_back();
        ++visited;
        const u32 key = ReadValue<u32>(node, 0x20);
        if (BbscIsMapFlagGroup(key)) {
            keys.push_back(key);
        }
        node = ReadValue<u64>(node, 0x10);
    }
    size_t kept = 0;
    size_t items = 0;
    for (const u32 key : keys) {
        const u64 storage = BbscFlagGroupStorage(key);
        if (storage == 0) {
            continue;
        }
        KeepBbscOwnWorldGroup(key, reinterpret_cast<const u8*>(storage));
        ++kept;
        if (key / 10'000 == 5) {
            for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                items += std::popcount(ReadValue<u8>(storage, b));
            }
        }
    }
    LOG_INFO(Debug,
             "BBSC own world: kept before the join load - {} map flag group(s) of {} in the table, "
             "{} item(s) taken in our own world",
             kept, visited, items);
    WriteBbscOwnWorldFile();
}

void NoteBbscOwnDreamRestored(u32 key, const u8* live) {
    if (BbscIsMapFlagGroup(key)) {
        return; // a map group: its first kept copy is the right one
    }
    if (const auto it = bbsc_own_world.find(key); it != bbsc_own_world.end()) {
        std::memcpy(it->second.data(), live, BbscWorldGroupBytes);
        bbsc_own_world_dirty = true;
    }
}

void WriteBbscOwnWorldFile() {
    if (!bbsc_own_world_dirty || bbsc_own_world.empty()) {
        return;
    }
    if (!bbsc_own_world_slot.has_value()) {
        bbsc_own_world_slot = BbscActiveSaveSlot();
        if (!bbsc_own_world_slot.has_value()) {
            return; // no save written in this run yet: the next write tries again
        }
    }
    std::vector<u8> out;
    const auto put32 = [&out](u32 v) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    };
    put32(BbscOwnWorldMagic);
    put32(static_cast<u32>(BbscWorldGroupBytes));
    put32(static_cast<u32>(bbsc_own_world.size()));
    for (const auto& [key, bytes] : bbsc_own_world) {
        put32(key);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    std::error_code ec;
    const auto path = BbscOwnWorldPath(*bbsc_own_world_slot);
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (!ec) {
        bbsc_own_world_dirty = false;
    }
}

bool LoadBbscOwnWorldFile(const BbscSaveSlot& slot) {
    std::ifstream file(BbscOwnWorldPath(slot), std::ios::binary);
    u32 magic = 0, group_bytes = 0, count = 0;
    const auto get32 = [&file](u32& v) {
        return static_cast<bool>(file.read(reinterpret_cast<char*>(&v), 4));
    };
    if (!file || !get32(magic) || magic != BbscOwnWorldMagic || !get32(group_bytes) ||
        group_bytes != BbscWorldGroupBytes || !get32(count) || count > 10'000) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        u32 key = 0;
        std::array<u8, BbscWorldGroupBytes> bytes{};
        if (!get32(key) || !file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            break;
        }
        bbsc_own_world.try_emplace(key, bytes); // what is in memory already is newer
    }
    bbsc_own_world_slot = slot;
    return !bbsc_own_world.empty();
}

// Home: true when we are in our own world, not joining or a guest, with the flag table there.
bool BbscHomeForOwnWorld() {
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    return state != 4 && state != 6 && ReadablePointer(global, 0x1598) &&
           ReadValue<u8>(global, 0x1592) != 0 &&
           ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0) != 0;
}

// Our own groups back into the live table. Returns the groups written (0 = nothing kept).
size_t RestoreBbscOwnWorld(std::string_view why) {
    if (bbsc_own_world.empty() || image_base == 0 || !BbscHomeForOwnWorld()) {
        return 0;
    }
    if (bbsc_dream_own.held != 0) {
        RestoreBbscOwnDreamState(why); // first: it keeps what the game changed in the Dream
    }
    size_t restored = 0;
    size_t bits = 0;
    size_t missing = 0;
    for (const auto& [key, own] : bbsc_own_world) {
        const u64 storage = BbscFlagGroupStorage(key);
        if (storage == 0) {
            ++missing;
            continue;
        }
        auto* live = reinterpret_cast<u8*>(storage);
        if (key / 10'000 == 5) {
            // Item pickups (kind 5) only ever get taken: never clear one. The live table holds
            // our own pickups plus those made in hosts' worlds (the guest loot view writes no
            // host bits), and a copy kept at the wrong moment (2026-10-12: an empty one, kept
            // after the join load had reset the map's items) must not bring taken items back.
            for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                bits += std::popcount(static_cast<u8>(own[b] & ~live[b]));
                live[b] |= own[b];
            }
            ++restored;
            continue;
        }
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            bits += std::popcount(static_cast<u8>(live[b] ^ own[b]));
        }
        std::memcpy(live, own.data(), BbscWorldGroupBytes);
        ++restored;
    }
    LOG_INFO(Debug,
             "BBSC own world ({}): {} flag group(s) of our own world put back ({} flags differed "
             "from the host's, {} groups not in the table)",
             why, restored, bits, missing);
    bbsc_own_world.clear();
    bbsc_own_world_dirty = false;
    if (bbsc_own_world_slot.has_value()) {
        std::error_code ec;
        const auto path = BbscOwnWorldPath(*bbsc_own_world_slot);
        std::filesystem::rename(path, std::filesystem::path(path).concat(".restored"), ec);
    }
    bbsc_own_world_slot.reset();
    return restored;
}

// A file an earlier run left (the game crashed or was closed while a guest): put back now.
bool RecoverBbscOwnWorldFile(const BbscSaveSlot& slot) {
    if (!bbsc_own_world.empty() || !BbscHomeForOwnWorld() || !LoadBbscOwnWorldFile(slot)) {
        return false;
    }
    LOG_WARNING(Debug, "BBSC own world: user {} slot {} was left with another player's world in it "
                       "(crash or quit as a guest) - putting our own back",
                slot.user, slot.slot);
    if (RestoreBbscOwnWorld("after a crash") != 0) {
        BbscAnnounce(u"Your own world was put back\n(the game had closed while you were a guest).\n"
                     u"Rest at a lamp or reload to see it.",
                     "own world restored after a crash");
    }
    return true;
}

void TickBbscOwnWorld() {
    static s64 last_ms = 0;
    static s64 recover_checked_ms = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    if (!BbscHomeForOwnWorld()) {
        WriteBbscOwnWorldFile(); // still away: a slot that was unknown may be known now
        return;
    }
    const u64 global = GetGlobalState();
    if (ReadValue<u8>(global, 0x08) != 0) {
        return; // loading: the load's own hooks put it back
    }
    if (!bbsc_own_world.empty()) {
        RestoreBbscOwnWorld("home");
        return;
    }
    const u64 me = GetLocalPlayerCharacter();
    if (me == 0 || GetCharacterHp(me) <= 0 || now - recover_checked_ms < BbscOwnWorldRecoverMs) {
        return;
    }
    recover_checked_ms = now;
    if (const auto slot = BbscActiveSaveSlot(); slot.has_value()) {
        RecoverBbscOwnWorldFile(*slot);
    }
}

// The load home: as early as the load's respawn point / character creation, before the map's
// events read the flags (and before shared progress merges the host's into them).
void RestoreBbscOwnWorldAtLoad() {
    if (!bbsc_own_world.empty()) {
        RestoreBbscOwnWorld("during the home load");
    }
}

// ---- Guest loot view (2026-09-27) --------------------------------------------------------------
// Report 2026-09-27: the guest still could not see the items in the host's world. Since 6D338776 the
// guest's game creates its own items (GuestItems.SpawnOwn / CreateOwn in bbsc_persistence.cpp), but
// from its own pickup flags (kind 5, 5AABxxxx, ItemLotParam getItemFlagId): whatever the guest had
// taken in its own world stayed hidden in every host world too. User decision: in the host's world
// the guest sees every item of the map, also those taken in its own world; only what it picks up
// during this session stays gone (GuestItems.MarkTaken sets those flags in the live table).
//
// So when a load into the host's world starts or creates the guest's character, the kind 5 groups of
// every block of that area (5AAB0..5AAB9 for B = 0..9; the blocks of one area join without a load)
// are set to "taken this session" only. Bits that appeared since our last write are the guest's own
// pickups and are kept from then on. Only the live table of the host's world changes: the guest's
// save is written from the own-world snapshot while it is away (0x0156AA50, global+0x1592 == 0).
//
// Taken items on disk (2026-10-08, the user: "save what item the guest took from the host world and
// make it untakable"). "Taken this session" was forgotten when the session ended, so a reconnect
// showed every item again. The taken bits are now kept per character (save slot, BbscActiveSaveSlot)
// in <mod>/guest-items/taken-<user>-<slot>.bin, for every host's world (an item the guest took in
// one host's world stays gone in all of them; the host's own items are untouched). Pickups are
// caught once a second while settled in the host's world (CaptureBbscGuestLoot), not only at the
// next load, so a disconnect right after a pickup still keeps it.
//
// No duplicates (2026-10-09, the user: "if you pick an item in your world and join another guy's
// world you won't be able to see or take that item"). This reverses the 2026-09-27 decision: an
// item taken in the guest's own world stays gone in every host's world too. The own world's bits come
// from the own world journal (bbsc_own_world, kept before any host write), so they are the guest's
// real pickups, not the host's.
struct BbscLootGroup {
    std::array<u8, BbscWorldGroupBytes> written{}; // what we last put in the live table
    std::array<u8, BbscWorldGroupBytes> taken{};   // picked up by the guest in a host's world
};
std::map<u32, BbscLootGroup> bbsc_loot_groups;
constexpr u32 BbscLootFileMagic = 0x544C4242; // "BBLT"
std::map<u32, std::array<u8, BbscWorldGroupBytes>> bbsc_loot_saved; // the file's groups
std::optional<BbscSaveSlot> bbsc_loot_slot; // the character the file belongs to (this session)
bool bbsc_loot_loaded = false;               // the file was looked for in this session
bool bbsc_loot_dirty = false;                // taken bits not written yet
u32 bbsc_loot_area = 0;                      // area of the last view applied (0 = none yet)

std::filesystem::path BbscLootFilePath(const BbscSaveSlot& slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::CapturesDir).parent_path() /
           "guest-items" / fmt::format("taken-{}-{}.bin", slot.user, slot.slot);
}

// Once per session, at the first view: which character this is and what it took before.
void LoadBbscGuestLootFile() {
    if (bbsc_loot_loaded) {
        return;
    }
    bbsc_loot_loaded = true;
    bbsc_loot_saved.clear();
    bbsc_loot_slot = BbscActiveSaveSlot();
    if (!bbsc_loot_slot.has_value()) {
        LOG_WARNING(Debug, "BBSC guest items: no save written in this run yet - items taken in this "
                           "session are kept until it ends only");
        return;
    }
    std::ifstream file(BbscLootFilePath(*bbsc_loot_slot), std::ios::binary);
    u32 magic = 0, group_bytes = 0, count = 0;
    const auto get32 = [&file](u32& v) {
        return static_cast<bool>(file.read(reinterpret_cast<char*>(&v), 4));
    };
    if (!file || !get32(magic) || magic != BbscLootFileMagic || !get32(group_bytes) ||
        group_bytes != BbscWorldGroupBytes || !get32(count) || count > 10'000) {
        LOG_INFO(Debug, "BBSC guest items: user {} slot {} has no taken-items file yet",
                 bbsc_loot_slot->user, bbsc_loot_slot->slot);
        return;
    }
    size_t items = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 key = 0;
        std::array<u8, BbscWorldGroupBytes> bytes{};
        if (!get32(key) || !file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            break;
        }
        bbsc_loot_saved[key] = bytes;
        for (const u8 b : bytes) {
            items += std::popcount(b);
        }
    }
    LOG_INFO(Debug, "BBSC guest items: user {} slot {} took {} item(s) in hosts' worlds before - "
                    "they stay gone",
             bbsc_loot_slot->user, bbsc_loot_slot->slot, items);
}

void WriteBbscGuestLootFile() {
    if (!bbsc_loot_dirty || !bbsc_loot_slot.has_value()) {
        return;
    }
    bbsc_loot_dirty = false;
    for (const auto& [key, group] : bbsc_loot_groups) {
        auto& saved = bbsc_loot_saved[key];
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            saved[b] |= group.taken[b];
        }
    }
    std::vector<u8> out;
    const auto put32 = [&out](u32 v) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    };
    put32(BbscLootFileMagic);
    put32(static_cast<u32>(BbscWorldGroupBytes));
    put32(static_cast<u32>(bbsc_loot_saved.size()));
    for (const auto& [key, bytes] : bbsc_loot_saved) {
        put32(key);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    std::error_code ec;
    const auto path = BbscLootFilePath(*bbsc_loot_slot);
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
}

bool BbscGuestLootActive() {
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    return (state == 4 || state == 6) && ReadablePointer(global, 0x1598) &&
           ReadValue<u8>(global, 0x1592) == 0;
}

// 2026-10-11 squad test (item 0x53 taken in the host's world at 19:52:26, again at home 19:53:31):
// an item the guest takes in a host's world is gone in the guest's OWN world too. The bit goes into
// the own world journal, which is what is put back at home (RestoreBbscOwnWorld).
void MarkBbscTakenInOwnWorld(u32 key, size_t byte, u8 bits) {
    if (const auto it = bbsc_own_world.find(key); it != bbsc_own_world.end() && byte < BbscWorldGroupBytes) {
        it->second[byte] |= bits;
        bbsc_own_world_dirty = true;
    }
}

void ApplyBbscGuestLootView(u32 packed_map, std::string_view why) {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_GUEST_ITEMS");
    if (!enabled || image_base == 0 || !BbscGuestLootActive()) {
        return;
    }
    const u32 area = (packed_map >> 24) & 0xFF;
    if (area == 0 || area > 99) {
        return;
    }
    LoadBbscGuestLootFile();
    size_t groups = 0;
    size_t shown = 0; // item flags cleared: items the guest now sees
    size_t kept = 0;  // flags of items the guest took this session
    for (u32 block = 0; block < 10; ++block) {
        for (u32 sub = 0; sub < 10; ++sub) {
            const u32 key = 50'000 + area * 100 + block * 10 + sub;
            const u64 storage = BbscFlagGroupStorage(key);
            if (storage == 0) {
                continue;
            }
            auto* live = reinterpret_cast<u8*>(storage);
            KeepBbscOwnWorldGroup(key, live);
            std::array<u8, BbscWorldGroupBytes> own{}; // taken in our own world
            if (const auto mine = bbsc_own_world.find(key); mine != bbsc_own_world.end()) {
                own = mine->second;
            }
            const auto [it, first] = bbsc_loot_groups.try_emplace(key);
            BbscLootGroup& group = it->second;
            if (first) {
                if (const auto saved = bbsc_loot_saved.find(key); saved != bbsc_loot_saved.end()) {
                    group.taken = saved->second; // taken in an earlier session
                }
            }
            for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                if (!first) {
                    const u8 picked = static_cast<u8>(live[b] & ~group.written[b] & ~group.taken[b] &
                                                      ~own[b]);
                    group.taken[b] |= picked;
                    bbsc_loot_dirty |= picked != 0;
                    if (picked != 0) {
                        MarkBbscTakenInOwnWorld(key, b, picked);
                    }
                }
                const u8 gone = static_cast<u8>(group.taken[b] | own[b]);
                shown += std::popcount(static_cast<u8>(live[b] & ~gone));
                kept += std::popcount(gone);
                live[b] = gone;
                group.written[b] = gone;
            }
            ++groups;
        }
    }
    if (groups != 0) {
        bbsc_loot_area = area;
        LOG_INFO(Debug,
                 "BBSC guest items ({}): area {} - {} item flag groups, {} item flags cleared, {} "
                 "taken in our own world or hosts' worlds kept gone",
                 why, area, groups, shown, kept);
    }
    WriteBbscGuestLootFile();
    WriteBbscOwnWorldFile();
}

// Once a second while settled in the host's world (same area as the last view, not loading, alive):
// new bits in the live item groups are the guest's pickups. Written to the file at once.
void CaptureBbscGuestLoot() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (now - last_ms < 1'000 || bbsc_loot_groups.empty() || bbsc_loot_area == 0) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    const u64 me = GetLocalPlayerCharacter();
    if (!BbscGuestLootActive() || !ReadablePointer(global, 0x14C8) ||
        ReadValue<u8>(global, 0x08) != 0 || me == 0 || GetCharacterHp(me) <= 0 ||
        ((ReadValue<u32>(global, 0x14C4) >> 24) & 0xFF) != bbsc_loot_area) {
        return;
    }
    size_t picked_total = 0;
    for (auto& [key, group] : bbsc_loot_groups) {
        if ((key / 100) % 100 != bbsc_loot_area) {
            continue;
        }
        const u64 storage = BbscFlagGroupStorage(key);
        if (storage == 0) {
            continue;
        }
        const auto* live = reinterpret_cast<const u8*>(storage);
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            const u8 picked = static_cast<u8>(live[b] & ~group.written[b] & ~group.taken[b]);
            if (picked != 0) {
                group.taken[b] |= picked;
                group.written[b] |= picked;
                picked_total += std::popcount(picked);
                MarkBbscTakenInOwnWorld(key, b, picked);
            }
        }
    }
    if (picked_total != 0) {
        bbsc_loot_dirty = true;
        WriteBbscGuestLootFile();
        LOG_INFO(Debug, "BBSC guest items: {} item(s) picked up in the host's world - saved, they stay "
                        "gone for this character",
                 picked_total);
    }
}

// Every load into the host's world (the join included, session state 4) creates our character at
// 0x0178D9C0, before the map's treasures are spawned: the item view goes in there.
void ApplyBbscGuestLootViewAtLoad() {
    const u64 global = GetGlobalState();
    if (image_base == 0 || !ReadablePointer(global, 0x14C8)) {
        return;
    }
    const u32 map = ReadValue<u32>(global, 0x14C4);
    if (IsUsablePackedMap(map)) {
        ApplyBbscGuestLootView(map, "load created the character");
    }
    // Report 2026-10-06: "died fighting Cleric Beast, respawned, echoes missing". The player's one
    // bloodstain record is [[0x0553B130]+0x30] (filled by the drop 0x013D2910): +0x18 the spot it
    // lies at (last firm ground), +0x38 the echoes it holds, +0x3C its map. Logged at every load
    // of a guest so a test shows whether the stain is in the map being loaded at all.
    if (BbscGuestLootActive()) {
        const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
        const u64 stain = ReadablePointer(slots, 0x38) ? ReadValue<u64>(slots, 0x30) : 0;
        if (ReadablePointer(stain, 0x40)) {
            const s32 held = ReadValue<s32>(stain, 0x38);
            if (held > 0) {
                const u32 stain_map = ReadValue<u32>(stain, 0x3C);
                LOG_INFO(Debug,
                         "BBSC bloodstain: {} echoes lie in map {:#x} at ({:.1f}, {:.1f}, {:.1f}); "
                         "this load is map {:#x}{}",
                         held, stain_map, ReadValue<float>(stain, 0x18),
                         ReadValue<float>(stain, 0x1C), ReadValue<float>(stain, 0x20), map,
                         stain_map == map ? "" : " (another map)");
            }
        }
    }
}

// Forget the session's pickups once we are no longer a guest in someone's world.
void TickBbscGuestLootReset() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (now - last_ms < 2'000 || bbsc_loot_groups.empty()) {
        return;
    }
    last_ms = now;
    if (!BbscGuestLootActive()) {
        WriteBbscGuestLootFile();
        LOG_INFO(Debug, "BBSC guest items: session over, {} tracked groups put away (kept in the "
                        "file)",
                 bbsc_loot_groups.size());
        bbsc_loot_groups.clear();
        bbsc_loot_loaded = false;
        bbsc_loot_area = 0;
        bbsc_loot_slot.reset();
    }
}

// Cross-map textures come out wrong, and the theory is that the guest ends up holding the map it
// came from as well as the host's. The asset opens in a 2026-09-21 log do interleave three map
// blocks (m24, m27, m32), but those lines carry no timestamps so ordering is not proof of
// residency. The game keeps its loaded maps in a list at CurrentMapListPointerOffset; walking it
// says outright how many are resident and which. Logged only when the set changes, so a long
// session stays readable.
//   list+0x10 -> array, array+0x18 count, array+0x20 entries (stride 0xA0), entry+0x08 packed id,
//   list+0x20 -> index of the map the player is considered to be in.
void TickBbscMapTrace() {
    static s64 last_check_ms = 0;
    static std::string last_shape;
    if (!EnvFlagEnabled("SHADPS4_BBSC_TRACE_MAPS")) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 1'000) {
        return;
    }
    last_check_ms = now;

    const u64 map_list = image_base != 0
                             ? ReadValue<u64>(image_base + CurrentMapListPointerOffset, 0)
                             : 0;
    if (map_list < 0x10000 || !HasMemoryAccess(map_list, 0x24, MemoryProt::CpuRead)) {
        return;
    }
    const s32 index = ReadValue<s32>(map_list, 0x20);
    const u64 map_array = ReadValue<u64>(map_list, 0x10);
    if (map_array < 0x10000 || !HasMemoryAccess(map_array, 0x28, MemoryProt::CpuRead)) {
        return;
    }
    const s32 count = ReadValue<s32>(map_array, 0x18);
    const u64 entries = ReadValue<u64>(map_array, 0x20);
    if (count <= 0 || count > 64 || entries < 0x10000) {
        return;
    }

    std::string shape;
    for (s32 i = 0; i < count; ++i) {
        const u64 entry = entries + static_cast<u64>(i) * 0xA0;
        if (!HasMemoryAccess(entry, 0x10, MemoryProt::CpuRead)) {
            break;
        }
        const u32 packed = ReadValue<u32>(entry, sizeof(u64));
        shape += fmt::format("{}m{:02d}_{:02d}", shape.empty() ? "" : " ", (packed >> 24) & 0xFF,
                             (packed >> 16) & 0xFF);
        if (i == index) {
            shape += "*"; // the one the player counts as being in
        }
    }
    if (shape == last_shape) {
        return;
    }
    last_shape = shape;
    LOG_INFO(Debug, "BBSC maps resident: {} ({} entries, session state {})", shape, count,
             GetSessionState());
}

void ArmBbscDeathLockClear();

// Last lamp position, the way the death respawn finds it (CONFIRMED statically, 2026-09-16):
// global+0x1528 = event point id of the last lamp (read by the respawn select 0x01332BC0, written
// by lamp travel 0x019423A0); the placement step resolves it with
// 0x0154AFC0(map list, &pos, &rot, point id), which the game also calls in normal play from
// 0x01581BE0 with map list = [0x0553B148]. It only finds points in the currently loaded map
// blocks, so a lamp in another map returns nothing.
std::optional<BbscTransform> ResolveLampPoint(s32 point_id) {
    const u64 map_list = ReadValue<u64>(image_base + 0x0553B148, 0);
    if (point_id == -1 || !ReadablePointer(map_list, 0x20)) {
        return std::nullopt;
    }
    alignas(16) std::array<float, 4> position{};
    alignas(16) std::array<float, 4> rotation{};
    using ResolvePoint = u8 PS4_SYSV_ABI (*)(u64 map_list, float* position, float* rotation,
                                             s32 point_id);
    if (BbGuestCall(reinterpret_cast<ResolvePoint>(image_base + 0x0154AFC0), map_list, position.data(),
                                                                rotation.data(), point_id) == 0) {
        return std::nullopt;
    }
    BbscTransform transform{};
    transform.position = position;
    transform.rotation = {0.0f, rotation[1], 0.0f, 0.0f};
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(position[i]) || std::abs(position[i]) > 20000.0f) {
            return std::nullopt;
        }
    }
    return transform;
}

// The nearest lamp of the loaded map to `near`. A guest's own last lamp belongs to their world:
// after following the host through the Hunter's Dream it is the Dream's lamp (point 2102961),
// which Old Yharnam does not contain, and the respawn fell back to "beside the partner"
// (2026-09-24). Every lamp of the loaded map is in FrpgNetMan's lamp list
// ([[0x0553B120]+0xC70]+0x30, std::list, entry +8 = lamp entity id, registered by 0x0146DA20),
// and a lamp's respawn point is its entity id + 1000 (the lamp update compares
// global+0x1528 - 1000 against its own id).
std::optional<BbscTransform> ResolveNearestLoadedLamp(const std::array<float, 4>& near,
                                                      s32* chosen_point) {
    const u64 net_man = ReadValue<u64>(image_base + 0x0553B120, 0);
    if (!ReadablePointer(net_man, 0xC78)) {
        return std::nullopt;
    }
    const u64 lamps = ReadValue<u64>(net_man, 0xC70);
    if (!ReadablePointer(lamps, 0x38)) {
        return std::nullopt;
    }
    const u64 head = ReadValue<u64>(lamps, 0x30);
    if (!ReadablePointer(head, 0x18)) {
        return std::nullopt;
    }
    std::optional<BbscTransform> best;
    float best_distance = 0.0f;
    u64 node = ReadValue<u64>(head, 0);
    for (int guard = 0; node != head && guard < 512 && ReadablePointer(node, 0x18); ++guard) {
        const u64 entry = ReadValue<u64>(node, 0x10);
        if (ReadablePointer(entry, 0x0C)) {
            const s32 point = ReadValue<s32>(entry, 0x08) + 1000;
            if (const auto lamp = ResolveLampPoint(point)) {
                float distance = 0.0f;
                for (size_t i = 0; i < 3; ++i) {
                    const float d = lamp->position[i] - near[i];
                    distance += d * d;
                }
                if (!best.has_value() || distance < best_distance) {
                    best = lamp;
                    best_distance = distance;
                    if (chosen_point != nullptr) {
                        *chosen_point = point;
                    }
                }
            }
        }
        node = ReadValue<u64>(node, 0);
    }
    return best;
}

std::optional<BbscTransform> ResolveLastLampTransform() {
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1530)) {
        return std::nullopt;
    }
    const s32 point_id = ReadValue<s32>(global, 0x1528);
    if (point_id == -1) {
        return std::nullopt;
    }
    const auto transform = ResolveLampPoint(point_id);
    if (!transform.has_value()) {
        LOG_INFO(Debug, "BBSC last lamp point {} is not in the loaded map", point_id);
    }
    return transform;
}

// ---- Map changes after a kept death (2026-09-25) ------------------------------------------------
// Every map change after a kept death stalled: the host's lamp travel froze and the guest's trip
// home after the Silencing Blank never loaded (all 5 captures since 2026-09-24: the StageTransition
// request is set, Warp.StageTransition.AcknowledgeCheck never comes). CONFIRMED statically:
//   * the HP-zero death handler 0x013813D0 sets event flag 4000 and the event manager's request
//     [[SprjEventMan 0x0553B108]+0x10]+8 = 4 (death reload) before the death script starts;
//   * the stage flow 0x0193AC10 arms a requested map change (task +0x153, ack check 0x0194100A)
//     only when the multiplayer members are ready and - while that request is 4 - the player's
//     death flag [[player+0x18]+0x30]+0x18 bit 0 is set (0x0194083A..0x0194087A);
//   * vanilla clears the request with the reload after the death. A kept death never reloads, so
//     the request stayed 4 and the revived player never had the death flag again.
// So a kept death puts the request back to 0 once the player is revived, as the reload would.
// Event flag 4000 stays set: the death handler returns early on it, which is why repeat deaths go
// through TickBbscDeadWatchdog, and that path is unchanged.
constexpr s32 BbscDeathReloadRequest = 4;

u64 BbscEventRequestAddress() {
    const u64 event_man = ReadValue<u64>(image_base + 0x0553B108, 0); // SprjEventMan
    const u64 request = ReadablePointer(event_man, 0x18) ? ReadValue<u64>(event_man, 0x10) : 0;
    return ReadablePointer(request, 0x0C) ? request + 0x08 : 0;
}

bool BbscEventFlag(u32 flag); // SprjEventFlagMan read, see the Hunter's Dream section

void ClearBbscStaleDeathRequest(std::string_view why) {
    const u64 address = BbscEventRequestAddress();
    if (address != 0 && ReadValue<s32>(address, 0) == BbscDeathReloadRequest) {
        WriteValue<s32>(address, 0, 0);
        LOG_INFO(Debug, "BBSC death reload request cleared ({}): lamps and map changes work again",
                 why);
    }
    // Event flag 4000 is the death handler's own "already handled" mark (0x013813D0 returns at
    // once while it is set), which the reload would have reset. Clearing it lets the next death
    // run the game's death script again - "YOU DIED" and all - instead of only the watchdog.
    // SetEventFlag 0x0132E5A0 panics without SprjEventState [0x0553B0D8] / SprjEventFlagMan.
    if (ReadValue<u64>(image_base + 0x0553B0D8, 0) != 0 &&
        ReadValue<u64>(image_base + 0x0553B100, 0) != 0 && BbscEventFlag(4000)) {
        using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
        BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, 4000, 0);
        LOG_INFO(Debug, "BBSC death script re-armed ({}): event flag 4000 cleared", why);
    }
}

// ---- Death curtain (2026-09-25) --------------------------------------------------------------
// A kept death used to pop the player to the lamp in plain view. Now it looks like the game's own
// death: fade to black, the game's loading screen, the revive and warp happen in the dark, then
// the screen fades back in. There is no real load. The pieces, CONFIRMED statically:
//   * SprjFade [0x05540068]+0x20 is the screen fade the stage flow uses for its own transitions:
//     +0x0C current RGBA, +0x1C target RGBA, +0x38 seconds left. Target (0,0,0,1) with 0.5 s is
//     its fade to black, (0,0,0,0) its fade back in (0x0193AC10; 0x04906F70 = {0.0f, 1.0f}).
//   * the loading screen: 0x0176D380(args) opens it (args+8 -> MenuMan+0x17C0, state
//     MenuMan[0x05562878]+0x804 = 1, true once the menu shows it, state 2); 0x0176D3C0() closes
//     it (state 3, true once back to 0). The stage flow calls the opener every frame until shown.
constexpr u64 SprjFadePointerOffset = 0x05540068;
constexpr u64 MenuManPointerOffset = 0x05562878;
constexpr u64 NowLoadingOpenOffset = 0x0176D380;
constexpr u64 NowLoadingCloseOffset = 0x0176D3C0;
constexpr u64 NowLoadingSetStateOffset = 0x0176D320;
constexpr float BbscCurtainFadeOutS = 0.6f;
constexpr float BbscCurtainFadeInS = 0.8f;
constexpr s64 BbscCurtainHoldMs = 2'500;      // black + loading screen, the "load"
constexpr s64 BbscCurtainShowWaitMs = 1'500;  // give up waiting for the loading screen to show
constexpr s64 BbscCurtainCloseWaitMs = 3'000; // give up waiting for it to close
constexpr s64 BbscCurtainLimitMs = 15'000;    // whatever happens, the screen comes back

enum class BbscCurtainStep : u8 { Idle, FadingOut, Loading, Closing, FadingIn };

struct BbscCurtainState {
    BbscCurtainStep step = BbscCurtainStep::Idle;
    s64 started_ms = 0;
    s64 step_ms = 0;
    std::string why;
    bool shown = false;
    const char16_t* notice = nullptr; // the respawn line, shown once the screen is back
    std::string_view notice_what;
};
BbscCurtainState bbsc_curtain;
bool BbscRespawnInPlace(std::string_view why); // below
void BbscRestockFromStorage(std::string_view why); // below
s64 bbsc_inplace_respawn_ms = 0; // last BbscRespawnInPlace revive

bool BbscCurtainActive() {
    return bbsc_curtain.step != BbscCurtainStep::Idle;
}

// The respawn lines wait for the screen to come back.
void BbscAnnounceAfterCurtain(const char16_t* text, std::string_view what) {
    if (BbscCurtainActive()) {
        bbsc_curtain.notice = text;
        bbsc_curtain.notice_what = what;
        return;
    }
    BbscAnnounce(text, what);
}

u64 BbscFadePlate() {
    const u64 fade = ReadValue<u64>(image_base + SprjFadePointerOffset, 0);
    const u64 plate = ReadablePointer(fade, 0x28) ? ReadValue<u64>(fade, 0x20) : 0;
    return ReadablePointer(plate, 0x40) ? plate : 0;
}

bool BbscFadeTo(float alpha, float seconds) {
    const u64 plate = BbscFadePlate();
    if (plate == 0) {
        return false;
    }
    const std::array<float, 4> target{0.0f, 0.0f, 0.0f, alpha};
    WriteValue(plate, 0x1C, target);
    WriteValue<float>(plate, 0x38, seconds);
    return true;
}

bool BbscLoadingScreen(bool open) {
    if (ReadValue<u64>(image_base + MenuManPointerOffset, 0) == 0) {
        return true; // no menu manager: nothing to show or close
    }
    if (open) {
        // MenuMan+0x808 is the loading screen's kind; the stage flow sets 2 for a death reload.
        WriteValue<s32>(ReadValue<u64>(image_base + MenuManPointerOffset, 0), 0x808, 2);
        alignas(16) static std::array<u32, 4> args{}; // args+8: MenuMan+0x17C0, the stage flow's 0
        using Open = bool PS4_SYSV_ABI (*)(const void* args);
        return BbGuestCall(reinterpret_cast<Open>(image_base + NowLoadingOpenOffset), args.data());
    }
    using Close = bool PS4_SYSV_ABI (*)();
    return BbGuestCall(reinterpret_cast<Close>(image_base + NowLoadingCloseOffset));
}

void BbscCurtainStepTo(BbscCurtainStep step, s64 now) {
    bbsc_curtain.step = step;
    bbsc_curtain.step_ms = now;
}

// ---- Load cover (2026-09-26) ------------------------------------------------------------------
// The guest's own loads - the summon load, the second reload after a cross-map join, every follow
// load - showed a bare black screen with the new map flashing up before the guest was placed
// beside the host. The user asked for the death curtain's look there too. The cover fades to black
// (the same SprjFade plate), holds it black through the load(s) and the placement - the stage flow
// fades back in by itself after a load, so the plate is set back to black every frame - and fades
// in once the guest stands beside the host. One cover spans the summon load and the cross-map
// second reload. It never holds the fade timer (+0x38 stays 0): the stage flow waits for that before
// it arms a map change. The game's loading screen (0x0176D380) is left to the stage flow: the
// death curtain's calls never got it to show, and a real load runs its own.
constexpr s64 BbscLoadCoverLimitMs = 75'000;   // whatever happens, the screen comes back
constexpr s64 BbscLoadCoverNoSessionMs = 3'000; // summon failed or the session ended
constexpr float BbscLoadCoverFadeOutS = 0.5f;

struct BbscLoadCoverState {
    bool active = false;
    bool game_screen_seen = false; // the game's own loading screen has shown during this cover
    bool own_screen = false;       // BBSC opened the loading screen (between / after the loads)
    s64 started_ms = 0;
    s64 load_over_ms = 0;          // the game's loading screen closed and no load is running
    s64 no_session_since_ms = 0;
    std::string why;
};
BbscLoadCoverState bbsc_load_cover;
s64 bbsc_join_quiet_until_ms = 0; // the join banner filter stays on until then (after a summon)

bool BbscLoadCoverEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_LOAD_COVER");
    return enabled;
}

void StartBbscLoadCover(std::string_view why) {
    if (!BbscLoadCoverEnabled() || image_base == 0 || bbsc_load_cover.active) {
        return; // one cover spans a summon and its second reload
    }
    BbscFadeTo(1.0f, BbscLoadCoverFadeOutS); // no plate during a load: the tick blacks it later
    bbsc_load_cover = {};
    bbsc_load_cover.active = true;
    bbsc_load_cover.started_ms = SteadyNowMs();
    bbsc_load_cover.why = std::string{why};
    LOG_INFO(Debug, "BBSC load cover: fading out ({})", why);
}

void EndBbscLoadCover(std::string_view how) {
    if (!bbsc_load_cover.active) {
        return;
    }
    bbsc_load_cover.active = false;
    if (bbsc_load_cover.why == "summon") {
        bbsc_join_quiet_until_ms = SteadyNowMs() + 8'000; // the join banner filter (BbscJoinQuiet)
    }
    if (bbsc_load_cover.own_screen) {
        bbsc_load_cover.own_screen = false;
        if (!BbscLoadingScreen(false)) {
            BbGuestCall(reinterpret_cast<void PS4_SYSV_ABI (*)(u32)>(image_base + NowLoadingSetStateOffset), 0);
        }
    }
    if (!BbscCurtainActive()) { // a death curtain brings the screen back itself
        BbscFadeTo(0.0f, BbscCurtainFadeInS);
    }
    LOG_INFO(Debug, "BBSC load cover: fading in after {} ms ({}; {})",
             SteadyNowMs() - bbsc_load_cover.started_ms, bbsc_load_cover.why, how);
}

void TickBbscLoadCover() {
    if (!bbsc_load_cover.active) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - bbsc_load_cover.started_ms > BbscLoadCoverLimitMs) {
        EndBbscLoadCover("time limit");
        return;
    }
    const s32 session_state = GetSessionState();
    if (session_state == 4 || session_state == 5 || session_state == 6) {
        bbsc_load_cover.no_session_since_ms = 0;
    } else if (bbsc_load_cover.no_session_since_ms == 0) {
        bbsc_load_cover.no_session_since_ms = now;
    } else if (now - bbsc_load_cover.no_session_since_ms > BbscLoadCoverNoSessionMs) {
        EndBbscLoadCover("no session");
        return;
    }
    // Test 2026-10-09 (mod 39, both guests): after a wipe the reload put the guests in the host's
    // map, so no follow load ran - and only the follow's arrival ended the cover. The loading screen
    // stayed up 75 s ("infinite loading screen unless you teleport"). A wipe cover ends once its
    // load is over and the respawn after it is done (curtain closed, alive), or 20 s after the load.
    if (bbsc_load_cover.why == "wipe" && bbsc_load_cover.game_screen_seen) {
        const u64 global = GetGlobalState();
        const u64 menu = ReadValue<u64>(image_base + MenuManPointerOffset, 0);
        const bool loading = (ReadablePointer(global, 0x10) && ReadValue<u8>(global, 0x08) != 0) ||
                             (ReadablePointer(menu, 0x810) && ReadValue<s32>(menu, 0x804) != 0 &&
                              !bbsc_load_cover.own_screen);
        if (loading) {
            bbsc_load_cover.load_over_ms = 0;
        } else if (bbsc_load_cover.load_over_ms == 0) {
            bbsc_load_cover.load_over_ms = now;
        } else {
            const s64 since = now - bbsc_load_cover.load_over_ms;
            const bool respawned =
                !BbscCurtainActive() && GetCharacterHp(GetLocalPlayerCharacter()) > 0;
            if ((respawned && since >= 2'000) || since >= 20'000) {
                EndBbscLoadCover(respawned ? "wipe reload done, guest respawned"
                                           : "wipe reload done, time limit after the load");
                return;
            }
        }
    }
    // 2026-09-28: the user found the BBSC overlay loading screen weird and wants the game's own -
    // the one a death reload shows (MENU_NowLoading: the "Bloodborne" logo on black). While a real
    // load runs, the game's loading screen is up (MenuMan+0x804 != 0) and the cover lets it be seen;
    // until it comes up we ask for the death kind (MenuMan+0x808 = 2, what the stage flow sets for a
    // death reload). Black only before and after the load (the flash of the new map before the
    // guest is placed).
    if (const u64 menu = ReadValue<u64>(image_base + MenuManPointerOffset, 0);
        ReadablePointer(menu, 0x810)) {
        if (ReadValue<s32>(menu, 0x804) != 0) {
            if (!bbsc_load_cover.game_screen_seen) {
                bbsc_load_cover.game_screen_seen = true;
                LOG_INFO(Debug, "BBSC load cover: the game's loading screen is up (kind {})",
                         ReadValue<s32>(menu, 0x808));
            }
            return;
        }
        if (!bbsc_load_cover.game_screen_seen) {
            WriteValue<s32>(menu, 0x808, 2);
        } else if (!BbscCurtainActive()) {
            // 2026-10-05 (tester): "it shows me pop in for a second and then it shows me the load
            // screen and then it shows me" - between the summon load and the join's second reload
            // (and after a load, until the guest is placed) the black plate did not hide the map.
            // The game's loading screen closed with its load, so BBSC opens it again (the death
            // curtain's kind) and keeps it up until EndBbscLoadCover; a load in between runs its
            // own on top. HYPOTHESIS until a test shows no pop-in.
            if (BbscLoadingScreen(true)) {
                if (!bbsc_load_cover.own_screen) {
                    LOG_INFO(Debug, "BBSC load cover: loading screen kept up until the guest is "
                                    "placed ({})",
                             bbsc_load_cover.why);
                }
                bbsc_load_cover.own_screen = true;
            }
        }
    }
    if (now - bbsc_load_cover.started_ms < static_cast<s64>(BbscLoadCoverFadeOutS * 1000.0f)) {
        return; // our own fade-out is still running
    }
    const u64 plate = BbscFadePlate();
    if (plate == 0) {
        return;
    }
    static constexpr std::array<float, 4> Black{0.0f, 0.0f, 0.0f, 1.0f};
    WriteValue(plate, 0x0C, Black);
    WriteValue(plate, 0x1C, Black);
    WriteValue<float>(plate, 0x38, 0.0f);
}

// Starts the curtain for a kept death; the respawn itself happens in TickBbscDeathCurtain once the
// screen is black. False when the fade is not there (then the caller respawns in view as before).
bool StartBbscDeathCurtain(std::string_view why) {
    if (BbscCurtainActive()) {
        return true;
    }
    if (!BbscFadeTo(1.0f, BbscCurtainFadeOutS)) {
        LOG_WARNING(Debug, "BBSC death curtain: no screen fade, respawning in view ({})", why);
        return false;
    }
    const s64 now = SteadyNowMs();
    bbsc_curtain = {};
    bbsc_curtain.why = std::string{why};
    bbsc_curtain.started_ms = now;
    BbscCurtainStepTo(BbscCurtainStep::FadingOut, now);
    LOG_INFO(Debug, "BBSC death curtain: fading out ({})", why);
    return true;
}

void EndBbscDeathCurtain(s64 now, std::string_view how) {
    BbscFadeTo(0.0f, BbscCurtainFadeInS);
    BbscCurtainStepTo(BbscCurtainStep::FadingIn, now);
    LOG_INFO(Debug, "BBSC death curtain: fading in after {} ms ({})", now - bbsc_curtain.started_ms,
             how);
}

void TickBbscDeathCurtain() {
    if (!BbscCurtainActive()) {
        return;
    }
    const s64 now = SteadyNowMs();
    const s64 in_step = now - bbsc_curtain.step_ms;
    if (now - bbsc_curtain.started_ms > BbscCurtainLimitMs &&
        bbsc_curtain.step != BbscCurtainStep::FadingIn) {
        // 0x0176D320 writes [MenuMan]+0x804 with no null check (review 2026-10-08): a quit to the
        // title can leave MenuMan gone when this limit fires.
        if (ReadValue<u64>(image_base + MenuManPointerOffset, 0) != 0) {
            BbGuestCall(reinterpret_cast<void PS4_SYSV_ABI (*)(u32)>(image_base + NowLoadingSetStateOffset), 0);
        }
        EndBbscDeathCurtain(now, "time limit");
        return;
    }
    switch (bbsc_curtain.step) {
    case BbscCurtainStep::FadingOut:
        if (in_step < static_cast<s64>(BbscCurtainFadeOutS * 1000.0f) + 100) {
            return;
        }
        if (!BbscRespawnInPlace(bbsc_curtain.why)) {
            // Same as before the curtain: a failed respawn falls back to the game's own reload.
            BbscFadeTo(0.0f, 0.2f);
            bbsc_curtain.step = BbscCurtainStep::Idle;
            // Not on top of a load that is already running (two loads at once crash the game).
            if (const u64 g = GetGlobalState(); ReadablePointer(g, 0x10) && ReadValue<u8>(g, 0x08) == 0) {
                RequestBbscDeathReload("death curtain: respawn in place failed");
            }
            return;
        }
        bbsc_curtain.shown = BbscLoadingScreen(true);
        BbscCurtainStepTo(BbscCurtainStep::Loading, now);
        return;
    case BbscCurtainStep::Loading:
        if (!bbsc_curtain.shown) {
            bbsc_curtain.shown = BbscLoadingScreen(true); // the stage flow also asks every frame
        }
        if (in_step < BbscCurtainHoldMs || (!bbsc_curtain.shown && in_step < BbscCurtainHoldMs +
                                                                      BbscCurtainShowWaitMs)) {
            return;
        }
        if (!bbsc_curtain.shown) {
            LOG_WARNING(Debug, "BBSC death curtain: the loading screen did not show, closing anyway");
        }
        BbscCurtainStepTo(BbscCurtainStep::Closing, now);
        return;
    case BbscCurtainStep::Closing:
        if (BbscLoadingScreen(false)) {
            EndBbscDeathCurtain(now, "loading screen closed");
        } else if (in_step > BbscCurtainCloseWaitMs) {
            BbGuestCall(reinterpret_cast<void PS4_SYSV_ABI (*)(u32)>(image_base + NowLoadingSetStateOffset), 0);
            EndBbscDeathCurtain(now, "loading screen forced closed");
        }
        return;
    case BbscCurtainStep::FadingIn:
        if (in_step < static_cast<s64>(BbscCurtainFadeInS * 1000.0f)) {
            return;
        }
        bbsc_curtain.step = BbscCurtainStep::Idle;
        if (bbsc_curtain.notice != nullptr) {
            BbscAnnounce(bbsc_curtain.notice, bbsc_curtain.notice_what);
        }
        return;
    case BbscCurtainStep::Idle:
        return;
    }
}

// Revive the local player and teleport it, without any load.
bool BbscRespawnInPlace(std::string_view why) {
    // Replicates the local-player branch of the game's event revive 0x013CBAD0(10000, ...)
    // (CONFIRMED statically), in the same order, WITHOUT its final CSMultiPlayMan::Stop call:
    //   chr+0x208 = (1.0, 1.0); warp chr->vtable[0x5F8](chr, &pos, &rot, 1); chr+0x1E4 |= 2;
    //   behaviour [[chr+0x3B0]+0x30]: if +0x384 != 0 play L"Idle_wild" (0x01A19F60);
    //   chr->vtable[0xA8](chr); HP = max (0x018BCD40(chr, [[chr+0x3B0]+0x20]+0xFC));
    //   [[chr+0x58]+0x38]+0x48 &= 0xFC; WorldChrMan+0x68 = 0 (local player);
    //   [[[chr+0x3B0]+0x58]+0x28]+0xC = 1; [chr+0x58]+0x114 &= 0xFFFE.
    // The earlier attempt used 0x01900710, which restores HP but leaves the death flags set
    // (player stayed invisible and could not move, run 2026-09-16).
    // Review 2026-10-08: the curtain fades for ~0.7 s first; a load, a host reload or a quit to the
    // title may have started meanwhile, and the calls below must not run on a world being torn down.
    if (const u64 g = GetGlobalState();
        !ReadablePointer(g, 0x1598) || ReadValue<u8>(g, 0x08) != 0 || !IsSprjLuaEventManReady()) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): a load or the title is under way - skipped",
                    why);
        return false;
    }
    const u64 player = GetLocalPlayerCharacter();
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(player, 0x410) || !ReadablePointer(world_chr, 0x70)) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): no local player", why);
        return false;
    }
    const u64 control = ReadValue<u64>(player, 0x58);
    const u64 modules = ReadValue<u64>(player, 0x3B0);
    if (!ReadablePointer(control, 0x288) || !ReadablePointer(modules, 0x60)) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): character modules unavailable", why);
        return false;
    }
    if (ReadValue<u64>(control, 0x280) == 0 && ReadValue<u64>(control, 0x80) == 0) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): character not physical (0x013CBAD0 guard)",
                    why);
        return false;
    }

    // A guest respawns at the HOST's last lamp, sent by the host with every follow packet
    // (2026-09-24). The guest's own last lamp belongs to the guest's world, and the nearest lamp
    // of the map can be a boss-arena lamp: a guest respawned inside a boss arena that way. If the
    // host's lamp is not in the loaded map, the guest respawns beside the host - warping together
    // would pull a guest in another map straight back to the host anyway.
    const u64 global = GetGlobalState();
    const bool guest = ReadablePointer(global, 0x1593) && ReadValue<u8>(global, 0x1592) == 0;
    std::optional<BbscTransform> target;
    std::string_view where = "last lamp";
    // Testers 2026-10-07: "when the host is in a different area when the guest respawns, the area
    // the host is in is not loaded and guests fall through the map". Beside-the-host used the host
    // character's position, which is in the coordinates of the host's map - not loaded here, so
    // nothing under the guest. The host's position is used only while the host's map is loaded
    // here; otherwise the nearest lamp of OUR map, and the follow then reloads us into the host's.
    bool partner_here = !guest;
    if (guest) {
        s32 host_lamp = -1;
        u32 host_map = 0;
        {
            std::scoped_lock lock{bbsc_follow_mutex};
            if (bbsc_follow_latest.received_ms != 0) {
                host_lamp = bbsc_follow_latest.lamp_point;
                host_map = bbsc_follow_latest.packed_map;
            }
        }
        partner_here = IsUsablePackedMap(host_map) &&
                       (host_map == GetCurrentPackedMap() || BbscMapResident(host_map));
        target = ResolveLampPoint(host_lamp);
        where = "host's last lamp";
        LOG_INFO(Debug, "BBSC respawn: host's last lamp is point {} ({}); host in map {:#x} ({})",
                 host_lamp, target.has_value() ? "in this map" : "not in this map", host_map,
                 partner_here ? "loaded here" : "NOT loaded here - not going beside the host");
        if (!target.has_value() && partner_here) {
            if (const u64 partner = FindLivingPartnerCharacter()) {
                target = ReadCharacterTransform(partner);
                where = "partner";
            }
        }
    } else {
        target = ResolveLastLampTransform();
    }
    if (!target.has_value()) {
        if (const auto body = ReadCharacterTransform(player)) {
            s32 point = -1;
            target = ResolveNearestLoadedLamp(body->position, &point);
            if (target.has_value()) {
                where = "nearest lamp of this map";
                LOG_INFO(Debug, "BBSC respawn: using lamp point {} (nearest to the body)", point);
            }
        }
    }
    if (!target.has_value() && partner_here) {
        if (const u64 partner = FindLivingPartnerCharacter()) {
            target = ReadCharacterTransform(partner);
            where = "partner";
        }
    }
    if (!target.has_value()) {
        target = ReadCharacterTransform(player);
        where = "own body";
    }
    if (!target.has_value()) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): no usable position", why);
        return false;
    }

    // Blood echoes are NOT dropped here: the death step does it itself with
    // 0x013D2910([SprjEventMan+0x80], chr, 1) now that only its reload call is skipped. Dropping
    // them here as well emptied the stain (run 2026-09-19 logged "dropped 0 blood echoes", because
    // the step had already zeroed the count at [[0x0553B130]+8]+0xF8).

    // 1. opacity / scale back to normal
    WriteValue<u64>(player, 0x208, 0x3F8000003F800000ull);

    // 2. warp
    const u64 vtable = ReadValue<u64>(player, 0);
    const u64 warp = ReadablePointer(vtable, 0x600) ? ReadValue<u64>(vtable, 0x5F8) : 0;
    const u64 reset = ReadablePointer(vtable, 0xB0) ? ReadValue<u64>(vtable, 0xA8) : 0;
    if (!IsEbootCodeAddress(warp) || !IsEbootCodeAddress(reset)) {
        LOG_WARNING(Debug, "BBSC respawn in place ({}): character vtable unexpected", why);
        return false;
    }
    alignas(16) std::array<float, 4> position = target->position;
    alignas(16) std::array<float, 4> rotation = target->rotation;
    position[3] = 1.0f;
    rotation[3] = 0.0f;
    // 0x013CBBCB/0x013CBBD8: the event revive stores the warp transform in global+0x1450/+0x1460.
    if (const u64 global = GetGlobalState(); ReadablePointer(global, 0x1470) &&
                                             HasMemoryAccess(global + 0x1450, 0x20,
                                                             MemoryProt::CpuWrite)) {
        for (size_t i = 0; i < 4; ++i) {
            WriteValue<float>(global, 0x1450 + i * 4, position[i]);
            WriteValue<float>(global, 0x1460 + i * 4, rotation[i]);
        }
    }
    using Warp = void PS4_SYSV_ABI (*)(u64 chr, float* position, float* rotation, u32 flag);
    BbGuestCall(reinterpret_cast<Warp>(warp), player, position.data(), rotation.data(), 1);
    WriteValue<u8>(player, 0x1E4, static_cast<u8>(ReadValue<u8>(player, 0x1E4) | 0x02));

    // 3. leave the death animation
    const u64 behaviour = ReadValue<u64>(modules, 0x30);
    if (ReadablePointer(behaviour, 0x388) && ReadValue<u8>(behaviour, 0x384) != 0) {
        using PlayAnimation = void PS4_SYSV_ABI (*)(u64 behaviour, u64 name_utf16);
        BbGuestCall(reinterpret_cast<PlayAnimation>(image_base + 0x01A19F60), behaviour,
                                                                 image_base + 0x04992C40);
    }

    // 4. state reset, HP, death flags, drawing
    using Reset = void PS4_SYSV_ABI (*)(u64 chr);
    BbGuestCall(reinterpret_cast<Reset>(reset), player);
    const u64 stats = ReadValue<u64>(modules, 0x20);
    if (ReadablePointer(stats, 0x100)) {
        using SetHp = void PS4_SYSV_ABI (*)(u64 chr, s32 hp);
        BbGuestCall(reinterpret_cast<SetHp>(image_base + 0x018BCD40), player, ReadValue<s32>(stats, 0xFC));
    }
    const u64 control_flags = ReadValue<u64>(control, 0x38);
    if (ReadablePointer(control_flags, 0x50)) {
        WriteValue<u8>(control_flags, 0x48,
                       static_cast<u8>(ReadValue<u8>(control_flags, 0x48) & 0xFC));
    }
    WriteValue<u8>(world_chr, 0x68, 0);
    const u64 draw = ReadValue<u64>(modules, 0x58);
    const u64 draw_state = ReadablePointer(draw, 0x30) ? ReadValue<u64>(draw, 0x28) : 0;
    if (ReadablePointer(draw_state, 0x10)) {
        WriteValue<u8>(draw_state, 0x0C, 1);
    }
    WriteValue<u16>(control, 0x114, static_cast<u16>(ReadValue<u16>(control, 0x114) & 0xFFFE));

    // 5. the death handler's globals (normally cleared by the reload this path skips)
    if (const u64 global = GetGlobalState(); ReadablePointer(global, 0x1528)) {
        WriteValue<u8>(global, 0x1522, 0);
        WriteValue<u8>(global, 0x1523, 0);
        // PartyGhostDeath_2 sets the forced-warp-home byte (+0x1520) before picking the
        // destination, and 0x01332BC0 sets +0x1524 when a lamp is the destination. Both only
        // matter to the reload that no longer happens, and a leftover +0x1520 sends the guest home
        // on the next transition (same byte OnLeave_Limit uses).
        WriteValue<u8>(global, 0x1520, 0);
        WriteValue<u8>(global, 0x1524, 0);
    }
    // PartyGhostDeath (0x01381FD0) and SoloPlayDeath_2 set the event state [0x0553B0D8] to 2 / 1;
    // the reload resets it (0x013129C0 writes 0). Reset it here as well.
    if (const u64 event_state = ReadValue<u64>(image_base + 0x0553B0D8, 0);
        ReadablePointer(event_state, 4) && HasMemoryAccess(event_state, 4, MemoryProt::CpuWrite)) {
        WriteValue<s32>(event_state, 0, 0);
    }
    ClearBbscStaleDeathRequest(why);
    ArmBbscDeathLockClear();

    LOG_INFO(Debug,
             "BBSC respawn in place ({}): revived (hp {}, type {}), warped to {} ({:.2f}, {:.2f}, "
             "{:.2f})",
             why, GetCharacterHp(player), ReadValue<s32>(player, 0x78), where, position[0],
             position[1], position[2]);
    bbsc_inplace_respawn_ms = SteadyNowMs();
    BbscRestockFromStorage(why);
    if (where == "host's last lamp") {
        BbscAnnounceAfterCurtain(u"You respawned at the host's lamp.",
                                 "respawn at the host's lamp");
    } else if (where == "partner") {
        BbscAnnounceAfterCurtain(u"You respawned next to the host.",
                                 "respawn beside the host");
    } else if (where == "last lamp") {
        BbscAnnounceAfterCurtain(u"You respawned at the lamp.",
                                 "respawn at the lamp");
    } else {
        BbscAnnounceAfterCurtain(u"You respawned.", "respawn");
    }
    return true;
}

// Safety net for a kept death that ends another way (the request is cleared once the local player
// has been alive for 4 s in a session), and a trace of any map change that does not start:
// global+8 is the StageTransition request byte (0x013CDE30), normally consumed within 0.5 s.
void TickBbscStageWatch() {
    static s64 revived_since_ms = 0;
    static s64 requested_since_ms = 0;
    static bool stuck_logged = false;
    const s64 now = SteadyNowMs();
    const s32 session_state = GetSessionState();
    const u64 global = GetGlobalState();
    if ((session_state != 3 && session_state != 6) || !ReadablePointer(global, 0x1600)) {
        revived_since_ms = 0;
        requested_since_ms = 0;
        stuck_logged = false;
        return;
    }
    const u64 request = BbscEventRequestAddress();
    const s32 event_request = request != 0 ? ReadValue<s32>(request, 0) : -1;
    const u64 player = GetLocalPlayerCharacter();
    const bool alive = player != 0 && GetCharacterHp(player) > 0;
    if (event_request == BbscDeathReloadRequest && alive) {
        if (revived_since_ms == 0) {
            revived_since_ms = now;
        } else if (now - revived_since_ms >= 4'000) {
            ClearBbscStaleDeathRequest("player alive for 4 s");
            revived_since_ms = 0;
        }
    } else {
        revived_since_ms = 0;
    }

    if (ReadValue<u8>(global, 0x08) == 0) {
        requested_since_ms = 0;
        stuck_logged = false;
        return;
    }
    if (requested_since_ms == 0) {
        requested_since_ms = now;
        return;
    }
    if (stuck_logged || now - requested_since_ms < 5'000) {
        return;
    }
    stuck_logged = true;
    // The stage flow's other conditions (0x019407B0..0x01940814): list A member tasks must not be
    // past state 6 or marked leaving (+0xED bit 1), list B tasks not past 4 or marked (+0x128 bit 2).
    std::string members;
    for (const u64 task : GetMultiPlayerInsTasks()) {
        members += fmt::format(" A(state {} flags {:#x})", ReadValue<s32>(task, 0x50),
                               ReadValue<u8>(task, 0xED));
    }
    const u64 man = ReadValue<u64>(image_base + MultiPlayManPointerOffset, 0);
    const u64 twins_begin = ReadablePointer(man, 0x48) ? ReadValue<u64>(man, 0x38) : 0;
    const u64 twins_end = twins_begin != 0 ? ReadValue<u64>(man, 0x40) : 0;
    if (twins_end > twins_begin && twins_end - twins_begin <= 16 * sizeof(u64)) {
        for (u64 slot = twins_begin; slot < twins_end; slot += sizeof(u64)) {
            const u64 twin = ReadValue<u64>(slot, 0);
            if (ReadablePointer(twin, 0x130)) {
                members += fmt::format(" B(id {} flags {:#x})", ReadValue<s32>(twin, 0xC8),
                                       ReadValue<u8>(twin, 0x128));
            }
        }
    }
    LOG_WARNING(Debug,
                "BBSC map change not started after 5 s: event request {}, player {}, save flags "
                "{}/{}/{} saving {}, members:{}",
                event_request, alive ? "alive" : "dead", ReadValue<u8>(global, 0x1547),
                ReadValue<u8>(global, 0x1548), ReadValue<s32>(global, 0x154C),
                ReadValue<s32>(global, 0x1554), members.empty() ? " none" : members);
}

// The guest's notice when the host dies. The game's own one ("Your beckoner has died") belongs to
// HostDead 0x01381870, which BBSC skips so that the guest can stay.
void TickBbscHostDeathNotice() {
    static std::array<u64, 4> announced{}; // host characters whose death was already announced
    static s64 last_check_ms = 0;
    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 250) {
        return;
    }
    last_check_ms = now;
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (GetSessionState() != 6 || !ReadablePointer(world_chr, 0x70)) {
        announced = {};
        return;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (chr == 0 || chr == local || !ReadablePointer(chr, 0x3C0) ||
            ReadValue<s32>(chr, 0x78) != 0) {
            continue; // not the host's character (type 0)
        }
        auto it = std::ranges::find(announced, chr);
        if (GetCharacterHp(chr) > 0) {
            if (it != announced.end()) {
                *it = 0;
            }
            continue;
        }
        if (it != announced.end()) {
            continue;
        }
        if (const auto free = std::ranges::find(announced, 0); free != announced.end()) {
            *free = chr;
        }
        BbscAnnounce(u"The host died.\nThey will respawn at the lamp.",
                     "host died");
    }
}

// Death-flow locks that only a reload clears (CONFIRMED statically, 2026-09-16):
//  * MenuMan [0x05562878]+0x2DC: menu brake. Set by the Lua step EventMenuBrake (0x01385EA0), which
//    HostDead (0x01381870) schedules on the partner of a dead host; it is bit 0x100 of the menu-busy
//    mask 0x0176B7E0. Reset only by the menu reset 0x017692F0 (0x01769405) or setter 0x0176BBE0.
//    With it set the guest could not use ladders or levers after a host death without a reload.
//  * [0x0553B130]+0x70: death/warp pending. Set by HostDead (0x01386C6F), SoloPlayDeath_2,
//    PartyGhostDeath_2 and the warp steps; cleared by the player create 0x01939470 (0x01939610).
//  * Event flag 4047 (0xFCF): set by the death Lua steps (HostDead 0x013819C6, SoloPlayDeath
//    0x01381C32, PartyGhostDeath 0x01381FDD); the player update 0x018FEF50 checks it before
//    special-effect events. Written with SetEventFlag 0x0132E5A0(_, flag, value).
// After a respawn in place (own or partner) these are cleared for BbscDeathLockWindowMs, so a
// brake scheduled a few seconds later is undone as well.
constexpr s64 BbscDeathLockWindowMs = 15'000;
s64 bbsc_death_lock_until_ms = 0;

// Diagnostic diff after a respawn in place, see TickBbscStateDiff.
constexpr s64 BbscStateDiffDelayMs = 6'000;
s64 bbsc_state_diff_at_ms = 0;

void ArmBbscDeathLockClear() {
    const s64 now = SteadyNowMs();
    bbsc_death_lock_until_ms = now + BbscDeathLockWindowMs;
    if (bbsc_state_diff_at_ms == 0) {
        bbsc_state_diff_at_ms = now + BbscStateDiffDelayMs;
    }
}

void TickBbscDeathLockClear() {
    if (bbsc_death_lock_until_ms == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now > bbsc_death_lock_until_ms) {
        bbsc_death_lock_until_ms = 0;
        return;
    }
    static s64 last_check_ms = 0;
    if (now - last_check_ms < 250) {
        return;
    }
    last_check_ms = now;
    const s32 session_state = GetSessionState();
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayerCharacter();
    if ((session_state != 3 && session_state != 6) || !ReadablePointer(global, 0x10) ||
        ReadValue<u8>(global, 0x08) != 0 || player == 0 || GetCharacterHp(player) <= 0 ||
        (ReadablePointer(ReadValue<u64>(player, 0x58), 0x118) &&
         (ReadValue<u16>(ReadValue<u64>(player, 0x58), 0x114) & 1) != 0)) {
        return; // loading, no session, or the local player is dead: the locks are legitimate
    }

    bool brake = false;
    bool pending = false;
    bool flag = false;
    u32 leaving_cleared = 0;
    // Slot "leaving" bytes [0x0553B130]+0xA9+slot*2 (reader 0x014EBA90). The death handler marks the
    // dead remote player's slot (0x018C4E30), and for the host's record (id 1) marks EVERY slot,
    // the guest's own included (0x018C4DE8 -> 0x014EB860). Normally cleared only when a slot is
    // reset (0x014E7C20 / 0x014E7C50) or a member leaves (0x0178ABF3).
    if (const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
        ReadablePointer(slots, 0xB0) && HasMemoryAccess(slots + 0xA9, 8, MemoryProt::CpuWrite)) {
        const u64 records = ReadValue<u64>(slots, 0x10);
        if (ReadablePointer(records, 0x10 + 4 * 0x6A0)) {
            for (u32 slot = 0; slot < 4; ++slot) {
                if (ReadValue<s32>(records, 0x10 + slot * 0x6A0) != -1 &&
                    ReadValue<u8>(slots, 0xA9 + slot * 2) != 0) {
                    WriteValue<u8>(slots, 0xA9 + slot * 2, 0);
                    leaving_cleared |= 1u << slot;
                }
            }
        }
    }
    if (const u64 menu = ReadValue<u64>(image_base + 0x05562878, 0);
        ReadablePointer(menu, 0x2E0) && HasMemoryAccess(menu + 0x2DC, 4, MemoryProt::CpuWrite) &&
        ReadValue<s32>(menu, 0x2DC) != 0) {
        WriteValue<s32>(menu, 0x2DC, 0);
        brake = true;
    }
    if (const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
        ReadablePointer(slots, 0x78) && HasMemoryAccess(slots + 0x70, 1, MemoryProt::CpuWrite) &&
        ReadValue<u8>(slots, 0x70) != 0) {
        WriteValue<u8>(slots, 0x70, 0);
        pending = true;
    }
    // SetEventFlag panics when SprjEventState [0x0553B0D8] or SprjEventFlagMan [0x0553B100] is null.
    if (ReadValue<u64>(image_base + 0x0553B0D8, 0) != 0 &&
        ReadValue<u64>(image_base + 0x0553B100, 0) != 0) {
        static bool flag_cleared_this_window = false;
        static s64 flag_window = 0;
        if (flag_window != bbsc_death_lock_until_ms) {
            flag_window = bbsc_death_lock_until_ms;
            flag_cleared_this_window = false;
        }
        if (!flag_cleared_this_window) {
            using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
            BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, 0xFCF, 0);
            flag_cleared_this_window = true;
            flag = true;
        }
    }
    if (brake || pending || flag || leaving_cleared != 0) {
        LOG_INFO(Debug,
                 "BBSC death locks cleared: menu brake={} death pending={} event flag 4047={} "
                 "slot leaving mask={:#x} (session state {})",
                 brake, pending, flag, leaving_cleared, session_state);
    }
}

// Diagnostic: which game state is still different after a respawn in place. A baseline of the
// structures below is taken every 3 s while the session is active and the local player is alive
// with no death flow running; BbscStateDiffDelayMs after a respawn / partner revive every byte that
// differs from the baseline is logged (positions and timers change too; look for flags).

struct BbscStateRegion {
    std::string_view name;
    u64 base;
    size_t size;
};

std::vector<BbscStateRegion> CollectBbscStateRegions() {
    std::vector<BbscStateRegion> regions;
    const auto add = [&regions](std::string_view name, u64 base, size_t size) {
        if (ReadablePointer(base, size)) {
            regions.push_back({name, base, size});
        }
    };
    const u64 player = GetLocalPlayerCharacter();
    add("slots[0x0553B130]", ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0), 0x100);
    add("menu[0x05562878]", ReadValue<u64>(image_base + 0x05562878, 0), 0x340);
    if (const u64 global = GetGlobalState(); global != 0) {
        add("global+0x1400", global + 0x1400, 0x400);
    }
    add("worldchr[0x0553E878]", ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0),
        0x100);
    if (player != 0) {
        add("chr", player, 0x600);
        const u64 control = ReadValue<u64>(player, 0x58);
        add("chr.control[+0x58]", control, 0x300);
        if (ReadablePointer(control, 0x40)) {
            add("chr.control.flags[+0x38]", ReadValue<u64>(control, 0x38), 0x100);
        }
    }
    return regions;
}

void TickBbscStateDiff() {
    static s64 last_baseline_ms = 0;
    static std::vector<std::pair<std::string, std::vector<u8>>> baseline;
    const s64 now = SteadyNowMs();
    const s32 session_state = GetSessionState();
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayerCharacter();
    const bool session = session_state == 3 || session_state == 6;
    const bool loading = !ReadablePointer(global, 0x10) || ReadValue<u8>(global, 0x08) != 0;
    const bool alive = player != 0 && GetCharacterHp(player) > 0;

    if (bbsc_state_diff_at_ms != 0 && now >= bbsc_state_diff_at_ms) {
        bbsc_state_diff_at_ms = 0;
        if (baseline.empty() || !session || loading || !alive) {
            LOG_INFO(Debug, "BBSC state diff skipped (baseline {}, session {}, loading {}, alive {})",
                     !baseline.empty(), session_state, loading, alive);
            return;
        }
        LOG_INFO(Debug, "BBSC state diff after respawn (session state {}, baseline {} ms old):",
                 session_state, now - last_baseline_ms);
        for (const auto& region : CollectBbscStateRegions()) {
            const auto it = std::ranges::find_if(
                baseline, [&](const auto& b) { return b.first == region.name; });
            if (it == baseline.end() || it->second.size() != region.size) {
                continue;
            }
            std::string line;
            size_t changes = 0;
            for (size_t i = 0; i < region.size; ++i) {
                const u8 now_byte = ReadValue<u8>(region.base, i);
                if (now_byte != it->second[i]) {
                    if (++changes <= 96) {
                        line += fmt::format(" +{:#x}:{:02x}->{:02x}", i, it->second[i], now_byte);
                    }
                }
            }
            if (changes != 0) {
                LOG_INFO(Debug, "BBSC state diff {} ({} bytes):{}", region.name, changes, line);
            }
        }
        return;
    }

    if (!session || loading || !alive || bbsc_death_lock_until_ms != 0 || bbsc_spectate_active ||
        now - last_baseline_ms < 3'000) {
        return;
    }
    // Only a clean baseline: every character in WorldChrMan (partners included) alive, no death flag.
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const s32 count = ReadablePointer(world_chr, 0x40) ? ReadValue<s32>(world_chr, 0x30) : 0;
    const u64 entries = count > 0 ? ReadValue<u64>(world_chr, 0x38) : 0;
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (chr == 0) {
            continue;
        }
        const u64 chr_control = ReadablePointer(chr, 0x60) ? ReadValue<u64>(chr, 0x58) : 0;
        if (GetCharacterHp(chr) <= 0 || !ReadablePointer(chr_control, 0x118) ||
            (ReadValue<u16>(chr_control, 0x114) & 1) != 0) {
            return;
        }
    }
    last_baseline_ms = now;
    baseline.clear();
    for (const auto& region : CollectBbscStateRegions()) {
        std::vector<u8> bytes(region.size);
        for (size_t i = 0; i < region.size; ++i) {
            bytes[i] = ReadValue<u8>(region.base, i);
        }
        baseline.emplace_back(std::string(region.name), std::move(bytes));
    }
}

// Partner side: the death handler 0x018C49F0 also runs on each machine's COPY of a dying remote
// player (HP 0, death flags, fade-out). Normal Bloodborne never revives a remote player, so when the
// owner respawns in place the copy stays dead-looking ("only the name" is visible). When a remote
// player's synced HP is back above 0 for BbscRemoteReviveSettleMs while its death flag
// ([chr+0x58]+0x114 bit0) is still set, apply the non-local steps of the event revive 0x013CBAD0:
// physics reset 0x018BCF30(chr, 1) (reads the same transform block as ReadCharacterTransform),
// Idle_wild, opacity chr+0x208, death flags [[chr+0x58]+0x38]+0x48 / +0x114, drawing on.
constexpr s64 BbscRemoteReviveSettleMs = 1'000;

void TickBbscRemoteRevive() {
    struct Pending {
        u64 chr;
        s64 since_ms;
    };
    static std::array<Pending, 8> pending{};
    static s64 last_check_ms = 0;
    const s64 now = SteadyNowMs();
    if (now - last_check_ms < 250) {
        return;
    }
    last_check_ms = now;

    const s32 session_state = GetSessionState();
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 global = GetGlobalState();
    if ((session_state != 3 && session_state != 6) || !ReadablePointer(world_chr, 0x70) ||
        !ReadablePointer(global, 0x10) || ReadValue<u8>(global, 0x08) != 0) {
        pending = {};
        return;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return;
    }
    for (s32 i = 0; i < count; ++i) {
        const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (chr == 0 || chr == local || !ReadablePointer(chr, 0x410)) {
            continue;
        }
        const u64 control = ReadValue<u64>(chr, 0x58);
        const u64 modules = ReadValue<u64>(chr, 0x3B0);
        if (!ReadablePointer(control, 0x118) || !ReadablePointer(modules, 0x60)) {
            continue;
        }
        const bool dead_flag = (ReadValue<u16>(control, 0x114) & 1) != 0;
        auto it = std::ranges::find_if(pending, [chr](const Pending& p) { return p.chr == chr; });
        if (!dead_flag || GetCharacterHp(chr) <= 0) {
            if (it != pending.end()) {
                *it = {};
            }
            continue;
        }
        if (it == pending.end()) {
            it = std::ranges::find_if(pending, [](const Pending& p) { return p.chr == 0; });
            if (it != pending.end()) {
                *it = {chr, now};
            }
            continue;
        }
        if (now - it->since_ms < BbscRemoteReviveSettleMs) {
            continue;
        }
        if (BbscPeerLifeCounts().second != 0) {
            continue; // the partner says it is dead: its copy is right, keep it down
        }
        *it = {};

        using PhysicsReset = void PS4_SYSV_ABI (*)(u64 chr, u8 flag);
        BbGuestCall(reinterpret_cast<PhysicsReset>(image_base + 0x018BCF30), chr, 1);
        const u64 behaviour = ReadValue<u64>(modules, 0x30);
        if (ReadablePointer(behaviour, 0x388) && ReadValue<u8>(behaviour, 0x384) != 0) {
            using PlayAnimation = void PS4_SYSV_ABI (*)(u64 behaviour, u64 name_utf16);
            BbGuestCall(reinterpret_cast<PlayAnimation>(image_base + 0x01A19F60), behaviour,
                                                                     image_base + 0x04992C40);
        }
        WriteValue<u64>(chr, 0x208, 0x3F8000003F800000ull);
        const u64 control_flags = ReadValue<u64>(control, 0x38);
        if (ReadablePointer(control_flags, 0x50)) {
            WriteValue<u8>(control_flags, 0x48,
                           static_cast<u8>(ReadValue<u8>(control_flags, 0x48) & 0xFC));
        }
        const u64 draw = ReadValue<u64>(modules, 0x58);
        const u64 draw_state = ReadablePointer(draw, 0x30) ? ReadValue<u64>(draw, 0x28) : 0;
        if (ReadablePointer(draw_state, 0x10)) {
            WriteValue<u8>(draw_state, 0x0C, 1);
        }
        WriteValue<u16>(control, 0x114, static_cast<u16>(ReadValue<u16>(control, 0x114) & 0xFFFE));
        LOG_INFO(Debug, "BBSC partner revive: remote player copy revived (hp {}, session state {})",
                 GetCharacterHp(chr), session_state);
        ArmBbscDeathLockClear();
    }
}

// ---- The death penalty in a kept death ---------------------------------------------------------
// A kept death skips the reload, and the reload is what normally makes a death cost anything. For
// a summoned guest there was never a cost to skip: PartyGhostDeath_2 (0x013824C0) only drops a
// bloodstain when the local character type is 2 or 0xC - the two invader types - so a co-op
// phantom (type 1) walks away with everything. In vanilla that is fair, because the phantom is
// thrown out of the session; here nobody is thrown anywhere. Reported 2026-09-22: "she didn't get
// you died prompt, but respawned after a while, but this doesn't take her echoes, so it's like she
// didn't die at all."
//
// DropBloodstain 0x013D2910(stain_root, chr, 1) is the entire penalty in one call (CONFIRMED
// statically from its decompile): it copies the map id and the death transform
// ([[[chr+0x58]+0x3B0]+0x68]+0x1E0 position, +0x1D0 rotation) into the record at [0x0553B130]+0x30,
// moves the player's blood echoes from [[0x0553B130]+8]+0x94 into that record at +0x38, zeroes
// them, sets the "a stain exists" byte at [0x0553B130]+0x28, and calls the stain spawner
// 0x01868A50(holder, amount >= 0). That spawner builds the object in the world that is already
// loaded, so the stain appears where the player died without any reload - which is exactly what a
// kept death needs.
//
// SoloPlayDeath_2 makes the same call for the host when its own guards allow it, so BBSC must not
// make it a second time. Instead the echo count is snapshotted when the death step starts and
// compared just before the respawn: the call only happens when the count did not move, which
// leaves the host's own drop alone and covers the guest, and covers the host too if its guard
// (global+0x1649) happened to be set.
constexpr u64 SprjEventManPointerOffset = 0x0553B108;
constexpr u64 DropBloodstainOffset = 0x013D2910;
constexpr u64 DeathTextEventOffset = 0x0132EAD0;
constexpr size_t PlayerEchoesOffset = 0x94; // [[0x0553B130]+8]+0x94
constexpr u32 DeathTextEventId = 0xFFA;

s32 bbsc_echoes_at_death = -1;
bool bbsc_kept_death_this_load = false; // see BbscRepeatDeathMs

// Code review 2026-09-23 A1 (CONFIRMED from the decompile of 0x013824C0): PartyGhostDeath_2 starts
// by deleting the objects at global+0x1698 and +0x16B8 and zeroing +0x1690 / +0x16B0. Those are
// the own-world save snapshot 0x0131E5C0 builds when a guest is summoned, which the save writer
// 0x0156AA50 uses for as long as the guest is in someone else's world (global+0x1592 == 0).
// Vanilla frees it because the guest is about to reload into their own world. A kept death does
// not go home, so freeing it left the guest's own-world state without a snapshot for the rest of
// the session - saving had nothing to write and the eventual trip home had nothing to restore
// (the "guest spawns at Iosefka's Clinic" reports).
// So when a death is kept, the four values are taken out of the step's reach before its body runs
// and put back on the next tick.
struct BbscSnapshotStash {
    bool held{};
    u32 v1690{};
    u64 v1698{};
    u32 v16b0{};
    u64 v16b8{};
};
BbscSnapshotStash bbsc_snapshot_stash{};

void StashBbscOwnWorldSnapshot() {
    const u64 global = GetGlobalState();
    if (bbsc_snapshot_stash.held || !ReadablePointer(global, 0x16C0) ||
        ReadValue<u8>(global, 0x1592) != 0 ||
        !HasMemoryAccess(global + 0x1690, 0x30, MemoryProt::CpuWrite)) {
        return; // in our own world there is no snapshot to protect
    }
    bbsc_snapshot_stash = {true, ReadValue<u32>(global, 0x1690), ReadValue<u64>(global, 0x1698),
                           ReadValue<u32>(global, 0x16B0), ReadValue<u64>(global, 0x16B8)};
    // The step only frees non-null pointers; with zeros it frees nothing.
    WriteValue<u64>(global, 0x1698, 0);
    WriteValue<u64>(global, 0x16B8, 0);
    LOG_INFO(Debug,
             "BBSC death: own-world snapshot kept out of the death step (1698={:#x}, 16b8={:#x})",
             bbsc_snapshot_stash.v1698, bbsc_snapshot_stash.v16b8);
}

void RestoreBbscOwnWorldSnapshot() {
    if (!bbsc_snapshot_stash.held) {
        return;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x16C0) ||
        !HasMemoryAccess(global + 0x1690, 0x30, MemoryProt::CpuWrite)) {
        return; // try again next tick
    }
    const BbscSnapshotStash stash = bbsc_snapshot_stash;
    bbsc_snapshot_stash = {};
    // Never overwrite something the game created in the meantime.
    if (ReadValue<u64>(global, 0x1698) == 0 && ReadValue<u64>(global, 0x16B8) == 0) {
        WriteValue<u32>(global, 0x1690, stash.v1690);
        WriteValue<u64>(global, 0x1698, stash.v1698);
        WriteValue<u32>(global, 0x16B0, stash.v16b0);
        WriteValue<u64>(global, 0x16B8, stash.v16b8);
        LOG_INFO(Debug, "BBSC death: own-world snapshot restored after the death step");
    } else {
        LOG_WARNING(Debug,
                    "BBSC death: snapshot slots were refilled meanwhile; stash not written back");
    }
}

bool BbscDeathPenaltyEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_DEATH_PENALTY");
    return enabled;
}

// [0x0553B130]+8: the local player's slot data. +0xA4 is the player type GetLocalPlayerType reads,
// +0x94 the blood echo total, +0xF8 the pending reward 0x01335400 fills in.
u64 GetPlayerSlotData() {
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    if (!ReadablePointer(slots, 0x10)) {
        return 0;
    }
    const u64 local = ReadValue<u64>(slots, 0x08);
    return ReadablePointer(local, PlayerEchoesOffset + 4) ? local : 0;
}

s32 GetBloodEchoes() {
    const u64 local = GetPlayerSlotData();
    return local == 0 ? -1 : ReadValue<s32>(local, PlayerEchoesOffset);
}

// SoloPlayDeath (0x01381B60) announces the death with
//   0x0132EAD0(event, 0xFFA, chr->vtable[0x558](chr), chr->vtable[0x598](chr))
// which is what puts "YOU DIED" on the screen and tells the other session members. The guest's
// PartyGhostDeath (0x01381FD0) never makes that call, because vanilla sends a dead phantom home
// rather than telling them anything. In a kept session there is no home to go to, so BBSC fires
// the same event with the same arguments from the guest's own death state.
//
// Repeat deaths (2026-09-25, user: "YOU DIED" never showed after the first death): the game's death
// script is armed once per map load, and clearing flag 4000 does not re-arm it - the 2nd death ran
// no death step at all (log: "repeat death in the same load"). BBSC raises the text itself there,
// with the Lua event object the game's other text effects use outside a death script:
// [SprjLuaEventMan 0x0553B0C8]+8 (0x015BAD90 raises "TextEffectEnd_NPCPK_Success" on it, toggling
// the same +0x8A byte SoloPlayDeath does). 0x0132EAD0 also passes the event to the other members
// (+0x8B set), as the game's own call does.
u64 BbscLuaEventObject() {
    const u64 man = ReadValue<u64>(image_base + SprjLuaEventManPointerOffset, 0);
    const u64 event = ReadablePointer(man, 0x10) ? ReadValue<u64>(man, 0x08) : 0;
    return ReadablePointer(event, 0x90) ? event : 0;
}

bool RaiseBbscDeathText(u64 event_object, std::string_view who) {
    if (!BbscDeathPenaltyEnabled() || image_base == 0) {
        return false;
    }
    const u64 player = GetLocalPlayerCharacter();
    if (event_object < 0x10000 || player == 0 || !ReadablePointer(event_object, 0x90) ||
        !ReadablePointer(player, 0x08)) {
        return false;
    }
    const u64 vtable = ReadValue<u64>(player, 0);
    if (!ReadablePointer(vtable, 0x5A0)) {
        return false;
    }
    const u64 cause = ReadValue<u64>(vtable, 0x558);
    const u64 detail = ReadValue<u64>(vtable, 0x598);
    if (!IsEbootCodeAddress(cause) || !IsEbootCodeAddress(detail)) {
        LOG_WARNING(Debug, "BBSC death text: character vtable unexpected, skipped");
        return false;
    }
    using Query = u32 PS4_SYSV_ABI (*)(u64 chr);
    using TextEvent = bool PS4_SYSV_ABI (*)(u64 event, u32 id, u32 first, u32 second);
    const u32 first = BbGuestCall(reinterpret_cast<Query>(cause), player);
    const u32 second = BbGuestCall(reinterpret_cast<Query>(detail), player);
    const bool accepted = BbGuestCall(reinterpret_cast<TextEvent>(image_base + DeathTextEventOffset), 
        event_object, DeathTextEventId, first, second);
    LOG_INFO(Debug, "BBSC death text: \"YOU DIED\" raised for the {} (args {}, {}, accepted {})",
             who, first, second, accepted);
    return true;
}

void ShowBbscGuestDeathText(u64 event_object) {
    const u64 lua_event = BbscLuaEventObject();
    LOG_INFO(Debug, "BBSC death text: PartyGhostDeath event {:#x}, Lua event manager's {:#x} ({})",
             event_object, lua_event, event_object == lua_event ? "same" : "different");
    RaiseBbscDeathText(event_object, "guest");
}

// Drop the stain unless the game already did it for this death. Returns true when BBSC made the
// call itself.
bool BbscApplyDeathPenalty(std::string_view why) {
    if (!BbscDeathPenaltyEnabled() || image_base == 0) {
        return false;
    }
    const s32 before = bbsc_echoes_at_death;
    bbsc_echoes_at_death = -1;
    const s32 now = GetBloodEchoes();
    if (before < 0 || now < 0) {
        return false; // no snapshot, or the slot data went away with the world
    }
    if (now == 0 || before == 0) {
        // The game's own drop ends with the count at zero, so this is both "it already happened"
        // and "there is nothing to take" - either way BBSC must not call it again. Nothing held at
        // the death: whatever came in since (a boss reward while spectating) stays with the player.
        LOG_INFO(Debug, "BBSC death penalty ({}): nothing to drop ({} echoes at the death step, {} "
                        "now)",
                 why, before, now);
        return false;
    }
    const u64 player = GetLocalPlayerCharacter();
    const u64 event_man = ReadValue<u64>(image_base + SprjEventManPointerOffset, 0);
    if (player == 0 || !ReadablePointer(event_man, 0x88)) {
        LOG_WARNING(Debug, "BBSC death penalty ({}): SprjEventMan or the player is gone", why);
        return false;
    }
    const u64 stain_root = ReadValue<u64>(event_man, 0x80);
    if (!ReadablePointer(stain_root, 0x10)) {
        LOG_WARNING(Debug, "BBSC death penalty ({}): no stain root at SprjEventMan+0x80", why);
        return false;
    }
    // 0x013D2910 does nothing at all when global+0x1770 is zero, so log it: a run where the
    // echoes survive with "gate 0" names the reason without another guess.
    const u64 global = GetGlobalState();
    const u8 gate = ReadValue<u8>(global, 0x1770);
    // 0x013D2910 places a guest's stain (own-world flag global+0x1592 == 0) at the forced summon
    // placement global+0x1500 / +0x1510 in map +0x14F0 - the vanilla "back home" spot, which in a
    // kept session is wherever the last join or follow warp put the guest, i.e. by the lamp
    // (reported 2026-09-24). The own-world branch uses the character's own position and map, so
    // the drop runs with the flag set and the flag goes straight back.
    const bool guest = ReadablePointer(global, 0x1593) && ReadValue<u8>(global, 0x1592) == 0;
    if (guest) {
        WriteValue<u8>(global, 0x1592, 1);
    }
    using DropBloodstain = void PS4_SYSV_ABI (*)(u64 stain_root, u64 chr, u8 drop);
    BbGuestCall(reinterpret_cast<DropBloodstain>(image_base + DropBloodstainOffset), stain_root, player, 1);
    if (guest) {
        WriteValue<u8>(global, 0x1592, 0);
    }

    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    const u64 record = ReadablePointer(slots, 0x38) ? ReadValue<u64>(slots, 0x30) : 0;
    s32 in_stain = ReadablePointer(record, 0x3C) ? ReadValue<s32>(record, 0x38) : -1;
    // Test 2026-10-07 22:10:56-22:11:38: the guest died in the Cleric Beast fight with 192 echoes,
    // spectated, was paid the boss's 4000 at 22:11:31, and this drop (deferred to the respawn) put
    // all 4192 into the stain in the arena. The drop takes whatever the player holds now; only what
    // was held at the death belongs in the stain - the rest was earned while dead and is given back.
    const s32 earned_while_dead = now - before;
    if (earned_while_dead > 0 && in_stain >= earned_while_dead) {
        const u64 local = GetPlayerSlotData();
        if (local != 0 && GetBloodEchoes() >= 0) {
            WriteValue<s32>(local, PlayerEchoesOffset, GetBloodEchoes() + earned_while_dead);
            WriteValue<s32>(record, 0x38, in_stain - earned_while_dead);
            in_stain -= earned_while_dead;
            LOG_INFO(Debug, "BBSC death penalty ({}): {} echoes earned while dead (a boss died "
                            "meanwhile) given back - only the {} held at the death are in the stain",
                     why, earned_while_dead, before);
        }
    }
    LOG_INFO(Debug,
             "BBSC death penalty ({}): dropped {} echoes (now {}, stain holds {}, type {}, gate {})",
             why, before, GetBloodEchoes(), in_stain, GetLocalPlayerType(), gate);
    return true;
}

// Late death steps (2026-10-13, host log 22:14:05-15): the watchdog revived the host in
// place, then the game's own death script still ran 7 s later (SoloPlayDeath, then
// SoloPlayDeath_2) for that same death - a second curtain, a second respawn, a second stain, and a
// grey void on the host's screen. A step that starts while the player is alive in a session within
// 15 s of a BBSC revive belongs to the death BBSC already handled: skip it and clear what its
// entry may already have set. A real new death has hp 0 and runs normally.
u8 bbsc_late_death_skip[3] = {};  // SoloPlayDeath, SoloPlayDeath_1, PartyGhostDeath
u8 bbsc_late_final_skip[2] = {};  // SoloPlayDeath_2, PartyGhostDeath_2
constexpr s64 BbscLateDeathStepMs = 15'000;

bool BbscLateDeathStep(std::string_view name) {
    if (bbsc_inplace_respawn_ms == 0 || SteadyNowMs() - bbsc_inplace_respawn_ms > BbscLateDeathStepMs) {
        return false;
    }
    const s32 session_state = GetSessionState();
    if (session_state != 3 && session_state != 6) {
        return false;
    }
    const u64 player = GetLocalPlayerCharacter();
    if (player == 0) {
        return false;
    }
    const s32 hp = GetCharacterHp(player);
    if (hp <= 0) {
        return false;
    }
    if (const u64 global = GetGlobalState(); ReadablePointer(global, 0x1528)) {
        WriteValue<u8>(global, 0x1522, 0);
        WriteValue<u8>(global, 0x1523, 0);
        WriteValue<u8>(global, 0x1520, 0);
        WriteValue<u8>(global, 0x1524, 0);
    }
    if (const u64 event_state = ReadValue<u64>(image_base + 0x0553B0D8, 0);
        ReadablePointer(event_state, 4) && HasMemoryAccess(event_state, 4, MemoryProt::CpuWrite)) {
        WriteValue<s32>(event_state, 0, 0);
    }
    ClearBbscStaleDeathRequest(name);
    ArmBbscDeathLockClear();
    LOG_INFO(Debug, "BBSC death: late {} {} ms after a BBSC respawn (player alive, hp {}) - skipped",
             name, SteadyNowMs() - bbsc_inplace_respawn_ms, hp);
    return true;
}

// Runs just before the death step's body. The body itself is harmless now that its reload call is
// a no-op, so nothing is skipped here: this only records that the local player died so the tick
// below can revive them (or wait out a boss fight first).
void PS4_SYSV_ABI BbscSpectateDeathHook(u64 tag, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr || bbsc_spectate_releasing || !BbscSpectateEnabled()) {
        return; // releasing: the reload the step asked for is being requested for real
    }
    const std::string_view name = tag == 0 ? "SoloPlayDeath_2" : "PartyGhostDeath_2";
    if (tag < 2 && BbscLateDeathStep(name)) {
        bbsc_late_final_skip[tag] = 1;
        return;
    }
    const s32 session_state = GetSessionState();
    if (session_state != 3 && session_state != 6) {
        // Solo play. The step's reload call is retargeted to a no-op whether or not a session
        // exists, so without asking for it here the player would stay dead with camera control
        // only (reported 2026-09-20: "in solo mode, after you die you can't respawn at all").
        bbsc_spectate_active = false;
        bbsc_request_death_reload = true;
        return;
    }
    // Invasions: vanilla rules (test 2026-10-05: the dead invader respawned in the host's world
    // like a co-op guest). A dead invader goes home; a host who dies with no co-op partner alive
    // while an invader is in the world ends the session the vanilla way, which sends the invader
    // home too. The session patches are lifted so the game's own teardown runs in full.
    const bool local_invader = session_state == 6 && IsInvaderType(GetLocalCharacterType());
    const bool host_beaten = session_state == 3 && !LivingPartnerExists() && RemoteInvaderCount() > 0;
    if (local_invader || host_beaten) {
        LOG_INFO(Debug, "BBSC death: {} - {} -> vanilla death, the invader goes home", name,
                 local_invader ? "the invader died" : "the host died with an invader in the world");
        SuspendBbscPersistenceOnce(local_invader ? "invader died" : "host died during an invasion");
        bbsc_spectate_active = false;
        bbsc_request_death_reload = true;
        return;
    }
    const bool guest_waits_for_host = session_state == 6 && !LivingPartnerExists();
    if (!guest_waits_for_host && !LivingPartnerExists()) {
        // No partner: ask for the reload the retargeted call site no longer performs, so a solo
        // death still ends the way the game intends.
        LOG_INFO(Debug, "BBSC spectate: {} runs normally (no living partner)", name);
        bbsc_spectate_active = false;
        bbsc_request_death_reload = true;
        return;
    }
    bbsc_echoes_at_death = GetBloodEchoes();
    bbsc_kept_death_this_load = true;
    StashBbscOwnWorldSnapshot();
    bbsc_spectate_name = name;
    bbsc_spectate_since_ms = SteadyNowMs();
    bbsc_spectate_chr = GetLocalPlayerCharacter();
    bbsc_spectate_alone_since_ms = 0;
    bbsc_spectate_logged_wait = false;
    bbsc_spectate_saw_wipe = guest_waits_for_host;
    bbsc_spectate_wipe_warped = false;
    bbsc_spectate_host_back_ms = 0;
    bbsc_spectate_revivals_at_death = bbsc_peer_revivals.load();
    bbsc_boss_cleared_since_ms = 0;
    bbsc_death_mode = BbscBossFightAtDeath() || guest_waits_for_host ? BbscDeathMode::Spectate
                                                                      : BbscDeathMode::RespawnNow;
    bbsc_spectate_active = true;
    if (guest_waits_for_host) {
        LOG_INFO(Debug, "BBSC death: {} with nobody alive (guest) -> wait for the host's reload, "
                        "no reload of our own",
                 name);
        return;
    }
    LOG_INFO(Debug, "BBSC death: {} without reload (session state {}, partner alive) -> {}", name,
             session_state,
             bbsc_death_mode == BbscDeathMode::Spectate
                 ? "boss fight: spectate until the boss dies or nobody is alive"
                 : "no boss fight: respawn at the last lamp without a reload");
}

// Game-thread tick: release the held death once nobody is left alive, the session is over, or the
// safety limit is reached.
void TickBbscSpectate() {
    NoteBbscBossBar();
    RestoreBbscOwnWorldSnapshot();
    if (bbsc_request_death_reload) {
        bbsc_request_death_reload = false;
        RequestBbscDeathReload("no living partner at the death step");
        return;
    }
    if (!bbsc_spectate_active || bbsc_spectate_releasing) {
        return;
    }
    const s64 now = SteadyNowMs();
    const s32 session_state = GetSessionState();
    const bool session_alive = session_state == 3 || session_state == 6;
    std::string_view reason;
    if (session_state == 6 && !bbsc_spectate_saw_wipe &&
        bbsc_boss_killed_ms < bbsc_spectate_since_ms &&
        bbsc_peer_revivals.load() != bbsc_spectate_revivals_at_death) {
        bbsc_spectate_saw_wipe = true;
        LOG_INFO(Debug, "BBSC spectate: a partner said dead and then alive while we were down - "
                        "it reloaded after a wipe, the guest follows");
    }
    if (session_alive && LivingPartnerExists()) {
        std::string_view respawn_why;
        const u64 chr = GetLocalPlayerCharacter();
        if (bbsc_death_mode == BbscDeathMode::RespawnNow) {
            respawn_why = "no boss fight";
        } else if (chr != 0 && bbsc_spectate_chr != 0 && chr != bbsc_spectate_chr) {
            respawn_why = "a load ended the boss fight";
        } else if (session_state == 6 && bbsc_spectate_saw_wipe && !bbsc_spectate_wipe_warped) {
            if (bbsc_spectate_host_back_ms == 0) {
                bbsc_spectate_host_back_ms = now;
                LOG_INFO(Debug, "BBSC spectate: the host is alive again after the wipe (it "
                                "reloaded) - the guest follows with its own reload");
                return;
            }
            if (now - bbsc_spectate_host_back_ms < BbscWipeHostSettleMs) {
                return;
            }
            bbsc_spectate_wipe_warped = true;
            const u64 real_host = FindRemoteHostCharacter();
            const u64 host = real_host != 0 && GetCharacterHp(real_host) > 0
                                 ? real_host
                                 : FindLivingPartnerCharacter();
            const auto host_transform = ReadCharacterTransform(host);
            if (host_transform.has_value()) {
                SummonPlacementDescriptor to{};
                to.packed_region = GetCurrentPackedMap();
                to.x = host_transform->position[0] + 1.0f;
                to.y = host_transform->position[1];
                to.z = host_transform->position[2];
                to.heading = host_transform->rotation[1];
                if (RequestBbscSessionWarp(GetGlobalState(), to, "wipe")) {
                    return; // the load builds a new character: "a load ended the boss fight"
                }
            }
            LOG_WARNING(Debug, "BBSC spectate: wipe reload could not be requested, respawning "
                               "in place instead");
            respawn_why = "the host reloaded after the wipe";
        } else if (bbsc_boss_killed_ms >= bbsc_spectate_since_ms) {
            if (bbsc_boss_cleared_since_ms == 0) {
                bbsc_boss_cleared_since_ms = now;
            } else if (now - bbsc_boss_cleared_since_ms >= BbscBossClearedSettleMs) {
                respawn_why = "boss killed";
            }
        }
        if (!respawn_why.empty()) {
            BbscApplyDeathPenalty(respawn_why);
            if (StartBbscDeathCurtain(respawn_why) || BbscRespawnInPlace(respawn_why)) {
                bbsc_spectate_active = false;
                return;
            }
            reason = "respawn in place failed";
        }
    }
    if (!reason.empty()) {
        // fall through to the release below: the normal reload is better than staying dead
    } else if (session_state != 3 && session_state != 6) {
        reason = "session ended";
    } else if (now - bbsc_spectate_since_ms > BbscSpectateSafetyMs) {
        reason = "safety limit";
    } else if (LivingPartnerExists()) {
        bbsc_spectate_alone_since_ms = 0;
        bbsc_spectate_logged_wait = false;
        return;
    } else {
        if (bbsc_spectate_alone_since_ms == 0) {
            bbsc_spectate_alone_since_ms = now;
            return;
        }
        if (now - bbsc_spectate_alone_since_ms < BbscSpectateReleaseDebounceMs) {
            return;
        }
        if (session_state == 6 && now - bbsc_spectate_alone_since_ms < BbscGuestWaitHostMs) {
            bbsc_spectate_saw_wipe = true;
            if (!bbsc_spectate_logged_wait) {
                bbsc_spectate_logged_wait = true;
                LOG_INFO(Debug, "BBSC spectate: nobody alive - guest waits for the host's reload "
                                "(up to {} s) instead of reloading itself",
                         BbscGuestWaitHostMs / 1'000);
            }
            return;
        }
        reason = session_state == 6 ? "no living partner and no host reload came"
                                    : "no living partner";
    }

    LOG_INFO(Debug, "BBSC spectate: releasing {} after {} ms ({})", bbsc_spectate_name,
             now - bbsc_spectate_since_ms, reason);
    bbsc_spectate_active = false;
    bbsc_spectate_releasing = true;
    RequestBbscDeathReload(reason);
    bbsc_spectate_releasing = false;
}

// Boss kill keeps the co-op (CONFIRMED statically, 2026-09-16). The boss clear Lua chain is
// BlockClear2 (0x013849A0: rewards, boss bar reset, marks every slot leaving via 0x014EB860)
// -> BlockClear2_1 (0x01385470) -> for a summoned guest (chr type 1) BlockClear2_2 (0x01385760:
// "final battle has ended" message, MissionSuccessed reward) which schedules
//   BlockClear2_3Leave (0x013858A0): CSMultiPlayMan::Stop + remote cleanup (leaves the session)
//   BlockClear2_3      (0x01385930): guest: death-pending, forced warp, home respawn point and
//                                    StageTransition (loads the guest's own world); host: only
//                                    clears event flags 4047 / 4000.
// Run 2026-09-16 without this: the guest left the room but stayed in the host's world without a
// character (camera only), the host saw the guest vanish. So for a live session the guest skips
// _3Leave and _3; the rewards in BlockClear2/_2 still run. BlockClear2_1 (host and guest) arms the
// death-lock clearing so the "leaving" slot marks set by BlockClear2 are removed again.
constexpr u64 BlockClear2FirstOffset = 0x01385470; // BlockClear2_1
constexpr u64 BlockClear2LeaveOffset = 0x013858A0; // BlockClear2_3Leave
constexpr u64 BlockClear2HomeOffset = 0x01385930;  // BlockClear2_3
u8 bbsc_boss_leave_skip = 0;
u8 bbsc_boss_home_skip = 0;
// After a guest skipped BlockClear2_3: clear its locks for this long, see TickBbscBossUnlock.
constexpr s64 BbscBossUnlockMs = 20'000;
s64 bbsc_boss_unlock_until_ms = 0;

bool BbscSessionAlive() {
    const s32 state = GetSessionState();
    return state == 3 || state == 6;
}

s32 LocalPlayerType() {
    const u64 player = GetLocalPlayerCharacter();
    return ReadablePointer(player, 0x80) ? ReadValue<s32>(player, 0x78) : -1;
}

void PS4_SYSV_ABI BbscBossClearHook(u64 tag, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr || !BbscSessionAlive()) {
        return;
    }
    if (tag == 0) {
        bbsc_boss_killed_ms = SteadyNowMs();
        ArmBbscDeathLockClear();
        LOG_INFO(Debug, "BBSC boss clear: BlockClear2_1 (player type {}), session kept{}",
                 LocalPlayerType(),
                 bbsc_spectate_active ? " - the spectating player respawns" : "");
    } else if (tag == 1) {
        bbsc_boss_leave_skip = 1;
        LOG_INFO(Debug, "BBSC boss clear: BlockClear2_3Leave skipped (guest stays in the session)");
    } else if (tag == 2 && LocalPlayerType() == 1) {
        bbsc_boss_home_skip = 1;
        bbsc_boss_unlock_until_ms = SteadyNowMs() + BbscBossUnlockMs;
        LOG_INFO(Debug, "BBSC boss clear: BlockClear2_3 skipped (guest stays in the host world)");
    }
}

// The boss-clear locks a skipped BlockClear2_3 leaves on the guest (CONFIRMED statically,
// 2026-09-27; report: after the boss the guest could not use a ladder or interact at all).
// BlockClear2 (0x013849A0) sets event flags 4047, 4000 and 4092 on every living member
// (0x01384E5E..0x01384E86) and MenuMan [0x05562878]+0x2F0 = 1. What undoes them is the end of the
// chain: BlockClear2_3 (0x01385930: +0x2F0 = 0, flags 4047/4000 = 0 - the host runs it) or the
// game's leave variant 0x013859F0 (+0x2F0 = 0 and, when 4092 is set, 4047/4000/4092 = 0, then it
// leaves the session). The guest skips both, so here are their unlocking parts without the leaving.
void TickBbscBossUnlock() {
    if (bbsc_boss_unlock_until_ms == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now > bbsc_boss_unlock_until_ms) {
        bbsc_boss_unlock_until_ms = 0;
        return;
    }
    static s64 last_ms = 0;
    if (now - last_ms < 1'000) {
        return; // again every second for the window: a late Lua step may set them once more
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayerCharacter();
    if (!BbscSessionAlive() || !ReadablePointer(global, 0x10) || ReadValue<u8>(global, 0x08) != 0 ||
        player == 0 || GetCharacterHp(player) <= 0) {
        return;
    }
    bool menu = false;
    if (const u64 menu_man = ReadValue<u64>(image_base + 0x05562878, 0);
        ReadablePointer(menu_man, 0x2F8) && HasMemoryAccess(menu_man + 0x2F0, 4, MemoryProt::CpuWrite) &&
        ReadValue<s32>(menu_man, 0x2F0) != 0) {
        WriteValue<s32>(menu_man, 0x2F0, 0);
        menu = true;
    }
    // SetEventFlag panics when SprjEventState [0x0553B0D8] or SprjEventFlagMan [0x0553B100] is null.
    if (ReadValue<u64>(image_base + 0x0553B0D8, 0) == 0 ||
        ReadValue<u64>(image_base + 0x0553B100, 0) == 0) {
        return;
    }
    using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
    const auto set = reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0);
    BbGuestCall(set, 0, 0xFCF, 0); // 4047
    BbGuestCall(set, 0, 0xFA0, 0); // 4000
    BbGuestCall(set, 0, 0xFFC, 0); // 4092
    static s64 logged_window = 0;
    if (logged_window != bbsc_boss_unlock_until_ms || menu) {
        logged_window = bbsc_boss_unlock_until_ms;
        LOG_INFO(Debug,
                 "BBSC boss clear: guest unlocked (event flags 4047/4000/4092 cleared, mission-clear "
                 "menu state {})",
                 menu ? "cleared" : "was already clear");
    }
}

void InstallBbscBossClearHooks() {
    static constexpr std::array<u8, 6> PushR15{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    static constexpr std::array<u8, 6> PushRbxRax{0x55, 0x48, 0x89, 0xE5, 0x53, 0x50};
    const bool ok =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + BlockClear2FirstOffset),
                             std::span<const u8>{PushR15}, 0, BbscBossClearHook) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + BlockClear2LeaveOffset),
                             std::span<const u8>{PushR15}, 1, BbscBossClearHook,
                             &bbsc_boss_leave_skip) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + BlockClear2HomeOffset),
                             std::span<const u8>{PushRbxRax}, 2, BbscBossClearHook,
                             &bbsc_boss_home_skip);
    if (!ok) {
        LOG_ERROR(Debug, "BBSC boss clear: failed to hook the BlockClear2 chain");
        return;
    }
    LOG_INFO(Debug, "BBSC boss clear keeps the co-op (BlockClear2_1 {:#x}, _3Leave {:#x}, _3 {:#x})",
             BlockClear2FirstOffset, BlockClear2LeaveOffset, BlockClear2HomeOffset);
}

// ---- Deliberate quit / partner gone -------------------------------------------------------------
// The persistence patches keep a session alive through anything the game would otherwise use to end
// it, which is right for loads and deaths but wrong for a deliberate quit: the player who quit to
// the title left the other one with a phantom partner that only closing the game cleared (reported
// 2026-09-20, "when going to menu cooperator stays ... they need to turn off the game").
//
// RegistReturnTitle (0x0138B990, Lua) is the first step of the quit flow - Check_ReturnTitle
// (0x0138BAD0) reads the menu's quit choice, OnReturnTitle (0x0138BAF0) then drives it. Suspending
// the patches there lets the vanilla teardown run for both sides; they go back on afterwards.
//
// The same suspension is the safety net for a partner who vanished without quitting (crash, power
// cut): after BbscPartnerGoneMs with a live session and no remote character at all, the game's own
// checks are allowed through so the session can end instead of hanging.
constexpr u64 RegistReturnTitleOffset = 0x0138B990;
constexpr s64 BbscQuitTeardownMs = 20'000;
// Long enough for a partner's slow first load (shader compilation) to never count as "gone".
constexpr s64 BbscPartnerGoneMs = 180'000;
// How long an invader waits after the host's death before leaving on its own (the host's own
// death ending the session normally comes first).
constexpr s64 BbscInvaderWinMs = 6'000;
s64 bbsc_persistence_restore_at_ms = 0;
s64 bbsc_partner_missing_since_ms = 0;

void SuspendBbscPersistence(std::string_view why) {
    bbsc_persistence_restore_at_ms = SteadyNowMs() + BbscQuitTeardownMs;
    Core::Bbsc::SetPersistentSessionPatches(false);
    LOG_INFO(Debug, "BBSC persistence suspended for {} ms ({})", BbscQuitTeardownMs, why);
}

void SuspendBbscPersistenceOnce(std::string_view why) {
    if (bbsc_persistence_restore_at_ms == 0) {
        SuspendBbscPersistence(why);
    }
}

// ---- Silencing Blank (2026-10-04) ----------------------------------------------------------------
// Report 2026-10-04: "the Silencing Blank is weird, might break the game"; guest log 2026-10-02:
// three uses (OnLeaveMagic at 20:07:11/17/24), nothing happened. global_event.lua, read with
// tools/gamedata/lua50.py (CONFIRMED):
//   OnLeaveMagic:    if IsCompleteEvent(4047) -> return; CustomLuaCallStart(4063, id);
//                    OnLeaveMenu_Yes(...)
//   OnLeaveMenu_Yes: if IsCompleteEvent(4047) -> return; SetLoadWait; tells the others
//                    (LuaCallStartPlus 4046, 1 = co-op phantom / 2-3 = invader); LeaveSession();
//                    if alive: SetEventFlag(4047, 1) and OnLeave_Limit in 5 s (the trip home).
// Event flag 4047 is the "death or leave already running" mark. Vanilla clears it with the reload
// after a death or a boss kill; BBSC keeps those without a reload and clears it only once per
// respawn, so whenever something set it again the item did nothing at all. And when it did run,
// BBSC's kept-session patches stopped LeaveSession halfway (the 2026-09-24 frozen guest).
// So when a living player uses it inside a session: 4047 is cleared right before the script reads
// it, and the session patches are suspended as for a quit to the title - the game's own leave runs
// in full (guest: home to its own world; host: the co-op ends and every guest goes home).
std::atomic<s64> bbsc_leave_event_ms{0}; // the game's last OnLeaveMagic / OnLeaveMenu_Yes

void BbscLeaveItemUsed(std::string_view event) {
    bbsc_leave_event_ms = SteadyNowMs();
    const s32 state = GetSessionState();
    if (image_base == 0 || (state != 3 && state != 6)) {
        return;
    }
    const u64 player = GetLocalPlayerCharacter();
    if (player == 0 || GetCharacterHp(player) <= 0 || bbsc_spectate_active) {
        LOG_INFO(Debug, "BBSC leave: {} while dead - left to the game", event);
        return;
    }
    const bool flag = BbscEventFlag(4047);
    if (flag && ReadValue<u64>(image_base + 0x0553B0D8, 0) != 0 &&
        ReadValue<u64>(image_base + 0x0553B100, 0) != 0) {
        using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
        BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, 4047, 0);
    }
    LOG_INFO(Debug, "BBSC leave: {} by the {} (event flag 4047 was {}) - the co-op ends", event,
             state == 3 ? "host" : "guest", flag ? "set, cleared" : "clear");
    if (bbsc_persistence_restore_at_ms == 0) {
        SuspendBbscPersistence(state == 3 ? "Silencing Blank: host ends the co-op"
                                          : "Silencing Blank: guest goes home");
    }
}


// Any player character in WorldChrMan that is not the local one, alive or dead.
int RemotePlayerCount() {
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return 0;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (count <= 0 || count > 64 || !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        return 0;
    }
    int remote = 0;
    for (s32 i = 0; i < count; ++i) {
        const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
        if (chr != 0 && chr != local && ReadablePointer(chr, 0x3B8)) {
            ++remote;
        }
    }
    return remote;
}

void PS4_SYSV_ABI BbscQuitToTitleHook(u64, const GuestRegisterSnapshot* registers) {
    // Who asked to return to the title matters. A player choosing "quit to title" from the menu
    // and the game bailing out on its own both land here, and only the first should tear the
    // session down. Sessions were ending by themselves (2026-09-21) with nothing failing in the
    // log beforehand, so record the caller and the session shape to tell the two apart.
    u64 caller = 0;
    if (registers != nullptr && HasMemoryAccess(registers->rsp, sizeof(u64), MemoryProt::CpuRead)) {
        std::memcpy(&caller, reinterpret_cast<const void*>(registers->rsp), sizeof(caller));
    }
    LOG_INFO(Debug,
             "BBSC quit to title requested: caller {:#x} session_state={} remote_players={} "
             "map={:#x}",
             caller > image_base ? caller - image_base : caller, GetSessionState(),
             RemotePlayerCount(), GetCurrentPackedMap());
    SuspendBbscPersistence("quit to title");
}

// ---- Stray forced warp on a host (2026-10-07) ---------------------------------------------------
// Report: kicking the guest with the Blank sent the host to the title, and loading back in left him
// falling under the map (grey screen). Host log 20:43:20: room timeout -> CSMultiPlayMan StopRequest
// -> LeaveRoom -> 0x01332BC0 (SummonedPlacement.SelectMap, point -1) -> StageTransition, then a 44 s
// load (menu files, the world list, the map again) and the host at (0, y, 0) falling (his placement
// headers). Every host room timeout on record did this when the host had died during the co-op
// (10-05 00:11, 10-06 21:31 and 21:59, 10-07 20:43) and none did without (10-06 21:16, 21:47).
// CONFIRMED in the 10-05 capture of that load: global+0x1520 = 1, the "forced warp" byte that makes
// 0x01332BC0 take the map from +0x14F0 (the summoned placement, -1 there) instead of the last lamp
// (+0x1528). The death / leave / boss handlers (PartyGhostDeath_2, HostDead, OnLeave_*, BlockClear2_*)
// raise it right before their reload; BBSC keeps those sessions without that reload, so nothing used
// it up. A player in its own world that is not a guest never needs it: BBSC takes it off again
// (tick, once nothing is loading for a while) and, as the last stop, at the map choice itself when
// +0x14F0 holds no map. The callers are logged so the next log names the handler.
bool BbscOwnWorldNonGuest(u64 global) {
    const s32 state = GetSessionState();
    return state != 4 && state != 6 && ReadablePointer(global, 0x1598) &&
           ReadValue<u8>(global, 0x1592) != 0;
}

void GuardBbscStrayForcedWarp(const GuestRegisterSnapshot& registers) {
    const u64 global = registers.rcx; // 0x01332C14: mov rcx, [0x05556678]
    if (image_base == 0 || global != GetGlobalState() || !ReadablePointer(global, 0x1598) ||
        ReadValue<u8>(global, 0x1520) == 0) {
        return;
    }
    // 0x01332BC0 keeps an rbp frame: [rbp+8] is its caller, [[rbp]+8] the one before.
    u64 caller = 0;
    u64 caller2 = 0;
    if (ReadablePointer(registers.rbp, 0x10)) {
        caller = ReadValue<u64>(registers.rbp, 8);
        const u64 up = ReadValue<u64>(registers.rbp, 0);
        caller2 = ReadablePointer(up, 0x10) ? ReadValue<u64>(up, 8) : 0;
    }
    const auto offset = [](u64 address) { return address > image_base ? address - image_base : 0; };
    const u32 forced_map = ReadValue<u32>(global, 0x14F0);
    // Mod 24 only cleared it when +0x14F0 held no map; 22:17:12 it held the host's own map and the
    // host still fell (the position is the summon placement's, not his). Not a guest = no forced warp.
    const bool clear = BbscOwnWorldNonGuest(global);
    if (clear) {
        WriteValue<u8>(global, 0x1520, 0);
    }
    LOG_INFO(Debug,
             "BBSC forced warp at a map choice (session state {}, own world {}, forced map {:#x}, "
             "point {}, callers {:#x} <- {:#x}): {}",
             GetSessionState(), ReadValue<u8>(global, 0x1592), forced_map,
             static_cast<s32>(registers.rsi), offset(caller), offset(caller2),
             clear ? "we are not a guest - taken off, the last lamp is used" : "kept");
}

void TickBbscStrayForcedWarp() {
    static s64 last_ms = 0;
    static s64 since_ms = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 500) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    if (!BbscOwnWorldNonGuest(global) || ReadValue<u8>(global, 0x08) != 0 ||
        ReadValue<u8>(global, 0x1520) == 0) {
        since_ms = 0;
        return;
    }
    if (since_ms == 0) {
        since_ms = now;
    }
    if (now - since_ms < 3'000) {
        return;
    }
    since_ms = 0;
    WriteValue<u8>(global, 0x1520, 0);
    LOG_INFO(Debug, "BBSC forced warp: a leftover forced-warp byte (+0x1520) taken off (session "
                    "state {}, own world, nothing loading) - it would send the next load to map {:#x}",
             GetSessionState(), ReadValue<u32>(global, 0x14F0));
}

void TickBbscSessionEnd() {
    const s64 now = SteadyNowMs();
    if (bbsc_persistence_restore_at_ms != 0 && now >= bbsc_persistence_restore_at_ms) {
        bbsc_persistence_restore_at_ms = 0;
        Core::Bbsc::SetPersistentSessionPatches(true);
    }
    const s32 session_state = GetSessionState();
    // Sent home: the host used a Silencing Blank (OnLeaveMagic) or the room vanished. The guest's
    // session drops to state 0 while the guest still stands in the host's world, and the kept
    // session patches then skipped the game's own trip home - the guest froze with the camera
    // stuck (2026-09-24). A guest never sees state 0 outside the host's world in a kept session
    // (deaths and follow loads stay in state 6), so this is only ever the end of the co-op: let
    // the vanilla teardown run, exactly like a quit to the title.
    static bool was_guest = false;
    const u64 end_global = GetGlobalState();
    const bool in_host_world =
        ReadablePointer(end_global, 0x1593) && ReadValue<u8>(end_global, 0x1592) == 0;
    if (session_state == 6) {
        was_guest = true;
    } else if (!in_host_world) {
        was_guest = false; // home again (or hosting): nothing left to end
    } else if (was_guest && session_state == 0 && bbsc_persistence_restore_at_ms == 0) {
        was_guest = false;
        SuspendBbscPersistence("guest sent home: session ended in the host's world");
    }
    // An invader who beat the host goes home, vanilla style. When the host had no co-op partner
    // alive, the host's own death already ends the session (BbscSpectateDeathHook); with partners
    // still alive the host spectates and the session lives on, so the invader leaves by itself
    // once the host has been dead for BbscInvaderWinMs: session patches lifted, then
    // CSMultiPlayMan::Stop - the session drops to state 0 and the "sent home" path above runs.
    // HYPOTHESIS until a log shows "BBSC invasion won".
    static s64 host_dead_since_ms = 0;
    if (session_state == 6 && IsInvaderType(GetLocalCharacterType()) &&
        GetCharacterHp(GetLocalPlayerCharacter()) > 0) {
        const u64 host = FindRemoteHostCharacter();
        if (host != 0 && GetCharacterHp(host) <= 0) {
            if (host_dead_since_ms == 0) {
                host_dead_since_ms = now;
            } else if (now - host_dead_since_ms >= BbscInvaderWinMs) {
                host_dead_since_ms = 0;
                SuspendBbscPersistenceOnce("invasion won: the invader goes home");
                const u64 session_manager =
                    ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
                if (ReadablePointer(session_manager, 0x128)) {
                    using Stop = u8 PS4_SYSV_ABI (*)(u64 session_manager);
                    const u8 stopped =
                        BbGuestCall(reinterpret_cast<Stop>(image_base + MultiPlayStopRequestOffset), 
                            session_manager);
                    LOG_INFO(Debug, "BBSC invasion won: the host is dead - leaving (Stop -> {})",
                             stopped);
                }
            }
        } else {
            host_dead_since_ms = 0;
        }
    } else {
        host_dead_since_ms = 0;
    }
    // "Gone" needs someone to have been here first. The host is in state 3 from the moment the
    // Beckoning Bell rings, long before a guest arrives, so counting from there fired while the
    // guest was still joining (2026-09-23: a suspension 28 s after the join, blocking the lamp)
    // - and with the session now ended on the host, it would close the room on a host who simply
    // waited three minutes for a guest.
    static bool partner_seen = false;
    if (session_state != 3 && session_state != 6) {
        partner_seen = false;
        bbsc_partner_missing_since_ms = 0;
        return;
    }
    if (RemotePlayerCount() > 0) {
        partner_seen = true;
        bbsc_partner_missing_since_ms = 0;
        return;
    }
    if (!partner_seen) {
        return; // still waiting for the first partner: nobody has gone anywhere
    }
    if (bbsc_partner_missing_since_ms == 0) {
        bbsc_partner_missing_since_ms = now;
        return;
    }
    if (now - bbsc_partner_missing_since_ms < BbscPartnerGoneMs || bbsc_persistence_restore_at_ms) {
        return;
    }
    bbsc_partner_missing_since_ms = 0;
    partner_seen = false;
    if (session_state == 3) {
        // Host (code review B1): lifting every patch let the vanilla checks run on a session BBSC
        // had kept in states vanilla never sees, and they dropped the host to the title with no
        // message. End it exactly the way StageTransition 0x013CDE30 does itself:
        // CSMultiPlayMan::Stop(rdi = [0x05540290]) then RemoteCleanup(rdi = global+0x16F8).
        const u64 session_manager = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
        const u64 global = GetGlobalState();
        if (!ReadablePointer(session_manager, 0x128) || !ReadablePointer(global, 0x1700) ||
            ReadValue<u8>(global, 0x08) != 0) {
            return; // (review 2026-10-08) not during a load: a partner reloading looks the same
        }
        const u64 remote = ReadValue<u64>(global, 0x16F8);
        if (!ReadablePointer(remote, 0xA0)) {
            return;
        }
        using Stop = u8 PS4_SYSV_ABI (*)(u64 session_manager);
        using Cleanup = void PS4_SYSV_ABI (*)(u64 remote);
        const u8 stopped =
            BbGuestCall(reinterpret_cast<Stop>(image_base + MultiPlayStopRequestOffset), session_manager);
        BbGuestCall(reinterpret_cast<Cleanup>(image_base + WorldChrManRemoteCleanupOffset), remote);
        LOG_INFO(Debug,
                 "BBSC partner gone for {} s: session ended on the host (Stop -> {}, remote "
                 "cleanup); host stays in their world",
                 BbscPartnerGoneMs / 1000, stopped);
        return;
    }
    // Guest: the vanilla path is what sends a guest home, so let it run - session patches only.
    SuspendBbscPersistence("host gone for 180 s");
}

// The lamp edits are only on while this player hosts a session (code review B3/B4).
void TickBbscLampRole() {
    static int last = -1;
    // A guest must never get the talk-script patches: a guest's lamp travel starts a load out of
    // the host's world that a kept session then stalls (2026-09-24, the guest's StageTransition
    // with the host removing the member and no load after it). Own world and player type 0 are
    // both required, not only the session state.
    const u64 global = GetGlobalState();
    const bool own_world = ReadablePointer(global, 0x1593) && ReadValue<u8>(global, 0x1592) != 0;
    const int hosting = GetSessionState() == 3 && own_world && GetLocalPlayerType() == 0 ? 1 : 0;
    if (hosting != last) {
        last = hosting;
        Core::Bbsc::SetHostLampPatches(hosting == 1);
    }
}

// Test 2026-10-07 (mod 24, 22:17:12): the host's room timed out and the host fell under the map
// again. The forced-warp log named the caller: 0x0138A51C, inside 0x0138A4A0 (called from the Lua
// dispatcher 0x01393323) - a Lua-callable handler with no role test at all: if the player is alive
// it sets the event state to 2, global+0x1520 = 1 and runs 0x01332BC0(-1) + StageTransition, i.e.
// "send this phantom home" from the forced summon placement. Here +0x14F0 held the host's own map,
// so the map-choice guard (no map at all) let it through, and the host loaded at the summon
// placement's junk position. A host or solo player in its own world has no home to go to: vanilla
// never warps a host when its room closes (10-06 21:16 / 21:47). So the handler is skipped for them.
constexpr u64 SendPhantomHomeOffset = 0x0138A4A0;
u8 bbsc_send_home_skip = 0;
bool bbsc_release_leave_locks = false;

// The leave scripts (OnLeavePlayer, OnLeaveMenu_Yes, OnRoomDisappeared, OnP2PTimeOut) set event
// flag 4047 ("leaving") and SetLoadWait (Lua event object +0x8D) right before they schedule this
// send-home step, and the load home normally clears both. Where BBSC skips the step, nothing clears
// them. Test 2026-10-08 (host, 3 guests): when two guests left, the host's game took one
// leave for its own (OnLeavePlayer: leaver == local id) and ran the step on the host at 22:36:42;
// skipped, the locks stayed - and nobody got the boss fog prompt after that ("no prompt to enter
// the fog", host and guest). Same lock as the host's Blank (mod 36).
void ReleaseBbscLeaveLocks(std::string_view why) {
    if (image_base == 0 || ReadValue<u64>(image_base + 0x0553B0D8, 0) == 0 ||
        ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0) == 0) {
        return;
    }
    const bool flag = BbscEventFlag(4047);
    if (flag) {
        using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
        BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, 4047, 0);
    }
    const u64 lua_man = ReadValue<u64>(image_base + 0x0553B0C8, 0);
    const u64 lua = ReadablePointer(lua_man, 0x10) ? ReadValue<u64>(lua_man, 8) : 0;
    const bool wait = ReadablePointer(lua, 0x90) && ReadValue<u8>(lua, 0x8D) != 0;
    if (wait) {
        WriteValue<u8>(lua, 0x8D, 0);
    }
    LOG_INFO(Debug, "BBSC leave ({}): leave locks released (flag 4047 {}, load wait {})", why,
             flag ? "was on" : "off", wait ? "was on" : "off");
}

void PS4_SYSV_ABI BbscSendPhantomHomeHook(u64, const GuestRegisterSnapshot* registers) {
    const u64 global = GetGlobalState();
    if (!BbscOwnWorldNonGuest(global)) {
        LOG_INFO(Debug, "BBSC leave: the game's send-home step runs (session state {}, own world {})",
                 GetSessionState(),
                 ReadablePointer(global, 0x1598) ? ReadValue<u8>(global, 0x1592) : 0xFF);
        return;
    }
    bbsc_send_home_skip = 1;
    bbsc_release_leave_locks = true; // after this tick: the step's caller may set them right after
    u64 caller = 0;
    if (registers != nullptr && HasMemoryAccess(registers->rsp, sizeof(u64), MemoryProt::CpuRead)) {
        caller = ReadValue<u64>(registers->rsp, 0);
    }
    LOG_INFO(Debug, "BBSC forced warp: the game's \"send the phantom home\" ran on a player in its "
                    "own world (session state {}, caller {:#x}) - skipped, nobody is sent anywhere",
             GetSessionState(), caller > image_base ? caller - image_base : 0);
}

void TickBbscLeaveLocks() {
    static s64 armed_ms = 0;
    const s64 now = SteadyNowMs();
    if (bbsc_release_leave_locks) {
        bbsc_release_leave_locks = false;
        armed_ms = now;
    }
    if (armed_ms == 0 || now - armed_ms < 1'000) {
        return;
    }
    armed_ms = 0;
    const u64 global = GetGlobalState();
    if (BbscOwnWorldNonGuest(global) && ReadValue<u8>(global, 0x08) == 0) {
        ReleaseBbscLeaveLocks("send-home skipped in our own world");
    }
}

// Safety net (test 2026-10-12 5p, host 23:13:13-19): the send-home skip released the locks,
// then OnRoomTimeOut (caller 0x1789F6B) set flag 4047 + the load wait again and no send-home step
// followed -> the host stood in its Dream with every menu/bell locked (soft lock). Within 60 s after
// a session ended, while we are home, alive and not loading: locks held 8 s -> released.
void TickBbscStuckLeaveLocks() {
    static s64 last_ms = 0;
    static s64 ended_ms = 0;
    static s64 held_ms = 0;
    static s32 last_state = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    if (state == 0 && last_state != 0) {
        ended_ms = now;
    }
    last_state = state;
    if (ended_ms == 0 || state != 0) {
        held_ms = 0;
        if (state != 0) {
            ended_ms = 0;
        }
        return;
    }
    if (now - ended_ms > 60'000) {
        ended_ms = 0;
        held_ms = 0;
        return;
    }
    const u64 global = GetGlobalState();
    const u64 me = GetLocalPlayerCharacter();
    if (!BbscOwnWorldNonGuest(global) || ReadValue<u8>(global, 0x08) != 0 || me == 0 ||
        GetCharacterHp(me) <= 0 || ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0) == 0) {
        held_ms = 0;
        return;
    }
    const u64 lua_man = ReadValue<u64>(image_base + 0x0553B0C8, 0);
    const u64 lua = ReadablePointer(lua_man, 0x10) ? ReadValue<u64>(lua_man, 8) : 0;
    const bool locked = BbscEventFlag(4047) || (ReadablePointer(lua, 0x90) && ReadValue<u8>(lua, 0x8D) != 0);
    if (!locked) {
        held_ms = 0;
        return;
    }
    if (held_ms == 0) {
        held_ms = now;
        return;
    }
    if (now - held_ms < 8'000) {
        return;
    }
    held_ms = 0;
    ReleaseBbscLeaveLocks("still locked 8 s after the session ended at home (OnRoomTimeOut etc.)");
}

// A guest left in someone else's world after its session ended (test 2026-10-08: the host's game
// closed at 22:38:39; a tester's game saw the room disappear and stayed in the host's world - no load
// home, nothing to do there). After BbscStrandedMs in session state 0 outside our own world with
// no load running, BBSC sends us home the way the game does it after a boss (BlockClear2_3,
// 0x01385930): event state 2, forced warp global+0x1520 = 1, home respawn point 0x01332BC0(-1),
// StageTransition 0x013CDE30.
constexpr s64 BbscStrandedMs = 15'000;
void TickBbscStrandedGuest() {
    static s64 since_ms = 0;
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    const u64 me = GetLocalPlayerCharacter();
    const bool stranded = GetSessionState() == 0 && ReadablePointer(global, 0x1598) &&
                          ReadValue<u8>(global, 0x1592) == 0 && ReadValue<u8>(global, 0x08) == 0 &&
                          me != 0 && GetCharacterHp(me) > 0;
    if (!stranded) {
        since_ms = 0;
        return;
    }
    if (since_ms == 0) {
        since_ms = now;
        return;
    }
    if (now - since_ms < BbscStrandedMs) {
        return;
    }
    since_ms = 0;
    const u64 lua_man = ReadValue<u64>(image_base + 0x0553B0C8, 0);
    const u64 lua = ReadablePointer(lua_man, 0x10) ? ReadValue<u64>(lua_man, 8) : 0;
    const u64 event_state = ReadValue<u64>(image_base + 0x0553B0D8, 0);
    if (lua == 0 || !ReadablePointer(event_state, 4) || !IsSprjLuaEventManReady()) {
        return;
    }
    LOG_WARNING(Debug, "BBSC leave: still in another player's world {} s after the session ended - "
                       "BBSC sends us home",
                BbscStrandedMs / 1000);
    WriteValue<s32>(event_state, 0, 2);
    WriteValue<u8>(global, 0x1520, 1);
    using HomePoint = void PS4_SYSV_ABI (*)(u64 lua, s32 point);
    using Transition = u64 PS4_SYSV_ABI (*)();
    BbGuestCall(reinterpret_cast<HomePoint>(image_base + 0x01332BC0), lua, -1);
    BbGuestCall(reinterpret_cast<Transition>(image_base + 0x013CDE30));
}

// Shop trace (2026-10-07). Report: in the host's Dream the Bath Messenger sold the guest everything
// (both saves new). The real shop list is built by 0x01C76870(out, first row, last row): for each
// ShopLineupParam row (getter 0x01F27450) it keeps only the rows 0x01F27570 calls unlocked - qwcId
// (+0x10) -1, or that event flag on (read inline from SprjEventFlagMan). So for the guest those
// flags were on in the live table. Which ones, and from where, the next log says: at every list
// build BBSC asks the same two functions about the same rows and logs the unlocked ones.
constexpr u64 ShopListBuildOffset = 0x01C76870;
constexpr u64 ShopLineupGetOffset = 0x01F27450;
constexpr u64 ShopLineupUnlockedOffset = 0x01F27570;

void PS4_SYSV_ABI BbscShopListHook(u64, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr || image_base == 0) {
        return;
    }
    const s32 first = static_cast<s32>(registers->rsi);
    const s32 last = static_cast<s32>(registers->rdx);
    if (first < 0 || last < first || last - first > 20'000) {
        return;
    }
    struct RowRef {
        s32 id;
        s32 pad;
        u64 row;
    };
    using GetRow = void PS4_SYSV_ABI (*)(RowRef* out, s32 id);
    using Unlocked = bool PS4_SYSV_ABI (*)(const RowRef* ref);
    const auto get = reinterpret_cast<GetRow>(image_base + ShopLineupGetOffset);
    const auto unlocked = reinterpret_cast<Unlocked>(image_base + ShopLineupUnlockedOffset);
    size_t rows = 0;
    size_t open = 0;
    size_t gated_open = 0;
    std::string shown;
    for (s32 id = first; id <= last; ++id) {
        RowRef ref{-1, 0, 0};
        BbGuestCall(get, &ref, id);
        if (ref.row == 0 || !ReadablePointer(ref.row, 0x20)) {
            continue;
        }
        ++rows;
        if (!BbGuestCall(unlocked, &ref)) {
            continue;
        }
        ++open;
        const s32 qwc = ReadValue<s32>(ref.row, 0x10);
        if (qwc >= 0) {
            ++gated_open;
            if (gated_open <= 16) {
                shown += fmt::format(" {}:{}", id, qwc);
            }
        }
    }
    const u64 global = GetGlobalState();
    LOG_INFO(Debug,
             "BBSC shop: rows {}-{} - {} rows, {} for sale, {} of them unlocked by a flag (session "
             "state {}, own world {}, Dream state held {:#x}); unlocked row:flag{}",
             first, last, rows, open, gated_open, GetSessionState(),
             ReadablePointer(global, 0x1598) ? ReadValue<u8>(global, 0x1592) : 0xFF,
             bbsc_dream_own.held, shown.empty() ? std::string(" none") : shown);
}

// Kin Bell trace (report 2026-10-12: "can't use Kin in the Dream"). In the item check 0x0157F200
// the Small Resonant / Sinister Bell branch (r12d 0xCD / 0xE1) calls 0x0191A750([0x0553E878]);
// a 0 there takes the "not usable" side (0x0157F7B0). The hook sits right after that call
// (0x0157F6D1: mov bl, al ; mov rdi, r14) and logs each change of goods id / result / map.
constexpr u64 BellCheckTraceOffset = 0x0157F6D1;
void PS4_SYSV_ABI BbscBellCheckHook(u64, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr || !HasMemoryAccess(registers->rbp - 0x1B8, 4, MemoryProt::CpuRead)) {
        return;
    }
    static u64 last_key = ~0ull;
    const u32 goods = ReadValue<u32>(registers->rbp - 0x1B8, 0);
    const u8 ok = static_cast<u8>(registers->rax & 0xFF);
    const u32 map = GetCurrentPackedMap();
    const u64 key = (u64{goods} << 40) ^ (u64{ok} << 32) ^ map ^ (u64(registers->r12 & 0xFF) << 48);
    if (key == last_key) {
        return;
    }
    last_key = key;
    const u64 global = GetGlobalState();
    LOG_INFO(Debug, "BBSC bell check: goods {} (kind {:#x}) in map {:#x} - 0x0191A750 says {} "
                    "(session state {}, own world {}) {}",
             goods, static_cast<u32>(registers->r12), map, ok, GetSessionState(),
             ReadablePointer(global, 0x1598) ? ReadValue<u8>(global, 0x1592) : 0xFF,
             ok ? "usable side" : "NOT usable side");
}

void InstallBbscQuitHook() {
    if (image_base == 0) {
        return;
    }
    {
        static constexpr std::array<u8, 5> BellCheck{0x88, 0xC3, 0x4C, 0x89, 0xF7};
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + BellCheckTraceOffset),
                                  std::span<const u8>{BellCheck}, 0, BbscBellCheckHook)) {
            LOG_ERROR(Debug, "BBSC bell check: failed to hook {:#x}", BellCheckTraceOffset);
        }
    }
    {
        static constexpr std::array<u8, 6> ShopPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ShopListBuildOffset),
                                  std::span<const u8>{ShopPrologue}, 0, BbscShopListHook)) {
            LOG_ERROR(Debug, "BBSC shop: failed to hook the shop list at {:#x}", ShopListBuildOffset);
        }
    }
    static constexpr std::array<u8, 6> Prologue{0x55, 0x48, 0x89, 0xE5, 0x53, 0x50};
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + SendPhantomHomeOffset),
                              std::span<const u8>{Prologue}, 0, BbscSendPhantomHomeHook,
                              &bbsc_send_home_skip)) {
        LOG_ERROR(Debug, "BBSC forced warp: failed to hook {:#x}", SendPhantomHomeOffset);
    }
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + RegistReturnTitleOffset),
                              std::span<const u8>{Prologue}, 0, BbscQuitToTitleHook)) {
        LOG_ERROR(Debug, "BBSC quit: failed to hook RegistReturnTitle at {:#x}",
                  RegistReturnTitleOffset);
        return;
    }
    LOG_INFO(Debug, "BBSC quit to title ends the session for both players (RegistReturnTitle {:#x})",
             RegistReturnTitleOffset);
}

// ---- Guest HP penalty (SpEffectParam 9006) -----------------------------------------------------
// A summoned guest carries special effect 9006, whose maxHpRate of 0.7 takes 30% off their maximum
// HP. Vanilla clears it on the reload that ends a co-op session; BBSC skips that reload, so the
// penalty stayed forever, through deaths included (reported 2026-09-20).
//
// Layout from a DLL proof of concept written by the friend testing BBSC, re-verified here against
// the local player: chr+0x1C8 is the special effect manager, +0x08 its first node; each node has
// the effect id at +0x40, its param row at +0x48 and the next node at +0x58; the rate itself is
// param+0x10. Writing 1.0 there removes the penalty, and maximum HP is scaled back up by the rate
// that was in force (the game does not recompute it on its own).
//
// Everything is signature-checked: only a node that really says 9006 with a rate below 1 is
// touched, so wrong offsets mean no match and no write rather than a corrupted character.
constexpr s64 BbscHpPenaltyIntervalMs = 1'000;
constexpr s32 BbscHpPenaltyEffectId = 9006;
// The same 0.7 maxHpRate on the other phantom types (SpEffectParam, tools/gamedata/param.py):
// 9026 invader (Sinister Bell; test 2026-10-05 "hp cut"), 9036 / 9046 / 9056 the other kinds.
constexpr s32 BbscHpPenaltyOtherIds[] = {9026, 9036, 9046, 9056};
s64 bbsc_hp_penalty_at_ms = 0;

bool BbscHpPenaltyEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_NO_HP_PENALTY");
    return enabled;
}

// Max HP lives where GetCharacterHp reads it: [[chr+0x3B0]+0x20]+0xFC (current HP at +0xF8).
bool RestoreBbscMaxHp(u64 chr, float rate) {
    if (rate <= 0.05f || rate >= 0.999f || !ReadablePointer(chr, 0x3B8)) {
        return false;
    }
    const u64 data = ReadValue<u64>(ReadValue<u64>(chr, 0x3B0), 0x20);
    if (!ReadablePointer(data, 0x100)) {
        return false;
    }
    const s32 max_hp = ReadValue<s32>(data, 0xFC);
    if (max_hp < 80 || max_hp > 4000) {
        return false;
    }
    const s32 restored = static_cast<s32>(static_cast<float>(max_hp) / rate + 0.5f);
    // Only restore a value that really looks like it was scaled: putting the rate back must land
    // on what is there now (+-1 for rounding). Otherwise this is already a full HP bar.
    const s32 back = static_cast<s32>(static_cast<float>(restored) * rate + 0.5f);
    if (restored <= max_hp || std::abs(back - max_hp) > 1) {
        return false;
    }
    WriteValue<s32>(data, 0xFC, restored);
    LOG_INFO(Debug, "BBSC HP penalty: max HP {} -> {} (rate {:.2f})", max_hp, restored, rate);
    return true;
}

void TickBbscHpPenalty() {
    if (!BbscHpPenaltyEnabled() || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - bbsc_hp_penalty_at_ms < BbscHpPenaltyIntervalMs) {
        return;
    }
    bbsc_hp_penalty_at_ms = now;
    const u64 player = GetLocalPlayerCharacter();
    if (!ReadablePointer(player, 0x1D0)) {
        return;
    }
    const u64 manager = ReadValue<u64>(player, 0x1C8);
    if (!ReadablePointer(manager, 0x10)) {
        return;
    }
    u64 node = ReadValue<u64>(manager, 0x08);
    for (int step = 0; step < 64 && node != 0; ++step) {
        if (!ReadablePointer(node, 0x60)) {
            return;
        }
        const s32 id = ReadValue<s32>(node, 0x40);
        const u64 param = ReadValue<u64>(node, 0x48);
        const bool penalty = id == BbscHpPenaltyEffectId ||
                             std::ranges::find(BbscHpPenaltyOtherIds, id) !=
                                 std::end(BbscHpPenaltyOtherIds);
        if (penalty && ReadablePointer(param, 0x14)) {
            const float rate = ReadValue<float>(param, 0x10);
            if (rate > 0.05f && rate < 0.999f) {
                WriteValue<float>(param, 0x10, 1.0f);
                LOG_INFO(Debug, "BBSC HP penalty: effect {} rate {:.2f} -> 1.0", id, rate);
                RestoreBbscMaxHp(player, rate);
            }
        }
        node = ReadValue<u64>(node, 0x58);
    }
}

// ---- Co-op fog walls ---------------------------------------------------------------------------
// Rebuilt from the game data on 2026-09-25 (tools/gamedata: msb.py, emevd.py, fog_survey.py,
// chalice_walls.py) instead of hand-kept id lists.
//
// Every co-op wall in the game - 18 in the base game and the DLC, 8397 placements across the
// Chalice blocks - is switched by one of two identical event scripts, 7600 (common.emevd) and
// 12907600 (m29.emevd), started once per wall by InitializeEvent(slot, 7600, object, fog SFX):
//     2005[3](object, 0); 2006[1](sfx, 1)     hide the wall object, delete the fog
//     wait for multiplayer state 3 or 2
//     2005[3](object, 1); 2006[2](sfx)        wall solid and drawn, fog spawned
//     wait for it to end; restart
// Every wall the map files name as a co-op wall (マルチ / マルチ閉じ込め壁) is handed to one of the
// two, and no boss wall is: boss walls are switched by their own boss events (2006[1]/[2] on the
// xx03800-range fog ids). The Chalice blocks reuse entity ids - 79 of the 88 Chalice co-op fog ids
// are boss or door walls in other blocks - so a wall is recognised by the script switching it, not
// by its id. The object (bank 2005) and SFX (bank 2006) instruction handlers are watched; when the
// running event is one of the two co-op wall scripts, the draw / collision / fog-on calls it makes
// for its own wall are refused. Its hide calls still run, so the wall stays hidden, crossable and
// without fog, in every map, and nothing else is touched. The scripts only raise the walls in
// multiplayer, so single player is unchanged.
//
// The old lists hid Old Yharnam's lamp object (2301950, o009900, driven by the lamp scripts
// 7000-7300) in every session, and named 2407000, 2707002 and 3502001 as walls; none of that
// survives here.
constexpr u64 ObjectDrawOffset = 0x013CABD0;      // (entity_id, on)
constexpr u64 ObjectCollisionOffset = 0x013CA580; // (entity_id, on)
// 0x01315FA0(map_list, entity_id) switches a map SFX on (EMEVD 2006[2] calls it directly);
// 0x01315E50(_, entity_id, 1) deletes it again (2006[1]).
constexpr u64 ObjectSfxEnableOffset = 0x01315FA0;
constexpr u64 EventObjectBankOffset = 0x017C46E0; // EMEVD bank 2005 handler (rdi, instance)
constexpr u64 EventSfxBankOffset = 0x017C64E0;    // EMEVD bank 2006 handler (rdi, instance)
constexpr std::array<s64, 2> BbscCoopWallEvents{7600, 12907600};
constexpr size_t BbscWallTraceLimit = 800;

u8 bbsc_wall_draw_skip = 0;
u8 bbsc_wall_collision_skip = 0;
u8 bbsc_wall_sfx_skip = 0;
// The wall the co-op wall script is switching right now, on this thread: the bank handler runs
// first and then calls draw / collision / fog-on synchronously on the same thread.
thread_local u32 bbsc_coop_wall_entity = 0;
// Each (entity, draw/collision) pair is logged once: a wall is enabled once, while doors, lamps
// and lifts toggle constantly, so without this the interesting lines drown in repeats.
std::mutex bbsc_wall_seen_mutex;
std::vector<u64> bbsc_wall_seen;

bool BbscNoFogWallsEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_NO_FOG_WALLS");
    return enabled;
}

bool BbscWallTraceEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_TRACE_WALLS");
    return enabled;
}

// The event a running EMEVD instance belongs to. [instance+0xA8]+8 is its EMEVD file, kept raw in
// memory (+0x10 event count, +0x18 event table, +0x28 instruction table, all offsets from the file
// start; the handlers themselves find arguments as file + [file+0x78] + [instruction+0x10]), and
// [instance+0xB0] is the instruction being executed. Events are 0x30 bytes (id, instruction count,
// first instruction offset from the table), instructions 0x20 bytes.
s64 BbscRunningEventId(u64 instance) {
    const u64 ref = ReadValue<u64>(instance, 0xA8);
    const u64 instruction = ReadValue<u64>(instance, 0xB0);
    if (!ReadablePointer(ref, 0x10)) {
        return -1;
    }
    const u64 file = ReadValue<u64>(ref, 0x08);
    if (!ReadablePointer(file, 0x90)) {
        return -1;
    }
    const s64 count = ReadValue<s64>(file, 0x10);
    const u64 events = file + ReadValue<u64>(file, 0x18);
    const u64 instructions = file + ReadValue<u64>(file, 0x28);
    if (count <= 0 || count > 100'000 || instruction < instructions ||
        !ReadablePointer(events, static_cast<size_t>(count) * 0x30)) {
        return -1;
    }
    const u64 index = (instruction - instructions) / 0x20;
    for (s64 k = 0; k < count; ++k) {
        const u64 event = events + static_cast<u64>(k) * 0x30;
        const u64 first = ReadValue<u64>(event, 0x10) / 0x20;
        if (index >= first && index < first + ReadValue<u64>(event, 0x08)) {
            return ReadValue<s64>(event, 0x00);
        }
    }
    return -1;
}

// tag 3 = bank 2005 (objects), tag 4 = bank 2006 (SFX); rsi is the running event instance.
void PS4_SYSV_ABI BbscWallScriptHook(u64 tag, const GuestRegisterSnapshot* registers) {
    bbsc_coop_wall_entity = 0;
    if (registers == nullptr) {
        return;
    }
    const u64 instance = registers->rsi;
    if (!ReadablePointer(instance, 0xC0)) {
        return;
    }
    const u64 instruction = ReadValue<u64>(instance, 0xB0);
    if (!ReadablePointer(instruction, 0x18)) {
        return;
    }
    // Arguments: resolved into [instance+0xB8] when the event has parameters (the wall scripts
    // do), otherwise read from the file, exactly as the handlers read them.
    u64 args = ReadValue<u64>(instance, 0xB8);
    if (args == 0) {
        const u64 file = ReadValue<u64>(ReadValue<u64>(instance, 0xA8), 0x08);
        if (!ReadablePointer(file, 0x80)) {
            return;
        }
        args = file + ReadValue<u64>(file, 0x78) + ReadValue<u64>(instruction, 0x10);
    }
    if (!ReadablePointer(args, 4)) {
        return;
    }
    s32 entity = ReadValue<s32>(args, 0);
    if (entity == 10000) {
        entity = ReadValue<s32>(instance, 0x60);
    }
    if (entity <= 0) {
        return;
    }
    const s64 event = BbscRunningEventId(instance);
    if (std::find(BbscCoopWallEvents.begin(), BbscCoopWallEvents.end(), event) ==
        BbscCoopWallEvents.end()) {
        return;
    }
    bbsc_coop_wall_entity = static_cast<u32>(entity);
    if (BbscWallTraceEnabled()) {
        const u64 key = (static_cast<u64>(0x100 + tag) << 32) | static_cast<u32>(entity);
        std::scoped_lock lock{bbsc_wall_seen_mutex};
        if (bbsc_wall_seen.size() < BbscWallTraceLimit &&
            std::find(bbsc_wall_seen.begin(), bbsc_wall_seen.end(), key) == bbsc_wall_seen.end()) {
            bbsc_wall_seen.push_back(key);
            LOG_INFO(Debug, "BBSC fog wall: co-op wall script {} switches {} {} (map {:#x})", event,
                     tag == 3 ? "object" : "fog", entity, GetCurrentPackedMap());
        }
    }
}

// tag 0 = draw (0x013CABD0), tag 1 = collision (0x013CA580), both (entity_id, on).
// tag 2 = fog on (0x01315FA0), which is (map_list, entity_id): the id is in rsi, there is no "on".
void PS4_SYSV_ABI BbscWallToggleHook(u64 tag, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    const u32 coop_wall = bbsc_coop_wall_entity;
    if (tag == 2) {
        const u32 sfx_id = static_cast<u32>(registers->rsi);
        if (coop_wall != 0 && sfx_id == coop_wall && BbscNoFogWallsEnabled()) {
            bbsc_wall_sfx_skip = 1;
            LOG_INFO(Debug, "BBSC fog wall: co-op fog {} kept off (map {:#x})", sfx_id,
                     GetCurrentPackedMap());
        }
        return;
    }
    const u32 entity_id = static_cast<u32>(registers->rdi);
    const bool on = (registers->rsi & 0xFFFFFFFF) != 0;
    const std::string_view what = tag == 0 ? "draw" : "collision";
    if (on && coop_wall != 0 && entity_id == coop_wall && BbscNoFogWallsEnabled()) {
        (tag == 0 ? bbsc_wall_draw_skip : bbsc_wall_collision_skip) = 1;
        LOG_INFO(Debug, "BBSC fog wall: co-op wall {} {} kept off (map {:#x})", entity_id, what,
                 GetCurrentPackedMap());
        return;
    }
    // Trace: only enables, and only inside a session.
    const s32 session_state = GetSessionState();
    if (!on || !BbscWallTraceEnabled() || (session_state != 3 && session_state != 6)) {
        return;
    }
    {
        const u64 key = (tag << 32) | entity_id;
        std::scoped_lock lock{bbsc_wall_seen_mutex};
        if (bbsc_wall_seen.size() >= BbscWallTraceLimit ||
            std::find(bbsc_wall_seen.begin(), bbsc_wall_seen.end(), key) !=
                bbsc_wall_seen.end()) {
            return;
        }
        bbsc_wall_seen.push_back(key);
    }
    u64 caller = 0;
    if (HasMemoryAccess(registers->rsp, sizeof(u64), MemoryProt::CpuRead)) {
        std::memcpy(&caller, reinterpret_cast<const void*>(registers->rsp), sizeof(caller));
    }
    LOG_INFO(Debug, "BBSC object {} on: entity {} (caller {:#x}, session state {})", what,
             entity_id, caller > image_base ? caller - image_base : caller, session_state);
}

void InstallBbscWallHooks() {
    if (image_base == 0 || (!BbscWallTraceEnabled() && !BbscNoFogWallsEnabled())) {
        return;
    }
    static constexpr std::array<u8, 6> Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    // 0x01315FA0 opens with PUSH RBP / R15 / R14 / R12 / RBX and no frame setup.
    static constexpr std::array<u8, 8> SfxPrologue{0x55, 0x41, 0x57, 0x41, 0x56,
                                                   0x41, 0x54, 0x53};
    // Both EMEVD bank handlers: PUSH RBP; MOV RBP, RSP; PUSH R15; PUSH R14 (read 2026-09-25).
    static constexpr std::array<u8, 8> BankPrologue{0x55, 0x48, 0x89, 0xE5,
                                                    0x41, 0x57, 0x41, 0x56};
    const bool ok =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + EventObjectBankOffset),
                             std::span<const u8>{BankPrologue}, 3, BbscWallScriptHook) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + EventSfxBankOffset),
                             std::span<const u8>{BankPrologue}, 4, BbscWallScriptHook) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ObjectDrawOffset),
                             std::span<const u8>{Prologue}, 0, BbscWallToggleHook,
                             &bbsc_wall_draw_skip) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ObjectCollisionOffset),
                             std::span<const u8>{Prologue}, 1, BbscWallToggleHook,
                             &bbsc_wall_collision_skip) &&
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ObjectSfxEnableOffset),
                             std::span<const u8>{SfxPrologue}, 2, BbscWallToggleHook,
                             &bbsc_wall_sfx_skip);
    if (!ok) {
        LOG_ERROR(Debug, "BBSC fog walls: failed to hook the wall scripts / draw / collision / fog");
        return;
    }
    LOG_INFO(Debug,
             "BBSC fog walls: co-op walls kept off wherever event 7600 / 12907600 raises them "
             "(base game, DLC, Chalice); boss walls untouched");
}

// ---- The Hunter's Dream together (2026-09-25) --------------------------------------------------
// A summoned guest who follows the host into the Hunter's Dream (map 0x15000000) may use the Doll,
// Gehrman, the workshop and the messengers there. The headstones and the Ritual Altars stay the
// host's: the guest travels by following the host, and Chalice Dungeons are not part of seamless
// play. Everything below was read off the Dream's own files (tools/gamedata/esd.py, esd_dump.py,
// emevd.py) and the eboot:
//
// 1. Talk scripts. Every NPC and device of the Dream is a talk script (t2103xx/t2106xx/t2107xx.esd,
//    one per map character with a TalkID). Each one starts with "if f19() == 1 -> client waiting"
//    and talks only while f19() == 0. f19 is byte +0x25 of the script's condition evaluator
//    (vtable 0x0535AAB0, talk id at +0x10; condition dispatcher 0x0203AD60 case 0x13). 0x0203ABB0
//    sets that byte every frame when the local character's type (+0x78) is not 0 or 8 - a
//    phantom - and nothing ever clears it. The talk update 0x0204C0B0 calls 0x0203ABB0 and then
//    steps the script, so BBSC clears the byte in between (hook on 0x0204C694, the instruction
//    right after the call), for the allowed scripts only.
// 2. Map events. The Dream's event script plays the Doll's level-up handshake and the messengers'
//    animations and gifts in events that end at once unless "character 10000 has type 0"
//    (4[03](cond, 10000, 0) followed by End-if-false). A guest would wait forever at the Doll: her
//    script sets flag 72100100 and opens the level-up menu only once events 12100112/12100113
//    have set 72100101 and 72100102. That check is evaluated by 0x017A3BE0 (condition +0x28
//    entity, +0x2C wanted type, result in bit 0 of +0x10). BBSC answers "yes" for the events of
//    the allowed interactions only; every other event (endings, boss fights, headstone messengers,
//    all of common.emevd) keeps the vanilla answer.
// 3. Looks. The NPC character constructor 0x018E4870 gives a character whose NpcParam row has
//    isChangeWanderGhost (+0x145 bit 7) the row's ghostModelId (+0x10C) and type 4, the see-through
//    "wandering ghost", whenever the local player is in someone else's world (SprjSessionManager
//    +0x124 == 6, or [global+0x16F8]+0x98 != -1). In the Dream those are the Doll (NpcParam 803000,
//    ghost model 8035) and Gehrman (804000, 8045); the messengers have no ghost model. BBSC clears
//    the bit in those two rows once the params are loaded (row lookup 0x01F226C0, repository
//    [0x05540340]). The rows are only used in the Dream, where vanilla never has a phantom.
// Gehrman is allowed until the night's end. With flag 1028, or 1029 without 1030, his script turns
// to "Submit Your Life / Refuse" (t210305, group 0x7FFFFFF7 state 0), and those start the ending
// or the final fight - never for a guest in the host's world, so he stays silent to the guest.
constexpr u32 BbscHuntersDreamMap = 0x15000000;
constexpr u64 TalkClientCheckedOffset = 0x0204C694; // MOV RDI, [RCX + R13*8 + 0x18]
constexpr u64 TalkConditionVtableOffset = 0x0535AAB0;
constexpr u64 CharacterTypeCheckOffset = 0x017A3BE0;
constexpr u64 GetEventFlagOffset = 0x01399BF0; // (unused, flag) -> bool, SprjEventFlagMan read
constexpr u64 EventFlagManPointerOffset = 0x0553B100;
constexpr u32 GehrmanTalkId = 210305;
constexpr u32 BbscWorkbenchTalkId = 210692;
constexpr u64 NpcParamRowOffset = 0x01F226C0; // (u32 out[4], id): out+8 = the row, or 0
constexpr u64 SoloParamRepositoryPointerOffset = 0x05540340;
constexpr size_t NpcParamFlagsOffset = 0x145;
constexpr u8 NpcParamWanderGhostBit = 0x80;
constexpr std::array<u32, 2> BbscDreamSolidNpcs{803000, 804000}; // the Doll, Gehrman

struct BbscDreamTalk {
    u32 id;
    const char* what;
};

// Identified by their menus and help texts (category 30): 10011000 "Use Blood Echoes to raise
// stats", 10011020 "Place items in storage", 10011080 "Memorize a Caryll Rune", 10011090 "Wear
// clothing to change appearance", 10011190 "Buy with Insight", 10011220/10011230 the first trick
// weapon and firearm, 10011290/10011400 the notebook and the Old Hunter Bell.
// The starter weapon messengers (t210694 trick weapon, t210695 firearm) are left out (2026-10-08,
// the user: "every player picks the weapon in his own world, then plays together"): their events
// 12101020/12101021 raise the weapons and sink the messengers once ANYONE buys, and that showed
// for the other player too ("if I take it, it disappears for him").
constexpr std::array<BbscDreamTalk, 10> BbscDreamGuestTalks{{
    {210304, "Plain Doll (level up)"},
    {GehrmanTalkId, "Gehrman"},
    {210691, "Bath Messengers (buy, sell)"},
    {210692, "Workbench (fortify, repair, blood gems, Arcane Haze)"},
    {210693, "Storage"},
    {210696, "attire"},
    {210697, "messenger adornments"},
    {210698, "Insight Bath Messengers"},
    {210699, "Memory Altar (Caryll runes, oaths)"},
    {210720, "messengers (notebook, Old Hunter Bell)"},
}};
// Not allowed: t210000-210003 and t210721 (headstones), t210050-210110 (Ritual Altars), t210306
// (Gehrman as the final boss).

// m21_00 events behind the allowed interactions, each read in full (tools/gamedata):
//   Doll      12100112/12100113 level-up handshake, 12100114-12100117 her animations,
//             12100123 her gift after the prayer at the grave
//   Gehrman   12100146, 12100165 his animations
//   workshop  12100450 workshop-tool flags, 12105210 the workbench's repair request (70000100)
//   messengers 12100020 adornment unlocks, 12101020/12101021 first weapons, 12101022/24/26/28
//             notebook and bells, 12105043 Insight messengers, 12105060/62/64/70 adornments
constexpr std::array<s64, 21> BbscDreamGuestEvents{
    12100020, 12100112, 12100113, 12100114, 12100115, 12100116, 12100117, 12100123,
    12100146, 12100165, 12100450, 12101022, 12101024, 12101026, 12101028, 12105043,
    12105060, 12105062, 12105064, 12105070, 12105210,
};

bool BbscDreamEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_DREAM");
    return enabled;
}

// A summoned guest standing in someone else's world, as 0x0203ABB0 sees it (type not 0 or 8).
bool BbscGuestInHostWorld() {
    if (GetSessionState() != 6) {
        return false;
    }
    const u64 global = GetGlobalState();
    if (!ReadablePointer(global, 0x1593) || ReadValue<u8>(global, 0x1592) != 0) {
        return false;
    }
    const s32 type = GetLocalCharacterType();
    return type > 0 && type != 8;
}

bool BbscEventFlag(u32 flag) {
    // 0x01399BF0 panics when SprjEventFlagMan does not exist.
    if (ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
        return false;
    }
    using GetEventFlag = bool PS4_SYSV_ABI (*)(u64 unused, u32 flag);
    return BbGuestCall(reinterpret_cast<GetEventFlag>(image_base + GetEventFlagOffset), 0, flag);
}

bool BbscNightIsOver() {
    return BbscEventFlag(1028) || (BbscEventFlag(1029) && !BbscEventFlag(1030));
}

u32 bbsc_dream_talk_logged = 0; // bit per BbscDreamGuestTalks entry, cleared on leaving the Dream

// The workbench's "Blood Gem Fortification" and "Arcane Haze" entries are switched on by flags
// 72100200 / 72100201, which t210692 sets only when the LOCAL player holds the Blood Gem Workshop
// Tool (4103) / the Workshop Haze Extractor (4102) (state 7 / 19, f47(3, item)). Test 2026-10-01:
// the guest got "The workshop tool required for this task is missing." at the host's workbench.
// In the host's Dream the guest gets the host's tools: the bits are taken from the host's m21_00
// snapshot (group 72100, sent with the map's flags) and set, never cleared. Game thread.
constexpr size_t BbscDreamToolGroupIndex = 20; // kind 7 = BbscWorldFlagKinds[2], block 0
constexpr std::array<u32, 2> BbscDreamToolFlags{72100200, 72100201};
bool bbsc_dream_tools_logged = false;

void BbscGiveHostWorkshopTools() {
    BbscWorldSnapshot host;
    if (!BbscHostBlockSnapshot(BbscHuntersDreamMap, host) ||
        (host.present & (u64{1} << BbscDreamToolGroupIndex)) == 0 ||
        ReadValue<u64>(image_base + 0x0553B0D8, 0) == 0 || // SprjEventState
        ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
        static bool waiting_logged = false;
        if (!waiting_logged) {
            waiting_logged = true;
            LOG_INFO(Debug, "BBSC Dream: workbench tools - no host Dream flags yet");
        }
        return;
    }
    std::array<bool, 2> host_has{};
    std::array<bool, 2> had{};
    for (size_t i = 0; i < BbscDreamToolFlags.size(); ++i) {
        const u32 bit = BbscDreamToolFlags[i] % 1000;
        host_has[i] = (host.groups[BbscDreamToolGroupIndex][bit / 8] & (0x80u >> (bit % 8))) != 0;
        had[i] = BbscEventFlag(BbscDreamToolFlags[i]);
        if (host_has[i] && !had[i]) {
            using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
            BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, BbscDreamToolFlags[i], 1);
        }
    }
    if (!bbsc_dream_tools_logged) {
        bbsc_dream_tools_logged = true;
        LOG_INFO(Debug,
                 "BBSC Dream: workbench tools - host has blood gems {} / Arcane Haze {}, guest had "
                 "{} / {}{}",
                 host_has[0], host_has[1], had[0], had[1],
                 (host_has[0] && !had[0]) || (host_has[1] && !had[1]) ? " -> host's given" : "");
    }
}

void PS4_SYSV_ABI BbscDreamTalkHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    // RCX = the block's talk records (0x48 bytes each), R13 = 9 * record index (see 0x0204C678):
    // +0x10 condition evaluator, +0x18 command executor, +0x20 talk id.
    const u64 record = registers->rcx + registers->r13 * 8;
    if (!ReadablePointer(record, 0x28)) {
        return;
    }
    const u64 condition = ReadValue<u64>(record, 0x10);
    if (!ReadablePointer(condition, 0x30) || ReadValue<u8>(condition, 0x25) == 0 ||
        ReadValue<u64>(condition, 0) != image_base + TalkConditionVtableOffset) {
        return; // host, solo, or no evaluator: nothing was refused
    }
    const u32 talk = ReadValue<u32>(condition, 0x10);
    size_t index = 0;
    while (index < BbscDreamGuestTalks.size() && BbscDreamGuestTalks[index].id != talk) {
        ++index;
    }
    if (index == BbscDreamGuestTalks.size()) {
        // World NPCs (2026-10-08, testers: "NPCs are ghosts and don't give the guest quest items").
        // Every talk script refuses a phantom the same way; outside the Dream the guest may talk to
        // every NPC script. Talk ids are AABnnn (map AA block B): nnn 000-099 are the lamps and
        // other devices (t240000-t240003 "warp chair"), 100+ the NPCs and windows. The Dream (21)
        // keeps its own list above, the Chalice maps (29) and m10 stay closed. The scripts read the
        // guest's OWN story flags (global groups are never synced), so each player's quests go on
        // as their own, and an item an NPC gives goes into the guest's inventory.
        const u32 map_area = talk / 10'000;
        if (map_area == 21 || map_area == 29 || map_area == 10 || talk % 1000 < 100 ||
            !BbscGuestInHostWorld()) {
            return;
        }
        WriteValue<u8>(condition, 0x25, 0);
        static std::vector<u32> logged;
        if (std::ranges::find(logged, talk) == logged.end() && logged.size() < 256) {
            logged.push_back(talk);
            LOG_INFO(Debug, "BBSC NPC talk: guest may talk to t{} (map m{}_{:02})", talk,
                     map_area, (talk / 1000) % 10);
        }
        return;
    }
    if (!BbscGuestInHostWorld()) {
        return;
    }
    if (talk == GehrmanTalkId && BbscNightIsOver()) {
        return;
    }
    WriteValue<u8>(condition, 0x25, 0);
    if (talk == BbscWorkbenchTalkId) {
        BbscGiveHostWorkshopTools();
    }
    if ((bbsc_dream_talk_logged & (1u << index)) == 0) {
        bbsc_dream_talk_logged |= 1u << index;
        LOG_INFO(Debug, "BBSC Dream: guest may use t{} ({})", talk,
                 BbscDreamGuestTalks[index].what);
    }
}

// Event id of an event instance, like BbscRunningEventId but with one access check for the whole
// event table: the character type check runs for every such condition of every event, each frame
// while a condition waits.
s64 BbscEventIdOf(u64 instance) {
    const u64 ref = ReadValue<u64>(instance, 0xA8);
    const u64 instruction = ReadValue<u64>(instance, 0xB0);
    if (!ReadablePointer(ref, 0x10)) {
        return -1;
    }
    const u64 file = ReadValue<u64>(ref, 0x08);
    if (!ReadablePointer(file, 0x90)) {
        return -1;
    }
    const s64 count = ReadValue<s64>(file, 0x10);
    const u64 events = file + ReadValue<u64>(file, 0x18);
    const u64 instructions = file + ReadValue<u64>(file, 0x28);
    if (count <= 0 || count > 100'000 || instruction < instructions ||
        !HasMemoryAccess(events, static_cast<size_t>(count) * 0x30, MemoryProt::CpuRead)) {
        return -1;
    }
    const u64 index = (instruction - instructions) / 0x20;
    for (s64 k = 0; k < count; ++k) {
        std::array<u64, 3> event{}; // id, instruction count, first instruction offset
        std::memcpy(event.data(),
                    reinterpret_cast<const void*>(events + static_cast<u64>(k) * 0x30),
                    sizeof(event));
        if (index >= event[2] / 0x20 && index < event[2] / 0x20 + event[1]) {
            return static_cast<s64>(event[0]);
        }
    }
    return -1;
}

// ---- Boss drops for the guest (2026-10-06) -------------------------------------------------------
// Report: after Cleric Beast the guest got the echoes but not the Saw Hunter Badge. Every main boss's
// reward event (m24_01 12411700 for that one) waits on 4[03](MAIN, 10000, type 0) - "the local
// player is the host" - before it awards the item lot (2003[04]), so a guest's copy of the event
// never gets there (read off the files with tools/gamedata/emevd.py; the award function 0x017DDC50
// itself has no guest gate for that call). The rest of the event after the wait sets the boss's
// story flags and the trophy, which must stay out of the guest's own progress, so the wait is NOT
// answered "yes" here. (Test 2026-10-07: a guest's copy never gets to that wait at all - it jumps
// on the multiplayer state to a type 1 wait first; the hook takes either, see the type check hook.)
// Instead, when a guest's copy of one of these events reaches the wait, BBSC
// gives the guest the lot the event would have given, as the event chooses it: the first-kill lot,
// or the repeat lot when the guest's own flag says the item was already taken (1003[01] skip).
// Table generated from every event file: {event, flag (0 none), first lot, repeat lot (0 none)}.
struct BbscBossReward {
    s64 event;
    u32 flag;
    s32 first_lot;
    s32 repeat_lot;
};
constexpr std::array<BbscBossReward, 17> BbscBossRewards{{
    {12101800, 6642, 15000, 15005},       // m21_00
    {12301800, 0, 80000000, 0},           // m23_00
    {12301700, 6644, 50800000, 50800005}, // m23_00
    {12401800, 0, 50000001, 0},           // m24_00
    {12411700, 6645, 50000010, 0},        // m24_01 (Cleric Beast: Saw Hunter Badge)
    {12411800, 0, 31000, 0},              // m24_01
    {12421800, 0, 80000300, 0},           // m24_02
    {12421700, 6332, 25700000, 25700005}, // m24_02
    {12601800, 0, 55100000, 0},           // m26_00
    {12701800, 6321, 2700990, 2700995},   // m27_00
    {12801800, 0, 50700000, 0},           // m28_00
    {13201800, 0, 51001900, 0},           // m32_00
    {13301800, 0, 80000200, 0},           // m33_00
    {13401800, 6674, 3401800, 3401802},   // m34_00
    {13401850, 6673, 3401850, 3401852},   // m34_00
    {13501850, 0, 3501850, 0},            // m35_00
    {13601800, 0, 3601800, 0},            // m36_00
}};
constexpr u64 ItemManPointerOffset = 0x0553D6E0;
constexpr u64 AwardItemLotOffset = 0x017DDC50; // (item man, lot, 0) as the 2003[04] handler calls it
std::mutex bbsc_boss_reward_mutex;
std::vector<s64> bbsc_boss_reward_queue; // events whose reward is due (filled in the hook)
std::vector<s64> bbsc_boss_reward_given; // events already rewarded in this game run

void NoteBbscBossRewardEvent(s64 event) {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_GUEST_ITEMS");
    if (!enabled || std::ranges::none_of(BbscBossRewards,
                                         [event](const auto& r) { return r.event == event; })) {
        return;
    }
    std::scoped_lock lock{bbsc_boss_reward_mutex};
    if (std::ranges::find(bbsc_boss_reward_given, event) == bbsc_boss_reward_given.end() &&
        std::ranges::find(bbsc_boss_reward_queue, event) == bbsc_boss_reward_queue.end()) {
        bbsc_boss_reward_queue.push_back(event);
        LOG_INFO(Debug, "BBSC boss drop: event {} reached its player-type wait on the guest - "
                        "reward queued",
                 event);
    }
}

// 2026-10-11 squad test: the item popup of a lot given through 0x017DDC50 stayed on the guest's
// screen ("Sword Hunter Badge", nothing closed it). Lots that hold one goods item (badges, keys,
// cocktails) are now put straight into the carried items, as the kit's items are (no popup; BBSC's
// own notice instead), and the lot's get flag is set as the award would. Read off ItemLotParam
// (tools/gamedata/param.py): {lot, goods id, count, get flag (0 none)}. Other lots (weapons, gems)
// still go through the award.
struct BbscBossGoodsLot {
    s32 lot;
    u32 goods;
    s32 count;
    u32 flag;
};
constexpr std::array<BbscBossGoodsLot, 18> BbscBossGoodsLots{{
    {15000, 4113, 1, 0},           {15005, 1400, 1, 0},     {80000000, 6109, 1, 5000},
    {50800000, 4118, 1, 0},        {50800005, 1400, 1, 0},  {50000001, 4001, 1, 0},
    {50000010, 4114, 1, 0},        {31000, 4000, 1, 0},     {80000300, 6539, 1, 5009},
    {25700005, 1500, 1, 0},        {55100000, 4323, 1, 50000305}, {2700995, 1500, 1, 52700955},
    {50700000, 7131, 3, 0},        {51001900, 1591, 1, 0},  {80000200, 6429, 1, 5006},
    {3401802, 1500, 1, 0},         {3401852, 1500, 1, 0},   {3501850, 4020, 1, 53501850},
}};
u64 BbscCarriedInventory();                   // see "Kit"
bool BbscInventoryHas(u64 inventory, u32 code); // see "Kit"

// True when the lot's goods are now carried.
bool GiveBbscBossGoods(const BbscBossGoodsLot& lot) {
    const u64 carried = BbscCarriedInventory();
    if (carried == 0) {
        return false;
    }
    using Fits = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* code, s32 count);
    using Add = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* key, s32 count);
    alignas(16) static u32 code = 0;
    alignas(16) static u32 key = 0;
    code = 0x40000000u | lot.goods;
    key = 0x80000000u | (3u << 28) | lot.goods;
    const s32 fits = BbGuestCall(reinterpret_cast<Fits>(image_base + 0x014D9A80), carried, &code, lot.count);
    const s32 slot =
        fits > 0 ? BbGuestCall(reinterpret_cast<Add>(image_base + 0x014DA0A0), carried, &key, lot.count) : -1;
    // Added for real (review 2026-10-11): "carried now" alone is also true when the guest had one
    // already (Blood Stone Chunks) and the add failed - then the award below gives it instead.
    const bool there = slot >= 0 && BbscInventoryHas(carried, code);
    if (there && lot.flag != 0) {
        using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
        BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, lot.flag, 1);
    }
    LOG_INFO(Debug, "BBSC boss drop: goods {} x{} put into the carried items (fits {}, slot {}, there "
                    "now: {}, get flag {})",
             lot.goods, lot.count, fits, slot, there, lot.flag);
    return there;
}

// Game tick: award the queued boss lots, once each.
void TickBbscBossRewards() {
    std::vector<s64> due;
    {
        std::scoped_lock lock{bbsc_boss_reward_mutex};
        due.swap(bbsc_boss_reward_queue);
    }
    for (const s64 event : due) {
        const auto it = std::ranges::find_if(BbscBossRewards,
                                             [event](const auto& r) { return r.event == event; });
        const u64 item_man = ReadValue<u64>(image_base + ItemManPointerOffset, 0);
        if (it == BbscBossRewards.end() || !ReadablePointer(item_man, 0x10) ||
            ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
            continue;
        }
        const bool taken = it->flag != 0 && BbscEventFlag(it->flag);
        const s32 lot = taken ? it->repeat_lot : it->first_lot;
        {
            std::scoped_lock lock{bbsc_boss_reward_mutex};
            bbsc_boss_reward_given.push_back(event);
        }
        if (lot == 0) {
            LOG_INFO(Debug, "BBSC boss drop: event {} - the guest already has it (flag {}), nothing "
                            "to give",
                     event, it->flag);
            continue;
        }
        const auto goods = std::ranges::find_if(BbscBossGoodsLots,
                                                [lot](const auto& g) { return g.lot == lot; });
        if (goods != BbscBossGoodsLots.end() && GiveBbscBossGoods(*goods)) {
            BbscAnnounce(u"Boss reward added to your items.", "boss reward");
            LOG_INFO(Debug, "BBSC boss drop: event {} - item lot {} given to the guest directly ({})",
                     event, lot, taken ? "repeat lot, flag already set" : "first kill");
            continue;
        }
        using AwardItemLot = void PS4_SYSV_ABI (*)(u64 item_man, s32 lot, u32 forced);
        BbGuestCall(reinterpret_cast<AwardItemLot>(image_base + AwardItemLotOffset), item_man, lot, 0);
        LOG_INFO(Debug, "BBSC boss drop: event {} - item lot {} given to the guest ({})", event, lot,
                 taken ? "repeat lot, flag already set" : "first kill");
    }
}



// Reliable packets waiting for a member that does not take them (see SendBacklog.Limit in
// bbsc_patch_tables.h): [SprjSessionManager]+0x188 is a list head, each node +0x10 -> queue, +0x30
// its packet count; +0x1B0 one more count. Logged while it grows past 100 (at most every 30 s).
void TickBbscSendBacklog() {
    static s64 last_ms = 0;
    static s64 logged_ms = 0;
    static s32 logged_total = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 2'000) {
        return;
    }
    last_ms = now;
    const u64 manager = ReadValue<u64>(image_base + 0x05540290, 0);
    if (!ReadablePointer(manager, 0x1B8)) {
        return;
    }
    const u64 head = ReadValue<u64>(manager, 0x188);
    if (!ReadablePointer(head, 0x18)) {
        return;
    }
    s32 total = ReadValue<s32>(manager, 0x1B0);
    u64 node = ReadValue<u64>(head, 0);
    for (int i = 0; i < 64 && node != head && ReadablePointer(node, 0x18); ++i) {
        const u64 queue = ReadValue<u64>(node, 0x10);
        if (ReadablePointer(queue, 0x34)) {
            total += ReadValue<s32>(queue, 0x30);
        }
        node = ReadValue<u64>(node, 0);
    }
    if (total < 100) {
        logged_total = 0;
        return;
    }
    if (total > logged_total && now - logged_ms >= 30'000) {
        logged_ms = now;
        logged_total = total;
        LOG_WARNING(Debug,
                    "BBSC send backlog: {} reliable packets waiting for a member that does not "
                    "answer (session state {}; vanilla ends the session at 1001, BBSC at 30000)",
                    total, GetSessionState());
    }
}


// ---- Starter weapons in the Dream, both directions (2026-10-08) ----------------------------------
// Test 2026-10-08 (host, cls_amg guest, both on mod 35): "if I take the weapons in the Dream
// they disappear for him, and if he takes them they disappear for me". The messengers' free shop
// (c1_84, ShopLineupParam 2000-2011) counts its sales in flags 72101000-72101002 (trick weapons)
// and 72101010/72101011 (firearms), group 72101. BBSC no longer copies the host's group into the
// guest (mod 35), and the flag write 0x013CFCC0 sends nothing - so the change crosses over some
// other way (not found yet). Both players now keep group 72101 as their own while in the Dream in
// a session: a change made while our own shop dialog runs (0x01729D40, the dialog's update that
// writes the sales flags at 0x0172A1FB) is our purchase and kept; any other change is put back and
// logged ("BBSC Dream shop: ... from the other player"), so the next log also shows how it came.
// OFF since mod 38: test 2026-10-08 showed the dialog hook never runs for this shop, so the guard
// undid the guest's OWN purchase; the weapon messengers are no longer the guest's at all.
constexpr u64 ShopDialogUpdateOffset = 0x01729D40;
constexpr u32 BbscStarterWeaponGroup = 72101;
std::atomic<s64> bbsc_shop_dialog_ms{0};

void PS4_SYSV_ABI BbscShopDialogHook(u64 /*tag*/, const GuestRegisterSnapshot* /*registers*/) {
    bbsc_shop_dialog_ms.store(SteadyNowMs(), std::memory_order_relaxed);
}

void TickBbscDreamShopGuard() {
    static s64 last_ms = 0;
    static bool have = false;
    static std::array<u8, BbscWorldGroupBytes> kept{};
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 200) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    if ((state != 3 && state != 6) || !ReadablePointer(global, 0x10) ||
        ReadValue<u8>(global, 0x08) != 0 || GetCurrentPackedMap() != BbscDreamStateMap) {
        have = false;
        return;
    }
    const u64 storage = BbscFlagGroupStorage(BbscStarterWeaponGroup);
    if (storage == 0) {
        have = false;
        return;
    }
    auto* live = reinterpret_cast<u8*>(storage);
    if (!have) {
        std::memcpy(kept.data(), live, BbscWorldGroupBytes);
        have = true;
        return;
    }
    if (std::memcmp(kept.data(), live, BbscWorldGroupBytes) == 0) {
        return;
    }
    const bool own = now - bbsc_shop_dialog_ms.load(std::memory_order_relaxed) < 1'500;
    size_t bits = 0;
    for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
        bits += std::popcount(static_cast<u8>(kept[b] ^ live[b]));
    }
    if (own) {
        std::memcpy(kept.data(), live, BbscWorldGroupBytes);
        LOG_INFO(Debug, "BBSC Dream shop: our own purchase ({} sales flag(s) of group 72101 changed, "
                        "session state {}) - kept",
                 bits, state);
    } else {
        std::memcpy(live, kept.data(), BbscWorldGroupBytes);
        LOG_WARNING(Debug, "BBSC Dream shop: {} sales flag(s) of group 72101 changed with our shop "
                           "closed - from the other player, put back (session state {})",
                    bits, state);
    }
}

// ---- Enemy drops for the guest (2026-10-08) ------------------------------------------------------
// Testers: "Wandering Madness [Nightmares] don't drop items for the guest". No enemy dropped anything
// for a guest: the drop is rolled in the game that owns the enemy - the host's - and the guest's game
// only sees it die. The kill reward 0x018C5070 does run in the guest's game for every death it sees
// (traced as "BBSC echo path: kill reward entry"); there BBSC reads the dead enemy's NpcParam row
// (chr->vtable[0x168](chr) + 8, the same lookup 0x018C5070 uses for getSoul at +0x28) and gives the
// guest its own roll of the enemy's item lot (itemLotId_1/_2 at +0x2C/+0x30) through the item lot
// award 0x017DDC50, as the boss items. Bosses (isSoulGetByBoss) are left to their reward events.
std::mutex bbsc_guest_drop_mutex;
std::vector<std::pair<s32, u32>> bbsc_guest_drop_queue; // (item lot, NpcParam row id or 0)

void NoteBbscGuestEnemyDeath(u64 dead) {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_GUEST_ITEMS");
    if (!enabled || image_base == 0 || !BbscGuestInHostWorld() || GetLocalPlayerType() != 1 ||
        !ReadablePointer(dead, 0x1E8) || dead == GetLocalPlayerCharacter() ||
        (ReadValue<u32>(dead, 0x1E1) & 0x240000) != 0) {
        return;
    }
    const u64 vtable = ReadablePointer(dead, 0x08) ? ReadValue<u64>(dead, 0) : 0;
    const u64 info_fn = ReadablePointer(vtable, 0x170) ? ReadValue<u64>(vtable, 0x168) : 0;
    if (!IsEbootCodeAddress(info_fn)) {
        return;
    }
    const u64 info = BbGuestCall(reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(info_fn), dead);
    const u64 row = ReadablePointer(info, 0x10) ? ReadValue<u64>(info, 8) : 0;
    if (!ReadablePointer(row, 0x150) || ((ReadValue<u8>(row, 329) >> 4) & 1) != 0) {
        return; // no NpcParam row (a player), or a boss
    }
    std::scoped_lock lock{bbsc_guest_drop_mutex};
    for (size_t i = 0; i < 2; ++i) {
        const s32 lot = ReadValue<s32>(row, 0x2C + i * 4);
        if (lot > 0 && bbsc_guest_drop_queue.size() < 64) {
            bbsc_guest_drop_queue.emplace_back(lot, ReadValue<u32>(row, 0x08));
        }
    }
}

void TickBbscGuestDrops() {
    std::vector<std::pair<s32, u32>> due;
    {
        std::scoped_lock lock{bbsc_guest_drop_mutex};
        due.swap(bbsc_guest_drop_queue);
    }
    if (due.empty()) {
        return;
    }
    const u64 item_man = ReadValue<u64>(image_base + ItemManPointerOffset, 0);
    const u64 global = GetGlobalState();
    if (!ReadablePointer(item_man, 0x10) || !BbscGuestInHostWorld() ||
        !ReadablePointer(global, 0x10) || ReadValue<u8>(global, 0x08) != 0 ||
        GetCharacterHp(GetLocalPlayerCharacter()) <= 0) {
        return; // dropped: no item manager, no longer a guest, loading or dead
    }
    using AwardItemLot = void PS4_SYSV_ABI (*)(u64 item_man, s32 lot, u32 forced);
    for (const auto& [lot, name] : due) {
        BbGuestCall(reinterpret_cast<AwardItemLot>(image_base + AwardItemLotOffset), item_man, lot, 0);
        LOG_INFO(Debug, "BBSC guest drop: enemy (name id {}) died - the guest's own roll of item "
                        "lot {}",
                 name, lot);
    }
}

// ---- Shared progress, merge mode (2026-10-07) ----------------------------------------------------
// The user: the guest's own world takes over everything the host has done, except NPC quests; a
// merge only ever ADDS (a boss dead for either player is dead for both, an open shortcut or a lit
// lamp stays open / lit), never removes, and the character (stats, items, equipment, echoes,
// Insight) is never touched.
//   * Bosses: the global "boss defeated" flags - every flag a boss defeat event sets below 100000
//     (2003[02] next to a 945x counter, all event files; tools/gamedata/emevd.py). The night-phase
//     flags 999 / 1000..1440 / 1082 are left out: only one of them is on at a time, an OR would set
//     two. The host sends the ones it has on (relay "SHAD" 0x26) every 5 s while hosting.
//   * The world: kind 1 groups (1AABxxxx: the map block's events - bosses, doors, shortcuts, lamps
//     lit, cutscenes seen) of every block the host sent during the session (the world snapshots of
//     "World state when warping together"). Kinds 5 (item pickups), 6 and 7 (NPC and object states)
//     are not merged.
// While the guest plays, BBSC only collects. Once the guest is home again (own world, not loading,
// alive) the saves are backed up (BackupBbscSaves) and the collected flags are OR-ed into the live
// flag table, which the game then saves as usual. The Hunter's Dream groups wait until BBSC has put
// the guest's own Dream state back. Nothing is kept on disk: quitting the game while still a guest
// skips the merge (it runs on the next trip home with the same host's data only in the same run).
// Switch SHADPS4_BBSC_SHARED_PROGRESS (Mods tab, on).
constexpr std::array<u32, 49> BbscProgressFlags{
    2200, 2300, 2301, 2400, 2401, 2402, 2411, 2412, 2420, 2421, 2500, 2600, 2601,
    2700, 2701, 2800, 3200, 3300, 3400, 3401, 3402, 3403, 3510, 3511, 3512, 3517,
    3600, 5910, 5911, 5912, 5913, 5914, 9180, 9450, 9451, 9452, 9453, 9454, 9455,
    9456, 9457, 9458, 9459, 9460, 9461, 9462, 9463, 9464, 9465,
};
constexpr std::array<u32, 5> BbscProgressFlagsMore{9466, 9467, 9468, 9469, 9471};
constexpr s64 BbscProgressSendMs = 5'000;
constexpr s64 BbscProgressSettleMs = 4'000;    // home, loaded and alive this long before merging
constexpr s64 BbscProgressHomeLimitMs = 180'000; // a session that ended longer ago is dropped

bool BbscSharedProgressEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_SHARED_PROGRESS");
    return enabled;
}

bool BackupBbscSaves(); // see "The BBSC hunter's kit"

std::mutex bbsc_progress_mutex;
std::vector<u32> bbsc_progress_host_flags;                         // guest: host's boss flags seen
std::map<u32, std::array<u8, BbscWorldGroupBytes>> bbsc_progress_groups; // guest: kind 1, OR-ed
bool bbsc_progress_was_guest = false;
s64 bbsc_progress_left_ms = 0;  // when we stopped being a guest
s64 bbsc_progress_home_ms = 0;  // home and settled since
bool bbsc_progress_merged_at_load = false; // merged during the trip home (checked once settled)
bool bbsc_progress_backed_up = false;      // saves backed up for this trip home

// OR the collected host progress into the live flag table (own world). Returns the boss flags and
// world flags it newly set, or {-1, 0} when there was nothing to merge. Keeps what it merged, so a
// second call only reports what did not stick. Game thread.
std::pair<s64, size_t> MergeBbscSharedProgress(std::string_view when) {
    std::vector<u32> flags;
    std::map<u32, std::array<u8, BbscWorldGroupBytes>> groups;
    {
        std::scoped_lock lock{bbsc_progress_mutex};
        flags = bbsc_progress_host_flags;
        groups = bbsc_progress_groups;
    }
    if (flags.empty() && groups.empty()) {
        return {-1, 0};
    }
    if (!bbsc_progress_backed_up) {
        bbsc_progress_backed_up = BackupBbscSaves();
    }
    using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
    const auto set = reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0);
    s64 bosses = 0;
    for (const u32 flag : flags) {
        if (!BbscEventFlag(flag)) {
            BbGuestCall(set, 0, flag, 1);
            ++bosses;
        }
    }
    size_t group_count = 0;
    size_t bits = 0;
    for (const auto& [key, bytes] : groups) {
        const u64 storage = BbscFlagGroupStorage(key);
        if (storage == 0) {
            continue;
        }
        auto* live = reinterpret_cast<u8*>(storage);
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            bits += std::popcount(static_cast<u8>(bytes[b] & ~live[b]));
            live[b] = static_cast<u8>(live[b] | bytes[b]);
        }
        ++group_count;
    }
    LOG_INFO(Debug,
             "BBSC shared progress ({}): merged into our own world - {} boss flag(s) newly set (of "
             "{} the host has), {} world flag(s) newly set in {} group(s); saves backed up first: {}",
             when, bosses, flags.size(), bits, group_count, bbsc_progress_backed_up);
    return {bosses, bits};
}

template <size_t N>
void ForEachProgressFlag(const std::array<u32, N>& list, auto&& fn) {
    for (const u32 flag : list) {
        fn(flag);
    }
}

// Host: tell the guests which boss flags we have on.
void TickBbscProgressBroadcast() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (!BbscSharedProgressEnabled() || image_base == 0 || now - last_ms < BbscProgressSendMs ||
        GetSessionState() != 3 || ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
        return;
    }
    last_ms = now;
    std::vector<u8> packet{'S', 'H', 'A', 'D', 0x26, 0};
    const auto add = [&packet](u32 flag) {
        if (BbscEventFlag(flag)) {
            const size_t at = packet.size();
            packet.resize(at + 4);
            std::memcpy(packet.data() + at, &flag, 4);
            ++packet[5];
        }
    };
    ForEachProgressFlag(BbscProgressFlags, add);
    ForEachProgressFlag(BbscProgressFlagsMore, add);
    Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(
        packet.data(), static_cast<u32>(packet.size()));
}

// ---- Shared progress on disk (2026-10-07) ---------------------------------------------------------
// The user: "can we make the shared progress realtime, crashes will be deadly this way". What the
// host did lived only in memory until the trip home, so a crash or a quit lost it. Writing straight
// into the guest's own world is not possible while it is away: its save takes the event flags from
// the LIVE table (0x01399DE0 writes SprjEventFlagMan +0x28, no own-world copy), so the collected
// progress goes to a file instead, every BbscProgressFileMs while a guest, and is merged as before
// once home. When the game ends before that, the file stays, and the next time that character is
// home (in any later run) it is merged then. The character is the save slot the game writes: the
// newest userdata00NN (0..9) of the user's CUSA00207\SPRJ0005 folder, written during this run.
constexpr s64 BbscProgressFileMs = 10'000;
constexpr s64 BbscProgressRecoverMs = 15'000;
constexpr u32 BbscProgressFileMagic = 0x50534242; // "BBSP"

std::optional<BbscSaveSlot> bbsc_progress_slot; // the character the collected progress is for

std::filesystem::path BbscPendingProgressPath(const BbscSaveSlot& slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::CapturesDir).parent_path() /
           "shared-progress" / fmt::format("pending-{}-{}.bin", slot.user, slot.slot);
}

// Memory -> file (the whole collection, rewritten). Game thread.
void WriteBbscPendingProgress() {
    if (!bbsc_progress_slot.has_value()) {
        return;
    }
    std::vector<u8> out;
    const auto put32 = [&out](u32 v) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    };
    {
        std::scoped_lock lock{bbsc_progress_mutex};
        if (bbsc_progress_host_flags.empty() && bbsc_progress_groups.empty()) {
            return;
        }
        put32(BbscProgressFileMagic);
        put32(static_cast<u32>(BbscWorldGroupBytes));
        put32(static_cast<u32>(bbsc_progress_host_flags.size()));
        for (const u32 flag : bbsc_progress_host_flags) {
            put32(flag);
        }
        put32(static_cast<u32>(bbsc_progress_groups.size()));
        for (const auto& [key, bytes] : bbsc_progress_groups) {
            put32(key);
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
    }
    std::error_code ec;
    const auto path = BbscPendingProgressPath(*bbsc_progress_slot);
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temp, path, ec); // replaces the previous one in one step
}

// File -> memory, OR-ed into whatever is collected already. True when a file was read.
bool LoadBbscPendingProgress(const BbscSaveSlot& slot) {
    std::ifstream file(BbscPendingProgressPath(slot), std::ios::binary);
    if (!file) {
        return false;
    }
    const auto get32 = [&file](u32& v) {
        return static_cast<bool>(file.read(reinterpret_cast<char*>(&v), 4));
    };
    u32 magic = 0;
    u32 group_bytes = 0;
    u32 count = 0;
    if (!get32(magic) || magic != BbscProgressFileMagic || !get32(group_bytes) ||
        group_bytes != BbscWorldGroupBytes || !get32(count) || count > 10'000) {
        return false;
    }
    std::vector<u32> flags(count);
    for (u32& flag : flags) {
        if (!get32(flag)) {
            return false;
        }
    }
    if (!get32(count) || count > 10'000) {
        return false;
    }
    std::map<u32, std::array<u8, BbscWorldGroupBytes>> groups;
    for (u32 i = 0; i < count; ++i) {
        u32 key = 0;
        std::array<u8, BbscWorldGroupBytes> bytes{};
        if (!get32(key) || !file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            return false;
        }
        groups[key] = bytes;
    }
    std::scoped_lock lock{bbsc_progress_mutex};
    for (const u32 flag : flags) {
        if (std::ranges::find(bbsc_progress_host_flags, flag) == bbsc_progress_host_flags.end()) {
            bbsc_progress_host_flags.push_back(flag);
        }
    }
    for (const auto& [key, bytes] : groups) {
        auto& merged = bbsc_progress_groups[key];
        for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
            merged[b] |= bytes[b];
        }
    }
    LOG_INFO(Debug, "BBSC shared progress: {} boss flag(s) and {} world group(s) from the file of "
                    "user {} slot {} (an earlier session that did not get home)",
             flags.size(), groups.size(), slot.user, slot.slot);
    return true;
}

// After a merge: the file is kept as ".merged" (the last one only), not deleted.
void FinishBbscPendingProgress() {
    if (!bbsc_progress_slot.has_value()) {
        return;
    }
    std::error_code ec;
    const auto path = BbscPendingProgressPath(*bbsc_progress_slot);
    if (std::filesystem::exists(path, ec)) {
        std::filesystem::rename(path, std::filesystem::path(path).concat(".merged"), ec);
    }
}

// Guest: collect while in the session; merge once home.
void TickBbscSharedProgress() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (!BbscSharedProgressEnabled() || image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    const bool own_world = ReadablePointer(global, 0x1598) && ReadValue<u8>(global, 0x1592) != 0;
    static s64 file_written_ms = 0;
    static s64 recover_checked_ms = 0;
    // Test 2026-10-08 (cls_amg invaded a tester's world): an invader is in session state 6 too,
    // and went home with the host's progress merged into its world. Only a co-op guest (player
    // type 1) collects; an invader (2) or a character not built yet (-1) does not.
    if (state == 6 && GetLocalPlayerType() != 1) {
        static bool logged = false;
        if (!logged && GetLocalPlayerType() == 2) {
            logged = true;
            LOG_INFO(Debug, "BBSC shared progress: we are an invader here - nothing is collected");
        }
        return;
    }
    if (state == 6) {
        // Which character this is, and what an earlier session left for it (once per session).
        if (!bbsc_progress_slot.has_value()) {
            bbsc_progress_slot = BbscActiveSaveSlot();
            if (bbsc_progress_slot.has_value()) {
                LoadBbscPendingProgress(*bbsc_progress_slot);
            }
        }
        if (now - file_written_ms >= BbscProgressFileMs) {
            file_written_ms = now;
            WriteBbscPendingProgress();
        }
        bbsc_progress_was_guest = true;
        bbsc_progress_left_ms = 0;
        bbsc_progress_home_ms = 0;
        bbsc_progress_merged_at_load = false;
        bbsc_progress_backed_up = false;
        // Kind 1 groups (indices 0..9) of every block snapshot the host sent.
        std::vector<BbscWorldSnapshot> blocks;
        {
            std::scoped_lock lock{bbsc_world_mutex};
            for (const auto& [map, snap] : bbsc_world_blocks) {
                if (snap.complete_ms != 0) {
                    blocks.push_back(snap);
                }
            }
        }
        std::scoped_lock lock{bbsc_progress_mutex};
        for (const BbscWorldSnapshot& snap : blocks) {
            // Not the Hunter's Dream (report 2026-10-07: "guest does not have the starter items in
            // the Hunter's Dream"): its groups hold the messengers' starter weapons, the Doll and
            // the workshop - the guest's own story. The host's Dream is only shown while the guest
            // is in it (ApplyBbscHostDreamState) and put back after (RestoreBbscOwnDreamState).
            if (snap.map == BbscDreamStateMap) {
                continue;
            }
            for (size_t i = 0; i < 10; ++i) {
                if ((snap.present & (u64{1} << i)) == 0) {
                    continue;
                }
                auto& merged = bbsc_progress_groups[BbscWorldGroupKey(snap.map, i)];
                for (size_t b = 0; b < BbscWorldGroupBytes; ++b) {
                    merged[b] |= snap.groups[i][b];
                }
            }
        }
        return;
    }
    if (!bbsc_progress_was_guest) {
        // A file an earlier session left (crash, quit while a guest): merged once this character
        // is home and settled, the same way as after a session.
        const u64 me = GetLocalPlayerCharacter();
        if (own_world && me != 0 && GetCharacterHp(me) > 0 && ReadValue<u8>(global, 0x08) == 0 &&
            now - recover_checked_ms >= BbscProgressRecoverMs) {
            recover_checked_ms = now;
            if (const auto slot = BbscActiveSaveSlot(); slot.has_value()) {
                RecoverBbscOwnWorldFile(*slot); // our own world back before the host's is added
                if (LoadBbscPendingProgress(*slot)) {
                    bbsc_progress_slot = slot;
                    bbsc_progress_was_guest = true;
                    bbsc_progress_left_ms = now;
                    bbsc_progress_home_ms = 0;
                    bbsc_progress_backed_up = false;
                }
            }
        }
        return;
    }
    if (bbsc_progress_left_ms == 0) {
        bbsc_progress_left_ms = now;
        WriteBbscPendingProgress(); // the session just ended: the newest state to disk
    }
    if (now - bbsc_progress_left_ms > BbscProgressHomeLimitMs) {
        std::scoped_lock lock{bbsc_progress_mutex};
        LOG_WARNING(Debug, "BBSC shared progress: not home within {} s of the session end (quit to "
                           "the title?) - {} boss flag(s) and {} world group(s) kept in the file, "
                           "merged the next time this character is home",
                    BbscProgressHomeLimitMs / 1000, bbsc_progress_host_flags.size(),
                    bbsc_progress_groups.size());
        bbsc_progress_host_flags.clear();
        bbsc_progress_groups.clear();
        bbsc_progress_was_guest = false;
        bbsc_progress_slot.reset();
        return;
    }
    const u64 player = GetLocalPlayerCharacter();
    const bool ready = own_world && ReadValue<u8>(global, 0x08) == 0 && player != 0 &&
                       GetCharacterHp(player) > 0 && bbsc_dream_own.held == 0 &&
                       ReadValue<u64>(image_base + 0x0553B0D8, 0) != 0 &&
                       ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) != 0;
    if (!ready) {
        bbsc_progress_home_ms = 0;
        return;
    }
    if (bbsc_progress_home_ms == 0) {
        bbsc_progress_home_ms = now;
    }
    if (now - bbsc_progress_home_ms < BbscProgressSettleMs) {
        return;
    }
    bbsc_progress_was_guest = false;
    const bool at_load = bbsc_progress_merged_at_load;
    bbsc_progress_merged_at_load = false;
    RestoreBbscOwnWorld("before the shared progress merge");
    const auto [bosses, bits] = MergeBbscSharedProgress("after the home load");
    {
        std::scoped_lock lock{bbsc_progress_mutex};
        bbsc_progress_host_flags.clear();
        bbsc_progress_groups.clear();
    }
    FinishBbscPendingProgress(); // merged: the file is done
    bbsc_progress_slot.reset();
    if (bosses < 0) {
        LOG_INFO(Debug, "BBSC shared progress: home again, nothing from the host to merge");
        return;
    }
    if (at_load && bosses == 0 && bits == 0) {
        LOG_INFO(Debug, "BBSC shared progress: the merge made during the home load is all there");
        BbscAnnounce(u"Shared progress saved:\nthe host's progress is now in your world.",
                     "shared progress merged at the home load");
        return;
    }
    // Test 2026-10-07: merged into a map that was already running, Cleric Beast's bar came up
    // and doors stayed shut until the game was reloaded - the map's events had read the old flags.
    // They are saved all the same; the next load (a lamp, a death, the title) shows them.
    BbscAnnounce(u"Shared progress saved.\nRest at a lamp to see all of it.",
                 "shared progress merged after the load");
}

// Shared progress, during the trip home (2026-10-07): the respawn point of the load home is resolved
// before the map's objects and events start. Once that load is in our own world, the merge goes in
// there, so the map comes up with the host's progress already in it. The tick above checks again
// once we are settled (and merges whatever did not stick, should the own-world table come later).
void ApplyBbscSharedProgressAtLoad() {
    if (!BbscSharedProgressEnabled() || image_base == 0 || !bbsc_progress_was_guest ||
        bbsc_progress_merged_at_load) {
        return;
    }
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    if (state == 4 || state == 6 || !ReadablePointer(global, 0x1598) ||
        ReadValue<u8>(global, 0x1592) == 0 || bbsc_dream_own.held != 0 ||
        ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
        return; // not home yet, or our own Dream state still to be put back (the tick merges then)
    }
    RestoreBbscOwnWorldAtLoad(); // our own world first, then the host's progress on top
    const auto [bosses, bits] = MergeBbscSharedProgress("during the home load");
    bbsc_progress_merged_at_load = bosses >= 0;
}

u8 bbsc_dream_type_skip = 0;
// Condition objects compiled by an allowed event. The compile step (bank 4 handler 0x017B9C50)
// evaluates every new condition once as (condition, context, event instance) and then files it in
// the instance's condition groups; later re-checks come through the group (0x012EBCA0), which
// passes its own caller's third argument on. If that is not the instance, the object is recognised
// from its compile-time check instead. Game thread only.
std::array<u64, 64> bbsc_dream_type_conditions{};
size_t bbsc_dream_type_next = 0;
std::array<s64, 32> bbsc_dream_type_logged{};

void PS4_SYSV_ABI BbscDreamTypeCheckHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    const u64 condition = registers->rdi;
    if (!ReadablePointer(condition, 0x30) || ReadValue<s32>(condition, 0x28) != 10000 ||
        !BbscGuestInHostWorld()) {
        return;
    }
    // Test 2026-10-07: still no badge. Every boss reward event jumps on the multiplayer state
    // (1003[105] state 1, label 1) before its type 0 wait, so a guest's copy waits on type 1 at
    // label 1 instead and never reaches type 0 (all 17 events, tools/gamedata/emevd.py + the game's
    // own common.emedf names). The reward is queued from either wait.
    const s8 type = ReadValue<s8>(condition, 0x2C);
    if (type != 0 && type != 1) {
        return;
    }
    const s64 event = ReadablePointer(registers->rdx, 0xC0) ? BbscEventIdOf(registers->rdx) : -1;
    if (event > 0) {
        NoteBbscBossRewardEvent(event); // a boss reward event the guest's copy never finishes
    }
    if (type != 0) {
        return;
    }
    auto known = std::ranges::find(bbsc_dream_type_conditions, condition);
    bool allowed = known != bbsc_dream_type_conditions.end();
    if (event > 0) {
        allowed = std::ranges::find(BbscDreamGuestEvents, event) != BbscDreamGuestEvents.end();
        if (allowed && known == bbsc_dream_type_conditions.end()) {
            bbsc_dream_type_conditions[bbsc_dream_type_next] = condition;
            bbsc_dream_type_next = (bbsc_dream_type_next + 1) % bbsc_dream_type_conditions.size();
        } else if (!allowed && known != bbsc_dream_type_conditions.end()) {
            *known = 0; // that memory now holds another event's condition
        }
    }
    if (!allowed) {
        return;
    }
    WriteValue<u8>(condition, 0x10, static_cast<u8>(ReadValue<u8>(condition, 0x10) | 1));
    bbsc_dream_type_skip = 1;
    if (event > 0 && std::ranges::find(bbsc_dream_type_logged, event) ==
                         bbsc_dream_type_logged.end()) {
        if (const auto free = std::ranges::find(bbsc_dream_type_logged, 0);
            free != bbsc_dream_type_logged.end()) {
            *free = event;
        }
        LOG_INFO(Debug, "BBSC Dream: event {} runs for the guest as for the host", event);
    }
}

// The NpcParam row of id, 0 while the params are not loaded (0x01F226C0 panics without the
// repository and returns no row before its NpcParam is there).
u64 BbscNpcParamRow(u32 id) {
    const u64 repository = ReadValue<u64>(image_base + SoloParamRepositoryPointerOffset, 0);
    if (!ReadablePointer(repository, 0x230) || ReadValue<s32>(repository, 0x220) == 0) {
        return 0;
    }
    // [[[repository+0x228]+0x70]+0x70] is the param file; 0x01F226C0 panics on a version below 3.
    const u64 resource = ReadValue<u64>(repository, 0x228);
    const u64 holder = ReadablePointer(resource, 0x78) ? ReadValue<u64>(resource, 0x70) : 0;
    const u64 file = ReadablePointer(holder, 0x78) ? ReadValue<u64>(holder, 0x70) : 0;
    if (!ReadablePointer(file, 0x40) || ReadValue<u16>(file, 8) < 3) {
        return 0;
    }
    alignas(16) std::array<u32, 4> found{};
    using NpcParamRow = void PS4_SYSV_ABI (*)(u32* out, u32 id);
    BbGuestCall(reinterpret_cast<NpcParamRow>(image_base + NpcParamRowOffset), found.data(), id);
    u64 row = 0;
    std::memcpy(&row, &found[2], sizeof(row));
    return row;
}

// See 3. above: the Doll and Gehrman keep their own look for a guest. True once done.
bool MakeBbscDreamNpcsSolid() {
    size_t done = 0;
    for (const u32 id : BbscDreamSolidNpcs) {
        const u64 row = BbscNpcParamRow(id);
        if (row == 0 || !HasMemoryAccess(row + NpcParamFlagsOffset, 1, MemoryProt::CpuWrite)) {
            continue;
        }
        const u8 flags = ReadValue<u8>(row, NpcParamFlagsOffset);
        if ((flags & NpcParamWanderGhostBit) != 0) {
            WriteValue<u8>(row, NpcParamFlagsOffset,
                           static_cast<u8>(flags & ~NpcParamWanderGhostBit));
        }
        ++done;
    }
    if (done == BbscDreamSolidNpcs.size()) {
        LOG_INFO(Debug, "BBSC Dream: the Doll and Gehrman keep their own look for a guest "
                        "(NpcParam 803000/804000 wander-ghost bit cleared)");
        return true;
    }
    return false;
}

// Logs the guest entering and leaving the host's Dream.
// ---- Workshop tools in the host's Dream (2026-10-04) ---------------------------------------------
// Report 2026-10-01/04: at the Memory Altar the guest gets "The workshop tool required for this task
// is missing." (FMG 30 10011070). t210699 (the altar) opens only if f47(3, 4104, 0, 1, 0) == 1 -
// the LOCAL player holds key item 4104, the Rune Workshop Tool - and t210692 (the workbench) gates
// Blood Gem Fortification and Arcane Haze the same way on 4103 / 4102. In the host's Dream the
// guest should have what the host has. The host's tools are known on the guest's side: event
// 12100450 sets 6630 (4103), 6631 (4104), 6632 (4105), 6633 (4102) for the tools held, and group 6
// is part of the host's Dream state the guest receives (BbscDreamStateKeys).
// The check is ESD expression bytecode in the loaded talk script (read off the files with
// tools/gamedata): 6F 43 82 <item i32> 40 41 40 89 = f47(3, item, 0, 1, 0), third argument = the
// comparison (0 count == 1; 3 count <= 1). On the guest's machine only, while the guest is in the
// host's Dream and the host has that tool, the comparison byte is set to 3 (true with or without
// the tool); it goes back to 0 on leaving. The scripts are found by these bytes off the game
// thread and every write re-checks the bytes around it.
struct BbscToolCheck {
    u32 item;
    u32 host_flag;
    const char* what;
};
constexpr std::array<BbscToolCheck, 3> BbscDreamToolChecks{{
    {4104, 6631, "Memory Altar (Rune Workshop Tool)"},
    {4103, 6630, "workbench blood gems (Blood Gem Workshop Tool)"},
    {4102, 6633, "workbench Arcane Haze (Workshop Haze Extractor)"},
}};
constexpr u8 BbscToolCompareEqual = 0x40;   // ESD integer 0
constexpr u8 BbscToolCompareAtMost = 0x43;  // ESD integer 3
std::mutex bbsc_tool_mutex;
std::vector<std::pair<size_t, u64>> bbsc_tool_sites; // {check index, address of the comparison byte}
std::atomic<bool> bbsc_tool_scan_running{false};
std::atomic<bool> bbsc_tool_scan_done{false};

std::array<u8, 11> BbscToolCheckBytes(u32 item, u8 compare) {
    return {0x6F, 0x43, 0x82, static_cast<u8>(item), static_cast<u8>(item >> 8),
            static_cast<u8>(item >> 16), static_cast<u8>(item >> 24), compare, 0x41, 0x40, 0x89};
}

// The 11 bytes around `site` still read like this check with either comparison.
bool BbscToolSiteIntact(u64 site, u32 item) {
    if (!HasMemoryAccess(site - 7, 11, MemoryProt::CpuWrite)) {
        return false;
    }
    std::array<u8, 11> here{};
    std::memcpy(here.data(), reinterpret_cast<const void*>(site - 7), here.size());
    return here == BbscToolCheckBytes(item, BbscToolCompareEqual) ||
           here == BbscToolCheckBytes(item, BbscToolCompareAtMost);
}

void StartBbscToolScan() {
    if (bbsc_tool_scan_running.exchange(true)) {
        return;
    }
    bbsc_tool_scan_done = false;
    std::thread([] {
        std::vector<std::pair<size_t, u64>> sites;
        for (size_t i = 0; i < BbscDreamToolChecks.size(); ++i) {
            const auto needle = BbscToolCheckBytes(BbscDreamToolChecks[i].item, BbscToolCompareEqual);
            for (const u64 hit :
                 Core::Bbsc::FindInMemory(needle, BbscScanLow(), 0x10000000000ULL, 8)) {
                sites.emplace_back(i, hit + 7);
            }
        }
        {
            std::scoped_lock lock{bbsc_tool_mutex};
            bbsc_tool_sites = std::move(sites);
        }
        bbsc_tool_scan_done = true;
        bbsc_tool_scan_running = false;
    }).detach();
}

// Game thread, while the guest is in the host's Dream (restore = on leaving).
void TickBbscDreamTools(bool restore) {
    static s64 last_ms = 0;
    static std::array<int, 3> logged{-1, -1, -1};
    const s64 now = SteadyNowMs();
    if (!restore && now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    std::scoped_lock lock{bbsc_tool_mutex};
    std::array<int, 3> found{};
    for (const auto& [index, site] : bbsc_tool_sites) {
        const BbscToolCheck& check = BbscDreamToolChecks[index];
        if (!BbscToolSiteIntact(site, check.item)) {
            continue;
        }
        const bool host_has = !restore && BbscEventFlag(check.host_flag);
        const u8 want = host_has ? BbscToolCompareAtMost : BbscToolCompareEqual;
        if (ReadValue<u8>(site, 0) != want) {
            WriteValue<u8>(site, 0, want);
        }
        found[index] = host_has ? 2 : 1;
    }
    if (restore) {
        bbsc_tool_sites.clear();
        logged = {-1, -1, -1};
        return;
    }
    for (size_t i = 0; i < found.size(); ++i) {
        if (bbsc_tool_scan_done && found[i] != logged[i]) {
            logged[i] = found[i];
            LOG_INFO(Debug, "BBSC Dream: {} - {}", BbscDreamToolChecks[i].what,
                     found[i] == 0   ? "check not found in memory"
                     : found[i] == 2 ? "host has the tool, guest may use it"
                                     : "host does not have the tool either");
        }
    }
}

void TickBbscDream() {
    static bool in_dream = false;
    static bool npcs_solid = false;
    if (image_base == 0 || !BbscDreamEnabled()) {
        return;
    }
    if (!npcs_solid) {
        npcs_solid = MakeBbscDreamNpcsSolid();
    }
    const bool now = GetCurrentPackedMap() == BbscHuntersDreamMap && BbscGuestInHostWorld();
    if (now && in_dream) {
        TickBbscDreamTools(false);
    }
    if (now == in_dream) {
        return;
    }
    in_dream = now;
    if (now) {
        StartBbscToolScan();
        LOG_INFO(Debug,
                 "BBSC Dream: guest in the host's Hunter's Dream - Doll, Gehrman{}, workshop and "
                 "messengers open to the guest; headstones and Ritual Altars stay the host's",
                 BbscNightIsOver() ? " (silent: the night is over)" : "");
    } else {
        LOG_INFO(Debug, "BBSC Dream: guest left the Hunter's Dream");
        TickBbscDreamTools(true);
        bbsc_dream_talk_logged = 0;
        bbsc_dream_tools_logged = false;
        bbsc_dream_type_logged.fill(0);
    }
}

// ---- Cinematic camera (2026-09-28) -------------------------------------------------------------
// The user wants to film the real game for the trailer, the way FromSoftware filmed theirs: with
// the developers' free camera. Retail 01.09 still has it (strings SprjDebugCam, DbgFreeCam,
// Game.IsPlaytestMode_EnableFreeCamera, the pad map "DebugCamPad"). CONFIRMED statically:
//   * the stage flow 0x0193AC10 calls the camera director's update 0x015715F0(dt, director) with
//     director = [stage flow + 0x110] (0x0193D982);
//   * director +0x10 -> the render camera, +0xD0 -> the debug free camera ("DbgFreeCam" in its
//     debug draw), +0xC4 != 0 -> 0x015715F0 copies the debug camera over the render camera (world
//     matrix +0x10..+0x4F: right, up, forward, position rows; +0x50..+0x5C fov and the projection
//     values) before the SprjCamera singleton [0x0553E8F8] takes its copy for the frame.
// BBSC drives that debug camera itself from keyboard and mouse (core/bbsc_cinematic.cpp: F1 on/off,
// WASD, mouse look...), starting from wherever the game camera is, with a glide so moves look like
// a camera crane. The hook runs at the director update's entry, on the game thread, so the camera
// written here is the one the frame is drawn with. Switch SHADPS4_BBSC_CINEMATIC (default on).
constexpr u64 CameraDirectorUpdateOffset = 0x015715F0;

struct BbscCineState {
    bool on = false;
    u64 director = 0;
    std::array<float, 3> pos{};
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f, fov = 0.0f, base_fov = 0.0f;
    std::array<float, 3> glide_pos{};
    float glide_yaw = 0.0f, glide_pitch = 0.0f, glide_roll = 0.0f, glide_fov = 0.0f;
    float speed = 4.0f;  // metres per second
    float glide_s = 0.35f; // seconds for the camera to catch up (0 = none)
    float right_sign = 1.0f, up_sign = 1.0f;
    s64 last_ms = 0;
    u64 fake_camera = 0; // our own camera object when the director has none
};
BbscCineState bbsc_cine;
alignas(16) std::array<u8, 0x200> bbsc_cine_camera_storage{};

float BbscWrapAngle(float a) {
    constexpr float Pi = 3.14159265f;
    while (a > Pi) a -= 2.0f * Pi;
    while (a < -Pi) a += 2.0f * Pi;
    return a;
}

void BbscCineBasis(float yaw, float pitch, float roll, std::array<float, 3>& r,
                   std::array<float, 3>& u, std::array<float, 3>& f) {
    f = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    // right = up_world x forward (then the game's own handedness), up = forward x right
    std::array<float, 3> r0{f[2], 0.0f, -f[0]};
    const float len = std::sqrt(r0[0] * r0[0] + r0[2] * r0[2]);
    r0 = len > 1e-5f ? std::array<float, 3>{r0[0] / len, 0.0f, r0[2] / len}
                     : std::array<float, 3>{1.0f, 0.0f, 0.0f};
    std::array<float, 3> u0{f[1] * r0[2] - f[2] * r0[1], f[2] * r0[0] - f[0] * r0[2],
                            f[0] * r0[1] - f[1] * r0[0]};
    const float c = std::cos(roll), sn = std::sin(roll);
    for (int i = 0; i < 3; ++i) {
        r[i] = (r0[i] * c + u0[i] * sn) * bbsc_cine.right_sign;
        u[i] = (u0[i] * c - r0[i] * sn) * bbsc_cine.up_sign;
    }
}

void PS4_SYSV_ABI BbscCinematicCameraHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    const u64 director = registers->rdi;
    Bbsc::CinematicInput in = Bbsc::TakeCinematicInput();
    // Dead in co-op: F1 looks around freely while waiting (2026-10-04, the user's pick over a
    // follow-the-partner camera). On respawn the camera goes back to the hunter by itself.
    // 2026-10-05 (the user: "auto free cam"): it also comes on by itself once the death has played
    // out for a moment while the partner fights on (spectating); F1 still turns it off, and it stays
    // off for the rest of that death.
    {
        constexpr s64 BbscAutoCamDelayMs = 2'000;
        static bool was_dead = false;
        static s64 spectate_since_ms = 0; // 0 = not spectating; -1 = auto-on done for this death
        const u64 player = GetLocalPlayerCharacter();
        const s32 hp = ReadablePointer(player, 0x3B8) ? GetCharacterHp(player) : -1;
        if (!bbsc_spectate_active) {
            spectate_since_ms = 0;
        } else if (spectate_since_ms == 0) {
            spectate_since_ms = SteadyNowMs();
        } else if (spectate_since_ms > 0 &&
                   SteadyNowMs() - spectate_since_ms >= BbscAutoCamDelayMs) {
            spectate_since_ms = -1;
            if (!in.enabled) {
                Bbsc::CinematicForceOn();
                in.enabled = true;
                LOG_INFO(Debug, "BBSC cinematic camera: on by itself - dead, the partner fights on "
                                "(F1 / L3 + R3 turns it off)");
            }
        }
        if (bbsc_spectate_active || hp == 0) {
            was_dead = true;
        } else if (hp > 0) {
            if (was_dead && in.enabled) {
                Bbsc::CinematicForceOff();
                in.enabled = false;
                LOG_INFO(Debug, "BBSC cinematic camera: off - respawned");
            }
            was_dead = false;
        }
    }
    if (!ReadablePointer(director, 0xD8)) {
        return;
    }
    if (!in.enabled) {
        if (bbsc_cine.on) {
            bbsc_cine.on = false;
            if (ReadablePointer(bbsc_cine.director, 0xD8)) {
                WriteValue<s32>(bbsc_cine.director, 0xC4, 0);
                if (bbsc_cine.fake_camera != 0 &&
                    ReadValue<u64>(bbsc_cine.director, 0xD0) == bbsc_cine.fake_camera) {
                    WriteValue<u64>(bbsc_cine.director, 0xD0, 0);
                }
            }
            LOG_INFO(Debug, "BBSC cinematic camera: back to the game camera");
        }
        return;
    }
    const u64 render = ReadValue<u64>(director, 0x10);
    if (!ReadablePointer(render, 0x60)) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (!bbsc_cine.on || bbsc_cine.director != director) {
        // Start where the game camera is.
        bbsc_cine.director = director;
        u64 debug_camera = ReadValue<u64>(director, 0xD0);
        if (!ReadablePointer(debug_camera, 0x60)) {
            // No debug camera object in this build: give the director one of ours (a plain
            // matrix block is all 0x015715F0 reads from it when +0xC4 is set).
            std::memcpy(bbsc_cine_camera_storage.data(), reinterpret_cast<const void*>(render),
                        0x60);
            debug_camera = reinterpret_cast<u64>(bbsc_cine_camera_storage.data());
            bbsc_cine.fake_camera = debug_camera;
            WriteValue<u64>(director, 0xD0, debug_camera);
        }
        std::array<float, 16> m{};
        std::memcpy(m.data(), reinterpret_cast<const void*>(render + 0x10), sizeof(m));
        const std::array<float, 3> f{m[8], m[9], m[10]};
        bbsc_cine.pos = {m[12], m[13], m[14]};
        bbsc_cine.pitch = std::asin(std::clamp(f[1], -1.0f, 1.0f));
        bbsc_cine.yaw = std::atan2(f[0], f[2]);
        bbsc_cine.roll = 0.0f;
        bbsc_cine.base_fov = ReadValue<float>(render, 0x50);
        bbsc_cine.fov = bbsc_cine.base_fov;
        bbsc_cine.right_sign = 1.0f;
        bbsc_cine.up_sign = 1.0f;
        std::array<float, 3> r{}, u{}, ff{};
        BbscCineBasis(bbsc_cine.yaw, bbsc_cine.pitch, 0.0f, r, u, ff);
        bbsc_cine.right_sign = r[0] * m[0] + r[1] * m[1] + r[2] * m[2] < 0.0f ? -1.0f : 1.0f;
        bbsc_cine.up_sign = u[0] * m[4] + u[1] * m[5] + u[2] * m[6] < 0.0f ? -1.0f : 1.0f;
        bbsc_cine.glide_pos = bbsc_cine.pos;
        bbsc_cine.glide_yaw = bbsc_cine.yaw;
        bbsc_cine.glide_pitch = bbsc_cine.pitch;
        bbsc_cine.glide_roll = 0.0f;
        bbsc_cine.glide_fov = bbsc_cine.fov;
        bbsc_cine.last_ms = now;
        bbsc_cine.on = true;
        LOG_INFO(Debug,
                 "BBSC cinematic camera: on at ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} pitch {:.1f} fov "
                 "{:.3f} (director {:#x}, {} debug camera, signs {} {})",
                 bbsc_cine.pos[0], bbsc_cine.pos[1], bbsc_cine.pos[2],
                 bbsc_cine.yaw * 57.2958f, bbsc_cine.pitch * 57.2958f, bbsc_cine.fov, director,
                 bbsc_cine.fake_camera == debug_camera ? "BBSC's own" : "the game's",
                 bbsc_cine.right_sign, bbsc_cine.up_sign);
    }
    const float dt = std::clamp(static_cast<float>(now - bbsc_cine.last_ms) / 1000.0f, 0.0f, 0.1f);
    bbsc_cine.last_ms = now;

    // Look: mouse (right button held) and arrow keys.
    constexpr float MouseRadPerPixel = 0.0022f;
    constexpr float KeyRadPerSecond = 1.1f;
    const float zoom_scale = bbsc_cine.base_fov > 0.0f ? bbsc_cine.fov / bbsc_cine.base_fov : 1.0f;
    bbsc_cine.yaw += in.mouse_dx * MouseRadPerPixel * zoom_scale * bbsc_cine.right_sign;
    bbsc_cine.pitch -= in.mouse_dy * MouseRadPerPixel * zoom_scale;
    const float key_turn = KeyRadPerSecond * dt * zoom_scale * (in.slow ? 0.3f : 1.0f);
    bbsc_cine.yaw += ((in.look_right ? key_turn : 0.0f) - (in.look_left ? key_turn : 0.0f)) *
                     bbsc_cine.right_sign;
    bbsc_cine.pitch += (in.look_up ? key_turn : 0.0f) - (in.look_down ? key_turn : 0.0f);
    // Controller right stick: same rate as the arrow keys at full tilt.
    const float pad_turn = KeyRadPerSecond * dt * zoom_scale * (in.slow ? 0.3f : 1.0f);
    bbsc_cine.yaw += in.pad_look_x * pad_turn * bbsc_cine.right_sign;
    bbsc_cine.pitch += in.pad_look_y * pad_turn;
    bbsc_cine.pitch = std::clamp(bbsc_cine.pitch, -1.53f, 1.53f);
    // Roll (Dutch angle) and zoom.
    bbsc_cine.roll += ((in.roll_right ? 0.6f : 0.0f) - (in.roll_left ? 0.6f : 0.0f)) * dt;
    if (in.roll_reset) {
        bbsc_cine.roll = 0.0f;
    }
    const float zoom_rate = std::pow(0.55f, dt);
    if (in.zoom_in) {
        bbsc_cine.fov *= zoom_rate;
    }
    if (in.zoom_out) {
        bbsc_cine.fov /= zoom_rate;
    }
    if (in.pad_zoom != 0.0f) {
        bbsc_cine.fov *= std::pow(zoom_rate, in.pad_zoom); // positive = R2 = zoom in
    }
    if (in.zoom_reset) {
        bbsc_cine.fov = bbsc_cine.base_fov;
    }
    bbsc_cine.fov = std::clamp(bbsc_cine.fov, 0.08f, 2.2f);
    // Speed and glide.
    for (int i = 0; i < std::abs(in.wheel); ++i) {
        bbsc_cine.speed *= in.wheel > 0 ? 1.25f : 0.8f;
    }
    bbsc_cine.speed = std::clamp(bbsc_cine.speed, 0.2f, 80.0f);
    bbsc_cine.glide_s = std::clamp(bbsc_cine.glide_s + 0.1f * static_cast<float>(in.smooth_steps),
                                   0.0f, 2.0f);
    if (in.wheel != 0 || in.smooth_steps != 0) {
        LOG_INFO(Debug, "BBSC cinematic camera: speed {:.1f} m/s, glide {:.1f} s", bbsc_cine.speed,
                 bbsc_cine.glide_s);
    }
    // Move in the camera's frame; up/down along the world's vertical.
    std::array<float, 3> r{}, u{}, f{};
    BbscCineBasis(bbsc_cine.yaw, bbsc_cine.pitch, 0.0f, r, u, f);
    const float step = bbsc_cine.speed * dt * (in.fast ? 4.0f : 1.0f) * (in.slow ? 0.25f : 1.0f);
    const float mf = std::clamp((in.forward ? 1.0f : 0.0f) - (in.back ? 1.0f : 0.0f) + in.pad_move_y,
                                -1.0f, 1.0f);
    const float mr = std::clamp((in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f) + in.pad_move_x,
                                -1.0f, 1.0f);
    const float mu = std::clamp((in.up ? 1.0f : 0.0f) - (in.down ? 1.0f : 0.0f) + in.pad_rise,
                                -1.0f, 1.0f);
    const std::array<float, 3>& side = r; // the game's right row is screen right
    for (int i = 0; i < 3; ++i) {
        bbsc_cine.pos[i] += (f[i] * mf + side[i] * mr) * step;
    }
    bbsc_cine.pos[1] += mu * step;

    // The glide: the drawn camera eases toward the flown one (a crane / steadicam feel).
    const float k = bbsc_cine.glide_s > 0.0f ? 1.0f - std::exp(-dt / bbsc_cine.glide_s) : 1.0f;
    for (int i = 0; i < 3; ++i) {
        bbsc_cine.glide_pos[i] += (bbsc_cine.pos[i] - bbsc_cine.glide_pos[i]) * k;
    }
    bbsc_cine.glide_yaw += BbscWrapAngle(bbsc_cine.yaw - bbsc_cine.glide_yaw) * k;
    bbsc_cine.glide_pitch += (bbsc_cine.pitch - bbsc_cine.glide_pitch) * k;
    bbsc_cine.glide_roll += (bbsc_cine.roll - bbsc_cine.glide_roll) * k;
    bbsc_cine.glide_fov += (bbsc_cine.fov - bbsc_cine.glide_fov) * k;

    const u64 debug_camera = ReadValue<u64>(director, 0xD0);
    if (!ReadablePointer(debug_camera, 0x60) ||
        !HasMemoryAccess(debug_camera + 0x10, 64, MemoryProt::CpuWrite)) {
        return;
    }
    BbscCineBasis(bbsc_cine.glide_yaw, bbsc_cine.glide_pitch, bbsc_cine.glide_roll, r, u, f);
    alignas(16) std::array<float, 16> m{r[0], r[1], r[2], 0.0f, u[0], u[1], u[2], 0.0f,
                                       f[0], f[1], f[2], 0.0f, bbsc_cine.glide_pos[0],
                                       bbsc_cine.glide_pos[1], bbsc_cine.glide_pos[2], 1.0f};
    std::memcpy(reinterpret_cast<void*>(debug_camera + 0x10), m.data(), sizeof(m));
    // Projection: the game's current values (aspect, near, far), our field of view.
    WriteValue<float>(debug_camera, 0x50, bbsc_cine.glide_fov);
    for (const size_t off : {0x54, 0x58, 0x5C}) {
        WriteValue<u32>(debug_camera, off, ReadValue<u32>(render, off));
    }
    WriteValue<s32>(director, 0xC4, 1);
}

void InstallBbscCinematicHook() {
    if (image_base == 0 || !EnvFlagEnabled("SHADPS4_BBSC_CINEMATIC")) {
        return;
    }
    static constexpr std::array<u8, 6> Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + CameraDirectorUpdateOffset),
                              std::span<const u8>{Prologue}, 0, BbscCinematicCameraHook)) {
        LOG_ERROR(Debug, "BBSC cinematic camera: hook failed at {:#x}",
                  CameraDirectorUpdateOffset);
        return;
    }
#ifdef BBSC_MOD
    constexpr const char* Look = "mouse to look, on by itself when you die in co-op";
#else
    constexpr const char* Look = "hold right mouse to look";
#endif
    LOG_INFO(Debug, "BBSC cinematic camera: ready - F1 on/off, {}, WASD, E/Q up/down, Shift/Alt "
                    "fast/slow, wheel speed, Z/X zoom, , . roll, [ ] glide", Look);
}

void InstallBbscDreamHooks() {
    if (image_base == 0 || !BbscDreamEnabled()) {
        return;
    }
    static constexpr std::array<u8, 5> TalkSite{0x4A, 0x8B, 0x7C, 0xE9, 0x18};
    static constexpr std::array<u8, 6> TypeCheckPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    const bool talk = InstallGuestCodeHook(
        reinterpret_cast<void*>(image_base + TalkClientCheckedOffset), std::span<const u8>{TalkSite},
        0, BbscDreamTalkHook);
    const bool type = InstallGuestCodeHook(
        reinterpret_cast<void*>(image_base + CharacterTypeCheckOffset),
        std::span<const u8>{TypeCheckPrologue}, 1, BbscDreamTypeCheckHook, &bbsc_dream_type_skip);
    if (!talk || !type) {
        LOG_ERROR(Debug, "BBSC Dream: hooks failed (talk {}, event type check {})", talk, type);
        return;
    }
    LOG_INFO(Debug,
             "BBSC Dream: a guest in the host's Hunter's Dream can use the Doll, Gehrman, the "
             "workshop and the messengers ({} talk scripts, {} map events)",
             BbscDreamGuestTalks.size(), BbscDreamGuestEvents.size());
}

// ---- Floor guard --------------------------------------------------------------------------------
// Crash 2026-09-29 (guest, access violation at 0x018CB16A): the host stayed in Cathedral Ward
// (m24_01) while the guest walked on towards the Tomb of Oedon and the guest's game streamed in
// m24_00. The character floor update 0x018CB0E0 (per character, called from 0x01514500) starts:
//     MOV RSI, [RBX + 0x288]            <- 0x018CB14F: the character's floor (a collision part)
//     TEST RSI, RSI / JZ no_floor
//     MOV RCX, [RSI + 0x20] / TEST RCX, RCX / JZ ...
//     MOV RDX, [RSI + 0x18] / MOV RDX, [RDX + 0x88]   <- crash: [RSI + 0x18] was garbage
//     CMP [RDX], 3 / then [[RCX + 0xB8] + 0x0A] or [[RCX + 0x70] + 0x27]
// A character standing on the collision of a map block this game has since dropped keeps a stale
// floor pointer. Vanilla never gets there: the co-op fog walls keep both players in one block, and
// BBSC removed those walls. The guard runs only while a session is up (state 3 or 6), checks the
// chain the function is about to read, and clears a floor that no longer reads back as one, so
// the function takes its own "no floor" path (the same as mid-air). Best effort: a freed floor
// whose memory still reads sane passes. One log line per character.
constexpr u64 ChrFloorUpdateOffset = 0x018CB14F;
constexpr size_t ChrFloorOffset = 0x288;

bool BbscFloorReadsBack(u64 floor) {
    if (!ReadablePointer(floor, 0x28)) {
        return false;
    }
    const u64 part = ReadValue<u64>(floor, 0x20);
    if (part == 0) {
        return true; // the function does not follow the chain for this floor
    }
    const u64 block = ReadValue<u64>(floor, 0x18);
    if (!ReadablePointer(block, 0x90)) {
        return false;
    }
    const u64 version = ReadValue<u64>(block, 0x88);
    if (!ReadablePointer(version, 4)) {
        return false;
    }
    const u32 layout = ReadValue<u32>(version, 0);
    if (layout > 0x10) {
        return false; // a real map layout version is a small number
    }
    return layout >= 3 ? ReadablePointer(ReadValue<u64>(part, 0xB8), 0x0B)
                       : ReadablePointer(ReadValue<u64>(part, 0x70), 0x28);
}

void PS4_SYSV_ABI BbscFloorGuardHook(u64, const GuestRegisterSnapshot* registers) {
    // The session manager pointer lives in the image's data and is always mapped.
    const u64 manager = *reinterpret_cast<const u64*>(image_base + MatchingStatePointerOffset);
    if (manager < 0x10000) {
        return;
    }
    const s32 state = ReadValue<s32>(manager, 0x124);
    if (state != 3 && state != 6) {
        return;
    }
    const u64 character = registers->rbx;
    const u64 floor = ReadValue<u64>(character, ChrFloorOffset);
    if (floor == 0 || BbscFloorReadsBack(floor)) {
        return;
    }
    WriteValue<u64>(character, ChrFloorOffset, 0);
    static std::mutex logged_mutex;
    static std::array<u64, 64> logged{};
    static size_t logged_next = 0;
    std::scoped_lock lock{logged_mutex};
    if (std::ranges::find(logged, character) != logged.end()) {
        return;
    }
    logged[logged_next] = character;
    logged_next = (logged_next + 1) % logged.size();
    LOG_WARNING(Debug,
                "BBSC floor guard: character {:#x} stood on a floor that is gone ({:#x}, map "
                "{:#x}, session state {}) - floor cleared instead of a crash",
                character, floor, GetCurrentPackedMap(), state);
}

// ---- Guest pickup trace -------------------------------------------------------------------------
// 2026-09-29: the guest saw the host world's items but could not pick any up. The map items are
// now placed for a guest (BBSC.Items.PlaceTreasure), and statically nothing on the pickup path of
// a plain treasure asks for the session state. These lines show where a guest's pickup stops:
//   0x012FD8E6  the pickup action (vtable 0x0531CC60 +0x10, 0x012FD880) after the reach search
//               0x017D9FB0: [RBP-0x40] = the item in reach (needs item +0x48 bit 0x40), 0 = none
//   0x012FDA61  the action button was accepted for that item (0x01CD91F0 returned true)
//   0x017D8570  take item (item manager, &id); 0x017D89F0 give its lot to the player
constexpr u64 PickupReachResultOffset = 0x012FD8E6;   // MOV R15D, [RBP-0x40] / TEST R15D, R15D
constexpr u64 PickupAcceptedOffset = 0x012FDA61;      // MOV RAX, [R13 + 0x3B0]
constexpr u64 PickupTakeItemOffset = 0x017D8570;
constexpr u64 PickupGiveLotOffset = 0x017D89F0;
constexpr u64 ItemManagerPointerOffset = 0x0553D6E0;

// Flags (+0x48) of map item id in the item manager's three lists (+0x18/+0x28/+0x38), or -1.
s32 BbscItemFlags(u32 id) {
    const u64 manager = ReadValue<u64>(image_base + ItemManagerPointerOffset, 0);
    if (!ReadablePointer(manager, 0x40)) {
        return -1;
    }
    for (const size_t list : {0x18, 0x28, 0x38}) {
        u64 item = ReadValue<u64>(manager, list);
        for (int guard = 0; item != 0 && guard < 1024; ++guard) {
            if (!ReadablePointer(item, 0x50)) {
                break;
            }
            if (ReadValue<u32>(item, 0x20) == id) {
                return ReadValue<u16>(item, 0x48);
            }
            item = ReadValue<u64>(item, 0);
        }
    }
    return -1;
}

std::atomic<u64> bbsc_pickup_action_runs{0};

// 2026-09-29 (57405528): no "Pick up" prompt for the guest and not one "in reach" line, so the
// reach search never found an item. Every 5 s while a guest, when it changes: how many map items
// the item manager holds, how many are pickable (+0x48 bit 0x40), the nearest one (position
// +0x30/+0x34/+0x38) and whether the pickup action ran at all.
void TickBbscItemSurvey() {
    static s64 last_ms = 0;
    static std::string last_line;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 5'000 || GetSessionState() != 6) {
        return;
    }
    last_ms = now;
    const u64 manager = ReadValue<u64>(image_base + ItemManagerPointerOffset, 0);
    if (!ReadablePointer(manager, 0x40)) {
        return;
    }
    const auto me = ReadCharacterTransform(GetLocalPlayerCharacter());
    std::array<u32, 3> total{};
    std::array<u32, 3> pickable{};
    float nearest = -1.0f;
    u32 nearest_id = 0;
    u32 nearest_flags = 0;
    for (size_t list = 0; list < 3; ++list) {
        u64 item = ReadValue<u64>(manager, 0x18 + list * 0x10);
        for (int guard = 0; item != 0 && guard < 1024; ++guard) {
            if (!ReadablePointer(item, 0x50)) {
                break;
            }
            ++total[list];
            const u16 flags = ReadValue<u16>(item, 0x48);
            if ((flags & 0x40) != 0) {
                ++pickable[list];
            }
            if (me.has_value()) {
                const float dx = ReadValue<float>(item, 0x30) - me->position[0];
                const float dy = ReadValue<float>(item, 0x34) - me->position[1];
                const float dz = ReadValue<float>(item, 0x38) - me->position[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (nearest < 0.0f || d < nearest) {
                    nearest = d;
                    nearest_id = ReadValue<u32>(item, 0x20);
                    nearest_flags = flags;
                }
            }
            item = ReadValue<u64>(item, 0);
        }
    }
    const std::string line = fmt::format(
        "items {}/{}/{} (pickable {}/{}/{}), nearest {:#x} flags {:#x} at {:.1f} m, pickup action "
        "{}",
        total[0], total[1], total[2], pickable[0], pickable[1], pickable[2], nearest_id,
        nearest_flags, nearest, bbsc_pickup_action_runs.load() > 0 ? "runs" : "never ran");
    if (line == last_line) {
        return;
    }
    last_line = line;
    LOG_INFO(Debug, "BBSC guest items: map {:#x} - {}", GetCurrentPackedMap(), line);
}

void TickBbscLifeBroadcast() {
    static s64 last_ms = 0;
    static u32 sequence = 0;
    static bool last_dead = false;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < BbscLifeSendMs) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    if (state != 3 && state != 6) {
        return;
    }
    u32 player_id = bbsc_life_own_id.load();
    if (player_id == 0) {
        player_id = static_cast<u32>(static_cast<u64>(now) * 2654435761ull) | 1u;
        bbsc_life_own_id = player_id;
    }
    const u64 player = GetLocalPlayerCharacter();
    const bool dead = bbsc_spectate_active || (player != 0 && GetCharacterHp(player) <= 0);
    struct {
        u8 magic[4] = {'S', 'H', 'A', 'D'};
        u8 type = 0x23;
        u8 id[4]{};
        u8 dead = 0;
        u8 sequence[4]{};
    } packet;
    static_assert(sizeof(packet) == 14);
    ++sequence;
    std::memcpy(packet.id, &player_id, 4);
    packet.dead = dead ? 1 : 0;
    std::memcpy(packet.sequence, &sequence, 4);
    Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet, sizeof(packet));
    if (dead != last_dead) {
        last_dead = dead;
        LOG_INFO(Debug, "BBSC life: told the others we are {} (session state {})",
                 dead ? "dead" : "alive", state);
    }
}

// ---- Unstuck key (2026-10-01) -------------------------------------------------------------------
// Test 2026-10-01: the host rolled into the guest at an elevator, got stuck inside the guest's
// character, backed out and floated (no gravity) from then on. Cause not found yet. F8 in the game
// window puts the local player back on the ground: the same in-map warp the respawn uses
// (chr->vtable[0x5F8], see BbscRespawnInPlace) to the player's own position 0.5 m higher, then the
// collision proxy reset 0x018BCF30(chr, 1) that the partner revive uses. Works solo and in co-op.
#if defined(_WIN32) && defined(BBSC_MOD) // the DLL; the built-in build has no F8
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int key);
extern "C" __declspec(dllimport) void* __stdcall GetForegroundWindow();
extern "C" __declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void* window,
                                                                                  unsigned long* pid);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
#endif

void TickBbscUnstuckKey() {
#if defined(_WIN32) && defined(BBSC_MOD) // the DLL; the built-in build has no F8
    static bool was_down = false;
    const bool down = (GetAsyncKeyState(0x77 /* VK_F8 */) & 0x8000) != 0;
    const bool pressed = down && !was_down;
    was_down = down;
    if (!pressed || image_base == 0) {
        return;
    }
    unsigned long pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        return; // F8 pressed in another window
    }
    const u64 player = GetLocalPlayerCharacter();
    const auto here = ReadCharacterTransform(player);
    if (!ReadablePointer(player, 0x410) || !here.has_value() || GetCharacterHp(player) <= 0) {
        LOG_INFO(Debug, "BBSC unstuck: F8 ignored (no living local player)");
        return;
    }
    const u64 vtable = ReadValue<u64>(player, 0);
    const u64 warp = ReadablePointer(vtable, 0x600) ? ReadValue<u64>(vtable, 0x5F8) : 0;
    if (!IsEbootCodeAddress(warp)) {
        return;
    }
    alignas(16) std::array<float, 4> position = here->position;
    alignas(16) std::array<float, 4> rotation = here->rotation;
    position[1] += 0.5f;
    position[3] = 1.0f;
    rotation[3] = 0.0f;
    using Warp = void PS4_SYSV_ABI (*)(u64 chr, float* position, float* rotation, u32 flag);
    reinterpret_cast<Warp>(warp)(player, position.data(), rotation.data(), 1);
    WriteValue<u8>(player, 0x1E4, static_cast<u8>(ReadValue<u8>(player, 0x1E4) | 0x02));
    using PhysicsReset = void PS4_SYSV_ABI (*)(u64 chr, u8 flag);
    reinterpret_cast<PhysicsReset>(image_base + 0x018BCF30)(player, 1);
    LOG_INFO(Debug, "BBSC unstuck: F8 - player put back at ({:.2f}, {:.2f}, {:.2f}), map {:#x}",
             position[0], position[1], position[2], GetCurrentPackedMap());
#endif
}

// 2026-10-01 test: the guest saw items it could not pick up. The prompt search 0x017D9FB0 takes
// every item with +0x48 bit 0x40 and has no session check of its own, so an item without the bit
// is "seen but not pickable". User rule: every item the guest can see must be pickable. Once a
// second, for a guest in the host's world, every map item whose pickup flag (+0x28, -1 = none) is
// not set gets switched on through the game's own enable 0x017D9E70(item manager, &id, 1), which
// also handles the item's group and its glow (its treasure refusal for guests is patched out,
// GuestItems.ShowTreasure). Taken items keep their flag set and stay off.
constexpr u64 ItemEnableOffset = 0x017D9E70;

void TickBbscGuestItemsPickable() {
    static s64 last_ms = 0;
    static u32 logged_total = ~0u;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000 || !BbscGuestInHostWorld()) {
        return;
    }
    last_ms = now;
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_GUEST_ITEMS");
    const u64 manager = ReadValue<u64>(image_base + ItemManagerPointerOffset, 0);
    if (!enabled || !ReadablePointer(manager, 0x40) ||
        ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) == 0) {
        return;
    }
    // 2026-10-11 squad test (item 0x53 taken again after a rejoin, 19:54:29): an item that was
    // already lying in the map when the item view marked it taken stayed pickable - the view only
    // stops items from being spawned. Such an item (shown, its taken flag set) is switched off.
    std::vector<u32> off;
    std::vector<u32> stale;
    u32 taken = 0;
    for (size_t list = 0; list < 3; ++list) {
        u64 item = ReadValue<u64>(manager, 0x18 + list * 0x10);
        for (int guard = 0; item != 0 && guard < 1024; ++guard) {
            if (!ReadablePointer(item, 0xC0)) {
                break;
            }
            const s32 flag = ReadValue<s32>(item, 0x28);
            const bool flag_set = flag > 0 && BbscEventFlag(static_cast<u32>(flag));
            if ((ReadValue<u16>(item, 0x48) & 0x40) == 0) {
                if (flag_set) {
                    ++taken;
                } else if (off.size() < 256) {
                    off.push_back(ReadValue<u32>(item, 0x20));
                }
            } else if (flag_set && stale.size() < 256) {
                stale.push_back(ReadValue<u32>(item, 0x20));
            }
            item = ReadValue<u64>(item, 0);
        }
    }
    using EnableItem = u64 PS4_SYSV_ABI (*)(u64 manager, u32* id, u32 on);
    const auto enable = reinterpret_cast<EnableItem>(image_base + ItemEnableOffset);
    u32 switched = 0;
    for (u32 id : off) {
        switched += BbGuestCall(enable, manager, &id, 1) != 0 ? 1 : 0;
    }
    if (!stale.empty()) {
        u32 hidden = 0;
        std::string ids;
        for (u32 id : stale) {
            hidden += BbGuestCall(enable, manager, &id, 0) != 0 ? 1 : 0;
            if (ids.size() < 120) {
                ids += fmt::format("{}{:#x}", ids.empty() ? "" : " ", id);
            }
        }
        LOG_INFO(Debug,
                 "BBSC guest items: {} item(s) already taken were still lying here - {} switched off, "
                 "map {:#x} ({})",
                 stale.size(), hidden, GetCurrentPackedMap(), ids);
    }
    const u32 total = static_cast<u32>(off.size()) * 1000 + taken;
    if (switched != 0 || total != logged_total) {
        logged_total = total;
        LOG_INFO(Debug,
                 "BBSC guest items: {} item(s) made pickable, {} left off (already taken), map {:#x}",
                 switched, taken, GetCurrentPackedMap());
    }
}

void PS4_SYSV_ABI BbscPickupHook(u64 tag, const GuestRegisterSnapshot* registers) {
    if (tag == 0) {
        bbsc_pickup_action_runs.fetch_add(1, std::memory_order_relaxed);
    }
    const s32 state = GetSessionState();
    switch (tag) {
    case 0: { // reach result: log only the guest, and only when the item in reach changes
        static std::atomic<u32> last_in_reach{0};
        const u32 id = ReadValue<u32>(registers->rbp - 0x40, 0);
        if (state != 6 || last_in_reach.exchange(id) == id || id == 0) {
            return;
        }
        LOG_INFO(Debug, "BBSC pickup: item {:#x} in reach of the guest (flags {:#x}, map {:#x})",
                 id, BbscItemFlags(id), GetCurrentPackedMap());
        return;
    }
    case 1:
        LOG_INFO(Debug, "BBSC pickup: button accepted for item {:#x} (session state {})",
                 static_cast<u32>(registers->r15), state);
        return;
    case 2: {
        const u32 id = ReadValue<u32>(registers->rsi, 0);
        LOG_INFO(Debug, "BBSC pickup: take item {:#x} (flags {:#x}, session state {})", id,
                 BbscItemFlags(id), state);
        return;
    }
    case 3:
        LOG_INFO(Debug, "BBSC pickup: lot given to the player (session state {})", state);
        return;
    case 4: { // the map-treasure search 0x017DA500 (list +0x28), the pickup action's third search
        static std::atomic<u32> last_treasure{0};
        const u32 id = ReadValue<u32>(registers->rbp - 0x58, 0);
        if (state != 6 || last_treasure.exchange(id) == id || id == 0) {
            return;
        }
        LOG_INFO(Debug, "BBSC pickup: treasure {:#x} in reach of the guest (flags {:#x}, map {:#x})",
                 id, BbscItemFlags(id), GetCurrentPackedMap());
        return;
    }
    default:
        return;
    }
}

void InstallBbscFloorGuardAndPickupTrace() {
    if (image_base == 0) {
        return;
    }
    if (EnvFlagEnabled("SHADPS4_BBSC_FLOOR_GUARD")) {
        static constexpr std::array<u8, 10> FloorSite{0x48, 0x8B, 0xB3, 0x88, 0x02,
                                                      0x00, 0x00, 0x48, 0x85, 0xF6};
        if (InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ChrFloorUpdateOffset),
                                 std::span<const u8>{FloorSite}, 0, BbscFloorGuardHook)) {
            LOG_INFO(Debug, "BBSC floor guard: on (0x018CB0E0 skips a floor that is gone)");
        } else {
            LOG_ERROR(Debug, "BBSC floor guard: hook failed at {:#x}", ChrFloorUpdateOffset);
        }
    }
    static constexpr std::array<u8, 7> ReachSite{0x44, 0x8B, 0x7D, 0xC0, 0x45, 0x85, 0xFF};
    static constexpr std::array<u8, 7> AcceptedSite{0x49, 0x8B, 0x85, 0xB0, 0x03, 0x00, 0x00};
    static constexpr std::array<u8, 6> Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    const bool reach =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + PickupReachResultOffset),
                             std::span<const u8>{ReachSite}, 0, BbscPickupHook);
    const bool accepted =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + PickupAcceptedOffset),
                             std::span<const u8>{AcceptedSite}, 1, BbscPickupHook);
    const bool take =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + PickupTakeItemOffset),
                             std::span<const u8>{Prologue}, 2, BbscPickupHook);
    const bool give =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + PickupGiveLotOffset),
                             std::span<const u8>{Prologue}, 3, BbscPickupHook);
    // 0x012FDC17: MOV EAX, [RBP-0x58] / MOV [RBP-0x64], EAX - right after the treasure search
    // 0x017DA500 (map treasures live in the item manager's list +0x28, which the first two searches
    // never look at; 2026-10-06 arena report: items seen, no "take" line).
    static constexpr std::array<u8, 6> TreasureSite{0x8B, 0x45, 0xA8, 0x89, 0x45, 0x9C};
    const bool treasure =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + 0x012FDC17),
                             std::span<const u8>{TreasureSite}, 4, BbscPickupHook);
    LOG_INFO(Debug, "BBSC pickup trace: reach={} treasure={} accepted={} take={} give={}", reach,
             treasure, accepted, take, give);
}

// ---- Custom co-op messages ---------------------------------------------------------------------
// The game looks every text up with 0x00F6F340(MsgRepository [0x05540300], version 0, category,
// id). A category's text is a raw FMG in memory at [[[repo+8] + version*8] + category*8]: +0x0C
// group count, +0x18 an absolute pointer to the string-offset table, groups from +0x28 (0x10 each:
// first index, first id, last id), and the text of id is fmg + table[first index + id - first id].
// BBSC points those table entries at its own text (bbsc_messages.inc, generated from and checked
// against the game's English text by tools/gamedata/gen_messages.py). The game's <?tags?> work in
// it as usual. Re-applied every two seconds: the repository only exists after boot, and a language
// change reloads it. The texts are English whatever the game language is.
constexpr u64 MsgRepositoryPointerOffset = 0x05540300;

struct BbscMessage {
    u32 category;
    u32 id;
    const char16_t* text;
};

constexpr BbscMessage BbscMessages[] = {
#include "bbsc_messages.inc"
};

bool BbscMessagesEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_MESSAGES");
    return enabled;
}

// The string-offset table entry holding the text of (category, id), and the FMG it is relative
// to; 0 when that category or id is not loaded.
u64 FindBbscMessageSlot(u32 category, u32 id, u64* fmg_out) {
    const u64 repo = ReadValue<u64>(image_base + MsgRepositoryPointerOffset, 0);
    const u64 versions = ReadablePointer(repo, 0x10) ? ReadValue<u64>(repo, 0x08) : 0;
    const u64 categories = ReadablePointer(versions, 0x08) ? ReadValue<u64>(versions, 0x00) : 0;
    if (categories == 0 || !ReadablePointer(categories, (category + 1) * sizeof(u64))) {
        return 0;
    }
    const u64 fmg = ReadValue<u64>(categories, category * sizeof(u64));
    if (!ReadablePointer(fmg, 0x30)) {
        return 0;
    }
    const s32 groups = ReadValue<s32>(fmg, 0x0C);
    u64 table = ReadValue<u64>(fmg, 0x18);
    if (table < 0x1000'0000) {
        table += fmg; // still a file offset (not seen, but cheap to allow)
    }
    if (groups <= 0 || groups > 100'000 ||
        !ReadablePointer(fmg + 0x28, static_cast<size_t>(groups) * 0x10)) {
        return 0;
    }
    for (s32 g = 0; g < groups; ++g) {
        const u64 group = fmg + 0x28 + static_cast<u64>(g) * 0x10;
        const s32 first_index = ReadValue<s32>(group, 0x00);
        const u32 first_id = ReadValue<u32>(group, 0x04);
        const u32 last_id = ReadValue<u32>(group, 0x08);
        if (id < first_id || id > last_id) {
            continue;
        }
        const u64 slot = table + static_cast<u64>(first_index + (id - first_id)) * 8;
        if (!ReadablePointer(slot, 8)) {
            return 0;
        }
        *fmg_out = fmg;
        return slot;
    }
    return 0;
}

void TickBbscMessages() {
    static s64 last_ms = 0;
    static size_t logged = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || !BbscMessagesEnabled() || now - last_ms < 2'000) {
        return;
    }
    last_ms = now;
    size_t changed = 0;
    size_t in_place = 0;
    for (const auto& message : BbscMessages) {
        if (message.category == 30 && (message.id == 105199 || message.id == 105299)) {
            continue; // the boss battle line, see TickBbscBossLine
        }
        u64 fmg = 0;
        const u64 slot = FindBbscMessageSlot(message.category, message.id, &fmg);
        if (slot == 0) {
            continue;
        }
        const u64 wanted = reinterpret_cast<u64>(message.text) - fmg;
        if (ReadValue<u64>(slot, 0) == wanted) {
            ++in_place;
        } else if (WriteValue<u64>(slot, 0, wanted)) {
            ++changed;
        }
    }
    if (changed != 0 || (in_place != logged && in_place == std::size(BbscMessages))) {
        LOG_INFO(Debug, "BBSC messages: {} of {} co-op messages replaced ({} newly)",
                 changed + in_place, std::size(BbscMessages), changed);
        logged = in_place;
    }
}

// ---- Boss battle line with the boss's name (2026-10-07) -----------------------------------------
// The user: "the host started the final battle" should say which boss. The game's line for that
// (30/105199, and 105299) is one fixed text with no boss tag. BBSC points it at its own text with the
// boss's name in it: the host sends the boss on its boss bar (relay "SHAD" 0x27, s32 the gauge's
// name id, once a second while hosting), and a guest also uses its own boss bar. The name comes from
// BbscBossNames (the Discord status table); unknown boss = "The host started a boss battle."
// The text is looked up when the banner is drawn, so it has to be in place by then: a host packet
// arrives within ~1 s of the bar coming up. HYPOTHESIS until a guest screenshot shows the name.
constexpr std::array<u32, 2> BbscBossLineIds{105199, 105299};
constexpr s64 BbscHostBossFreshMs = 10'000;
std::array<char16_t, 160> bbsc_boss_line_text{};
std::atomic<s32> bbsc_host_boss_id{-1};
std::atomic<s64> bbsc_host_boss_ms{0};
const char* BbscBossNameById(s32 id); // see "Discord status"

// The name id on our own boss bar, or -1.
s32 BbscGaugeBossId() {
    const u64 gauge = ReadValue<u64>(image_base + BossGaugePointerOffset, 0);
    if (!ReadablePointer(gauge, 0x1690)) {
        return -1;
    }
    for (int slot = 0; slot < 3; ++slot) {
        const size_t at = static_cast<size_t>(slot) * 0x14;
        if (ReadValue<s32>(gauge, 0x165C + at) != -1) {
            return ReadValue<s32>(gauge, 0x1658 + at);
        }
    }
    return -1;
}

void TickBbscBossLine() {
    static s64 last_ms = 0;
    static s64 last_send_ms = 0;
    static s32 written_for = -2;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || !BbscMessagesEnabled() || now - last_ms < 200) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    const s32 own = BbscGaugeBossId();
    if (state == 3 && now - last_send_ms >= 1'000) {
        last_send_ms = now;
        std::array<u8, 9> packet{'S', 'H', 'A', 'D', 0x27};
        std::memcpy(packet.data() + 5, &own, 4);
        Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(packet.data(),
                                                                       packet.size());
    }
    s32 boss = -1;
    if (now - bbsc_host_boss_ms.load() < BbscHostBossFreshMs && bbsc_host_boss_id.load() != -1) {
        boss = bbsc_host_boss_id.load();
    } else if (own != -1) {
        boss = own;
    }
    if (boss != written_for) {
        written_for = boss;
        const char* name = boss != -1 ? BbscBossNameById(boss) : nullptr;
        const std::string text = name != nullptr
                                     ? fmt::format("The host started the battle against\n{}.", name)
                                     : std::string{"The host started a boss battle."};
        bbsc_boss_line_text = {};
        for (size_t i = 0; i < text.size() && i + 1 < bbsc_boss_line_text.size(); ++i) {
            bbsc_boss_line_text[i] = static_cast<unsigned char>(text[i]);
        }
        if (name != nullptr) {
            LOG_INFO(Debug, "BBSC boss line: \"{}\" (boss {})", name, boss);
        }
    }
    for (const u32 id : BbscBossLineIds) {
        u64 fmg = 0;
        const u64 slot = FindBbscMessageSlot(30, id, &fmg);
        const u64 wanted = reinterpret_cast<u64>(bbsc_boss_line_text.data()) - fmg;
        if (slot != 0 && ReadValue<u64>(slot, 0) != wanted) {
            WriteValue<u64>(slot, 0, wanted);
        }
    }
}

// ---- Co-op banner lines (2026-09-25) ------------------------------------------------------------
// The game's co-op lines ("... has arrived", "Entered the world of ...") are banners: 0x01AE78B0
// builds the text of a message id (category 101, then 30; "?ActionEventInfo?" when neither has it),
// a <?tag?> is filled in and the result is copied into the banner at [0x05581410] (0x055813D8 on).
// Which id comes from the multiplayer role table 0x0553D750 (0x22 roles, 0x80 bytes each):
//   +0x30  the line a summoned player gets - 0x0138A750 / 0x0138AAF0, <?hostName?>, for the local
//          role (player vtable +0x560)
//   +0x34  the line the host gets when a member's character is created - 0x0196BA20, <?joinName?>
// Warping together re-runs both for the same two people: the host saw the summon line again, and
// the guest - whose role reads 0 after the rejoin, and role 0 has no line (id 0) - saw
// "?ActionEventInfo?". While a warp is on, both columns point at BBSC's "caught up" lines.
// BBSC's own notices borrow the +0x30 column for one synchronous call to 0x0138A750. (The info
// window 0x01311150 used before only exists while its menu is open, so those never showed.)
// The carriers are placeholder "*" texts of category 30 that no talk script uses.
constexpr u64 RoleTableOffset = 0x0553D750;
constexpr size_t RoleCount = 0x22;
constexpr size_t RoleStride = 0x80;
constexpr size_t RoleGuestLine = 0x30;
constexpr size_t RoleHostLine = 0x34;
constexpr u64 SummonedLineOffset = 0x0138A750;
constexpr u64 BannerPointerOffset = 0x05581410;
constexpr u64 PlaylogSystemPointerOffset = 0x055404B0; // used by the banner call (0x01FF5D70)
constexpr u32 BbscNoticeCarrierId = 15000006;
constexpr u32 BbscGuestCaughtUpId = 15000011;  // "You have caught up with <?hostName?>."
constexpr u32 BbscHostCaughtUpId = 15000012;   // "<?joinName?> has caught up with you."
constexpr u32 BbscDreamArrivalId = 15000035;   // "The Hunter's Dream opens to you."

struct BbscRoleLines {
    std::array<u32, RoleCount> guest{};
    std::array<u32, RoleCount> host{};
    bool saved = false;
};
BbscRoleLines bbsc_role_lines; // the game's own values
u32 bbsc_guest_line = 0;       // the guest column's override while a warp is on
s64 bbsc_guest_line_until_ms = 0;
s64 bbsc_host_line_until_ms = 0;
bool bbsc_guest_line_applied = false;
bool bbsc_host_line_applied = false;

u64 BbscRoleLine(size_t role, size_t column) {
    return image_base + RoleTableOffset + role * RoleStride + column;
}

bool SaveBbscRoleLines() {
    if (bbsc_role_lines.saved) {
        return true;
    }
    if (image_base == 0 || !HasMemoryAccess(image_base + RoleTableOffset, RoleCount * RoleStride,
                                            MemoryProt::CpuWrite)) {
        return false;
    }
    for (size_t role = 0; role < RoleCount; ++role) {
        bbsc_role_lines.guest[role] = ReadValue<u32>(BbscRoleLine(role, RoleGuestLine), 0);
        bbsc_role_lines.host[role] = ReadValue<u32>(BbscRoleLine(role, RoleHostLine), 0);
    }
    // The table is filled by a static initializer; an all-zero column means it has not run yet.
    const bool filled = std::ranges::any_of(bbsc_role_lines.host, [](u32 id) { return id != 0; });
    bbsc_role_lines.saved = filled;
    return filled;
}

void WriteBbscRoleColumn(size_t column, const std::array<u32, RoleCount>* values, u32 all) {
    for (size_t role = 0; role < RoleCount; ++role) {
        WriteValue<u32>(BbscRoleLine(role, column), 0, values ? (*values)[role] : all);
    }
}

void ArmBbscRejoinLines(bool guest, u32 target_map, s64 for_ms) {
    if (!BbscMessagesEnabled()) {
        return;
    }
    const s64 until = SteadyNowMs() + for_ms;
    if (guest) {
        bbsc_guest_line = target_map == 0x15000000 ? BbscDreamArrivalId : BbscGuestCaughtUpId;
        bbsc_guest_line_until_ms = until;
    } else {
        bbsc_host_line_until_ms = until;
    }
}

// Points the role table at the "caught up" lines while a warp is on, and back afterwards.
void TickBbscNoticeRestore(); // below
void TickBbscRejoinLines() {
    TickBbscNoticeRestore();
    if (!BbscMessagesEnabled() || !SaveBbscRoleLines()) {
        return;
    }
    const s64 now = SteadyNowMs();
    const bool guest = now < bbsc_guest_line_until_ms;
    const bool host = now < bbsc_host_line_until_ms;
    if (guest != bbsc_guest_line_applied) {
        bbsc_guest_line_applied = guest;
        WriteBbscRoleColumn(RoleGuestLine, guest ? nullptr : &bbsc_role_lines.guest,
                            bbsc_guest_line);
        LOG_INFO(Debug, "BBSC co-op lines: guest summon line {}",
                 guest ? "now says \"caught up\" (warping together)" : "back to the game's");
    }
    if (host != bbsc_host_line_applied) {
        bbsc_host_line_applied = host;
        WriteBbscRoleColumn(RoleHostLine, host ? nullptr : &bbsc_role_lines.host,
                            BbscHostCaughtUpId);
        LOG_INFO(Debug, "BBSC co-op lines: host join line {}",
                 host ? "now says \"caught up\" (warping together)" : "back to the game's");
    }
}

// The carrier text stays ours until the banner has drawn it. 0x0138A750 only queues the banner and
// the text is looked up when it is drawn, a few frames later: putting the original entry back at
// once made the guest's respawn notice read "?ActionEventInfo?" (screenshots 2026-09-28).
constexpr s64 BbscNoticeHoldMs = 10'000;
u64 bbsc_notice_slot = 0;
bool bbsc_banner_own_pending = false; // a BBSC notice is queued (the join banner filter lets it be)
u64 bbsc_notice_original = 0;
s64 bbsc_notice_until_ms = 0;

void TickBbscNoticeRestore() {
    if (bbsc_notice_slot != 0 && SteadyNowMs() > bbsc_notice_until_ms) {
        WriteValue<u64>(bbsc_notice_slot, 0, bbsc_notice_original);
        bbsc_notice_slot = 0;
    }
}

// BBSC's own notices, for moments the game has no line for (respawning, the host falling).
void BbscAnnounce(const char16_t* text, std::string_view what) {
    if (image_base == 0 || !BbscMessagesEnabled()) {
        return;
    }
    if (ReadValue<u64>(image_base + BannerPointerOffset, 0) == 0 ||
        ReadValue<u64>(image_base + PlaylogSystemPointerOffset, 0) == 0 ||
        ReadValue<u64>(image_base + MsgRepositoryPointerOffset, 0) == 0 || !SaveBbscRoleLines()) {
        LOG_WARNING(Debug, "BBSC notice ({}): the co-op banner is not there yet", what);
        return;
    }
    u64 fmg = 0;
    const u64 slot = FindBbscMessageSlot(30, BbscNoticeCarrierId, &fmg);
    if (slot == 0) {
        LOG_WARNING(Debug, "BBSC notice ({}): message table not loaded", what);
        return;
    }
    if (bbsc_notice_slot != slot) {
        TickBbscNoticeRestore(); // a different table (language change): put the old one back
        bbsc_notice_original = ReadValue<u64>(slot, 0);
    }
    if (!WriteValue<u64>(slot, 0, reinterpret_cast<u64>(text) - fmg)) {
        return;
    }
    bbsc_notice_slot = slot;
    bbsc_notice_until_ms = SteadyNowMs() + BbscNoticeHoldMs;
    std::array<u32, RoleCount> column{};
    for (size_t role = 0; role < RoleCount; ++role) {
        column[role] = ReadValue<u32>(BbscRoleLine(role, RoleGuestLine), 0);
    }
    WriteBbscRoleColumn(RoleGuestLine, nullptr, BbscNoticeCarrierId);
    alignas(16) static const std::array<u8, 16> args{}; // args+8 == 0: "not skipped"
    using SummonedLine = void PS4_SYSV_ABI (*)(u64 unused, u64 unused2, const void* args);
    BbGuestCall(reinterpret_cast<SummonedLine>(image_base + SummonedLineOffset), 0, 0, args.data());
    WriteBbscRoleColumn(RoleGuestLine, &column, 0);
    bbsc_banner_own_pending = true; // BBSC's own: never hidden by the join filter
    LOG_INFO(Debug, "BBSC notice shown: {}", what);
}

// ---- Join banner filter (2026-10-05) ------------------------------------------------------------
// Tester: "some messages prompts appear like invading and random stuff when joining". The join
// re-runs the game's session steps (summon load, the cross-map second reload, the rejoin), and each
// can queue a co-op banner. Every banner producer (14 of them, 0x01382720 .. 0x0196BA20, CONFIRMED
// statically) builds its text, stores it at [0x055813E0] and sets the byte 0x055813D8; the banner
// menu's update 0x01BFF910 shows it when that byte is 1 (0x01BFFAC1) and clears it (0x01BFFBE7).
// The hook on that update logs every banner's text, and while a guest is joining - from the summon
// until 8 s after the guest is placed - clears the byte instead, so the banner never shows.
// BBSC's own notices (BbscAnnounce) always show. HYPOTHESIS until a join log shows "hidden".
constexpr u64 BannerMenuUpdateOffset = 0x01BFF910;
constexpr u64 BannerPendingOffset = 0x055813D8; // u8: 1 = a banner is queued
constexpr u64 BannerTextOffset = 0x055813E0;    // const char16_t*: its text

std::string BbscBannerText() {
    // Every banner in the 2026-10-06 logs read "" as a pointer: the producers copy the text INTO the
    // banner block (see "Co-op banner lines": "copied into the banner ... 0x055813D8 on"), so the
    // bytes at 0x055813E0 are the characters themselves when they are not a readable pointer.
    // HYPOTHESIS until a log shows real banner text.
    u64 text = ReadValue<u64>(image_base + BannerTextOffset, 0);
    if (!ReadablePointer(text, 2)) {
        text = image_base + BannerTextOffset;
    }
    std::string out;
    for (size_t i = 0; i < 120 && ReadablePointer(text + i * 2, 2); ++i) {
        const u16 c = ReadValue<u16>(text, i * 2);
        if (c == 0) {
            break;
        }
        out.push_back(c == '\n' ? ' ' : (c < 0x80 ? static_cast<char>(c) : '?'));
    }
    return out;
}

bool BbscJoinQuiet() {
    const s32 state = GetSessionState();
    if (state != 4 && state != 5 && state != 6) {
        return false;
    }
    return (bbsc_load_cover.active && bbsc_load_cover.why == "summon") ||
           SteadyNowMs() < bbsc_join_quiet_until_ms;
}

void PS4_SYSV_ABI BbscBannerMenuHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr || ReadValue<u8>(image_base + BannerPendingOffset, 0) != 1) {
        return;
    }
    static std::string last_text;
    static s64 last_ms = 0;
    const std::string text = BbscBannerText();
    const bool own = bbsc_banner_own_pending;
    bbsc_banner_own_pending = false;
    const bool hide = !own && BbscJoinQuiet();
    if (hide) {
        WriteValue<u8>(image_base + BannerPendingOffset, 0, 0);
    }
    const s64 now = SteadyNowMs();
    if (text != last_text || now - last_ms > 2'000 || hide) {
        LOG_INFO(Debug, "BBSC banner: \"{}\" {} (session state {})", text,
                 hide ? "hidden - guest joining" : (own ? "shown (BBSC's own)" : "shown"),
                 GetSessionState());
    }
    last_text = text;
    last_ms = now;
}

void InstallBbscBannerHook() {
    if (image_base == 0 || !BbscMessagesEnabled()) {
        return;
    }
    static constexpr std::array<u8, 6> PushR15{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + BannerMenuUpdateOffset),
                              std::span<const u8>{PushR15}, 0, BbscBannerMenuHook)) {
        LOG_ERROR(Debug, "BBSC banner: failed to hook the banner menu update {:#x}",
                  BannerMenuUpdateOffset);
        return;
    }
    LOG_INFO(Debug, "BBSC banner: banners logged; the ones a joining guest gets are hidden");
}

// ---- Death watchdog and death-step trace --------------------------------------------------------
// A cooperator who died twice stayed a corpse even on a build that ran the death step in full
// (reported 2026-09-20 on 19295BDF9094F72F), so the guest path fails somewhere the host path does
// not. Two answers to that: log which step of the death script actually runs, and revive the player
// regardless of whether any of them do.
//
// The script's steps, all Lua-registered: SoloPlayDeath (0x01381B60) picks _1 or _2 through
// 0x0131DDC0, SoloPlayDeath_1 (0x01381D90) only waits, PartyGhostDeath (0x01381FD0) is the
// summoned guest's entry point and PartyGhostDeath_2 (0x013824C0) its final step.
constexpr u64 SoloPlayDeathOffset = 0x01381B60;
constexpr u64 SoloPlayDeathFirstOffset = 0x01381D90;
constexpr u64 PartyGhostDeathOffset = 0x01381FD0;
constexpr s64 BbscDeadWatchdogMs = 15'000;
// The game arms its death script ("OnEvent_4000_Hp", an HP <= 0 condition registered by the map's
// init 0x0137D7A0 through 0x01329D30) once per map load, and a kept death never reloads. So the
// FIRST death after a load runs SoloPlayDeath / PartyGhostDeath as usual, and every later one runs
// nothing at all: the player lay dead until this watchdog fired 15 s later, and kept their echoes
// because no step had recorded them (local test 2026-09-24, deaths 2 and 3). After a kept death in
// the same load, a death is handled here after a short pause instead.
constexpr s64 BbscRepeatDeathMs = 3'500;
s64 bbsc_dead_since_ms = 0;
s64 bbsc_death_script_ms = 0; // last SoloPlayDeath / PartyGhostDeath entry (death script running)
bool bbsc_death_text_raised = false; // "YOU DIED" raised by BBSC for the current repeat death
// A repeat death gets its "YOU DIED" this long after the HP hit zero, unless the game's own death
// script has started by then (it raises the text itself).
constexpr s64 BbscRepeatDeathTextMs = 500;

void PS4_SYSV_ABI BbscDeathStepTraceHook(u64 tag, const GuestRegisterSnapshot* registers) {
    static constexpr std::array<std::string_view, 3> Names{"SoloPlayDeath", "SoloPlayDeath_1",
                                                           "PartyGhostDeath"};
    if (tag >= Names.size() || registers == nullptr) {
        return;
    }
    if (BbscLateDeathStep(Names[tag])) {
        bbsc_late_death_skip[tag] = 1;
        return;
    }
    LOG_INFO(Debug, "BBSC death step: {} (session state {}, chr type {}, own world {}, echoes {})",
             Names[tag], GetSessionState(), GetLocalPlayerType(),
             ReadValue<u8>(GetGlobalState(), 0x1592), GetBloodEchoes());
    if (tag == 0 || tag == 2) {
        bbsc_death_script_ms = SteadyNowMs();
    }
    if (tag == 2) {
        // PartyGhostDeath(param_1, event) - rsi is the death event state object the text event
        // needs. SoloPlayDeath raises it itself; the guest's state never does.
        ShowBbscGuestDeathText(registers->rsi);
    }
}

// ---- Echo reward trace (2026-09-25) --------------------------------------------------------------
// Echoes were still halved for the guest with both 0.5 constants patched (0x018FC947 and
// 0x01335589 are the only two reads of a 0.5 in the reward code, and no special effect has a
// 0.5 soul rate). So the guest's reward takes a path not yet seen. These observers log every step a
// kill reward can take, with the caller, while a session is live:
//   0x018C5070(dead chr)                       kill reward entry, run for the local player
//   0x018FC860(souls in xmm0, player, dead, bonus, guest_share)   the local reward calculation
//   0x018FC1E0(dead, killer, info)             reward computed for a killer (sends packet 0x26
//                                              when the killer is a remote player)
//   0x018FC600(player, amount, _, scaled)      the player's "add echoes" (vtable +0x3D0)
constexpr u64 KillRewardEntryOffset = 0x018C5070;
constexpr u64 KillRewardCalcOffset = 0x018FC860;
constexpr u64 KillRewardForKillerOffset = 0x018FC1E0;
constexpr u64 PlayerAddEchoesOffset = 0x018FC600;
constexpr int BbscEchoPathLogLimit = 300;
std::atomic<int> bbsc_echo_path_logged{0};

void PS4_SYSV_ABI BbscEchoPathHook(u64 tag, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    if (tag == 0) {
        NoteBbscGuestEnemyDeath(registers->rdi); // before the log limit below
    }
    const s32 session_state = GetSessionState();
    if ((session_state != 3 && session_state != 6) ||
        bbsc_echo_path_logged.fetch_add(1, std::memory_order_relaxed) >= BbscEchoPathLogLimit) {
        return;
    }
    u64 caller = 0;
    if (HasMemoryAccess(registers->rsp, sizeof(u64), MemoryProt::CpuRead)) {
        std::memcpy(&caller, reinterpret_cast<const void*>(registers->rsp), sizeof(caller));
    }
    const u64 from = caller > image_base ? caller - image_base : caller;
    const s32 type = GetLocalPlayerType();
    switch (tag) {
    case 0: {
        const u64 dead = registers->rdi;
        const u32 flags = ReadablePointer(dead, 0x1E8) ? ReadValue<u32>(dead, 0x1E1) : 0;
        LOG_INFO(Debug,
                 "BBSC echo path: kill reward entry, dead chr {:#x} flags(+0x1E1)&0x240000={:#x} "
                 "(caller {:#x}, player type {}, session {})",
                 dead, flags & 0x240000, from, type, session_state);
        break;
    }
    case 1: {
        float souls = 0.0f;
        std::memcpy(&souls, registers->ymm[0].data(), sizeof(souls));
        std::array<u8, 4> patched{};
        std::memcpy(patched.data(), reinterpret_cast<const void*>(image_base + 0x018FC94B), 4);
        LOG_INFO(Debug,
                 "BBSC echo path: reward calc souls {:.0f} bonus {} guest_share {} (patch bytes "
                 "{:02X} {:02X} {:02X} {:02X}; caller {:#x}, player type {}, session {})",
                 souls, registers->rdx & 0xFF, registers->rcx & 0xFF, patched[0], patched[1],
                 patched[2], patched[3], from, type, session_state);
        break;
    }
    case 2:
        LOG_INFO(Debug,
                 "BBSC echo path: reward for a killer, dead {:#x} killer {:#x} (caller {:#x}, "
                 "player type {}, session {})",
                 registers->rdi, registers->rsi, from, type, session_state);
        break;
    case 3:
        LOG_INFO(Debug,
                 "BBSC echo path: add echoes {} to chr {:#x} scaled {} (caller {:#x}, player type "
                 "{}, session {}, local player {:#x})",
                 static_cast<s32>(registers->rsi), registers->rdi, registers->rcx & 0xFF, from,
                 type, session_state, GetLocalPlayerCharacter());
        break;
    default:
        break;
    }
}

// ---- Echo counter on the HUD (2026-10-08) ---------------------------------------------------------
// Testers: "the guest's echoes are held until they go home". The guest log of that run (mod 23) has
// every kill paid and the echoes spent at the Doll while still a guest - the number is right, the
// HUD is not. The add (0x018FC600) puts each gain into MenuMan +0x210 (+0x20C the part above 0) and
// the HUD's echo widget (update 0x01656C40, vtable slot 0x05378B38, object built by 0x01657A80)
// takes it from there each frame and rolls the number up. If the widget does not run for a guest,
// the gains pile up there and show all at once at home - which is what was seen. Not confirmed yet:
// this traces whether the widget runs while a guest, and how long a gain waits.
constexpr u64 HudEchoWidgetUpdateOffset = 0x01656C40;
std::atomic<s64> bbsc_echo_widget_ms{0};

void PS4_SYSV_ABI BbscEchoWidgetHook(u64 /*tag*/, const GuestRegisterSnapshot* /*registers*/) {
    bbsc_echo_widget_ms.store(SteadyNowMs(), std::memory_order_relaxed);
}

void TickBbscEchoCounter() {
    static s64 last_ms = 0;
    static s64 pending_since_ms = 0;
    static bool logged = false;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const u64 menu = ReadValue<u64>(image_base + 0x05562878, 0);
    const s32 pending = ReadablePointer(menu, 0x214) ? ReadValue<s32>(menu, 0x210) : 0;
    if (pending == 0) {
        pending_since_ms = 0;
        logged = false;
        return;
    }
    if (pending_since_ms == 0) {
        pending_since_ms = now;
    }
    if (logged || now - pending_since_ms < 3'000) {
        return;
    }
    logged = true;
    const s64 widget = bbsc_echo_widget_ms.load(std::memory_order_relaxed);
    LOG_WARNING(Debug,
                "BBSC echo counter: {} echoes have waited {} s for the HUD (session state {}, "
                "player type {}); the HUD echo widget last ran {}",
                pending, (now - pending_since_ms) / 1000, GetSessionState(), GetLocalPlayerType(),
                widget == 0 ? std::string{"never"} : fmt::format("{} ms ago", now - widget));
}

void InstallBbscEchoPathHooks() {
    if (image_base == 0) {
        return;
    }
    {
        static constexpr std::array<u8, 6> WidgetPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + HudEchoWidgetUpdateOffset),
                                  std::span<const u8>{WidgetPrologue}, 0, BbscEchoWidgetHook)) {
            LOG_ERROR(Debug, "BBSC echo counter: could not hook the HUD echo widget");
        }
    }
    // PUSH RBP; MOV RBP, RSP; PUSH R15 (or R14 for 0x018FC600) - read 2026-09-25.
    static constexpr std::array<u8, 6> R15Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    static constexpr std::array<u8, 6> R14Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56};
    const struct {
        u64 offset;
        std::span<const u8> prologue;
        u64 tag;
    } hooks[] = {
        {KillRewardEntryOffset, R15Prologue, 0},
        {KillRewardCalcOffset, R15Prologue, 1},
        {KillRewardForKillerOffset, R15Prologue, 2},
        {PlayerAddEchoesOffset, R14Prologue, 3},
    };
    int installed = 0;
    for (const auto& hook : hooks) {
        if (InstallGuestCodeHook(reinterpret_cast<void*>(image_base + hook.offset), hook.prologue,
                                 hook.tag, BbscEchoPathHook)) {
            ++installed;
        } else {
            LOG_ERROR(Debug, "BBSC echo path: could not hook {:#x}", hook.offset);
        }
    }
    LOG_INFO(Debug, "BBSC echo path: {} of 4 reward observers installed", installed);
}

// Every change of the local blood echo total while a session is live, with the player type, so a
// halved reward shows up as a number next to the host's for the same kill (echoes still reported
// halved for the guest on 2026-09-24 although Echoes.CoopHalfRate is applied).
void TickBbscEchoTrace() {
    static s32 last = -1;
    const s32 session_state = GetSessionState();
    const s32 now = GetBloodEchoes();
    if ((session_state != 3 && session_state != 6) || now < 0) {
        last = now;
        return;
    }
    if (last >= 0 && now != last) {
        LOG_INFO(Debug, "BBSC echoes: {:+} (total {} -> {}, player type {}, session state {})",
                 now - last, last, now, GetLocalPlayerType(), session_state);
    }
    last = now;
}

// Last resort: the player has been dead for 15 s and nothing revived them. Whatever step went
// missing, they do not get to stay a corpse.
void TickBbscDeadWatchdog() {
    if (!BbscSpectateEnabled()) {
        return;
    }
    const s64 now = SteadyNowMs();
    const s32 session_state = GetSessionState();
    const bool session_alive = session_state == 3 || session_state == 6;
    const u64 global = GetGlobalState();
    if (!session_alive || (ReadablePointer(global, 0x10) && ReadValue<u8>(global, 0x08) != 0)) {
        bbsc_kept_death_this_load = false; // a load re-arms the game's own death script
    }
    const u64 player = GetLocalPlayerCharacter();
    const s32 hp = ReadablePointer(player, 0x3B8) ? GetCharacterHp(player) : -1;
    // A held death during a boss fight is meant to stay dead until the boss is over.
    const bool spectating = bbsc_spectate_active && bbsc_death_mode == BbscDeathMode::Spectate;
    if (!session_alive || hp > 0 || hp < 0 || spectating || BbscCurtainActive()) {
        bbsc_dead_since_ms = 0;
        return;
    }
    if (bbsc_dead_since_ms == 0) {
        bbsc_dead_since_ms = now;
        bbsc_death_text_raised = false;
        // Record the echoes now: a repeat death gets no death step to do it, and without a
        // snapshot the penalty below drops nothing.
        if (!bbsc_spectate_active) {
            bbsc_echoes_at_death = GetBloodEchoes();
        }
        return;
    }
    // The game's death script started for this death: its last step hands over to TickBbscSpectate
    // after about 3.5 s, so only the long limit applies.
    const bool script_running = bbsc_death_script_ms >= bbsc_dead_since_ms - 2'000;
    const bool repeat = bbsc_kept_death_this_load && !bbsc_spectate_active && !script_running;
    if (repeat && !bbsc_death_text_raised && now - bbsc_dead_since_ms >= BbscRepeatDeathTextMs) {
        bbsc_death_text_raised = true;
        if (!RaiseBbscDeathText(BbscLuaEventObject(), "repeat death")) {
            LOG_WARNING(Debug, "BBSC death text: no Lua event object, repeat death stays silent");
        }
    }
    const s64 limit = repeat ? BbscRepeatDeathMs : BbscDeadWatchdogMs;
    if (now - bbsc_dead_since_ms < limit) {
        return;
    }
    bbsc_dead_since_ms = 0;
    if (repeat && BbscBossFightAtDeath() && LivingPartnerExists()) {
        // Same rule as the death step: in a boss fight the dead player waits for the boss kill or
        // for nobody to be left alive (TickBbscSpectate).
        bbsc_spectate_name = "repeat death";
        bbsc_spectate_since_ms = now;
        bbsc_spectate_alone_since_ms = 0;
        bbsc_boss_cleared_since_ms = 0;
        bbsc_death_mode = BbscDeathMode::Spectate;
        bbsc_spectate_active = true;
        LOG_INFO(Debug, "BBSC death: repeat death in a boss fight (session state {}) -> spectate "
                        "until the boss dies or nobody is alive",
                 session_state);
        return;
    }
    if (repeat) {
        LOG_INFO(Debug,
                 "BBSC death: repeat death in the same load (the game's death script only runs "
                 "once per load), handled by BBSC after {} ms (session state {}, echoes {})",
                 limit, session_state, bbsc_echoes_at_death);
    } else {
        LOG_WARNING(Debug,
                    "BBSC death watchdog: dead for {} ms with no respawn (session state {}, "
                    "spectate {}), reviving",
                    limit, session_state, bbsc_spectate_active);
    }
    bbsc_spectate_active = false;
    BbscApplyDeathPenalty("watchdog");
    if (!StartBbscDeathCurtain("watchdog") && !BbscRespawnInPlace("watchdog")) {
        RequestBbscDeathReload("watchdog: respawn in place failed");
    }
}

// ---- Enemy scaling (like Elden Ring Seamless Co-op) ----------------------------------------------
// Vanilla: Lua_MultiDoping (0x0138BC70, a Lua function boss scripts call when the fight starts)
// gives the boss SpEffect 7500 (HP x1.5) with 1 guest or 7501 (x2.0) with 2, read from
// [[global+0x16F8]+0x08]; normal enemies get nothing (CONFIRMED statically 2026-10-01: the only code
// uses of 7500/7501 are there, NpcParam has no co-op field). BBSC switches that call off (patch
// group Scaling, bbsc_patch_tables.h) and instead:
//   - copies the clean HP-only row 7500 over SpEffect 7502 (normal enemies) and 7503 (bosses) -
//     neither is used anywhere in the game;
//   - puts 7502 / 7503 into a free spEffectID slot (-1) of every NpcParam row with teamType 23
//     (enemy) or isSoulGetByBoss (byte 329 bit 4 = boss), so every enemy carries it from spawn;
//   - rewrites the two rows (maxHpRate +16, attack power rates +72..+84) and every scaled row's
//     superArmorDurability (+280, stagger) for the number of guests, once a second.
// The param tables are found in memory by their header (type name at +0x0C, row count u16 at +0x0A,
// rows from +0x40: u32 id, pad, u64 data offset from the table start), on a thread of their own.
// Settings: SHADPS4_BBSC_SCALING (on), BBSC_SCALE_ENEMY_HP 40, BBSC_SCALE_BOSS_HP 100,
// BBSC_SCALE_DAMAGE 15, BBSC_SCALE_ENEMY_STAGGER 15, BBSC_SCALE_BOSS_STAGGER 20 (% per guest).
constexpr u64 LuaMultiDopingOffset = 0x0138BC70;
constexpr u32 BbscScaleTemplateEffect = 7500;
constexpr u32 BbscEnemyScaleEffect = 7502;
constexpr u32 BbscBossScaleEffect = 7503;
constexpr size_t SpEffectRowSize = 528;
constexpr int BbscMaxGuests = 4;

struct BbscScaledNpc {
    u64 row = 0;
    s16 armor = 0;
    bool boss = false;
};

enum class BbscScaleState : int { Idle, Scanning, Ready, Failed };
std::atomic<BbscScaleState> bbsc_scale_state{BbscScaleState::Idle};
std::vector<u64> bbsc_scale_rows_enemy; // SpEffect 7502 rows (every table copy found)
std::vector<u64> bbsc_scale_rows_boss;  // SpEffect 7503 rows
std::vector<BbscScaledNpc> bbsc_scaled_npcs;
std::string bbsc_scale_written;

bool BbscScalingEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_SCALING");
    return enabled;
}

int BbscScalePercent(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return std::clamp(std::atoi(value), 0, 1000);
}

// Host decides (2026-10-05, the user's list): each player had their own BBSC_SCALE_* settings, so a
// guest with other numbers saw other enemy HP than the host. The host now sends its five numbers
// over the relay ("SHAD" 0x24, every 2 s while hosting) and a summoned guest scales with those while
// they are fresh; alone or hosting, everyone uses their own.
constexpr s64 BbscScaleSendMs = 2'000;
constexpr s64 BbscScaleFreshMs = 15'000;
struct BbscScaleNumbers {
    std::array<int, 5> percent{}; // enemy HP, boss HP, damage, enemy stagger, boss stagger
    bool operator==(const BbscScaleNumbers&) const = default;
};
std::mutex bbsc_scale_host_mutex;
BbscScaleNumbers bbsc_scale_host;
s64 bbsc_scale_host_ms = 0;

const BbscScaleNumbers& BbscOwnScaleNumbers() {
    static const BbscScaleNumbers own{{BbscScalePercent("BBSC_SCALE_ENEMY_HP", 40),
                                       BbscScalePercent("BBSC_SCALE_BOSS_HP", 100),
                                       BbscScalePercent("BBSC_SCALE_DAMAGE", 15),
                                       BbscScalePercent("BBSC_SCALE_ENEMY_STAGGER", 15),
                                       BbscScalePercent("BBSC_SCALE_BOSS_STAGGER", 20)}};
    return own;
}

void TickBbscScaleBroadcast() {
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (!BbscScalingEnabled() || image_base == 0 || now - last_ms < BbscScaleSendMs ||
        GetSessionState() != 3) {
        return;
    }
    last_ms = now;
    struct {
        u8 magic[4] = {'S', 'H', 'A', 'D'};
        u8 type = 0x24;
        u8 percent[5 * 2]{};
    } packet;
    static_assert(sizeof(packet) == 15);
    for (size_t i = 0; i < 5; ++i) {
        const u16 value = static_cast<u16>(BbscOwnScaleNumbers().percent[i]);
        std::memcpy(packet.percent + i * 2, &value, 2);
    }
    Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet, sizeof(packet));
}

// Guests in the session, as Lua_MultiDoping counts them.
s32 GetBbscGuestCount() {
    const u64 global = GetGlobalState();
    const u64 transition = global >= 0x10000 ? ReadValue<u64>(global, 0x16F8) : 0;
    return transition >= 0x10000 ? ReadValue<s32>(transition, 0x08) : 0;
}

// Guests the scaling counts (test 2026-10-06). The game's count above is left behind when a session
// ends: a guest back home logged "1 guest(s) (game count 1, members 1)" and fought solo bosses at
// x2.00. And it drops to 0 for ~2.5 s whenever a guest dies or reloads ("game count 0, members 2"),
// so enemies spawned in that window got no scaling. Outside a session it is 0; inside, the higher of
// the game's count and the member count ([[global+0x16F8]+0x18], self included) minus one.
s32 GetBbscScalingGuests() {
    const s32 state = GetSessionState();
    if (state != 3 && state != 6) {
        return 0;
    }
    // [global+0x16F8]: +0x08 cooperators, +0x0C invaders (see ApplyBbscHunterCap) - an invader is
    // not a guest to scale for.
    const u64 global = GetGlobalState();
    const u64 population = ReadablePointer(global, 0x1700) ? ReadValue<u64>(global, 0x16F8) : 0;
    const s32 invaders = ReadablePointer(population, 0x10) ? ReadValue<s32>(population, 0x0C) : 0;
    const s32 members = GetBbscSessionMemberCount(global);
    return std::clamp(std::max(GetBbscGuestCount(), members - 1 - std::max(invaders, 0)), 0,
                      BbscMaxGuests);
}

// A row's data from its index entry's offset, checked (2026-10-08, crash hunt: many runs ended
// right after the scaling setup wrote into these tables). The data must lie after the table's index
// and within 64 MB of the table, and `size` bytes of it must be writable; 0 otherwise. A table
// caught while the game is still building it has offsets that fail this, and is left alone.
u64 BbscCheckedParamRow(u64 table, u16 count, u64 offset, size_t size) {
    const u64 row = offset < 0x10000000 ? table + offset : offset;
    const u64 index_end = table + 0x40 + static_cast<u64>(count) * 0x18;
    if (row < index_end || row - table > 0x4000000 ||
        !HasMemoryAccess(row, size, MemoryProt::CpuWrite)) {
        return 0;
    }
    return row;
}

// Data of row `id` in a param table, or 0 (`size`: bytes of it the caller will touch).
u64 BbscParamRow(u64 table, u32 id, size_t size = 8) {
    const u16 count = ReadValue<u16>(table, 0x0A);
    for (u32 i = 0; i < count; ++i) {
        const u64 entry = table + 0x40 + static_cast<u64>(i) * 0x18;
        if (ReadValue<u32>(entry, 0) == id) {
            return BbscCheckedParamRow(table, count, ReadValue<u64>(entry, 8), size);
        }
    }
    return 0;
}

// min_rows / first_id: the table's row count must be above min_rows and its first row id first_id.
std::vector<u64> FindBbscParamTables(std::string_view type, u16 min_rows = 1000,
                                     u32 first_id = 0) {
    std::vector<u8> needle(type.begin(), type.end());
    needle.push_back(0);
    std::vector<u64> tables;
    for (const u64 hit : Core::Bbsc::FindInMemory(needle, BbscScanLow(), 0x10000000000ULL, 16)) {
        const u64 table = hit - 0x0C;
        if (ReadValue<u16>(table, 0x0A) > min_rows && ReadValue<u32>(table, 0x40) == first_id) {
            tables.push_back(table);
        }
    }
    return tables;
}

void PrepareBbscScaling() {
    const s64 started = SteadyNowMs();
    bbsc_scale_rows_enemy.clear();
    bbsc_scale_rows_boss.clear();
    bbsc_scaled_npcs.clear();
    const auto speffect_tables = FindBbscParamTables("SP_EFFECT_PARAM_ST");
    const auto npc_tables = FindBbscParamTables("NPC_PARAM_ST");
    size_t enemies = 0;
    size_t bosses = 0;
    size_t full = 0;
    size_t hunters = 0;
    const char* hunters_env = std::getenv("BBSC_SCALE_NPC_HUNTERS");
    const bool scale_hunters = hunters_env != nullptr && std::string_view{hunters_env} == "1";
    for (const u64 table : speffect_tables) {
        const u64 source = BbscParamRow(table, BbscScaleTemplateEffect, SpEffectRowSize);
        const u64 enemy = BbscParamRow(table, BbscEnemyScaleEffect, SpEffectRowSize);
        const u64 boss = BbscParamRow(table, BbscBossScaleEffect, SpEffectRowSize);
        if (source == 0 || enemy == 0 || boss == 0 || ReadValue<float>(source, 16) != 1.5F) {
            continue;
        }
        std::array<u8, SpEffectRowSize> clean{};
        std::memcpy(clean.data(), reinterpret_cast<const void*>(source), clean.size());
        std::memcpy(reinterpret_cast<void*>(enemy), clean.data(), clean.size());
        std::memcpy(reinterpret_cast<void*>(boss), clean.data(), clean.size());
        bbsc_scale_rows_enemy.push_back(enemy);
        bbsc_scale_rows_boss.push_back(boss);
    }
    for (const u64 table : npc_tables) {
        const u16 count = ReadValue<u16>(table, 0x0A);
        for (u32 i = 0; i < count; ++i) {
            const u64 offset = ReadValue<u64>(table + 0x40 + static_cast<u64>(i) * 0x18, 8);
            const u64 row = BbscCheckedParamRow(table, count, offset, 336); // reads up to +329
            if (row == 0) {
                continue;
            }
            // isSoulGetByBoss is also on the 17 hostile NPC hunters (Henryk, Djura...: teamType 25 /
            // 29, game data 2026-10-08); boss HP made Henryk "invincible" for a host (a tester's report),
            // so only teamType 23 rows count as bosses. The hunters stay unscaled like in vanilla
            // (2026-10-13: still "really high HP" with enemy scaling) unless BBSC_SCALE_NPC_HUNTERS=1.
            const u8 team = ReadValue<u8>(row, 303);
            const bool flagged = ((ReadValue<u8>(row, 329) >> 4) & 1) != 0;
            const bool boss = flagged && team == 23;
            if (!flagged && team != 23) {
                continue;
            }
            if (flagged && team != 23 && !scale_hunters) {
                ++hunters;
                continue;
            }
            const s32 effect = boss ? BbscBossScaleEffect : BbscEnemyScaleEffect;
            bool placed = false;
            for (size_t slot = 0; slot < 8 && !placed; ++slot) {
                placed = ReadValue<s32>(row, 72 + slot * 4) == effect;
            }
            for (size_t slot = 0; slot < 8 && !placed; ++slot) {
                if (ReadValue<s32>(row, 72 + slot * 4) == -1) {
                    placed = WriteValue(row, 72 + slot * 4, effect);
                }
            }
            if (!placed) {
                ++full;
                continue;
            }
            bbsc_scaled_npcs.push_back({row, ReadValue<s16>(row, 280), boss});
            ++(boss ? bosses : enemies);
        }
    }
    const bool ok = !bbsc_scale_rows_enemy.empty() && !bbsc_scaled_npcs.empty();
    if (!ok) {
        bbsc_scale_state = BbscScaleState::Failed;
        return;
    }
    LOG_INFO(Debug,
             "BBSC scaling: {} SpEffect / {} NpcParam table(s) found in {} ms; {} enemy and {} boss "
             "rows carry the scaling effect ({} with no free slot, {} NPC hunter rows left unscaled){}",
             speffect_tables.size(), npc_tables.size(), SteadyNowMs() - started, enemies, bosses,
             full, hunters, ok ? "" : " - NOT READY, no scaling this session");
    bbsc_scale_state = ok ? BbscScaleState::Ready : BbscScaleState::Failed;
}

void TickBbscScaling() {
    if (!BbscScalingEnabled() || image_base == 0) {
        return;
    }
    if (bbsc_scale_state != BbscScaleState::Ready) {
        return;
    }
    BbscScaleNumbers numbers = BbscOwnScaleNumbers();
    bool from_host = false;
    if (GetSessionState() == 6) {
        std::scoped_lock lock{bbsc_scale_host_mutex};
        if (bbsc_scale_host_ms != 0 && SteadyNowMs() - bbsc_scale_host_ms < BbscScaleFreshMs) {
            numbers = bbsc_scale_host;
            from_host = true;
        }
    }
    const int enemy_hp = numbers.percent[0];
    const int boss_hp = numbers.percent[1];
    const int damage = numbers.percent[2];
    const int enemy_stagger = numbers.percent[3];
    const int boss_stagger = numbers.percent[4];
    const s32 guests = GetBbscGuestCount();
    const int extra = GetBbscScalingGuests();
    const std::string key = fmt::format("{} {} {} {} {} {}", extra, enemy_hp, boss_hp, damage,
                                        enemy_stagger, boss_stagger);
    if (key == bbsc_scale_written) {
        return;
    }
    const auto rate = [extra](int percent) {
        return 1.0F + static_cast<float>(percent) * static_cast<float>(extra) / 100.0F;
    };
    const auto write_row = [&](u64 row, float hp) {
        WriteValue(row, 16, hp);
        for (size_t at = 72; at <= 84; at += 4) {
            WriteValue(row, at, rate(damage));
        }
    };
    for (const u64 row : bbsc_scale_rows_enemy) {
        write_row(row, rate(enemy_hp));
    }
    for (const u64 row : bbsc_scale_rows_boss) {
        write_row(row, rate(boss_hp));
    }
    for (const BbscScaledNpc& npc : bbsc_scaled_npcs) {
        const float armor = static_cast<float>(npc.armor) * rate(npc.boss ? boss_stagger : enemy_stagger);
        WriteValue(npc.row, 280, static_cast<s16>(std::min(armor, 32767.0F)));
    }
    bbsc_scale_written = key;
    LOG_INFO(Debug,
             "BBSC scaling: {} guest(s) (game count {}, members {}) -> enemies HP x{:.2f}, bosses HP "
             "x{:.2f}, damage x{:.2f}, stagger +{}% / +{}% (bosses) - {} settings",
             extra, guests, GetBbscSessionMemberCount(GetGlobalState()), rate(enemy_hp),
             rate(boss_hp), rate(damage), enemy_stagger * extra, boss_stagger * extra,
             from_host ? "the host's" : "own");
}

// Boss bar (tester 2026-10-05): "the full boss hp bar shows 100% but because the boss health is
// actually 130% the bar doesn't start dropping until you've depleted 30%". The boss bar HUD
// 0x0164C200 draws [[chr+0x3B0]+0x20]+0xF8 / +0xFC (HP / max HP, CONFIRMED statically); the scaling
// effect raises the HP but that max stays the unscaled one, so the bar is over 100% until the extra
// is gone (STRONGLY INDICATED by the report). Twice a second every non-player character in
// WorldChrMan with HP above its max gets the max raised to its HP; the bar (and anything else that
// reads HP / max, like the small enemy bars) then starts at 100% and drops from the first hit.
constexpr s64 BbscMaxHpFixMs = 500;

void TickBbscScaledMaxHp() {
    static s64 last_ms = 0;
    static size_t logged = 0;
    const s64 now = SteadyNowMs();
    if (!BbscScalingEnabled() || image_base == 0 || now - last_ms < BbscMaxHpFixMs) {
        return;
    }
    last_ms = now;
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x850 + 64 * 8)) {
        return;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    // Character handles: block (h >> 14) & 0x3F of [WorldChrMan+0x850 + block*8] (+0 count, +8
    // entries of 0x38 with the ChrIns at +0), as the boss bar decodes them. Players are block 0's
    // first entries; they are skipped by the "HP above max" test anyway (and the local one by name).
    const u64 blocks_info = ReadValue<u64>(world_chr, 0x08);
    const s32 block_count = ReadablePointer(blocks_info, 0x1C)
                                ? std::clamp(ReadValue<s32>(blocks_info, 0x18) + 2, 0, 64)
                                : 0;
    size_t fixed = 0;
    for (s32 b = 0; b < block_count; ++b) {
        const u64 block = ReadValue<u64>(world_chr, 0x850 + static_cast<size_t>(b) * 8);
        if (!ReadablePointer(block, 0x10)) {
            continue;
        }
        const s32 count = ReadValue<s32>(block, 0);
        const u64 entries = ReadValue<u64>(block, 8);
        if (count <= 0 || count > 0x3FFF ||
            !ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
            continue;
        }
        for (s32 i = 0; i < count; ++i) {
            const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
            if (chr == 0 || chr == local || !ReadablePointer(chr, 0x3B8)) {
                continue;
            }
            const u64 modules = ReadValue<u64>(chr, 0x3B0);
            const u64 data = ReadablePointer(modules, 0x28) ? ReadValue<u64>(modules, 0x20) : 0;
            if (!ReadablePointer(data, 0x100)) {
                continue;
            }
            const s32 hp = ReadValue<s32>(data, 0xF8);
            const s32 max_hp = ReadValue<s32>(data, 0xFC);
            if (max_hp <= 0 || hp <= max_hp || hp > max_hp * 20) {
                continue;
            }
            WriteValue<s32>(data, 0xFC, hp);
            ++fixed;
            if (logged < 40) {
                ++logged;
                LOG_INFO(Debug, "BBSC scaling: character {:#x} (block {} #{}) HP {} over its max {} - "
                                "max raised so its bar starts full{}",
                         chr, b, i, hp, max_hp, BossFightActive() ? " (boss fight on)" : "");
            }
        }
    }
    (void)fixed;
}

// Solid NPCs for a guest (2026-10-12, testers test: "all NPCs are phantoms except the Dream
// ones and the man in the red hood in the Cathedral Ward"). The constructor patch
// SolidNpcs.NoWanderGhost (0x018E4FBF) did not keep them solid, while clearing the row's
// isChangeWanderGhost bit (+0x145 bit 7) does - that is what the Doll and Gehrman get
// (MakeBbscDreamNpcsSolid) and they were the solid ones; the red-hooded man's row never had the bit.
// Same edit on every NpcParam row, once the param tables are loaded (5 s steps, up to 3 minutes).
// Only how a host world's NPC looks to a guest changes; host and solo never read the bit.
void StartBbscSolidNpcs() {
    if (!EnvFlagEnabled("SHADPS4_BBSC_SOLID_NPCS")) {
        return;
    }
    std::thread{[] {
        for (int attempt = 0; attempt < 36; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto tables = FindBbscParamTables("NPC_PARAM_ST");
            size_t rows = 0;
            size_t cleared = 0;
            for (const u64 table : tables) {
                const u16 count = ReadValue<u16>(table, 0x0A);
                for (u32 i = 0; i < count; ++i) {
                    const u64 offset =
                        ReadValue<u64>(table + 0x40 + static_cast<u64>(i) * 0x18, 8);
                    const u64 row = BbscCheckedParamRow(table, count, offset, 336);
                    if (row == 0 ||
                        !HasMemoryAccess(row + NpcParamFlagsOffset, 1, MemoryProt::CpuWrite)) {
                        continue;
                    }
                    ++rows;
                    const u8 flags = ReadValue<u8>(row, NpcParamFlagsOffset);
                    if ((flags & NpcParamWanderGhostBit) != 0) {
                        WriteValue<u8>(row, NpcParamFlagsOffset,
                                       static_cast<u8>(flags & ~NpcParamWanderGhostBit));
                        ++cleared;
                    }
                }
            }
            if (rows != 0) {
                LOG_INFO(Debug,
                         "BBSC solid NPCs: {} NpcParam table(s), {} rows - wander-ghost bit "
                         "cleared on {} (a guest sees the host world's NPCs as they are)",
                         tables.size(), rows, cleared);
                return;
            }
        }
        LOG_INFO(Debug, "BBSC solid NPCs: NpcParam table not found after 3 minutes");
    }}.detach();
}

// The params are loaded at boot, before the title screen (and before BBSC's game tick runs): look
// for them from start-up, every 5 s for up to 3 minutes, so the first map's enemies already spawn
// with the effect.
void StartBbscScaling() {
    if (!BbscScalingEnabled() || bbsc_scale_state != BbscScaleState::Idle) {
        return;
    }
    bbsc_scale_state = BbscScaleState::Scanning;
    std::thread{[] {
        for (int attempt = 0; attempt < 36; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            PrepareBbscScaling();
            if (bbsc_scale_state == BbscScaleState::Ready) {
                return;
            }
            bbsc_scale_state = BbscScaleState::Scanning;
        }
        bbsc_scale_state = BbscScaleState::Failed;
        LOG_INFO(Debug, "BBSC scaling: param tables not found after 3 minutes - no scaling");
    }}.detach();
}
// ---- PvP toggle item: the Duel Bell (2026-10-05) ------------------------------------------------
// The user's list: "a custom item to make the pvp enabled while in coop and it turns it off if u use
// it again". Pieces (CONFIRMED statically unless marked):
//   * Item: goods 4313, an unused key item (no name, but the item name / info / caption tables have
//     a slot for it, so BBSC's texts can name it). Its rows in every EquipParamGoods copy in memory
//     become a normal usable bell: refCategory 2 (a SpEffect on yourself) with refId 7511, the bell
//     ring animation (goodsUseAnim 14), usable alone, as host and as guest, not used up.
//     (Goods 201/206/226 look free too but are the bells' "ringing" forms: replaceItemId_bySpEffect.)
//   * Effect: SpEffect 7511 is in the table but nothing uses it (no param row, no script). It becomes
//     a copy of the Beckoning Bell's 9000 without its special state (stateInfo +0x156 = 0: no sign)
//     that lasts 1 s. The player's SpEffect list is [chr+0x1C8]+8 (entries: +0x48 the param row,
//     +0x58 next; 0x018FEF50 walks it the same way), so the bell is seen being rung by its row.
//   * Giving it: the inventory is [player->vtable[0x1C8](player) + 0x5B0]; 0x014D9A80(inventory,
//     &(0x40000000 | id), count) says how many fit and 0x014DA0A0(inventory, &item key, count) adds
//     them (both called by the item-lot give 0x017D89F0; test 2026-10-05: calling only the first
//     showed the notice but gave nothing). It is added
//     once when the player is in the game and does not have it yet (the inventory's own hash lookup,
//     as 0x014D9A80 does it). It goes into the save like any item.
//   * PvP: a character's team is chr+0x88; 0x018C2A00 sets it from the player type chr+0x78 (host 0
//     -> 19, cooperator 1 -> 20, invader 2 -> 21). While PvP is on, every player's game makes the
//     OTHER players' characters team 21 (invader), so its own attacks hit them and their hits count;
//     back to what they were when PvP ends. HYPOTHESIS until a test shows hits landing.
//   * Agreement: the host decides. Relay "SHAD" 0x25 + u8 kind + u8 on + u32 sequence; kind 0 = the
//     host's state (sent every second while hosting), kind 1 = a guest rang the bell and asks for
//     that state. A guest follows the host's state.
constexpr u32 BbscDuelBellGoodsId = 4313;
constexpr u32 BbscDuelBellEffectId = 7511;
constexpr u32 BbscDuelBellSourceEffectId = 9000; // Beckoning Bell
constexpr u32 BbscDuelBellItemCode = 0x40000000u | BbscDuelBellGoodsId;
constexpr u64 InventoryFitsOffset = 0x014D9A80;
constexpr u64 InventoryAddOffset = 0x014DA0A0;
constexpr u32 BbscPvpTeam = 21;
constexpr u32 BbscInvadableEffectId = 9020;
constexpr s64 BbscPvpSendMs = 1'000;

std::atomic<bool> bbsc_duel_bell_ready{false};
std::vector<u64> bbsc_duel_bell_effect_rows; // SpEffect 7511 rows (every table copy)
std::vector<u64> bbsc_blank_effect_rows;     // SpEffect 7 rows: the Silencing Blank (goods 111)
std::vector<u64> bbsc_invadable_effect_rows; // SpEffect 9020 rows: "Bell Maiden" (invaders welcome)
std::atomic<bool> bbsc_pvp_on{false};
std::atomic<int> bbsc_pvp_request{-1}; // host side: a guest asked for this state (0 / 1)
std::atomic<int> bbsc_pvp_host_state{-1}; // guest side: the host's last state (0 / 1)
std::atomic<s64> bbsc_pvp_host_state_ms{0};
std::map<u64, u32> bbsc_pvp_original_team; // character -> its team before PvP

// ---- The BBSC hunter's kit (2026-10-05) ---------------------------------------------------------
// The user: "our own stuff - summoning, joining, pvp, invading - with our rules, including no insight
// cost", given to every character (new ones too) into the carried items, vanilla bells hidden.
// Each kit item is an unused goods slot that has item name / info / caption text slots (msg 10, 20,
// 24); its row becomes a copy of the vanilla item it replaces (same refId: the same SpEffect, so the
// game's own sign / search / leave code runs), then: no Insight (consumeHeroPoint +56 = 0), its own
// icon and sort position, and no "ringing" form (replaceItemId_bySpEffect +100 / +104 = -1: the
// vanilla forms 201/206/226 turn back into the vanilla bells). The vanilla bells and the Silencing
// Blank stay in inventories but cannot be used any more (+68 enable bits cleared) and sort last;
// their texts point at the kit (gen_messages.py).
struct BbscKitItem {
    u32 id;
    u32 copy_of;  // the vanilla goods row it copies
    u16 icon;
    s32 sort_id;
    const char16_t* name;
};
constexpr BbscKitItem BbscKit[] = {
    {4016, 200, 71, 240010, u"Hunt Bell"},       // Beckoning Bell (SpEffect 9000: search)
    {4121, 205, 37, 240011, u"Kin Bell"},        // Small Resonant Bell (9005: cooperator sign)
    {11201, 225, 70, 240012, u"Bloodlust Bell"}, // Sinister Resonant Bell (9025: invader sign)
    {4313, 0, 7, 240013, u"Duel Bell"},          // own row, see PrepareBbscDuelBell
    {11251, 111, 9, 240014, u"Parting Blank"},   // Silencing Blank (SpEffect 7: leave)
};
constexpr u32 BbscHiddenVanilla[] = {200, 205, 225, 111};
constexpr size_t GoodsRowSize = 108;

void PrepareBbscKitRows(u64 table) {
    for (const BbscKitItem& item : BbscKit) {
        const u64 row = BbscParamRow(table, item.id, GoodsRowSize);
        const u64 source = item.copy_of != 0 ? BbscParamRow(table, item.copy_of, GoodsRowSize) : 0;
        if (row == 0 || (item.copy_of != 0 && source == 0)) {
            continue;
        }
        if (source != 0 && ReadValue<u8>(source, 68) != 0) { // not hidden yet: still the original
            std::array<u8, GoodsRowSize> copy{};
            std::memcpy(copy.data(), reinterpret_cast<const void*>(source), copy.size());
            std::memcpy(reinterpret_cast<void*>(row), copy.data(), copy.size());
        }
        WriteValue<u8>(row, 56, 0);              // consumeHeroPoint: no Insight
        WriteValue<u16>(row, 44, item.icon);     // iconId
        WriteValue<s32>(row, 28, item.sort_id);  // sortId
        WriteValue<s32>(row, 100, -1);           // replaceItemId_bySpEffect
        WriteValue<s32>(row, 104, -1);           // replaceTriggerSpEffectId
        WriteValue<u8>(row, 58, 0);              // goodsType: normal item (some slots were keys)
    }
    for (const u32 id : BbscHiddenVanilla) {
        if (const u64 row = BbscParamRow(table, id); row != 0) {
            WriteValue<u8>(row, 68, 0);       // usable nowhere
            WriteValue<s32>(row, 28, 999000); // sorted last
        }
    }
}

void PrepareBbscDuelBell() {
    const auto goods_tables = FindBbscParamTables("EQUIP_PARAM_GOODS_ST", 100, 1);
    const auto speffect_tables = FindBbscParamTables("SP_EFFECT_PARAM_ST");
    size_t goods = 0;
    std::vector<u64> effects;
    std::vector<u64> blanks;
    std::vector<u64> maidens;
    for (const u64 table : speffect_tables) {
        if (const u64 blank = BbscParamRow(table, 7); blank != 0) {
            blanks.push_back(blank);
        }
        if (const u64 maiden = BbscParamRow(table, BbscInvadableEffectId); maiden != 0) {
            maidens.push_back(maiden);
        }
        const u64 source = BbscParamRow(table, BbscDuelBellSourceEffectId, SpEffectRowSize);
        const u64 effect = BbscParamRow(table, BbscDuelBellEffectId, SpEffectRowSize);
        if (source == 0 || effect == 0) {
            continue;
        }
        std::array<u8, SpEffectRowSize> row{};
        std::memcpy(row.data(), reinterpret_cast<const void*>(source), row.size());
        std::memcpy(reinterpret_cast<void*>(effect), row.data(), row.size());
        WriteValue<float>(effect, 8, 1.0f); // effectEndurance: 1 s
        WriteValue<u16>(effect, 0x156, 0);  // stateInfo: no summon sign
        effects.push_back(effect);
    }
    for (const u64 table : goods_tables) {
        PrepareBbscKitRows(table);
        const u64 row = BbscParamRow(table, BbscDuelBellGoodsId, GoodsRowSize);
        if (row == 0) {
            continue;
        }
        WriteValue<s32>(row, 0, static_cast<s32>(BbscDuelBellEffectId)); // refId
        WriteValue<s32>(row, 28, 240013);  // sortId: with the kit
        WriteValue<s32>(row, 36, 0);       // yesNoDialogMessageId: none
        WriteValue<u16>(row, 44, 7);       // iconId: the Sinister Bell's
        WriteValue<s16>(row, 54, 1);       // maxNum
        WriteValue<u8>(row, 58, 0);        // goodsType: normal item
        WriteValue<u8>(row, 59, 2);        // refCategory: SpEffect
        WriteValue<u8>(row, 62, 14);       // goodsUseAnim: ring a bell
        WriteValue<u8>(row, 63, 0);        // opmeMenuType
        WriteValue<u8>(row, 64, 0);        // useLimitCategory
        WriteValue<u8>(row, 68, 0x1F);     // usable alive / grey / white / black / in co-op
        WriteValue<u8>(row, 69, 0x10);     // isDeposit (storage)
        WriteValue<s32>(row, 100, -1);     // replaceItemId_bySpEffect
        WriteValue<s32>(row, 104, -1);     // replaceTriggerSpEffectId
        ++goods;
    }
    const bool ok = goods != 0 && !effects.empty();
    LOG_INFO(Debug, "BBSC duel bell: goods {} in {} of {} table(s), SpEffect {} in {} table(s){}",
             BbscDuelBellGoodsId, goods, goods_tables.size(), BbscDuelBellEffectId,
             effects.size(), ok ? "" : " - NOT READY, no PvP bell");
    // Review 2026-10-08: the game thread reads these vectors; they are filled only once, here, by
    // this (scan) thread, and published by the ready flag - never replaced while being read.
    if (ok && !bbsc_duel_bell_ready) {
        bbsc_duel_bell_effect_rows = std::move(effects);
        bbsc_blank_effect_rows = std::move(blanks);
        bbsc_invadable_effect_rows = std::move(maidens);
        bbsc_duel_bell_ready.store(true, std::memory_order_release);
    }
}

void StartBbscDuelBell() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    std::thread{[] {
        for (int attempt = 0; attempt < 36 && !bbsc_duel_bell_ready; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            PrepareBbscDuelBell();
        }
    }}.detach();
}

// The items the local player carries, or 0: [[0x0553B130]+8] + 0x328 (embedded), the list the
// item-lot give 0x017D89F0 fills first (through 0x014D97F0).
u64 BbscCarriedInventory() {
    const u64 holder = ReadValue<u64>(image_base + 0x0553B130, 0);
    const u64 data = ReadablePointer(holder, 0x10) ? ReadValue<u64>(holder, 0x08) : 0;
    return ReadablePointer(data, 0x328 + 0x90) ? data + 0x328 : 0;
}

// The storage box (Hunter's Dream), or 0: [player->vtable[0x1C8](player) + 0x5B0], where the give
// puts what does not fit (test 2026-10-05: the bell given there showed in the box, not the items).
u64 BbscStorageInventory() {
    const u64 player = GetLocalPlayerCharacter();
    const u64 vtable = ReadablePointer(player, 0x410) ? ReadValue<u64>(player, 0) : 0;
    const u64 getter = ReadablePointer(vtable, 0x1D0) ? ReadValue<u64>(vtable, 0x1C8) : 0;
    if (!IsEbootCodeAddress(getter)) {
        return 0;
    }
    using GameData = u64 PS4_SYSV_ABI (*)(u64 chr);
    const u64 data = BbGuestCall(reinterpret_cast<GameData>(getter), player);
    const u64 inventory = ReadablePointer(data, 0x5B8) ? ReadValue<u64>(data, 0x5B0) : 0;
    return ReadablePointer(inventory, 0x90) ? inventory : 0;
}

// The inventory's own lookup (0x014D9A80): hash [inv+0x68][code % 0x7E1] (s16, <0 = none) into the
// entries at [inv+0x78] (8 bytes: u32 code, u32 slot in bits 0-11 / next + 1 in bits 12-23); the
// slot (16 bytes, u32 != 0 when used) is [inv+0x58] + slot*16 below [inv+0x24], else [inv+0x48].
// The slot entry (16 bytes: +0 item key, +8 count) of slot `slot`, or 0.
u64 BbscInventorySlotEntry(u64 inventory, u32 slot) {
    const u32 split = ReadValue<u32>(inventory, 0x24);
    const u64 at = slot < split ? ReadValue<u64>(inventory, 0x58) + slot * 16ull
                                : ReadValue<u64>(inventory, 0x48) + (slot - split) * 16ull;
    return ReadablePointer(at, 16) ? at : 0;
}

// The inventory slot holding `code` (0x40000000 | goods id), or -1 (same lookup as BbscInventoryHas).
s32 BbscInventorySlot(u64 inventory, u32 code) {
    const u64 hashes = ReadValue<u64>(inventory, 0x68);
    const u64 entries = ReadValue<u64>(inventory, 0x78);
    if (!ReadablePointer(hashes, 0x7E1 * 2) || entries == 0) {
        return -1;
    }
    s64 index = ReadValue<s16>(hashes, (code % 0x7E1) * 2);
    for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
        if (!ReadablePointer(entries + static_cast<u64>(index) * 8, 8)) {
            return -1;
        }
        const u32 entry_code = ReadValue<u32>(entries, static_cast<u64>(index) * 8);
        const u32 info = ReadValue<u32>(entries, static_cast<u64>(index) * 8 + 4);
        if (entry_code == code) {
            const u32 slot = info & 0xFFF;
            const u64 at = BbscInventorySlotEntry(inventory, slot);
            return at != 0 && ReadValue<u32>(at, 0) != 0 ? static_cast<s32>(slot) : -1;
        }
        const u32 next = (((info >> 12) & 0xFFF) + 0xFFFF) & 0xFFFF;
        if ((next & 0x8000) != 0) {
            return -1;
        }
        index = next;
    }
    return -1;
}

// Takes every one of `code` out of the inventory, the way the storage deposit 0x014DA750 takes an
// item out of its source: 0x014D94A0(inventory, slot, -count, &over) and, at 0 left, 0x014DAB20
// (inventory, slot, 1) frees the slot. True when it is gone.
bool BbscInventoryRemove(u64 inventory, u32 code) {
    const s32 slot = BbscInventorySlot(inventory, code);
    if (slot < 0) {
        return false;
    }
    const u64 entry = BbscInventorySlotEntry(inventory, static_cast<u32>(slot));
    const s32 count = entry != 0 ? ReadValue<s32>(entry, 8) : 0;
    alignas(16) static s32 over = 0;
    over = 0;
    using Change = s32 PS4_SYSV_ABI (*)(u64 inventory, u32 slot, s32 delta, s32* over);
    using Free = void PS4_SYSV_ABI (*)(u64 inventory, u32 slot, s32 one);
    const s32 left = BbGuestCall(reinterpret_cast<Change>(image_base + 0x014D94A0), 
        inventory, static_cast<u32>(slot), -std::max(count, 1), &over);
    if (over == 0 && left == 0) {
        BbGuestCall(reinterpret_cast<Free>(image_base + 0x014DAB20), inventory, static_cast<u32>(slot), 1);
    }
    return BbscInventorySlot(inventory, code) < 0;
}

bool BbscInventoryHas(u64 inventory, u32 code) {
    const u64 hashes = ReadValue<u64>(inventory, 0x68);
    const u64 entries = ReadValue<u64>(inventory, 0x78);
    if (!ReadablePointer(hashes, 0x7E1 * 2) || entries == 0) {
        return false;
    }
    s64 index = ReadValue<s16>(hashes, (code % 0x7E1) * 2);
    for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
        if (!ReadablePointer(entries + static_cast<u64>(index) * 8, 8)) {
            return false;
        }
        const u32 entry_code = ReadValue<u32>(entries, static_cast<u64>(index) * 8);
        const u32 info = ReadValue<u32>(entries, static_cast<u64>(index) * 8 + 4);
        if (entry_code == code) {
            const u32 slot = info & 0xFFF;
            const u32 split = ReadValue<u32>(inventory, 0x24);
            const u64 at = slot < split ? ReadValue<u64>(inventory, 0x58) + slot * 16ull
                                        : ReadValue<u64>(inventory, 0x48) + (slot - split) * 16ull;
            return ReadablePointer(at, 4) && ReadValue<u32>(at, 0) != 0;
        }
        const u32 next = (((info >> 12) & 0xFFF) + 0xFFFF) & 0xFFFF; // next + 1 stored; 0 = end
        if ((next & 0x8000) != 0) {
            return false;
        }
        index = next;
    }
    return false;
}

// Count of `code` (0x40000000 | goods id) in the inventory, 0 when none.
s32 BbscInventoryCount(u64 inventory, u32 code) {
    const s32 slot = BbscInventorySlot(inventory, code);
    const u64 entry = slot >= 0 ? BbscInventorySlotEntry(inventory, static_cast<u32>(slot)) : 0;
    return entry != 0 ? ReadValue<s32>(entry, 8) : 0;
}

// Restock after a respawn in place (2026-10-13, testers: "no vials after respawning").
// A lamp rest or a death reload tops up Blood Vials (goods 1000) and Quicksilver Bullets (900) -
// the two EquipParamGoods rows with isAutoReplenish (+0x47 bit 0) - from the storage box; the
// in-place respawn skips that reload, so nothing came back. Same here: the inventory's own fit
// check 0x014D9A80 says how many more the player can carry (maxNum 20, runes included as far as
// the game counts them there), the add 0x014DA0A0 puts them in, and only what arrived is taken
// out of storage (0x014D94A0 delta, 0x014DAB20 frees an emptied slot). The game's own restock
// routine was not found (HYPOTHESIS that the fit check counts the runes).
void BbscRestockFromStorage(std::string_view why) {
    const u64 carried = BbscCarriedInventory();
    const u64 storage = BbscStorageInventory();
    if (carried == 0 || storage == 0) {
        LOG_INFO(Debug, "BBSC restock ({}): skipped (carried {:#x}, storage {:#x})", why, carried,
                 storage);
        return;
    }
    using Fits = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* code, s32 count);
    using Add = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* key, s32 count);
    using Change = s32 PS4_SYSV_ABI (*)(u64 inventory, u32 slot, s32 delta, s32* over);
    using Free = void PS4_SYSV_ABI (*)(u64 inventory, u32 slot, s32 one);
    alignas(16) static u32 code = 0;
    alignas(16) static u32 key = 0;
    alignas(16) static s32 over = 0;
    std::string summary;
    for (const auto& [goods, name] : std::array<std::pair<u32, std::string_view>, 2>{
             {{1000, "Blood Vials"}, {900, "Quicksilver Bullets"}}}) {
        const u32 item = 0x40000000u | goods;
        const s32 stored = BbscInventoryCount(storage, item);
        const s32 before = BbscInventoryCount(carried, item);
        if (stored <= 0) {
            summary += fmt::format(" {} {} (none stored)", name, before);
            continue;
        }
        code = item;
        key = 0x80000000u | (3u << 28) | goods;
        const s32 fits = BbGuestCall(reinterpret_cast<Fits>(image_base + 0x014D9A80), carried, &code, stored);
        const s32 want = std::min(fits, stored);
        if (want <= 0) {
            summary += fmt::format(" {} {} (full)", name, before);
            continue;
        }
        BbGuestCall(reinterpret_cast<Add>(image_base + 0x014DA0A0), carried, &key, want);
        const s32 moved = std::clamp(BbscInventoryCount(carried, item) - before, 0, want);
        if (moved > 0) {
            const s32 slot = BbscInventorySlot(storage, item);
            if (slot >= 0) {
                over = 0;
                const s32 left = BbGuestCall(reinterpret_cast<Change>(image_base + 0x014D94A0), 
                    storage, static_cast<u32>(slot), -moved, &over);
                if (over == 0 && left == 0) {
                    BbGuestCall(reinterpret_cast<Free>(image_base + 0x014DAB20), storage,
                                                                    static_cast<u32>(slot), 1);
                }
            }
        }
        summary += fmt::format(" {} {} -> {} (storage {} -> {})", name, before, before + moved,
                               stored, BbscInventoryCount(storage, item));
    }
    LOG_INFO(Debug, "BBSC restock ({}):{}", why, summary);
}

// The user's rule: saves are backed up before anything touches them. Giving the bell writes it into
// the save, so first every Bloodborne save folder BBSC can find - <base>\home\<user>\savedata\
// CUSA00207 (the game's save id) and the older <base>\savedata\<user>\CUSA00207, with <base> the
// portable "user" folder in the working directory or %APPDATA%\shadPS4 - is copied to
// <mod folder>\save-backups\before-duel-bell-<time>\<user>. User 1004 is left alone (the user's
// rule). False when nothing could be copied.
bool BackupBbscSavesUnchecked();
// Called from game ticks: a filesystem error (access denied, a strange folder name) must not throw
// out of it (review 2026-10-08: the directory walk's ++ throws even with an error_code).
bool BackupBbscSaves() {
    try {
        return BackupBbscSavesUnchecked();
    } catch (const std::exception& e) {
        LOG_WARNING(Debug, "BBSC save backup failed: {}", e.what());
        return false;
    }
}

bool BackupBbscSavesUnchecked() {
    namespace fs = std::filesystem;
    std::vector<fs::path> bases;
    std::error_code ec;
    bases.push_back(fs::current_path(ec) / "user");
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        bases.push_back(fs::path(appdata) / "shadPS4");
    }
#endif
    const auto stamp = std::chrono::system_clock::now().time_since_epoch();
    const fs::path out = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir).parent_path() /
                         "save-backups" /
                         fmt::format("before-duel-bell-{}",
                                     std::chrono::duration_cast<std::chrono::seconds>(stamp).count());
    std::vector<std::pair<fs::path, fs::path>> saves; // save folder, backup folder
    for (const fs::path& base : bases) {
        for (const auto& user : fs::directory_iterator(base / "home", ec)) {
            saves.emplace_back(user.path() / "savedata" / "CUSA00207",
                               out / ("home-" + user.path().filename().string()));
        }
        ec.clear();
        for (const auto& user : fs::directory_iterator(base / "savedata", ec)) {
            saves.emplace_back(user.path() / "CUSA00207",
                               out / ("savedata-" + user.path().filename().string()));
        }
        ec.clear();
    }
    size_t copied = 0;
    // BBSC_SAVE_DIR: the save folder itself (bbhost), copied as it is.
    if (const char* save_dir = std::getenv("BBSC_SAVE_DIR"); save_dir != nullptr && save_dir[0]) {
        const fs::path game(save_dir);
        if (fs::is_directory(game, ec)) {
            const fs::path target = out / "save-dir" / game.filename();
            fs::create_directories(target.parent_path(), ec);
            fs::copy(game, target, fs::copy_options::recursive, ec);
            if (!ec) {
                ++copied;
                LOG_INFO(Debug, "BBSC duel bell: save backed up {} -> {}",
                         Common::FS::PathToUTF8String(game), Common::FS::PathToUTF8String(target));
            }
        }
        ec.clear();
        if (copied != 0) {
            return true; // the host's own save is the one in use; shadPS4's are not touched
        }
    }
    for (const auto& [game, target] : saves) {
        if (!fs::is_directory(game, ec) || game.parent_path().filename() == "1004" ||
            game.parent_path().parent_path().filename() == "1004") {
            continue;
        }
        fs::create_directories(target, ec);
        fs::copy(game, target / "CUSA00207", fs::copy_options::recursive, ec);
        if (!ec) {
            ++copied;
            LOG_INFO(Debug, "BBSC duel bell: save backed up {} -> {}",
                     Common::FS::PathToUTF8String(game), Common::FS::PathToUTF8String(target));
        }
    }
    return copied != 0;
}

// The kit every BBSC player gets (the user, 2026-10-05: given when you start playing, even as a new
// player, only once - it checks whether you carry it or have it in storage). Each kit item missing
// from BOTH the carried items and the storage box goes into the carried items. Checked once per
// loaded character (every new local player character), so a save loaded later is checked too; the
// saves are backed up before anything is added.
void TickBbscDuelBellGive() {
    static s64 last_ms = 0;
    static u64 checked_player = 0;
    const s64 now = SteadyNowMs();
    if (!bbsc_duel_bell_ready || now - last_ms < 5'000) {
        return;
    }
    last_ms = now;
    const u64 player = GetLocalPlayerCharacter();
    const u64 global = GetGlobalState();
    if (player == checked_player || !ReadablePointer(player, 0x410) ||
        GetCharacterHp(player) <= 0 || !ReadablePointer(global, 0x1598) ||
        ReadValue<u8>(global, 0x08) != 0) {
        return; // already checked, not in the game yet, dead, or loading
    }
    const u64 carried = BbscCarriedInventory();
    const u64 storage = BbscStorageInventory();
    if (carried == 0 || storage == 0) {
        return;
    }
    checked_player = player;
    const auto has = [&](u32 id) {
        const u32 code = 0x40000000u | id;
        return BbscInventoryHas(carried, code) || BbscInventoryHas(storage, code);
    };
    std::vector<const BbscKitItem*> missing;
    for (const BbscKitItem& item : BbscKit) {
        if (!has(item.id)) {
            missing.push_back(&item);
        }
    }
    // The vanilla items the kit replaces (and the bells' "ringing" forms), carried or stored, are
    // taken out (the user: "old items are still there, they are not hidden").
    // The Dream's messenger holds out the vanilla Beckoning Bell (event 12101024, item lot 10010,
    // "given" flag 6622) to every character with Insight - next to the kit's Hunt Bell it only
    // confuses (2026-10-08: "the host has the old bells on the ground in the Dream"). In our own
    // world the flag is set, so the messenger no longer offers it.
    if (ReadValue<u8>(global, 0x1592) != 0 && ReadValue<u64>(image_base + 0x0553B0D8, 0) != 0 &&
        ReadValue<u64>(image_base + EventFlagManPointerOffset, 0) != 0 && !BbscEventFlag(6622)) {
        using SetEventFlag = u64 PS4_SYSV_ABI (*)(u64 unused, u32 flag, u8 value);
        BbGuestCall(reinterpret_cast<SetEventFlag>(image_base + 0x0132E5A0), 0, 6622, 1);
        LOG_INFO(Debug, "BBSC kit: the Dream messenger's vanilla Beckoning Bell marked as given "
                        "(flag 6622) - the kit's Hunt Bell replaces it");
    }
    static constexpr u32 Retired[] = {200, 201, 205, 206, 225, 226, 111};
    bool retire = false;
    for (const u32 id : Retired) {
        retire = retire || has(id);
    }
    if (missing.empty() && !retire) {
        LOG_INFO(Debug, "BBSC kit: all carried or in storage, no vanilla bells left");
        return;
    }
    if (!BackupBbscSaves()) {
        LOG_WARNING(Debug, "BBSC kit: no save folder found to back up - nothing given or removed");
        return;
    }
    for (const u32 id : Retired) {
        for (const u64 inventory : {carried, storage}) {
            if (BbscInventoryHas(inventory, 0x40000000u | id)) {
                const bool gone = BbscInventoryRemove(inventory, 0x40000000u | id);
                LOG_INFO(Debug, "BBSC kit: vanilla goods {} taken out of the {} ({})", id,
                         inventory == carried ? "carried items" : "storage box",
                         gone ? "gone" : "STILL THERE");
            }
        }
    }
    if (missing.empty()) {
        return;
    }
    // 0x014D9A80(inventory, &code, count) is how many fit; 0x014DA0A0(inventory, &key, count) adds,
    // with the item key (0x01A89070: bit 31, category index in bits 28-30 - goods 3 - and the id).
    using Fits = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* code, s32 count);
    using Add = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* key, s32 count);
    static std::u16string notice;
    notice = u"BBSC items added:";
    size_t given = 0;
    for (const BbscKitItem* item : missing) {
        alignas(16) static u32 code = 0;
        alignas(16) static u32 key = 0;
        code = 0x40000000u | item->id;
        key = 0x80000000u | (3u << 28) | item->id;
        const s32 fits = BbGuestCall(reinterpret_cast<Fits>(image_base + InventoryFitsOffset), carried, &code, 1);
        const s32 slot =
            fits > 0 ? BbGuestCall(reinterpret_cast<Add>(image_base + InventoryAddOffset), carried, &key, 1) : -1;
        const bool now_there = BbscInventoryHas(carried, code);
        LOG_INFO(Debug, "BBSC kit: goods {} added to the carried items (fits {}, slot {}, there now: "
                        "{})",
                 item->id, fits, slot, now_there);
        if (now_there) {
            ++given;
            notice += given == 1 ? u"\n" : u", ";
            notice += item->name;
        }
    }
    if (given != 0) {
        BbscAnnounce(notice.c_str(), "kit given");
    }
}

// True while the local player carries one of these SpEffect rows.
bool BbscLocalEffectActive(const std::vector<u64>& rows) {
    const u64 player = GetLocalPlayerCharacter();
    const u64 effects = ReadablePointer(player, 0x1D0) ? ReadValue<u64>(player, 0x1C8) : 0;
    if (!ReadablePointer(effects, 0x10)) {
        return false;
    }
    u64 entry = ReadValue<u64>(effects, 0x08);
    for (int guard = 0; entry != 0 && guard < 256; ++guard) {
        if (!ReadablePointer(entry, 0x60)) {
            return false;
        }
        const u64 row = ReadValue<u64>(entry, 0x48);
        for (const u64 wanted : rows) {
            if (row == wanted) {
                return true;
            }
        }
        entry = ReadValue<u64>(entry, 0x58);
    }
    return false;
}

bool BbscDuelBellEffectActive() {
    return bbsc_duel_bell_ready.load(std::memory_order_acquire) &&
           BbscLocalEffectActive(bbsc_duel_bell_effect_rows);
}

// Silencing Blank used by the HOST (test 2026-10-05: the guest was kicked, froze and got "session
// lost", the host ended on the title screen). Only a guest's Blank raises OnLeaveMagic (0x018FEF50:
// session state 6 or in another world); the host's goes to 0x015BE2A0, which removes the guests,
// so BbscLeaveItemUsed never ran and the kept-session patches stayed on - 10 s later the room
// timeout's cleanup was cut short (BBSC.Persist.SkippedCleanup) and the host fell to the title.
// The Blank puts SpEffect 7 on its user; TickBbscHostBlank only logs it now (2026-10-06: suspending
// the patches on the host sent the host to the title, see there).
// Invasions everywhere (the user's pick, 2026-10-05: solo or not, any area, boss dead or not).
// Vanilla invites invaders only into a world with a Bell-Ringing Woman: common.emevd event 9240
// gives the player SpEffect 9020 (stateInfo 191, next to the bells' 188-190 - the invader search)
// and 9100, event 9260 takes them off again (2004[8] / 2004[21] on entity 10000; STRONGLY
// INDICATED, HYPOTHESIS until an invasion log shows a SummonType 2 search). BBSC keeps 9020 on
// every living player in their own world or hosting, re-adding it when the game takes it off. It is
// added the way Lua_MultiDoping (0x0138BC70) adds 7500/7501: chr->vtable[0x3F0](chr, id, chr, 0, 0,
// 0, 1.0 x5 in xmm0-4, 0 on the stack). Switch SHADPS4_BBSC_INVADE_ANYWHERE (on).
// In someone else's world: [[global+0x16F8]+0x98] != -1 (the second half of the usual guest gate).
bool BbscInAnotherWorld() {
    const u64 global = GetGlobalState();
    const u64 transition = ReadablePointer(global, 0x1700) ? ReadValue<u64>(global, 0x16F8) : 0;
    return ReadablePointer(transition, 0xA0) && ReadValue<s32>(transition, 0x98) != -1;
}

bool BbscInvadeAnywhereEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_INVADE_ANYWHERE");
    return enabled;
}

// The local player's SpEffect node with this id (+0x40 id, +0x58 next), 0 when it has none.
u64 BbscLocalEffectNode(s32 id) {
    const u64 player = GetLocalPlayerCharacter();
    const u64 effects = ReadablePointer(player, 0x1D0) ? ReadValue<u64>(player, 0x1C8) : 0;
    if (!ReadablePointer(effects, 0x10)) {
        return 0;
    }
    u64 entry = ReadValue<u64>(effects, 0x08);
    for (int guard = 0; entry != 0 && guard < 256; ++guard) {
        if (!ReadablePointer(entry, 0x60)) {
            return 0;
        }
        if (ReadValue<s32>(entry, 0x40) == id) {
            return entry;
        }
        entry = ReadValue<u64>(entry, 0x58);
    }
    return 0;
}

// Takes a SpEffect off the local player the way EraseEventSpecialEffect (Lua, 0x013183D0) does:
// find the node by id in [chr+0x1C8]+8, then 0x017E1D90(manager, node, 1).
bool BbscEraseLocalEffect(s32 id) {
    const u64 node = BbscLocalEffectNode(id);
    if (node == 0 || image_base == 0) {
        return false;
    }
    const u64 manager = ReadValue<u64>(GetLocalPlayerCharacter(), 0x1C8);
    using EraseFn = void PS4_SYSV_ABI (*)(u64 manager, u64 node, s32 now);
    BbGuestCall(reinterpret_cast<EraseFn>(image_base + 0x017E1D90), manager, node, 1);
    return BbscLocalEffectNode(id) == 0;
}

// Test 2026-10-05 14:42: with 9020 on both players, the host world found the invader's sign
// (SummonType 2), claimed it, opened its room and signalled the invader - who never joined
// (no JoinSession_Black). The invader carried 9020 too. HYPOTHESIS: a player whose own world is
// open to invaders refuses to go out as a phantom. So while a sign of ours is out (Kin Bell 9005,
// Bloodlust Bell 9025) 9020 comes off and stays off.
constexpr s32 BbscSignEffectIds[] = {9005, 9025};

void TickBbscInvadable() {
    static s64 last_ms = 0;
    static size_t added = 0;
    const s64 now = SteadyNowMs();
    if (!BbscInvadeAnywhereEnabled() || !bbsc_duel_bell_ready || bbsc_invadable_effect_rows.empty() ||
        now - last_ms < 2'000) {
        return;
    }
    last_ms = now;
    const s32 state = GetSessionState();
    const u64 player = GetLocalPlayerCharacter();
    const u64 global = GetGlobalState();
    const bool alive = ReadablePointer(player, 0x410) && GetCharacterHp(player) > 0;
    const bool global_ok = ReadablePointer(global, 0x1598);
    const bool loading = global_ok && ReadValue<u8>(global, 0x08) != 0;
    const bool has = alive && BbscLocalEffectActive(bbsc_invadable_effect_rows);
    s32 sign = 0;
    for (const s32 id : BbscSignEffectIds) {
        if (alive && BbscLocalEffectNode(id) != 0) {
            sign = id;
        }
    }
    // Test 2026-10-05: no "world open" line at all - say every 30 s what keeps it shut.
    static s64 last_why_ms = 0;
    if (now - last_why_ms > 30'000) {
        last_why_ms = now;
        LOG_INFO(Debug, "BBSC invasions: state {} alive {} loading {} own world {:#x} in another "
                        "world {} spectating {} sign effect {} -> SpEffect {} {}",
                 state, alive, loading, global_ok ? ReadValue<u8>(global, 0x1592) : 0xFF,
                 BbscInAnotherWorld(), bbsc_spectate_active, sign, BbscInvadableEffectId,
                 has ? "on" : "off");
    }
    if (sign != 0) {
        if (has) {
            const bool erased = BbscEraseLocalEffect(static_cast<s32>(BbscInvadableEffectId));
            LOG_INFO(Debug, "BBSC invasions: sign out (SpEffect {}) - world closed to invaders "
                            "while it is (SpEffect {} taken off: {})",
                     sign, BbscInvadableEffectId, erased);
        }
        return;
    }
    if ((state != 0 && state != 3) || !alive || bbsc_spectate_active || !global_ok || loading ||
        BbscInAnotherWorld()) {
        return; // a guest / invader, dead, loading, or in someone else's world
    }
    if (has) {
        return;
    }
    const u64 vtable = ReadValue<u64>(player, 0);
    const u64 add = ReadablePointer(vtable, 0x3F8) ? ReadValue<u64>(vtable, 0x3F0) : 0;
    if (!IsEbootCodeAddress(add)) {
        return;
    }
    // AddSpEffect(u64 chr, s32 id, u64 source, s32, s32, s32, float x5, s32): five float arguments,
    // which GuestCall (integers only) cannot pass under a guest CPU: Core::CallGuestWithFloats
    // (bbport-android; on an x86-64 host the same call, through a guest stub).
    BbNet::EnsureGuestThread();
    {
        const std::array<std::uint64_t, 7> ints{
            player, static_cast<std::uint64_t>(static_cast<s64>(static_cast<s32>(BbscInvadableEffectId))),
            player, 0, 0, 0, 0};
        const std::array<float, 5> floats{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        CallGuestWithFloats(add, ints, floats);
    }
    if (added++ < 20) {
        LOG_INFO(Debug, "BBSC invasions: world open to invaders (SpEffect {} added, session state {}, "
                        "now on: {})",
                 BbscInvadableEffectId, state, BbscLocalEffectActive(bbsc_invadable_effect_rows));
    }
}

void TickBbscHostBlank() {
    static bool was = false;
    const bool now = bbsc_duel_bell_ready && !bbsc_blank_effect_rows.empty() && GetSessionState() == 3 &&
                     BbscLocalEffectActive(bbsc_blank_effect_rows);
    // Test 2026-10-06 (host abady): with the session patches suspended here, the room timeout 10 s
    // after the Blank (0x01789B60: hosting, alone, timer out -> CSMultiPlayMan::Stop) ran with the
    // stage Stop sites back to vanilla, a StageTransition followed and the host landed on the title
    // with a grey screen. The same host's room timeout at 21:16:54 with the patches ON went to
    // session state 0 cleanly, no load. The game's own Blank already removes the guests (and each
    // guest's "sent home" path suspends on the guest's side), so the host keeps its patches.
    static s64 blank_ms = 0;
    if (now && !was) {
        blank_ms = SteadyNowMs();
        LOG_INFO(Debug, "BBSC leave: host leave (Blank, guest leave or session end) - the guests go home, the "
                        "host's room closes on its own timeout (session patches stay on)");
    }
    was = now;
    // Test 2026-10-08 (host): after his Blank "he couldn't use anything until he exited, then
    // it put him somewhere". The leave script (OnLeaveMagic -> OnLeaveMenu_Yes) calls SetLoadWait
    // (0x013358F0: Lua event object +0x8D = 1, "wait for the load") for everyone, then LeaveSession;
    // a guest's load home clears it, but the host stays in its world - no load - and everything
    // that waits on it stayed blocked until his death reload at 20:13. Once the room is closed
    // (state 0, own world, no load running), the wait is lifted for the host.
    if (blank_ms != 0) {
        const s64 t = SteadyNowMs();
        const u64 global = GetGlobalState();
        const u64 lua_man = ReadValue<u64>(image_base + 0x0553B0C8, 0);
        const u64 lua = ReadablePointer(lua_man, 0x10) ? ReadValue<u64>(lua_man, 8) : 0;
        if (t - blank_ms > 120'000) {
            blank_ms = 0;
        } else if (GetSessionState() == 0 && ReadablePointer(global, 0x1598) &&
                   ReadValue<u8>(global, 0x1592) != 0 && ReadValue<u8>(global, 0x08) == 0 &&
                   ReadablePointer(lua, 0x90) && ReadValue<u8>(lua, 0x8D) != 0) {
            WriteValue<u8>(lua, 0x8D, 0);
            blank_ms = 0;
            LOG_INFO(Debug, "BBSC leave: the host's Blank left the game waiting for a load that never "
                            "comes (Lua +0x8D) - lifted, the host can act again");
        }
    }
}

// The guest's Parting Blank (2026-10-07). Report: "can't blank (guest)". Guest log 20:42:58-20:43:09,
// a minute after Cleric Beast died: four uses of goods 11251, the use ran (item state 3), and the
// game never raised OnLeaveMagic - the same item at 20:14:54 that day raised it 0.9 s after the use.
// OnLeaveMagic comes from the character's SpEffect walk 0x018FEF50: an entry with the "leave" bit
// (param +0x15F & 0x400000, SpEffect 7) and chr->vtable[0x1B8](chr) true, then for a guest (state 6)
// 0x01339870(lua, "OnLeaveMagic") and 0x014EB860(). What failed after the boss kill is not known
// yet (HYPOTHESIS: the boss-clear state BBSC keeps for the guest). So when the guest's Blank was used
// and the game's leave did not follow within BbscGuestBlankWaitMs, BBSC makes that same call, and
// logs the effect entry it found so the next log shows which step stopped.
constexpr u32 BbscPartingBlankGoodsId = 11251;
constexpr s64 BbscGuestBlankWaitMs = 3'000;

void TickBbscGuestBlank() {
    static s64 used_ms = 0;
    static bool was_using = false;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || GetSessionState() != 6) {
        used_ms = 0;
        was_using = false;
        return;
    }
    const u64 player = GetLocalPlayerCharacter();
    if (!ReadablePointer(player, 0x498) || GetCharacterHp(player) <= 0 || bbsc_spectate_active) {
        used_ms = 0;
        was_using = false;
        return;
    }
    const bool item = ReadValue<u32>(player, 0x1C0) == BbscPartingBlankGoodsId &&
                      ReadValue<u32>(player, 0x490) == 3;
    const bool effect = bbsc_duel_bell_ready.load(std::memory_order_acquire) &&
                        !bbsc_blank_effect_rows.empty() &&
                        BbscLocalEffectActive(bbsc_blank_effect_rows);
    const bool using_blank = item || effect;
    if (using_blank && !was_using && used_ms == 0) {
        used_ms = now;
        LOG_INFO(Debug, "BBSC leave: the guest's Parting Blank is in use (use running {}, SpEffect 7 "
                        "on {}) - waiting for the game's own leave",
                 item, effect);
    }
    was_using = using_blank;
    if (used_ms == 0) {
        return;
    }
    if (bbsc_leave_event_ms.load() >= used_ms) {
        used_ms = 0; // the game's own leave ran (BbscLeaveItemUsed)
        return;
    }
    if (now - used_ms < BbscGuestBlankWaitMs) {
        return;
    }
    used_ms = 0;
    const u64 node = BbscLocalEffectNode(7);
    const u64 param = node != 0 ? ReadValue<u64>(node, 0x48) : 0;
    const u64 lua_holder = ReadValue<u64>(image_base + 0x0553B0C8, 0);
    const u64 lua = ReadablePointer(lua_holder, 0x10) ? ReadValue<u64>(lua_holder, 8) : 0;
    LOG_WARNING(Debug,
                "BBSC leave: the guest's Parting Blank did not start the game's leave in {} s "
                "(SpEffect 7 entry {:#x} flags {:#x}, leave bit {}, event flag 4047 {}, Lua {:#x}) - "
                "BBSC raises OnLeaveMagic itself",
                BbscGuestBlankWaitMs / 1000, node, node != 0 ? ReadValue<u32>(node, 0x1C) : 0,
                ReadablePointer(param, 0x168) &&
                    (ReadValue<u64>(param, 0x15F) & 0x400000) != 0,
                BbscEventFlag(4047), lua);
    // Review 2026-10-08: not while a load runs, and only still a living guest (the game's own leave
    // may have started meanwhile without passing our trace).
    const u64 global_now = GetGlobalState();
    if (lua == 0 || !ReadablePointer(global_now, 0x10) || ReadValue<u8>(global_now, 0x08) != 0 ||
        GetSessionState() != 6 || GetCharacterHp(GetLocalPlayerCharacter()) <= 0) {
        return;
    }
    // As 0x018FF74D..0x018FF759: the Lua event by name, then every remote slot marked as leaving.
    using LuaEvent = void PS4_SYSV_ABI (*)(u64 lua, u64 name);
    using MarkSlotsLeaving = void PS4_SYSV_ABI (*)();
    BbGuestCall(reinterpret_cast<LuaEvent>(image_base + 0x01339870), lua, image_base + 0x0492EA75);
    BbGuestCall(reinterpret_cast<MarkSlotsLeaving>(image_base + 0x014EB860));
}

void SendBbscPvpPacket(u8 kind, bool on) {
    static u32 sequence = 0;
    struct {
        u8 magic[4] = {'S', 'H', 'A', 'D'};
        u8 type = 0x25;
        u8 kind = 0;
        u8 on = 0;
        u8 sequence[4]{};
    } packet;
    static_assert(sizeof(packet) == 11);
    packet.kind = kind;
    packet.on = on ? 1 : 0;
    ++sequence;
    std::memcpy(packet.sequence, &sequence, 4);
    Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet, sizeof(packet));
}

void SetBbscPvp(bool on, std::string_view why) {
    if (bbsc_pvp_on.exchange(on) == on) {
        return;
    }
    LOG_INFO(Debug, "BBSC PvP: {} ({})", on ? "ON - hunters can harm each other" : "off", why);
    BbscAnnounce(on ? u"PvP is ON.\nPlayers can hurt each other."
                    : u"PvP is OFF.",
                 on ? "pvp on" : "pvp off");
}

// Every other player character: team 21 while PvP is on, its own team back when it ends.
void ApplyBbscPvpTeams() {
    const bool on = bbsc_pvp_on;
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!ReadablePointer(world_chr, 0x68)) {
        return;
    }
    const u64 local = ReadValue<u64>(world_chr, 0x60);
    const s32 count = ReadValue<s32>(world_chr, 0x30);
    const u64 entries = ReadValue<u64>(world_chr, 0x38);
    if (on && count > 0 && count <= 64 &&
        ReadablePointer(entries, static_cast<size_t>(count) * 0x38)) {
        for (s32 i = 0; i < count; ++i) {
            const u64 chr = ReadValue<u64>(entries, static_cast<size_t>(i) * 0x38);
            if (chr == 0 || chr == local || !ReadablePointer(chr, 0x90)) {
                continue;
            }
            const u32 team = ReadValue<u32>(chr, 0x88);
            if (team == BbscPvpTeam) {
                continue;
            }
            const bool first = bbsc_pvp_original_team.emplace(chr, team).second;
            WriteValue<u32>(chr, 0x88, BbscPvpTeam);
            static s64 last_log_ms = 0;
            if (first || SteadyNowMs() - last_log_ms > 5'000) {
                last_log_ms = SteadyNowMs();
                LOG_INFO(Debug, "BBSC PvP: player character {:#x} (type {}) team {} -> {}{}", chr,
                         ReadValue<s32>(chr, 0x78), team, BbscPvpTeam,
                         first ? "" : " (the game had set it back)");
            }
        }
    }
    if (!on && !bbsc_pvp_original_team.empty()) {
        for (const auto& [chr, team] : bbsc_pvp_original_team) {
            if (ReadablePointer(chr, 0x90) && ReadValue<u32>(chr, 0x88) == BbscPvpTeam) {
                WriteValue<u32>(chr, 0x88, team);
            }
        }
        LOG_INFO(Debug, "BBSC PvP: {} player character(s) back on their own team",
                 bbsc_pvp_original_team.size());
        bbsc_pvp_original_team.clear();
    }
}

void TickBbscPvp() {
    if (image_base == 0) {
        return;
    }
    StartBbscDuelBell();
    TickBbscDuelBellGive();
    TickBbscHostBlank();
    TickBbscInvadable();
    const s32 state = GetSessionState();
    const s64 now = SteadyNowMs();
    // The bell: one toggle per ring (the effect lasts 1 s).
    static bool bell_was = false;
    const bool bell = bbsc_duel_bell_ready && BbscDuelBellEffectActive();
    if (bell && !bell_was) {
        const bool want = !bbsc_pvp_on;
        if (state == 3) {
            SetBbscPvp(want, "the host rang the Duel Bell");
            SendBbscPvpPacket(0, want);
        } else if (state == 6) {
            SendBbscPvpPacket(1, want);
            LOG_INFO(Debug, "BBSC PvP: the guest rang the Duel Bell - asking the host for {}",
                     want ? "on" : "off");
        } else {
            LOG_INFO(Debug, "BBSC PvP: Duel Bell rung outside co-op (session state {})", state);
            BbscAnnounce(u"PvP needs another player in your game.", "pvp alone");
        }
    }
    bell_was = bell;
    if (state == 3) {
        if (const int asked = bbsc_pvp_request.exchange(-1); asked >= 0) {
            SetBbscPvp(asked != 0, "a guest rang the Duel Bell");
            SendBbscPvpPacket(0, asked != 0);
        }
        static s64 last_send_ms = 0;
        if (now - last_send_ms >= BbscPvpSendMs) {
            last_send_ms = now;
            SendBbscPvpPacket(0, bbsc_pvp_on);
        }
    } else if (state == 6) {
        const int host = bbsc_pvp_host_state;
        if (host >= 0 && now - bbsc_pvp_host_state_ms.load() < 5'000) {
            SetBbscPvp(host != 0, "the host's state");
        }
    } else if (bbsc_pvp_on) {
        SetBbscPvp(false, "the co-op ended");
    }
    ApplyBbscPvpTeams();
}

// Trace: a boss script asked for the vanilla co-op buff (now skipped by the Scaling patches).
void PS4_SYSV_ABI BbscMultiDopingHook(u64 tag, const GuestRegisterSnapshot* registers) {
    (void)tag;
    if (registers == nullptr) {
        return;
    }
    LOG_INFO(Debug,
             "BBSC scaling: Lua_MultiDoping for boss entity {} with {} guest(s) - vanilla 7500/7501 "
             "{}, BBSC scaling {}",
             static_cast<s32>(registers->rdx), GetBbscGuestCount(),
             BbscScalingEnabled() ? "skipped" : "applies", BbscScalingEnabled() ? "applies" : "off");
}

void InstallBbscDeathTraceHooks() {
    if (image_base == 0 || !BbscSpectateEnabled()) {
        return;
    }
    static constexpr std::array<u8, 6> PushR15{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    static constexpr std::array<u8, 6> PushRbxRax{0x55, 0x48, 0x89, 0xE5, 0x53, 0x50};
    static constexpr std::array<u8, 6> PushR14Rbx{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56};
    const std::array<std::tuple<u64, std::span<const u8>, u64>, 3> hooks{{
        {SoloPlayDeathOffset, std::span<const u8>{PushR15}, 0},
        {SoloPlayDeathFirstOffset, std::span<const u8>{PushRbxRax}, 1},
        {PartyGhostDeathOffset, std::span<const u8>{PushR14Rbx}, 2},
    }};
    for (const auto& [offset, prologue, tag] : hooks) {
        // Skip flag: a late step for a death BBSC already revived from (BbscLateDeathStep).
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + offset), prologue, tag,
                                  BbscDeathStepTraceHook, &bbsc_late_death_skip[tag])) {
            LOG_ERROR(Debug, "BBSC death trace: failed to hook {:#x}", offset);
        }
    }
    // 0x0138B2D0: push rbp; mov rbp, rsp; push r15 (same prologue bytes as SoloPlayDeath).
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + FailedBossAreaMissionOffset),
                              std::span<const u8>{PushR15}, 0, BbscFailedBossAreaHook)) {
        LOG_ERROR(Debug, "BBSC boss fight: failed to hook Failed_BossAreaMission {:#x}",
                  FailedBossAreaMissionOffset);
    }
    StartBbscScaling();
    StartBbscSolidNpcs();
    // 0x0138BC70 Lua_MultiDoping: same prologue (push rbp; mov rbp, rsp; push r15).
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + LuaMultiDopingOffset),
                              std::span<const u8>{PushR15}, 0, BbscMultiDopingHook)) {
        LOG_ERROR(Debug, "BBSC scaling: failed to hook Lua_MultiDoping {:#x}", LuaMultiDopingOffset);
    }
    LOG_INFO(Debug, "BBSC death steps traced (SoloPlayDeath {:#x}, _1 {:#x}, PartyGhostDeath {:#x})",
             SoloPlayDeathOffset, SoloPlayDeathFirstOffset, PartyGhostDeathOffset);
}

void InstallBbscSpectateHooks() {
    if (!BbscSpectateEnabled() || image_base == 0) {
        return;
    }
    static constexpr std::array<u8, 6> Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    const std::array<std::pair<u64, std::string_view>, 2> hooks{{
        {SoloPlayDeathFinalOffset, "SoloPlayDeath_2"},
        {PartyGhostDeathFinalOffset, "PartyGhostDeath_2"},
    }};
    for (u64 tag = 0; tag < hooks.size(); ++tag) {
        const auto& [offset, name] = hooks[tag];
        // The step runs, only its reload call is retargeted (Group::Spectate). The skip flag is
        // set only for a late step after a BBSC revive (BbscLateDeathStep).
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + offset),
                                  std::span<const u8>{Prologue}, tag, BbscSpectateDeathHook,
                                  &bbsc_late_final_skip[tag])) {
            LOG_ERROR(Debug, "BBSC spectate: failed to hook {} at {:#x}", name, offset);
            return;
        }
    }
    LOG_INFO(Debug, "BBSC spectate enabled (SoloPlayDeath_2 {:#x}, PartyGhostDeath_2 {:#x})",
             SoloPlayDeathFinalOffset, PartyGhostDeathFinalOffset);
    InstallBbscBossClearHooks();
}

bool ArmBbscRejoinV7(u64 state, u32 death_selected_map) {
    ArmBbscReinsertV8();
    const u32 summoned_map = ReadValue<u32>(state, 0x14C4);
    if (!IsUsablePackedMap(summoned_map)) {
        LOG_WARNING(Debug, "BBSC rejoin v7 skipped: summoned placement map {:#x} is not usable",
                    summoned_map);
        return true;
    }
    const u64 reload_rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (reload_rules < 0x10000 || !HasMemoryAccess(reload_rules, 0x88, MemoryProt::CpuRead)) {
        LOG_WARNING(Debug, "BBSC rejoin v7 skipped: summon reload rules unavailable");
        return true;
    }
    if (ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0) == 0) {
        LOG_WARNING(Debug, "BBSC rejoin v7 skipped: WorldChrMan unavailable");
        return true;
    }
    const s32 phase_before = ReadValue<s32>(reload_rules, 0x84);
    // v7.1: never call the native here. 0x0131E5C0 freezes the player's OWN world into the save
    // snapshot (the save writer 0x0156AA50 uses global+0x1698/+0x16B8/+0x16D8 while
    // global+0x1592 == 0); called in the host world it would store host-world data in the guest
    // save. Only the session-relevant effect (phase 3) is applied.
    if (!HasMemoryAccess(reload_rules + 0x84, 4, MemoryProt::CpuWrite)) {
        LOG_WARNING(Debug, "BBSC rejoin v7 skipped: summon reload phase not writable");
        return true;
    }
    WriteValue<s32>(reload_rules, 0x84, 3);
    const s32 phase_after = ReadValue<s32>(reload_rules, 0x84);
    bbsc_rejoin_armed_ms = SteadyNowMs();
    bbsc_rejoin_armed_map = 0; // nothing for TickBbscGuestRejoin to track
    LOG_INFO(Debug,
             "BBSC rejoin v7: summon reload snapshot taken at kept death (respawn map {:#x}, "
             "summoned map {:#x}, members={}, reload phase {} -> {})",
             death_selected_map, summoned_map, GetBbscSessionMemberCount(state), phase_before,
             phase_after);
    return phase_after != 3; // phase 3: native reload in progress, no member replay
}

// Called from the persistence no-op cleanup hook. Returns true when the member replay should arm
// for this kept end; false when the rejoin redirected the load (replay would only interfere).
bool ArmBbscRejoin() {
    if (!BbscRejoinEnabled()) {
        return true;
    }
    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    const s32 session_state =
        multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead)
            ? ReadValue<s32>(multi_play, 0x124)
            : -1;
    if (session_state == 3) {
        // Host death: the session survives, but the host's stage load destroys its member tasks
        // (0x01947207, kept alive by the Tasks patch group), so the host stops seeing the guest.
        // The same reinsert pass rebuilds the guest's character after the host respawns; there is
        // no phantom type or summoned reload involved on this side.
        static s64 host_last_ms = 0;
        const s64 host_now = SteadyNowMs();
        if (host_last_ms == 0 || host_now - host_last_ms > BbscRejoinDescriptorTimeoutMs) {
            host_last_ms = host_now;
            ArmBbscReinsertV8();
        }
        return true;
    }
    if (session_state != 6) {
        return true; // only a joined guest rejoins the host world
    }

    const s64 now = SteadyNowMs();
    std::scoped_lock rejoin_lock{bbsc_rejoin_mutex};
    if (bbsc_rejoin_armed_map != 0 &&
        (bbsc_rejoin_deferred || now - bbsc_rejoin_armed_ms < BbscRejoinDescriptorTimeoutMs)) {
        return false; // this load is already redirected
    }
    if (pending_summon_reload_map.load(std::memory_order_acquire) != 0) {
        LOG_INFO(Debug, "BBSC rejoin skipped: an in-world summoned reload is already pending");
        return true;
    }

    const u64 state = GetGlobalState();
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1649, MemoryProt::CpuWrite)) {
        LOG_WARNING(Debug, "BBSC rejoin skipped: global state not writable");
        return true;
    }
    if (ReadValue<u8>(state, 0x08) == 0) {
        LOG_INFO(Debug, "BBSC rejoin skipped: kept end is not inside a stage transition request");
        return true;
    }
    if (pending_summon_reload_state_map.load(std::memory_order_acquire) != 0) {
        LOG_INFO(Debug, "BBSC rejoin skipped: a summoned reload is already pending");
        return true;
    }
    if (BbscRejoinV7Enabled()) {
        // Several kept-end hooks can fire during one death load; snapshot once per load.
        static s64 v7_last_ms = 0;
        if (v7_last_ms != 0 && now - v7_last_ms < BbscRejoinDescriptorTimeoutMs) {
            return false;
        }
        v7_last_ms = now;
        return ArmBbscRejoinV7(state, ReadValue<u32>(state, 0x0C));
    }

    std::optional<SummonPlacementDescriptor> host;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        host = seamless_received_host_placement;
    }
    if (!host.has_value() || !IsUsablePackedMap(host->packed_region)) {
        // Same-map claims usually arrive without a placement header (and a later header-less create
        // response clears it). v4 only needs the host map, which is the map this guest was inserted
        // into when it joined (recorded at SummonedMapReload.Native while joining/joined).
        const u32 joined_map = bbsc_joined_host_map.load(std::memory_order_acquire);
        if (!IsUsablePackedMap(joined_map)) {
            LOG_WARNING(Debug, "BBSC rejoin skipped: no host placement and no recorded join map");
            return true;
        }
        SummonPlacementDescriptor from_join{};
        from_join.packed_region = joined_map;
        host = from_join;
    }
    // v3: never set the forced-warp byte (+0x1520) or call the selector here. v1 and v2 both did,
    // and both hit "Dantelion2 Panic: SprjLuaEventMan: accessed an uninitialized singleton" as a
    // summoned client. Only a same-map respawn is redirected: the death load keeps its own map and
    // spawn, and descriptor finalize swaps the loaded world for the host world (SummonedMapReload).
    const u32 death_selected_map = ReadValue<u32>(state, 0x0C);
    if (death_selected_map != host->packed_region) {
        LOG_INFO(Debug,
                 "BBSC rejoin skipped: respawn map {:#x} differs from host map {:#x} (cross-map "
                 "respawn not supported yet)",
                 death_selected_map, host->packed_region);
        return true;
    }
    // v4: no writes at all. v3 wrote only the summoned placement banks (+0x14C4/+0x14D0/+0x14E0)
    // and still panicked right after the kept death: a summoned client's death path reads that
    // placement ("return to summoned coords") and enters summon event handling while the stage is
    // being torn down. Only the descriptor-finalize reload is armed, and its post-reload placement
    // refresh is suppressed for this request.
    bbsc_rejoin_suppress_placement_refresh.store(true, std::memory_order_release);
    pending_summon_reload_state_map.store(host->packed_region, std::memory_order_release);
    bbsc_rejoin_armed_ms = now;
    bbsc_rejoin_armed_map = host->packed_region;
    LOG_INFO(Debug,
             "BBSC rejoin v4: same-map death load {:#x} armed for summoned reload into host world "
             "(host placement ({:.2f}, {:.2f}, {:.2f}))",
             death_selected_map, host->x, host->y, host->z);
    return false;
}

// Game-thread tick: reports when the descriptor-finalize hook consumed the request, and clears a
// request that was never consumed so it cannot hit a later unrelated same-map load.
void TickBbscGuestRejoin() {
    std::scoped_lock rejoin_lock{bbsc_rejoin_mutex};
    if (bbsc_rejoin_armed_map == 0) {
        return;
    }
    if (pending_summon_reload_state_map.load(std::memory_order_acquire) == 0) {
        // v5: the descriptor hook either ran the reload or (SprjLuaEventMan missing) moved the
        // request to the in-world deferred reload (pending_summon_reload_map).
        if (pending_summon_reload_map.load(std::memory_order_acquire) == bbsc_rejoin_armed_map) {
            if (!bbsc_rejoin_deferred) {
                bbsc_rejoin_deferred = true;
                bbsc_rejoin_armed_ms = SteadyNowMs();
            } else if (SteadyNowMs() - bbsc_rejoin_armed_ms > BbscRejoinDeferredTimeoutMs) {
                u32 expected = bbsc_rejoin_armed_map;
                pending_summon_reload_map.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
                bbsc_rejoin_deferred_request.store(false, std::memory_order_release);
                LOG_WARNING(Debug,
                            "BBSC rejoin v5: in-world reload into {:#x} never became possible; "
                            "request cleared (last result: {})",
                            bbsc_rejoin_armed_map, deferred_summon_reload.result);
                bbsc_rejoin_deferred = false;
                bbsc_rejoin_armed_map = 0;
            }
            return;
        }
        LOG_INFO(Debug, "BBSC rejoin: summoned reload into host world started (map {:#x}, {})",
                 bbsc_rejoin_armed_map,
                 bbsc_rejoin_deferred ? "in-world after respawn" : "at descriptor finalize");
        bbsc_rejoin_deferred = false;
        bbsc_rejoin_armed_map = 0;
        return;
    }
    if (SteadyNowMs() - bbsc_rejoin_armed_ms > BbscRejoinDescriptorTimeoutMs) {
        u32 expected = bbsc_rejoin_armed_map;
        if (pending_summon_reload_state_map.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            bbsc_rejoin_suppress_placement_refresh.store(false, std::memory_order_release);
            LOG_WARNING(Debug,
                        "BBSC rejoin: load descriptor never matched host map {:#x}; request cleared",
                        bbsc_rejoin_armed_map);
        }
        bbsc_rejoin_armed_map = 0;
    }
}

// ---- Up to five hunters (2026-09-27, untested with more than two players) -----------------------
// Engine capacity (CONFIRMED statically + host capture 2026-09-26): the session object [0x0553D6D0]
// is built with max members +0xC = 5 (0x017875D0); the member list [global+0x16F8] has 5 entries
// (+0x1C, 0x14 each; +0x08 cooperators without the host, +0x0C invaders, +0x10 others, +0x14 all);
// the game asks the room server for 5 slots (shadnet log: "max= 5"). The summon builder 0x01874710
// accepts a sign while members < 5, remote players + pending < 4 (0x01874BBE) and, for a co-op
// sign, cooperators + pending < summon manager +0x1FC (constructor 0x01870C50 sets 3; +0x200 = 3
// invaders). Retail lets in only 2 guests, so another limit exists that is not found yet. Here the
// builder's cooperator maximum goes to 4 (host + 4 = 5 hunters) and every build logs the counts,
// so the next session with three or more guests shows where a sign is still turned away.
bool BbscHuntersEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_FIVE_HUNTERS");
    return enabled;
}

// ---- Discord status (BBSC mod) ------------------------------------------------------------------
// Gathered on the game tick once a second and copied out by GetBbscPresence (the mod's Discord
// thread). Names are the game's own English text: place names = item.msgbnd 19 (id = area*100 +
// block*10, e.g. m24_01 -> 2410 Central Yharnam), boss names = item.msgbnd 18 (NpcName), whose id is
// what SetBossGauge (0x01336C80) stores at gauge slot +0x1658 next to the handle (+0x165C).
constexpr std::array<std::pair<s32, const char*>, 21> BbscPlaceNames{{
    {2100, "Hunter's Dream"},
    {2110, "Abandoned Old Workshop"},
    {2200, "Hemwick Charnel Lane"},
    {2300, "Old Yharnam"},
    {2400, "Cathedral Ward"},
    {2410, "Central Yharnam"},
    {2411, "Iosefka's Clinic"},
    {2420, "Upper Cathedral Ward"},
    {2421, "Healing Church Workshop"},
    {2422, "Altar of Despair"},
    {2500, "Forsaken Cainhurst Castle"},
    {2600, "Nightmare of Mensis"},
    {2700, "Forbidden Woods"},
    {2800, "Yahar'gul, Unseen Village"},
    {3200, "Byrgenwerth"},
    {3201, "Lecture Building"},
    {3202, "Moonside Lake"},
    {3300, "Nightmare Frontier"},
    {3400, "Hunter's Nightmare"},
    {3500, "Research Hall"},
    {3600, "Fishing Hamlet"},
}};
constexpr std::array<std::pair<s32, const char*>, 87> BbscBossNames{{
    {1000, "Madman's Escort"},
    {1010, "Forgotten Madman"},
    {6400, "Old Hunter Henryk"},
    {6500, "Valtr, Master of the League"},
    {6505, "Valtr, Beast Eater"},
    {6530, "Yamamura the Wanderer"},
    {6540, "Younger Madaras Twin"},
    {6550, "Mensis Scholar Damian"},
    {6600, "Henriett"},
    {6640, "Defector Antal"},
    {7040, "Tomb Prospector Olek"},
    {7045, "Tomb Prospector Gremia"},
    {7060, "Beastclaw Jozef"},
    {7160, "Queen Killer"},
    {7300, "Vitus"},
    {7310, "Madman Wallar"},
    {101000, "Degenerate Beast"},
    {106000, "Brainsucker"},
    {127000, "Maneater Boar"},
    {209000, "Blood-starved Beast"},
    {209010, "Blood-starved Beast"},
    {210000, "The Witch of Hemwick"},
    {212010, "Shadow of Yharnam"},
    {212020, "Shadow of Yharnam"},
    {212030, "Shadow of Yharnam"},
    {216000, "Keeper of the Old Lords"},
    {218000, "Loran Silverbeast"},
    {232000, "Martyr Logarius"},
    {251000, "Ebrietas, Daughter of the Cosmos"},
    {251010, "Ebrietas, Daughter of the Cosmos"},
    {257000, "Celestial Emissary"},
    {257010, "Celestial Emissary"},
    {271000, "Father Gascoigne"},
    {272000, "Father Gascoigne"},
    {304000, "Merciless Watcher"},
    {304001, "Watcher Chieftain"},
    {304002, "Merciless Watcher"},
    {304003, "Merciless Watcher"},
    {305000, "Pthumerian Descendant"},
    {305001, "Pthumerian Descendant"},
    {305002, "Pthumerian Elder"},
    {306000, "Pthumerian Bride"},
    {306001, "Yharnam, Pthumerian Queen"},
    {313000, "Undead Giant"},
    {403000, "Living Failures"},
    {450000, "Laurence, the First Vicar"},
    {451000, "Ludwig the Accursed"},
    {451005, "Ludwig, the Holy Blade"},
    {452000, "Lady Maria of the Astral Clocktower"},
    {453000, "Orphan of Kos"},
    {454000, "Orphan of Kos"},
    {500000, "Cleric Beast"},
    {501000, "Watchdog of the Old Lords"},
    {502000, "Vicar Amelia"},
    {504000, "Loran Darkbeast"},
    {504010, "Abhorrent Beast"},
    {507000, "The One Reborn"},
    {508000, "Darkbeast Paarl"},
    {508010, "Loran Darkbeast"},
    {509000, "Bloodletting Beast"},
    {509010, "Bloodletting Beast"},
    {510000, "Rom, the Vacuous Spider"},
    {510010, "Rom, the Vacuous Spider"},
    {511000, "Great One Beast"},
    {512000, "Amygdala"},
    {512010, "Amygdala"},
    {540000, "Moon Presence"},
    {551000, "Mergo's Wet Nurse"},
    {551010, "Mergo's Wet Nurse"},
    {750000, "Beast-possessed Soul"},
    {802000, "Slug Princess"},
    {803000, "Heroine Doll"},
    {804000, "Gehrman, the First Hunter"},
    {805000, "Gehrman, the First Hunter"},
    {899000, "Micolash, Host of the Nightmare"},
    {899010, "Vileblood Hunter Alfred"},
    {900020, "The Vileblood"},
    {900030, "The Executioners"},
    {900040, "Hunter of Hunters"},
    {901040, "Nameless Tomb Prospector"},
    {901060, "Izzy's Admirer"},
    {901070, "Nameless Madman"},
    {901110, "Bone Ash Hunter Carla"},
    {901150, "Drifting Vileblood Leo"},
    {901160, "Ardeo Man"},
    {901170, "Defeated Hunter"},
    {901180, "Choir Intelligencer Edgar"},
}};

const char* FindBbscName(std::span<const std::pair<s32, const char*>> names, s32 id) {
    for (const auto& [key, text] : names) {
        if (key == id) {
            return text;
        }
    }
    return nullptr;
}

const char* BbscBossNameById(s32 id) {
    return FindBbscName(BbscBossNames, id);
}

std::string BbscAreaName(u32 packed_map) {
    const s32 area = static_cast<s32>((packed_map >> 24) & 0xFF);
    const s32 block = static_cast<s32>((packed_map >> 16) & 0xFF);
    if (area == 29) {
        return "Chalice Dungeon";
    }
    if (const char* name = FindBbscName(BbscPlaceNames, area * 100 + block * 10)) {
        return name;
    }
    if (const char* name = FindBbscName(BbscPlaceNames, area * 100)) {
        return name;
    }
    return fmt::format("m{:02}_{:02}", area, block);
}

std::mutex bbsc_presence_mutex;
BbscPresence bbsc_presence;
s64 bbsc_presence_tick_ms = 0;
s32 bbsc_presence_boss_id = -1;

void TickBbscPresence() {
    const s64 now = SteadyNowMs();
    if (now - bbsc_presence_tick_ms < 1000) {
        return;
    }
    bbsc_presence_tick_ms = now;

    BbscPresence next;
    const u32 map = GetCurrentPackedMap();
    next.in_game = IsUsablePackedMap(map);
    if (next.in_game) {
        next.area = BbscAreaName(map);
    }
    next.max_hunters = BbscHuntersEnabled() ? 5 : 3;
    const s32 state = GetSessionState();
    next.role = state == 3 ? 1 : state == 6 ? 2 : 0;
    if (next.role != 0) {
        const s32 members = GetBbscSessionMemberCount(GetGlobalState());
        next.hunters = std::clamp(members, 2, next.max_hunters);
    }
    // 2026-10-06: no test has had more than one guest in the world yet (every log: "cooperators 1
    // members 2"). Each change of the head count is logged, on host and guests alike, so a 4- or
    // 5-player test shows who got in, when, and who dropped.
    {
        static s32 last_key = -1;
        const u64 global = GetGlobalState();
        const u64 population = ReadablePointer(global, 0x1700) ? ReadValue<u64>(global, 0x16F8) : 0;
        if (ReadablePointer(population, 0x18)) {
            const s32 coop = ReadValue<s32>(population, 0x08);
            const s32 inv = ReadValue<s32>(population, 0x0C);
            const s32 all = ReadValue<s32>(population, 0x14);
            const s32 key = state * 10000 + coop * 1000 + inv * 100 + all;
            if (key != last_key) {
                last_key = key;
                LOG_INFO(Debug,
                         "BBSC hunters: {} in the world - {} cooperator(s), {} invader(s), session "
                         "state {} ({})",
                         all, coop, inv, state,
                         state == 3 ? "hosting" : state == 6 ? "guest" : "alone");
            }
        }
    }

    s32 boss_id = -1;
    const u64 gauge = ReadValue<u64>(image_base + BossGaugePointerOffset, 0);
    if (next.in_game && ReadablePointer(gauge, 0x1690)) {
        for (int slot = 0; slot < 3; ++slot) {
            const size_t at = static_cast<size_t>(slot) * 0x14;
            if (ReadValue<s32>(gauge, 0x165C + at) != -1) {
                boss_id = ReadValue<s32>(gauge, 0x1658 + at);
                break;
            }
        }
    }
    std::scoped_lock lock{bbsc_presence_mutex};
    if (boss_id != -1) {
        const char* name = FindBbscName(BbscBossNames, boss_id);
        next.boss = name != nullptr ? name : "a boss";
        next.boss_since_s = boss_id == bbsc_presence_boss_id && bbsc_presence.boss_since_s != 0
                                ? bbsc_presence.boss_since_s
                                : std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
        if (boss_id != bbsc_presence_boss_id) {
            LOG_INFO(Debug, "BBSC Discord status: boss bar {} ({})", boss_id, next.boss);
        }
    }
    bbsc_presence_boss_id = boss_id;
    bbsc_presence = std::move(next);
}


void ApplyBbscHunterCap(u64 summon_manager) {
    if (!BbscHuntersEnabled() || !ReadablePointer(summon_manager, 0x208) ||
        !HasMemoryAccess(summon_manager + 0x1FC, 4, MemoryProt::CpuWrite)) {
        return;
    }
    const s32 before = ReadValue<s32>(summon_manager, 0x1FC);
    if (before >= 1 && before < 4) {
        WriteValue<s32>(summon_manager, 0x1FC, 4);
    }
    const u64 global = GetGlobalState();
    const u64 population = ReadablePointer(global, 0x1700) ? ReadValue<u64>(global, 0x16F8) : 0;
    const u64 session = ReadValue<u64>(image_base + 0x0553D6D0, 0);
    if (!ReadablePointer(population, 0x18)) {
        return;
    }
    const s32 coop = ReadValue<s32>(population, 0x08);
    const s32 inv = ReadValue<s32>(population, 0x0C);
    const s32 other = ReadValue<s32>(population, 0x10);
    const s32 all = ReadValue<s32>(population, 0x14);
    static s32 logged = -1;
    const s32 key = coop * 1000 + inv * 100 + other * 10 + all;
    if (key != logged) {
        logged = key;
        LOG_INFO(Debug,
                 "BBSC hunters: summon build - cooperators {} invaders {} others {} members {} "
                 "(max members {}, max cooperators {} -> {}, max invaders {})",
                 coop, inv, other, all, ReadablePointer(session, 0x10) ? ReadValue<s32>(session, 0xC) : -1,
                 before, ReadValue<s32>(summon_manager, 0x1FC), ReadValue<s32>(summon_manager, 0x200));
    }
}

// ---- The Beckoning Bell without Insight (2026-09-27) -------------------------------------------
// EquipParamGoods row 200 (Beckoning Bell) has consumeHeroPoint = 1 (row byte 0x38); no other bell
// has an Insight cost. The loaded row is found with the game's own lookup 0x01F1E3D0(out, id)
// (SoloParamRepository [0x05540340]) and the byte set to 0: ringing it neither needs nor uses Insight.
void TickBbscFreeBell() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_FREE_BELL");
    static s64 last_ms = 0;
    static bool done = false;
    if (!enabled || done || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_ms < 2'000) {
        return;
    }
    last_ms = now;
    const u64 repo = ReadValue<u64>(image_base + 0x05540340, 0);
    if (!ReadablePointer(repo, 0x158) || ReadValue<s32>(repo, 0x148) == 0 ||
        ReadValue<u64>(repo, 0x150) == 0) {
        return; // params not loaded yet
    }
    struct alignas(8) LookupResult {
        u32 id;
        u32 pad;
        u64 row;
    } result{0xFFFFFFFFu, 0, 0};
    using Lookup = void PS4_SYSV_ABI (*)(LookupResult* out, u32 id);
    BbGuestCall(reinterpret_cast<Lookup>(image_base + 0x01F1E3D0), &result, 200);
    if (!ReadablePointer(result.row, 0x40) ||
        !HasMemoryAccess(result.row + 0x38, 1, MemoryProt::CpuWrite)) {
        return;
    }
    const u8 cost = ReadValue<u8>(result.row, 0x38);
    if (cost == 1) {
        WriteValue<u8>(result.row, 0x38, 0);
    }
    done = true;
    LOG_INFO(Debug, "BBSC free bell: Beckoning Bell Insight cost {} -> {}", cost,
             ReadValue<u8>(result.row, 0x38));
}

void PS4_SYSV_ABI TraceEntry(u64 tag, const GuestRegisterSnapshot* registers) {
    if (tag >= Sites.size() || registers == nullptr) {
        return;
    }

    const auto& site = Sites[tag];
    if (site.offset == SummonBuildEntryOffset) {
        ApplyBbscHunterCap(registers->rdi);
    }
    if (summon_build_host_placement_hook_installed && site.offset == SummonBuildEntryOffset) {
        ApplyCrossMapSummonHostPlacement(*registers);
    }
    if (cross_map_guest_handoff_hook_installed && site.offset == CrossMapGuestHandoffOffset) {
        ApplyCrossMapSummonGuestPlacement(*registers);
    }
    if (summon_reload_state_hook_installed && site.offset == StageWarpDescriptorFinalizeOffset) {
        ApplySummonReloadStateAtDescriptor(*registers);
    }
    if (site.offset == 0x0178D9C0) {
        BbscConnectFirstReloadFinished();
    }
    if (site.offset == 0x0178D9C0 && BbscRejoinEnabled()) {
        RestoreBbscSignTypeBeforeReloadFinish();
    }
    if (site.offset == 0x0178D9C0) {
        RestoreBbscOwnWorldAtLoad(); // only when the load is home (see "Own world journal")
        ApplyBbscHostWorldFlagsAtLoad();
        ApplyBbscGuestLootViewAtLoad();
    }
    if (site.offset == 0x01938F65) { // Warp.RespawnPoint.Resolve: a load picks its spawn point
        RestoreBbscOwnWorldAtLoad();
        ApplyBbscSharedProgressAtLoad();
    }
    if (site.offset == 0x01332C1B) { // SummonedPlacement.SelectMap: a load picks its map
        GuardBbscStrayForcedWarp(*registers);
    }
    if (site.offset == BbscPersistNoOpOffset) {
        if (ArmBbscRejoin()) {
            ArmBbscReinsert();
        }
    }
    if (site.offset == BbscGameTickOffset) {
        bbsc_tick_map.store(GetCurrentPackedMap(), std::memory_order_release);
        TickBbscRelayHostLock();
        TickBbscBossRewards();
        TickBbscGuestDrops();
        TickBbscSendBacklog();
        TickBbscLeaveLocks();
        TickBbscStuckLeaveLocks();
        TickBbscStrandedGuest();
        TickBbscBossLine();
        TickBbscProgressBroadcast();
        TickBbscOwnWorld();
        TickBbscEchoCounter();
        TickBbscSharedProgress();
        TickBbscSpectate();
        TickBbscFollowBroadcast();
        TickBbscWorldFlagsBroadcast();
        TickBbscFollowHost();
        TickBbscLoadCover();
        TickBbscHpPenalty();
        TickBbscLampRole();
        TickBbscMessages();
        TickBbscRejoinLines();
        TickBbscDream();
        TickBbscItemSurvey();
        TickBbscGuestItemsPickable();
        TickBbscUnstuckKey();
        TickBbscLifeBroadcast();
        TickBbscSessionEnd();
        TickBbscStrayForcedWarp();
        TickBbscGuestBlank();
        TickBbscDeadWatchdog();
        TickBbscDeathCurtain();
        TickBbscStageWatch();
        TickBbscHostDeathNotice();
        TickBbscBossUnlock();
        CaptureBbscGuestLoot();
        TickBbscGuestLootReset();
        TickBbscDreamStateRestore();
        TickBbscFreeBell();
        TickBbscEchoTrace();
        TickBbscMapTrace();
        TickBbscPresence();
        TickBbscScaling();
        TickBbscScaleBroadcast();
        TickBbscScaledMaxHp();
        TickBbscPvp();
        if (BbscSpectateEnabled()) {
            TickBbscRemoteRevive();
            TickBbscDeathLockClear();
            TickBbscStateDiff();
            TickBbscJoinTeleport();
        }
        TickBbscGuestRejoin();
        if (BbscRejoinEnabled()) {
            TrackBbscSignType();
            TickBbscReinsertV8();
            TickBbscSelfHeal();
            static s64 last_shape_ms = 0;
            if (const s64 now = SteadyNowMs(); now - last_shape_ms > 1'000) {
                last_shape_ms = now;
                LogBbscSessionShape("tick");
            }
        }
    }
    if (site.offset == 0x0131E5C0) { // SummonedMapReload.Native: remember the joined host map
        BbscFillConnectFirstReceivedPlacement();
        const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
        const s32 session_state =
            multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead)
                ? ReadValue<s32>(multi_play, 0x124)
                : -1;
        const u32 current_map = GetCurrentPackedMap();
        if ((session_state == 4 || session_state == 6) && IsUsablePackedMap(current_map)) {
            bbsc_joined_host_map.store(current_map, std::memory_order_release);
        }
    }
    if (deferred_summon_reload_hook_installed && site.offset == SosStatusUpdateOffset) {
        RefreshSeamlessLocalPlacement();
        // BBSC isolation switch: SHADPS4_BBSC_NO_GUEST_HOOKS=1 skips the fork's per-tick guest
        // pre-match warp, deferred reload and responder re-ring (byte patches stay installed).
        static const bool no_guest_hooks = EnvFlagEnabled("SHADPS4_BBSC_NO_GUEST_HOOKS");
        if (!no_guest_hooks) {
            if (!BbscConnectFirstEnabled()) { // connect-first replaces the teleport-first warp
                ApplyPreMatchCrossMapGuestWarp();
            }
            ApplyDeferredSummonReload();
            ResumePreMatchCrossMapResponder();
        }
        MaybeRunBbscReinsert();
    }
    if (healing_fountain_host_availability_hook_installed &&
        site.offset == HealingFountainAvailabilityOffset) {
        ApplyHealingFountainHostAvailability(*registers);
    }
    if (site.kind == TraceKind::LuaSessionEvent) {
        // Only session-relevant Lua events; the dispatcher also runs gameplay events.
        const std::string name = ReadGuestAscii(registers->rsi, 64);
        static constexpr std::array<std::string_view, 8> Interesting{
            "Session", "Kick", "Room", "Match", "Multiplay", "Summon", "Reload", "Leave"};
        if (!std::ranges::any_of(Interesting, [&name](std::string_view part) {
                return name.find(part) != std::string::npos;
            })) {
            return;
        }
    }
    if (site.kind == TraceKind::ActionFlags && !HasActionTransition(registers->rdi)) {
        return;
    }
    if (site.kind == TraceKind::GoodsParamLookup &&
        !IsTrackedGoods(static_cast<s32>(registers->rsi))) {
        return;
    }
    if (site.offset == CharacterWarpSetOrCopyFloorOffset) {
        const u64 return_address = ReadValue<u64>(registers->rsp, 0);
        if (return_address >= image_base) {
            const u64 return_offset = return_address - image_base;
            if (std::ranges::contains(BackgroundFloorCopyReturnOffsets, return_offset)) {
                return;
            }
        }
    }
    if ((site.kind == TraceKind::UseItemExecution || site.kind == TraceKind::GoodsActionSubmit) &&
        !IsTrackedGoods(GetUseItemExecutionGoods(site, *registers))) {
        return;
    }
    if (site.kind == TraceKind::SosValidityCode) {
        observed_sos_area.store(static_cast<s32>(registers->rax), std::memory_order_relaxed);
    }
    s32 responder_goods = -1;
    if (site.kind == TraceKind::ResponderBellAvailability) {
        if (site.offset == 0x0157F686) {
            responder_goods = static_cast<s32>(registers->r12);
            if (responder_goods != 205 && responder_goods != 225) {
                return;
            }
            responder_availability_frame = registers->rbp;
            responder_availability_goods = responder_goods;
        } else {
            if (responder_availability_frame != registers->rbp) {
                return;
            }
            responder_goods = responder_availability_goods;
            if (site.offset == 0x015800D1) {
                responder_availability_frame = 0;
                responder_availability_goods = -1;
            }
        }
    }

    const u64 hit = site_hits[tag].fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 dense_capture_limit =
        site.kind == TraceKind::ActionFlags || site.kind == TraceKind::GoodsParamLookup ? 2048 : 64;
    if (hit > dense_capture_limit && hit % 300 != 0) {
        return;
    }

    const u64 sequence = event_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    try {
        std::scoped_lock lock{capture_mutex};
        if (!capture_file) {
            return;
        }
        capture_file << "{\"type\":\"entry\",\"seq\":" << sequence << ",\"time_ms\":" << timestamp
                     << ",\"site\":\"" << site.name << "\",\"offset\":\"0x" << std::hex
                     << site.offset << std::dec << "\",\"hit\":" << hit << ",\"registers\":{";
        capture_file << "\"rax\":";
        WriteHex(capture_file, registers->rax);
        capture_file << ",\"rdi\":";
        WriteHex(capture_file, registers->rdi);
        capture_file << ",\"rsi\":";
        WriteHex(capture_file, registers->rsi);
        capture_file << ",\"rdx\":";
        WriteHex(capture_file, registers->rdx);
        capture_file << ",\"rcx\":";
        WriteHex(capture_file, registers->rcx);
        capture_file << ",\"r8\":";
        WriteHex(capture_file, registers->r8);
        capture_file << ",\"r9\":";
        WriteHex(capture_file, registers->r9);
        capture_file << ",\"rsp\":";
        WriteHex(capture_file, registers->rsp);
        capture_file << '}';

        switch (site.kind) {
        case TraceKind::SosCondition:
            WriteQwords(capture_file, "condition_qwords", registers->rdi, 8);
            break;
        case TraceKind::SosValidityDispatch: {
            capture_file << ",\"validity_dispatch\":{";
            capture_file << "\"player\":";
            WriteHex(capture_file, registers->rbx);
            const u64 vtable = ReadValue<u64>(registers->rbx, 0);
            const u64 function = ReadValue<u64>(vtable, 0x628);
            capture_file << ",\"vtable\":";
            WriteHex(capture_file, vtable);
            capture_file << ",\"function\":";
            WriteHex(capture_file, function);
            if (function >= image_base &&
                function - image_base < MemoryPatcher::g_eboot_image_size) {
                capture_file << ",\"function_offset\":\"0x" << std::hex << function - image_base
                             << std::dec << '\"';
            }
            capture_file << '}';
            break;
        }
        case TraceKind::SosValidityCode:
            capture_file << ",\"validity_code\":{";
            capture_file << "\"previous\":" << static_cast<s64>(static_cast<s32>(registers->r15));
            capture_file << ",\"current\":" << static_cast<s64>(static_cast<s32>(registers->rax));
            capture_file << ",\"object\":";
            WriteHex(capture_file, registers->r14);
            capture_file << ",\"source\":";
            WriteHex(capture_file, registers->rbx);
            capture_file << '}';
            WriteQwords(capture_file, "condition_qwords", registers->r14, 6);
            break;
        case TraceKind::SosEvent: {
            const u64 state = GetGlobalState();
            capture_file << ",\"sos_event\":{";
            capture_file << "\"global_state\":";
            WriteHex(capture_file, state);
            if (state >= 0x10000) {
                capture_file << ",\"warp_1520\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1520));
                capture_file << ",\"menu_1544\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1544));
                capture_file << ",\"transition_1547\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1547));
                capture_file << ",\"active_1578\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1578));
            }
            capture_file << '}';
            WritePlacementState(capture_file, state);
            break;
        }
        case TraceKind::MatchingEvent:
            WriteQwords(capture_file, "event_qwords", registers->rdx, 6);
            break;
        case TraceKind::MatchingSignal:
            break;
        case TraceKind::MatchingLeaveDecision: {
            const u64 return_address = ReadValue<u64>(registers->rsp, 0);
            const u64 reload_state =
                ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
            capture_file << ",\"matching_leave_decision\":{";
            const bool multi_play_site = site.offset == MultiPlayStageUidPolicyOffset ||
                                         site.offset == MultiPlayStopRequestOffset;
            const bool stop_task_site = site.offset == MatchingStopTaskEnqueueOffset;
            const u64 object = registers->rdi;
            capture_file << "\"object\":";
            WriteHex(capture_file, object);
            capture_file << ",\"return_address\":";
            WriteHex(capture_file, return_address);
            capture_file << ",\"caller_offset\":";
            WriteHex(capture_file, return_address >= image_base ? return_address - image_base : 0);
            capture_file << ",\"reload_state\":";
            WriteHex(capture_file, reload_state);
            capture_file << ",\"reload_phase\":"
                         << (reload_state >= 0x10000 ? ReadValue<s32>(reload_state, 0x84) : -1);
            capture_file << ",\"current_map\":";
            WriteHex(capture_file, GetCurrentPackedMap());
            if (multi_play_site && object >= 0x10000 &&
                HasMemoryAccess(object, 0x288, MemoryProt::CpuRead)) {
                const u64 matching_controller = ReadValue<u64>(object, 0x18);
                capture_file << ",\"multi_play_state\":" << ReadValue<s32>(object, 0x124);
                capture_file << ",\"multi_play_aux_state\":" << ReadValue<s32>(object, 0x120);
                capture_file << ",\"matching_controller\":";
                WriteHex(capture_file, matching_controller);
                capture_file << ",\"stop_completion_task\":";
                WriteHex(capture_file, ReadValue<u64>(object, 0x280));

                if (site.offset == MultiPlayStageUidPolicyOffset) {
                    const u64 local_state_root =
                        ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
                    const u64 local_state =
                        local_state_root >= 0x10000 &&
                                HasMemoryAccess(local_state_root, 0x68, MemoryProt::CpuRead)
                            ? ReadValue<u64>(local_state_root, 0x60)
                            : 0;
                    const u64 stage_manager =
                        registers->rbp >= 0x148 + 0x10000 &&
                                HasMemoryAccess(registers->rbp - 0x148, sizeof(u64),
                                                MemoryProt::CpuRead)
                            ? ReadValue<u64>(registers->rbp - 0x148, 0)
                            : 0;
                    capture_file << ",\"stage_uid\":";
                    WriteHex(capture_file, static_cast<u32>(registers->r13));
                    capture_file << ",\"cached_stage_uid\":";
                    WriteHex(capture_file, local_state >= 0x10000 &&
                                                   HasMemoryAccess(local_state + 0x3F8, sizeof(u32),
                                                                   MemoryProt::CpuRead)
                                               ? ReadValue<u32>(local_state, 0x3F8)
                                               : 0);
                    capture_file << ",\"stage_manager\":";
                    WriteHex(capture_file, stage_manager);
                }
            } else if (stop_task_site && object >= 0x10000 &&
                       HasMemoryAccess(object, 0x408, MemoryProt::CpuRead)) {
                capture_file << ",\"stop_reason\":";
                WriteHex(capture_file, static_cast<u32>(registers->rsi));
                capture_file << ",\"matching_state\":" << ReadValue<s32>(object, 0x118);
                capture_file << ",\"controller_operation\":" << ReadValue<s32>(object, 0x3C0);
                capture_file << ",\"room_id\":";
                WriteHex(capture_file, ReadValue<u64>(object, 0x400));
            } else if (site.offset == WorldChrManRemoteCleanupOffset || site.offset == 0x013001E0) {
                const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
                const bool readable = multi_play >= 0x10000 &&
                                      HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead);
                capture_file << ",\"multi_play_state\":"
                             << (readable ? ReadValue<s32>(multi_play, 0x124) : -1);
                const u64 state = GetGlobalState();
                capture_file << ",\"state_1592\":"
                             << (state >= 0x10000
                                     ? static_cast<s32>(ReadValue<u8>(state, 0x1592))
                                     : -1);
            } else if (object >= 0x10000 && HasMemoryAccess(object, 0x460, MemoryProt::CpuRead)) {
                capture_file << ",\"vtable\":";
                WriteHex(capture_file, ReadValue<u64>(object, 0));
                capture_file << ",\"matching_state\":" << ReadValue<s32>(object, 0x118);
                capture_file << ",\"room_id\":";
                WriteHex(capture_file, ReadValue<u64>(object, 0x400));
            }
            capture_file << ",\"frame_chain\":[";
            u64 frame = registers->rbp;
            for (size_t index = 0; index < 8 && frame >= 0x10000 &&
                                   HasMemoryAccess(frame, 2 * sizeof(u64), MemoryProt::CpuRead);
                 ++index) {
                if (index != 0) {
                    capture_file << ',';
                }
                const u64 frame_return = ReadValue<u64>(frame, sizeof(u64));
                WriteHex(capture_file, frame_return);
                const u64 next = ReadValue<u64>(frame, 0);
                if (next <= frame || next - frame > 0x100000) {
                    break;
                }
                frame = next;
            }
            capture_file << ']';
            capture_file << '}';
            if (site.offset == 0x00CC5390) {
                WriteQwords(capture_file, "matching_leave_object_400", object + 0x400, 12);
            }
            break;
        }
        case TraceKind::SummonRequest:
        case TraceKind::MultiplayerInsert:
            WriteObjectState(capture_file, registers->rdi);
            break;
        case TraceKind::SummonCandidate:
            WriteSummonCandidate(capture_file, site, *registers);
            break;
        case TraceKind::SummonSelection:
            WriteSummonSelection(capture_file, site, *registers);
            break;
        case TraceKind::SummonBuild:
            WriteSummonBuild(capture_file, site, *registers);
            break;
        case TraceKind::GetSosRequest:
            WriteGetSosState(capture_file, registers->rdi);
            break;
        case TraceKind::GetSosArea:
            capture_file << ",\"get_sos_area\":{";
            capture_file << "\"player\":";
            WriteHex(capture_file, registers->r13);
            capture_file << ",\"source\":";
            WriteHex(capture_file, registers->rdx);
            if (registers->rdx >= 0x10000) {
                capture_file << ",\"value\":"
                             << static_cast<s64>(ReadValue<s32>(registers->rdx, 0));
            }
            capture_file << '}';
            break;
        case TraceKind::GetSosInsert:
            capture_file << ",\"get_sos_insert\":{";
            capture_file << "\"event_code\":" << static_cast<s64>(static_cast<s32>(registers->rsi));
            capture_file << ",\"observed_sos_area\":"
                         << observed_sos_area.load(std::memory_order_relaxed);
            capture_file << '}';
            WriteGetSosState(capture_file, registers->rdi);
            break;
        case TraceKind::SosStatus:
            capture_file << ",\"sos_status\":{";
            capture_file << "\"object\":";
            WriteHex(capture_file, registers->rbx);
            capture_file << ",\"observed_sos_area\":"
                         << observed_sos_area.load(std::memory_order_relaxed);
            if (site.name == "SosStatus.NativeResult") {
                capture_file << ",\"primary_present\":" << ReadValue<s32>(registers->rbp - 0x9C, 0);
                capture_file << ",\"primary_value\":" << ReadValue<s32>(registers->rbp - 0x90, 0);
                capture_file << ",\"secondary_present\":"
                             << ReadValue<s32>(registers->rbp - 0x84, 0);
                capture_file << ",\"secondary_value\":" << ReadValue<s32>(registers->rbp - 0x78, 0);
                capture_file << ",\"event_bits\":"
                             << static_cast<u32>(ReadValue<u8>(registers->rbp - 0x6C, 0));
            }
            capture_file << '}';
            if (site.offset == SosStatusUpdateOffset) {
                capture_file << ",\"pre_match_guest_warp\":{";
                capture_file << "\"result\":\"" << pre_match_guest_warp.result << '"';
                capture_file << ",\"state\":";
                WriteHex(capture_file, pre_match_guest_warp.state);
                capture_file << ",\"current_map\":";
                WriteHex(capture_file, pre_match_guest_warp.current_map);
                capture_file << ",\"target_map\":";
                WriteHex(capture_file, pre_match_guest_warp.target_map);
                capture_file << ",\"selected_map\":";
                WriteHex(capture_file, pre_match_guest_warp.selected_map);
                capture_file << ",\"multi_play_state\":" << pre_match_guest_warp.multi_play_state;
                capture_file << ",\"placement_written\":"
                             << (pre_match_guest_warp.placement_written ? "true" : "false");
                capture_file << ",\"select_result\":"
                             << (pre_match_guest_warp.select_result ? "true" : "false");
                capture_file << ",\"responder_resume_armed\":"
                             << (pre_match_guest_warp.responder_resume_armed ? "true" : "false");
                capture_file << ",\"transition_result\":" << pre_match_guest_warp.transition_result;
                capture_file << '}';
                capture_file << ",\"responder_resume\":{";
                capture_file << "\"result\":\"" << responder_resume.result << '"';
                capture_file << ",\"target_map\":";
                WriteHex(capture_file, responder_resume.target_map);
                capture_file << ",\"current_map\":";
                WriteHex(capture_file, responder_resume.current_map);
                capture_file << ",\"target_area\":" << responder_resume.target_area;
                capture_file << ",\"current_area\":" << responder_resume.current_area;
                capture_file << ",\"state\":";
                WriteHex(capture_file, responder_resume.state);
                capture_file << ",\"multi_play\":";
                WriteHex(capture_file, responder_resume.multi_play);
                capture_file << ",\"multi_play_state\":" << responder_resume.multi_play_state;
                capture_file << ",\"matching_controller\":";
                WriteHex(capture_file, responder_resume.matching_controller);
                capture_file << ",\"player\":";
                WriteHex(capture_file, responder_resume.player);
                capture_file << ",\"ready_observations\":" << responder_resume.ready_observations;
                capture_file << ",\"attempts\":" << responder_resume.attempts;
                capture_file << ",\"effect_active\":"
                             << (responder_resume.effect_active ? "true" : "false");
                capture_file << ",\"native_result\":"
                             << (responder_resume.native_result ? "true" : "false") << '}';
                capture_file << ",\"deferred_summon_reload\":{";
                capture_file << "\"result\":\"" << deferred_summon_reload.result << '"';
                capture_file << ",\"target_map\":";
                WriteHex(capture_file, deferred_summon_reload.target_map);
                capture_file << ",\"current_map\":";
                WriteHex(capture_file, deferred_summon_reload.current_map);
                capture_file << ",\"summon_reload_started\":"
                             << (deferred_summon_reload.summon_reload_started ? "true" : "false");
                capture_file << '}';
            }
            break;
        case TraceKind::SosMessenger:
            WriteMessengerState(capture_file, registers->rdi);
            break;
        case TraceKind::ActionFlags:
            WriteActionFlags(capture_file, registers->rdi, registers->rsp);
            break;
        case TraceKind::UseItem:
            WriteUseItemState(capture_file, site, *registers);
            break;
        case TraceKind::UseItemExecution:
            WriteUseItemExecution(capture_file, site, *registers);
            break;
        case TraceKind::GoodsActionSubmit:
            WriteGoodsActionSubmit(capture_file, *registers);
            break;
        case TraceKind::GoodsParamLookup:
            WriteGoodsParamLookup(capture_file, *registers);
            break;
        case TraceKind::BellAvailability:
            WriteBellAvailability(capture_file, site, *registers);
            break;
        case TraceKind::ResponderBellAvailability:
            WriteResponderBellAvailability(capture_file, site, *registers, responder_goods);
            break;
        case TraceKind::NetworkAreaRegion:
            capture_file << ",\"network_area_region\":{";
            capture_file << "\"object\":";
            WriteHex(capture_file, registers->r14);
            capture_file << ",\"sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
            capture_file << ",\"previous_region\":"
                         << static_cast<s64>(ReadValue<s32>(registers->r14, 0x44));
            capture_file << ",\"selected_region\":"
                         << static_cast<s64>(static_cast<s32>(registers->r15));
            capture_file << '}';
            break;
        case TraceKind::CrossMapGuestHandoff:
            capture_file << ",\"cross_map_guest_handoff\":{";
            capture_file << "\"result\":\"" << guest_placement_handoff.result << '"';
            capture_file << ",\"object\":";
            WriteHex(capture_file, guest_placement_handoff.object);
            capture_file << ",\"state\":";
            WriteHex(capture_file, guest_placement_handoff.state);
            capture_file << ",\"current_map\":";
            WriteHex(capture_file, guest_placement_handoff.current_map);
            capture_file << ",\"received_map\":";
            WriteHex(capture_file, guest_placement_handoff.received_map);
            capture_file << ",\"selected_map\":";
            WriteHex(capture_file, guest_placement_handoff.selected_map);
            capture_file << ",\"transported_host\":"
                         << (guest_placement_handoff.transported_host ? "true" : "false");
            capture_file << ",\"placement_refreshed\":"
                         << (guest_placement_handoff.placement_refreshed ? "true" : "false");
            capture_file << ",\"select_result\":"
                         << (guest_placement_handoff.select_result ? "true" : "false");
            capture_file << ",\"summon_reload_armed\":"
                         << (guest_placement_handoff.summon_reload_armed ? "true" : "false");
            capture_file << ",\"transition_result\":" << guest_placement_handoff.transition_result;
            capture_file << '}';
            WritePlacementState(capture_file, guest_placement_handoff.state);
            break;
        case TraceKind::SosPlacement:
            WriteQwords(capture_file, "position_qwords", registers->rax + 0x14A0, 2);
            WriteQwords(capture_file, "orientation_qwords", registers->rax + 0x14B0, 2);
            capture_file << ",\"map_id\":";
            WriteHex(capture_file, static_cast<u32>(registers->rcx));
            break;
        case TraceKind::GlobalState:
            WritePlacementState(capture_file, registers->rax);
            break;
        case TraceKind::ReceivedPlacementConditional:
            WriteFloatVector(capture_file, "received_position", registers->rdi);
            WriteFloatVector(capture_file, "received_orientation", registers->rsi);
            WriteU32(capture_file, "matching_key", registers->rdx);
            WriteMapId(capture_file, "received_map_id", registers->rcx);
            WritePlacementState(capture_file, registers->r8);
            break;
        case TraceKind::ReceivedPlacementSet:
            WriteFloatVector(capture_file, "received_position", registers->rdi);
            WriteFloatVector(capture_file, "received_orientation", registers->rsi);
            WriteU32(capture_file, "matching_key", registers->rdx);
            WriteMapId(capture_file, "received_map_id", registers->rcx);
            WritePlacementState(capture_file, registers->rax);
            break;
        case TraceKind::PlacementSelect:
            capture_file << (site.offset == 0x01332CD0 ? ",\"selected_map_id\":"
                                                       : ",\"default_map_id\":");
            WriteHex(capture_file, static_cast<u32>(registers->rax));
            WritePlacementState(capture_file, registers->rcx);
            break;
        case TraceKind::WarpCommand:
            WriteQwords(capture_file, "parameter_qwords", registers->rsi, 12);
            break;
        case TraceKind::CharacterWarp:
            WriteCharacterWarp(capture_file, site, *registers);
            break;
        case TraceKind::WarpResolved:
            WriteFloatVector(capture_file, "position", registers->rdi);
            WriteFloatVector(capture_file, "orientation", registers->rsi);
            WriteFloatVector(capture_file, "camera_orientation", registers->rdx);
            WriteMapId(capture_file, "map_id", registers->rcx);
            break;
        case TraceKind::WarpLocalApply:
            WriteMapId(capture_file, "map_id", registers->rsi);
            WriteFloatVector(capture_file, "position", registers->rdx);
            WriteFloatVector(capture_file, "orientation", registers->rcx);
            break;
        case TraceKind::StageWarp:
            WriteStageWarp(capture_file, site, *registers);
            break;
        case TraceKind::RespawnPointWarp:
            capture_file << ",\"respawn_point_warp\":{";
            capture_file << "\"warp_info_id\":" << static_cast<s32>(registers->rdi) << '}';
            WriteStageWarpState(capture_file, GetGlobalState());
            break;
        case TraceKind::StageWarpDescriptor:
            WriteStageWarpDescriptor(capture_file, *registers);
            if (site.offset == StageWarpDescriptorFinalizeOffset) {
                capture_file << ",\"summon_reload_state\":{";
                capture_file << "\"result\":\"" << summon_reload_state.result << '"';
                capture_file << ",\"target_map\":";
                WriteHex(capture_file, summon_reload_state.target_map);
                capture_file << ",\"descriptor_map\":";
                WriteHex(capture_file, summon_reload_state.descriptor_map);
                capture_file << ",\"reload_state\":";
                WriteHex(capture_file, summon_reload_state.reload_state);
                capture_file << ",\"state_before\":" << summon_reload_state.state_before;
                capture_file << ",\"state_after\":" << summon_reload_state.state_after;
                capture_file << ",\"summon_reload_started\":"
                             << (summon_reload_state.summon_reload_started ? "true" : "false");
                capture_file << ",\"state_setter_used\":"
                             << (summon_reload_state.state_setter_used ? "true" : "false");
                capture_file << ",\"placement_refreshed\":"
                             << (summon_reload_state.placement_refreshed ? "true" : "false") << '}';
            }
            break;
        case TraceKind::StageWarpAcknowledge:
            WriteStageWarpAcknowledge(capture_file, *registers);
            break;
        case TraceKind::StageWarpRespawnResolve:
            WriteStageWarpTask(capture_file, registers->r13, false);
            break;
        case TraceKind::StageWarpTransformResolve:
            WriteRespawnTransformResolve(capture_file, *registers);
            break;
        case TraceKind::StageWarpPlacement:
            WriteStageWarpTask(capture_file, registers->rdi, true);
            break;
        case TraceKind::SummonPoint:
            WriteQwords(capture_file, "query_qwords", registers->rsi, 8);
            break;
        case TraceKind::MapReload:
            WritePlacementState(capture_file, GetGlobalState());
            break;
        case TraceKind::HealingFountainRegistration:
            WriteHealingFountainRegistration(capture_file, *registers);
            break;
        case TraceKind::HealingFountainAvailability:
            WriteHealingFountainAvailability(capture_file, *registers);
            break;
        case TraceKind::ChairRespawnNotification:
            WriteChairRespawnNotification(capture_file, *registers);
            break;
        case TraceKind::WorldStateValidation:
            WriteWorldStateValidation(capture_file, site, *registers);
            break;
        case TraceKind::LuaSessionEvent:
            WriteLuaSessionEvent(capture_file, *registers);
            break;
        case TraceKind::EnginePanic:
            WriteEnginePanicStack(capture_file, *registers);
            break;
        }
        capture_file << "}\n";
        capture_file.flush();

        if (hit <= 8 || hit % 300 == 0) {
            LOG_INFO(Debug,
                     "Bloodborne RE entry {} hit={} rdi={:#x} rsi={:#x} rdx={:#x} capture={}",
                     site.name, hit, registers->rdi, registers->rsi, registers->rdx,
                     Common::FS::PathToUTF8String(capture_path));
        }
    } catch (...) {
        // An observer must never unwind into guest code.
    }
}

bool VerifySites() {
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    for (const auto& site : Sites) {
        if ((summon_build_host_placement_hook_installed && site.offset == SummonBuildEntryOffset) ||
            (cross_map_guest_handoff_hook_installed && site.offset == CrossMapGuestHandoffOffset) ||
            (summon_reload_state_hook_installed &&
             site.offset == StageWarpDescriptorFinalizeOffset) ||
            (deferred_summon_reload_hook_installed && site.offset == SosStatusUpdateOffset) ||
            (healing_fountain_host_availability_hook_installed &&
             site.offset == HealingFountainAvailabilityOffset)) {
            continue;
        }
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        if (site.offset > image_size || expected.size() > image_size - site.offset) {
            LOG_ERROR(Debug, "Bloodborne RE site {} lies outside the eboot image", site.name);
            return false;
        }
        const auto actual = std::span<const u8>{
            reinterpret_cast<const u8*>(image_base + site.offset), expected.size()};
        if (!std::ranges::equal(actual, expected)) {
            LOG_ERROR(Debug,
                      "Bloodborne RE signature mismatch at {} ({:#x}): expected={} actual={}",
                      site.name, site.offset, BytesToHex(expected), BytesToHex(actual));
            return false;
        }
    }
    return true;
}

// BBSC connect-first cross-map summon (2026-09-17). The seamless broker (shadnet-p2p
// bloodborne_summon_broker.cpp) marks a sign whose AreaId differs from the searching host's map as
// Preparing, hides it from the host, and answers the guest's next advertisement with the host
// placement only; it hands out the claim once the advertisement's AreaId equals the host map. The
// fork answered that by teleporting the guest first (ApplyPreMatchCrossMapGuestWarp) and re-ringing
// the bell, but after that load the game never posted the sign again (runs 2026-09-17, also with all
// BBSC changes off), so the join never happened. Connect first instead: remember the prepared host
// map, keep the guest where it is, and advertise with AreaId = host map. The broker then lets the
// host find and claim the sign and delivers the claim with the placement; the join-time handoff
// (ApplyCrossMapSummonGuestPlacement) loads the guest straight into the host's map.
constexpr s64 BbscConnectFirstTimeoutMs = 10 * 60 * 1'000;
std::atomic<u32> bbsc_connect_first_map{0};
std::atomic<s64> bbsc_connect_first_since_ms{0};

bool BbscConnectFirstEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_CONNECT_FIRST");
    return enabled;
}

bool BbscConnectFirstPlacementFresh() {
    return SteadyNowMs() - bbsc_connect_first_since_ms.load(std::memory_order_acquire) <=
           BbscConnectFirstTimeoutMs;
}

} // namespace

void OnBbscHostBossPacket(std::int32_t boss) {
    bbsc_host_boss_id = boss;
    bbsc_host_boss_ms = SteadyNowMs();
}

void OnBbscProgressPacket(const std::uint32_t* flags, std::size_t count) {
    if (flags == nullptr || bbsc_relay_session_state.load() != 6) {
        return;
    }
    const auto known = [](u32 flag) {
        return std::ranges::find(BbscProgressFlags, flag) != BbscProgressFlags.end() ||
               std::ranges::find(BbscProgressFlagsMore, flag) != BbscProgressFlagsMore.end();
    };
    size_t added = 0;
    {
        std::scoped_lock lock{bbsc_progress_mutex};
        for (size_t i = 0; i < count; ++i) {
            if (known(flags[i]) && std::ranges::find(bbsc_progress_host_flags, flags[i]) ==
                                       bbsc_progress_host_flags.end()) {
                bbsc_progress_host_flags.push_back(flags[i]);
                ++added;
            }
        }
    }
    if (added != 0) {
        LOG_INFO(Debug, "BBSC shared progress: the host has {} more boss flag(s) for our world",
                 added);
    }
}

void NoteBbscRelayHost(std::uint32_t sender, std::uint32_t packed_map, float x, float y, float z) {
    std::scoped_lock lock{bbsc_relay_mutex};
    const s64 now = SteadyNowMs();
    std::erase_if(bbsc_relay_hosts, [now](const auto& entry) {
        return now - entry.second.ms > 60'000;
    });
    bbsc_relay_hosts[sender] = {packed_map, x, y, z, now};
}

std::uint32_t GetBbscRelayHost() {
    return BbscRelayHostForPackets();
}

std::int32_t GetBbscRelaySessionState() {
    return bbsc_relay_session_state.load();
}

void OnBbscLifePacket(std::uint32_t player, std::uint8_t dead, std::uint32_t sequence) {
    if (player == bbsc_life_own_id.load()) {
        return;
    }
    bool changed = false;
    {
        std::scoped_lock lock{bbsc_life_mutex};
        BbscPeerLife& life = bbsc_peer_life[player];
        if (life.received_ms != 0 && sequence != 0 && sequence <= life.sequence &&
            SteadyNowMs() - life.received_ms < BbscLifeFreshMs) {
            return; // older than what we have
        }
        changed = life.received_ms == 0 || life.dead != dead;
        if (life.received_ms != 0 && life.dead != 0 && dead == 0) {
            ++bbsc_peer_revivals;
        }
        life = {dead, sequence, SteadyNowMs()};
    }
    if (changed) {
        LOG_INFO(Debug, "BBSC life: player {:08x} says it is {}", player, dead ? "dead" : "alive");
    }
}

void OnBbscScalePacket(const std::uint16_t (&percent)[5]) {
    BbscScaleNumbers numbers;
    for (size_t i = 0; i < 5; ++i) {
        numbers.percent[i] = std::clamp<int>(percent[i], 0, 1000);
    }
    bool changed = false;
    {
        std::scoped_lock lock{bbsc_scale_host_mutex};
        changed = bbsc_scale_host_ms == 0 || !(bbsc_scale_host == numbers);
        bbsc_scale_host = numbers;
        bbsc_scale_host_ms = SteadyNowMs();
    }
    if (changed) {
        LOG_INFO(Debug,
                 "BBSC scaling: the host's settings are enemy HP {}%, boss HP {}%, damage {}%, "
                 "stagger {}% / {}% per guest (used while summoned)",
                 numbers.percent[0], numbers.percent[1], numbers.percent[2], numbers.percent[3],
                 numbers.percent[4]);
    }
}

void OnBbscPvpPacket(std::uint8_t kind, std::uint8_t on) {
    if (kind == 0) {
        bbsc_pvp_host_state = on != 0 ? 1 : 0;
        bbsc_pvp_host_state_ms = SteadyNowMs();
    } else if (kind == 1) {
        bbsc_pvp_request = on != 0 ? 1 : 0;
        LOG_INFO(Debug, "BBSC PvP: a guest rang the Duel Bell and asks for {}", on ? "on" : "off");
    }
}

// Arrives on the signaling receive thread, so it does no game-memory work at all: it records the
// message and TickBbscFollowHost decides, on the game thread, whether to act on it.
void OnBbscWorldFlagsPacket(const void* data, std::uint32_t size) {
    using Libraries::Np::NpSignaling::SignalingBbscWorldFlags;
    if (!BbscWorldSyncEnabled() || data == nullptr || size != sizeof(SignalingBbscWorldFlags)) {
        return;
    }
    SignalingBbscWorldFlags packet;
    std::memcpy(&packet, data, sizeof(packet));
    // Version 1: the map block's groups; version 2: the host's Hunter's Dream state.
    const bool dream = packet.version == 2;
    const size_t group_total = dream ? BbscDreamGroupCount : BbscWorldGroupCount;
    const size_t chunk_total = dream ? BbscDreamChunkCount : BbscWorldChunkCount;
    if ((packet.version != 1 && !dream) || packet.chunk_count != chunk_total ||
        packet.chunk >= chunk_total || !IsUsablePackedMap(packet.packed_map)) {
        return;
    }
    const size_t first = packet.chunk * BbscWorldChunkGroups;
    const size_t count = std::min<size_t>(
        packet.group_count, std::min(BbscWorldChunkGroups, group_total - first));
    bool first_for_map = false;
    u32 map = 0;
    {
        std::scoped_lock lock{bbsc_world_mutex};
        BbscWorldSnapshot& snap = dream ? bbsc_dream_assembling : bbsc_world_assembling;
        BbscWorldSnapshot& ready = dream ? bbsc_dream_ready : bbsc_world_ready;
        if (snap.map != packet.packed_map || snap.snapshot != packet.snapshot) {
            snap = BbscWorldSnapshot{};
            snap.map = packet.packed_map;
            snap.snapshot = packet.snapshot;
        }
        for (size_t i = 0; i < count; ++i) {
            if ((packet.present & (1u << i)) != 0) {
                std::memcpy(snap.groups[first + i].data(), packet.groups[i], BbscWorldGroupBytes);
                snap.present |= u64{1} << (first + i);
            }
        }
        snap.chunks |= 1u << packet.chunk;
        if (snap.chunks == (1u << chunk_total) - 1 && snap.complete_ms == 0) {
            snap.complete_ms = SteadyNowMs();
            map = snap.map;
            if (dream) {
                first_for_map = ready.map != snap.map;
            } else {
                first_for_map = !bbsc_world_blocks.contains(snap.map);
                bbsc_world_blocks[snap.map] = snap;
            }
            ready = snap;
        }
    }
    if (first_for_map) {
        LOG_INFO(Debug, "BBSC world state: host's {} for map {:#x} received",
                 dream ? "Hunter's Dream state" : "flags", map);
    }
}

void OnBbscHostFollowPacket(std::uint32_t packed_map, float x, float y, float z, float heading,
                            std::uint32_t sequence, std::int32_t lamp_point) {
    if (!BbscWarpTogetherEnabled()) {
        return;
    }
    const s64 now = SteadyNowMs();
    std::scoped_lock lock{bbsc_follow_mutex};
    // An older announcement overtaking a newer one is dropped - but only while the last one is
    // fresh. A host that restarted its game counts from 1 again, and without the staleness test a
    // guest that kept running ignored it for good (code review B5).
    if (bbsc_follow_latest.received_ms != 0 && sequence != 0 &&
        sequence <= bbsc_follow_latest.sequence &&
        now - bbsc_follow_latest.received_ms < BbscFollowStaleMs) {
        return;
    }
    if (lamp_point != bbsc_follow_latest.lamp_point) {
        LOG_INFO(Debug, "BBSC warp together: host's last lamp is now point {}", lamp_point);
    }
    // The host says nothing while a load runs (TickBbscFollowBroadcast), so its first report after
    // a silence, or one far from the last, is where it came out: the lamp it travelled to, or the
    // lamp it respawned at. A guest that follows arrives there too (user, 2026-09-27: "after
    // tping it should tp to the lamp not to me"), not wherever the host has walked off to since.
    const BbscFollowMessage previous = bbsc_follow_latest;
    const float dx = x - previous.x;
    const float dy = y - previous.y;
    const float dz = z - previous.z;
    // A jump only counts inside one map: the coordinates are per map, so walking across a border
    // changes them without any teleport.
    const bool silence = previous.received_ms != 0 && now - previous.received_ms > 1'500;
    const bool arrived = previous.received_ms == 0 || silence ||
                         (packed_map == previous.packed_map &&
                          dx * dx + dy * dy + dz * dz > 30.0f * 30.0f);
    bbsc_follow_latest = {packed_map, x, y, z, heading, sequence, lamp_point, now};
    if (arrived) {
        bbsc_follow_latest.arrival_ms = now;
        bbsc_follow_latest.arrival_by_load = silence;
        bbsc_follow_latest.arrival_map = packed_map;
        bbsc_follow_latest.arrival_x = x;
        bbsc_follow_latest.arrival_y = y;
        bbsc_follow_latest.arrival_z = z;
        bbsc_follow_latest.arrival_heading = heading;
    } else {
        bbsc_follow_latest.arrival_map = previous.arrival_map;
        bbsc_follow_latest.arrival_x = previous.arrival_x;
        bbsc_follow_latest.arrival_y = previous.arrival_y;
        bbsc_follow_latest.arrival_z = previous.arrival_z;
        bbsc_follow_latest.arrival_heading = previous.arrival_heading;
        bbsc_follow_latest.arrival_ms = previous.arrival_ms;
        bbsc_follow_latest.arrival_by_load = previous.arrival_by_load;
    }
}

std::optional<std::uint32_t> GetBbscConnectFirstAreaId() {
    if (!BbscConnectFirstEnabled()) {
        return std::nullopt;
    }
    const u32 map = bbsc_connect_first_map.load(std::memory_order_acquire);
    if (map == 0) {
        return std::nullopt;
    }
    const s32 session_state = GetSessionState();
    if (session_state == 4 || session_state == 6 ||
        SteadyNowMs() - bbsc_connect_first_since_ms.load(std::memory_order_acquire) >
            BbscConnectFirstTimeoutMs) {
        bbsc_connect_first_map.store(0, std::memory_order_release);
        LOG_INFO(Debug, "BBSC connect-first: target map {:#x} cleared (session state {})", map,
                 session_state);
        return std::nullopt;
    }
    return map;
}

std::optional<std::string> GetSeamlessHostPlacementHeader() {
    std::optional<SummonPlacementDescriptor> placement;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        placement = seamless_host_placement;
    }
    if (!placement.has_value()) {
        return std::nullopt;
    }

    std::ostringstream out;
    out << "1," << std::hex << std::setfill('0') << std::setw(8) << placement->packed_region << ','
        << std::setw(8) << std::bit_cast<u32>(placement->x) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->y) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->z) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->heading) << ',' << std::dec << placement->area;
    return out.str();
}

bool SetSeamlessHostPlacementHeader(std::string_view value) {
    std::array<std::string_view, 7> fields{};
    size_t begin = 0;
    for (size_t field_index = 0; field_index < fields.size(); ++field_index) {
        const size_t end = value.find(',', begin);
        const bool final_field = field_index + 1 == fields.size();
        if ((final_field && end != std::string_view::npos) ||
            (!final_field && end == std::string_view::npos)) {
            return false;
        }
        fields[field_index] =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        begin = end == std::string_view::npos ? value.size() : end + 1;
    }
    if (fields[0] != "1") {
        return false;
    }

    auto parse_u32 = [](std::string_view field, int base, u32& output) {
        const auto [end, error] =
            std::from_chars(field.data(), field.data() + field.size(), output, base);
        return error == std::errc{} && end == field.data() + field.size();
    };
    SummonPlacementDescriptor placement{};
    std::array<u32, 4> float_bits{};
    if (!parse_u32(fields[1], 16, placement.packed_region) ||
        !parse_u32(fields[2], 16, float_bits[0]) || !parse_u32(fields[3], 16, float_bits[1]) ||
        !parse_u32(fields[4], 16, float_bits[2]) || !parse_u32(fields[5], 16, float_bits[3])) {
        return false;
    }
    const auto [area_end, area_error] =
        std::from_chars(fields[6].data(), fields[6].data() + fields[6].size(), placement.area, 10);
    if (area_error != std::errc{} || area_end != fields[6].data() + fields[6].size()) {
        return false;
    }
    placement.x = std::bit_cast<float>(float_bits[0]);
    placement.y = std::bit_cast<float>(float_bits[1]);
    placement.z = std::bit_cast<float>(float_bits[2]);
    placement.heading = std::bit_cast<float>(float_bits[3]);
    if (!IsUsablePackedMap(placement.packed_region) || !std::isfinite(placement.x) ||
        !std::isfinite(placement.y) || !std::isfinite(placement.z) ||
        !std::isfinite(placement.heading)) {
        return false;
    }

    if (BbscConnectFirstEnabled()) {
        // Called on the relay thread (mod): the tick's copy of the map, not the live map list.
        const u32 current_map = bbsc_tick_map.load(std::memory_order_acquire);
        if (IsUsablePackedMap(current_map) && current_map != placement.packed_region &&
            bbsc_connect_first_map.exchange(placement.packed_region, std::memory_order_acq_rel) !=
                placement.packed_region) {
            LOG_INFO(Debug,
                     "BBSC connect-first: host is in map {:#x} (guest in {:#x}); advertising "
                     "there instead of teleporting first",
                     placement.packed_region, current_map);
        }
        if (current_map != placement.packed_region) {
            bbsc_connect_first_since_ms.store(SteadyNowMs(), std::memory_order_release);
        } else if (const u32 old = bbsc_connect_first_map.exchange(0, std::memory_order_acq_rel);
                   old != 0) {
            // Test 2026-10-07 (third player): the host was in the Dream for a moment while a tester
            // put his sign down; the host came back to Central Yharnam 2 minutes later, but a
            // placement in our own map never cleared the old target, so the sign stayed
            // advertised in the Dream for 10 minutes and the host never saw it.
            LOG_INFO(Debug,
                     "BBSC connect-first: the host is in our map {:#x} now - the sign goes back "
                     "to it (was advertised in {:#x})",
                     current_map, old);
        }
    }

    std::scoped_lock lock{seamless_placement_mutex};
    if (BbscConnectFirstEnabled()) {
        if (bbsc_tick_map.load(std::memory_order_acquire) != placement.packed_region) {
            bbsc_connect_first_placement = placement;
        } else {
            bbsc_connect_first_placement.reset();
        }
    }
    if (!seamless_received_host_placement.has_value() ||
        !IsSameSummonPlacement(*seamless_received_host_placement, placement)) {
        seamless_received_host_placement_consumed = false;
    }
    seamless_received_host_placement = placement;
    return true;
}

void ClearSeamlessHostPlacementHeader() {
    // A host that stopped looking for signs: its map is no target for our sign either.
    if (const u32 old = bbsc_connect_first_map.exchange(0, std::memory_order_acq_rel); old != 0) {
        LOG_INFO(Debug, "BBSC connect-first: no word from that host for a while - the sign goes "
                        "back to our own map (was advertised in {:#x})",
                 old);
    }
    std::scoped_lock lock{seamless_placement_mutex};
    seamless_received_host_placement.reset();
    seamless_received_host_placement_consumed = false;
    bbsc_connect_first_placement.reset();
}

void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id) {
    if (image_base == 0 || MemoryPatcher::g_game_serial != "CUSA03173") {
        return;
    }

    const u64 caller_offset = return_address >= image_base && return_address - image_base <
                                                                  MemoryPatcher::g_eboot_image_size
                                  ? return_address - image_base
                                  : 0;
    const u64 reload_state = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const s32 reload_phase = reload_state >= 0x10000 ? ReadValue<s32>(reload_state, 0x84) : -1;
    const u64 global_state = GetGlobalState();
    const u32 current_map = GetCurrentPackedMap();
    const u32 requested_map = global_state >= 0x10000 ? ReadValue<u32>(global_state, 0x0C) : 0;
    const u32 pending_reload = pending_summon_reload_map.load(std::memory_order_acquire);
    const u32 pending_state = pending_summon_reload_state_map.load(std::memory_order_acquire);
    const u64 sequence = event_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

    try {
        std::scoped_lock lock{capture_mutex};
        if (capture_file) {
            capture_file << "{\"type\":\"matching2_leave_room\",\"seq\":" << sequence
                         << ",\"time_ms\":" << timestamp << ",\"return_address\":";
            WriteHex(capture_file, return_address);
            capture_file << ",\"caller_offset\":";
            WriteHex(capture_file, caller_offset);
            capture_file << ",\"room_id\":" << room_id << ",\"reload_state\":";
            WriteHex(capture_file, reload_state);
            capture_file << ",\"reload_phase\":" << reload_phase << ",\"global_state\":";
            WriteHex(capture_file, global_state);
            capture_file << ",\"current_map\":";
            WriteHex(capture_file, current_map);
            capture_file << ",\"requested_map\":";
            WriteHex(capture_file, requested_map);
            capture_file << ",\"pending_reload_map\":";
            WriteHex(capture_file, pending_reload);
            capture_file << ",\"pending_state_map\":";
            WriteHex(capture_file, pending_state);
            capture_file << "}\n";
            capture_file.flush();
        }
    } catch (...) {
        // Diagnostics must never unwind into an NP library call.
    }

    LOG_INFO(Debug,
             "Bloodborne RE Matching2 leave room={} caller={:#x} offset={:#x} reload_phase={} "
             "current_map={:#x} requested_map={:#x}",
             room_id, return_address, caller_offset, reload_phase, current_map, requested_map);
}

void InstallSeamlessCoopPatches() {
    if ((negative_area_patch_installed && area_flag_patch_installed &&
         responder_bell_area_patch_installed && responder_bell_common_patch_installed &&
         active_bell_negative_area_patch_installed && active_bell_area_flag_patch_installed &&
         responder_search_negative_area_patch_installed && sos_status_area_patch_installed &&
         summon_candidate_area_patch_installed && summon_build_area_patch_installed &&
         summon_build_world_state_patch_installed && summon_build_negative_event_patch_installed &&
         summon_build_host_placement_hook_installed && cross_map_guest_handoff_hook_installed &&
         summon_reload_state_hook_installed && deferred_summon_reload_hook_installed &&
         healing_fountain_host_availability_hook_installed) ||
        !EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP")) {
        return;
    }
    if (MemoryPatcher::g_game_serial != "CUSA03173") {
        LOG_INFO(Debug, "Bloodborne seamless declined: this is {}, not CUSA03173",
                 MemoryPatcher::g_game_serial.empty() ? "an unknown title"
                                                      : MemoryPatcher::g_game_serial);
        return;
    }

    const auto* param_sfo = Common::Singleton<PSF>::Instance();
    const std::string_view app_version = param_sfo->GetString("APP_VER").value_or("Unknown");
    if (app_version != "01.09") {
        LOG_ERROR(Debug,
                  "Bloodborne seamless patches require CUSA03173 01.09; loaded version is {}",
                  app_version);
        return;
    }

    const auto base = MemoryPatcher::g_eboot_address;
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    const auto summon_build_entry = std::ranges::find_if(
        Sites, [](const TraceSite& site) { return site.offset == SummonBuildEntryOffset; });
    const auto cross_map_guest_handoff = std::ranges::find_if(
        Sites, [](const TraceSite& site) { return site.offset == CrossMapGuestHandoffOffset; });
    const auto deferred_summon_reload = std::ranges::find_if(
        Sites, [](const TraceSite& site) { return site.offset == SosStatusUpdateOffset; });
    const auto summon_reload_state = std::ranges::find_if(Sites, [](const TraceSite& site) {
        return site.offset == StageWarpDescriptorFinalizeOffset;
    });
    const auto healing_fountain_host_availability = std::ranges::find_if(
        Sites, [](const auto& site) { return site.offset == HealingFountainAvailabilityOffset; });
    if (summon_build_entry == Sites.end() || cross_map_guest_handoff == Sites.end() ||
        deferred_summon_reload == Sites.end() || summon_reload_state == Sites.end() ||
        healing_fountain_host_availability == Sites.end()) {
        LOG_ERROR(Debug, "Bloodborne seamless functional hook site is missing");
        return;
    }
    const auto summon_build_entry_expected =
        std::span<const u8>{summon_build_entry->prologue.data(), summon_build_entry->prologue_size};
    const auto cross_map_guest_handoff_expected = std::span<const u8>{
        cross_map_guest_handoff->prologue.data(), cross_map_guest_handoff->prologue_size};
    const auto deferred_summon_reload_expected = std::span<const u8>{
        deferred_summon_reload->prologue.data(), deferred_summon_reload->prologue_size};
    const auto summon_reload_state_expected = std::span<const u8>{
        summon_reload_state->prologue.data(), summon_reload_state->prologue_size};
    const auto healing_fountain_host_availability_expected =
        std::span<const u8>{healing_fountain_host_availability->prologue.data(),
                            healing_fountain_host_availability->prologue_size};
    if (base == 0 || BeckoningAreaComparisonOffset > image_size ||
        BeckoningAreaComparison.size() > image_size - BeckoningAreaComparisonOffset ||
        BeckoningAreaFlagResultOffset > image_size ||
        BeckoningAreaFlagResult.size() > image_size - BeckoningAreaFlagResultOffset ||
        ResponderBellAreaResultOffset > image_size ||
        ResponderBellAreaResult.size() > image_size - ResponderBellAreaResultOffset ||
        ResponderBellCommonResultOffset > image_size ||
        ResponderBellCommonResult.size() > image_size - ResponderBellCommonResultOffset ||
        ActiveBellAreaComparisonOffset > image_size ||
        ActiveBellAreaComparison.size() > image_size - ActiveBellAreaComparisonOffset ||
        ActiveBellAreaFlagResultOffset > image_size ||
        ActiveBellAreaFlagResult.size() > image_size - ActiveBellAreaFlagResultOffset ||
        ResponderSearchAreaRangeResultOffset > image_size ||
        ResponderSearchAreaRangeResult.size() > image_size - ResponderSearchAreaRangeResultOffset ||
        SosStatusAreaRestrictionOffset > image_size ||
        SosStatusAreaRestriction.size() > image_size - SosStatusAreaRestrictionOffset ||
        SummonCandidateAreaRestrictionOffset > image_size ||
        SummonCandidateAreaRestriction.size() > image_size - SummonCandidateAreaRestrictionOffset ||
        SummonBuildAreaRestrictionOffset > image_size ||
        SummonBuildAreaRestriction.size() > image_size - SummonBuildAreaRestrictionOffset ||
        SummonBuildWorldStateRestrictionOffset > image_size ||
        SummonBuildWorldStateRestriction.size() >
            image_size - SummonBuildWorldStateRestrictionOffset ||
        SummonBuildNegativeEventRestrictionOffset > image_size ||
        SummonBuildNegativeEventRestriction.size() >
            image_size - SummonBuildNegativeEventRestrictionOffset ||
        StageTransitionStopMatchingCallOffset > image_size ||
        StageTransitionStopMatchingCall.size() >
            image_size - StageTransitionStopMatchingCallOffset ||
        SummonBuildEntryOffset > image_size ||
        summon_build_entry_expected.size() > image_size - SummonBuildEntryOffset ||
        CrossMapGuestHandoffOffset > image_size ||
        cross_map_guest_handoff_expected.size() > image_size - CrossMapGuestHandoffOffset ||
        SosStatusUpdateOffset > image_size ||
        deferred_summon_reload_expected.size() > image_size - SosStatusUpdateOffset ||
        StageWarpDescriptorFinalizeOffset > image_size ||
        summon_reload_state_expected.size() > image_size - StageWarpDescriptorFinalizeOffset ||
        HealingFountainAvailabilityOffset > image_size ||
        healing_fountain_host_availability_expected.size() >
            image_size - HealingFountainAvailabilityOffset ||
        std::ranges::any_of(CrossMapNativeCalls, [image_size](const auto& site) {
            return site.offset > image_size || site.prologue_size > image_size - site.offset;
        })) {
        LOG_ERROR(Debug, "Bloodborne seamless patches lie outside the eboot image");
        return;
    }

    auto* area_comparison = reinterpret_cast<u8*>(base + BeckoningAreaComparisonOffset);
    const auto actual_area_comparison =
        std::span<const u8>{area_comparison, BeckoningAreaComparison.size()};
    if (!std::ranges::equal(actual_area_comparison, BeckoningAreaComparison)) {
        LOG_ERROR(Debug,
                  "Bloodborne Beckoning area signature mismatch at {:#x}: expected={} actual={}",
                  BeckoningAreaComparisonOffset, BytesToHex(BeckoningAreaComparison),
                  BytesToHex(actual_area_comparison));
        return;
    }

    auto* area_flag_result = reinterpret_cast<u8*>(base + BeckoningAreaFlagResultOffset);
    const auto actual_area_flag_result =
        std::span<const u8>{area_flag_result, BeckoningAreaFlagResult.size()};
    if (!std::ranges::equal(actual_area_flag_result, BeckoningAreaFlagResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne Beckoning area-flag signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  BeckoningAreaFlagResultOffset, BytesToHex(BeckoningAreaFlagResult),
                  BytesToHex(actual_area_flag_result));
        return;
    }

    auto* responder_bell_area_result = reinterpret_cast<u8*>(base + ResponderBellAreaResultOffset);
    const auto actual_responder_bell_area_result =
        std::span<const u8>{responder_bell_area_result, ResponderBellAreaResult.size()};
    if (!std::ranges::equal(actual_responder_bell_area_result, ResponderBellAreaResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne responder Bell area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ResponderBellAreaResultOffset, BytesToHex(ResponderBellAreaResult),
                  BytesToHex(actual_responder_bell_area_result));
        return;
    }

    auto* responder_bell_common_result =
        reinterpret_cast<u8*>(base + ResponderBellCommonResultOffset);
    const auto actual_responder_bell_common_result =
        std::span<const u8>{responder_bell_common_result, ResponderBellCommonResult.size()};
    if (!std::ranges::equal(actual_responder_bell_common_result, ResponderBellCommonResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne responder Bell common signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ResponderBellCommonResultOffset, BytesToHex(ResponderBellCommonResult),
                  BytesToHex(actual_responder_bell_common_result));
        return;
    }

    auto* active_bell_area_comparison =
        reinterpret_cast<u8*>(base + ActiveBellAreaComparisonOffset);
    const auto actual_active_bell_area_comparison =
        std::span<const u8>{active_bell_area_comparison, ActiveBellAreaComparison.size()};
    if (!std::ranges::equal(actual_active_bell_area_comparison, ActiveBellAreaComparison)) {
        LOG_ERROR(Debug,
                  "Bloodborne active Bell area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ActiveBellAreaComparisonOffset, BytesToHex(ActiveBellAreaComparison),
                  BytesToHex(actual_active_bell_area_comparison));
        return;
    }

    auto* active_bell_area_flag_result =
        reinterpret_cast<u8*>(base + ActiveBellAreaFlagResultOffset);
    const auto actual_active_bell_area_flag_result =
        std::span<const u8>{active_bell_area_flag_result, ActiveBellAreaFlagResult.size()};
    if (!std::ranges::equal(actual_active_bell_area_flag_result, ActiveBellAreaFlagResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne active Bell area-flag signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ActiveBellAreaFlagResultOffset, BytesToHex(ActiveBellAreaFlagResult),
                  BytesToHex(actual_active_bell_area_flag_result));
        return;
    }

    auto* responder_search_area_range_result =
        reinterpret_cast<u8*>(base + ResponderSearchAreaRangeResultOffset);
    const auto actual_responder_search_area_range_result = std::span<const u8>{
        responder_search_area_range_result, ResponderSearchAreaRangeResult.size()};
    if (!std::ranges::equal(actual_responder_search_area_range_result,
                            ResponderSearchAreaRangeResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne responder-search area-range signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  ResponderSearchAreaRangeResultOffset, BytesToHex(ResponderSearchAreaRangeResult),
                  BytesToHex(actual_responder_search_area_range_result));
        return;
    }

    auto* sos_status_area_restriction =
        reinterpret_cast<u8*>(base + SosStatusAreaRestrictionOffset);
    const auto actual_sos_status_area_restriction =
        std::span<const u8>{sos_status_area_restriction, SosStatusAreaRestriction.size()};
    if (!std::ranges::equal(actual_sos_status_area_restriction, SosStatusAreaRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne SOS-status area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  SosStatusAreaRestrictionOffset, BytesToHex(SosStatusAreaRestriction),
                  BytesToHex(actual_sos_status_area_restriction));
        return;
    }

    auto* summon_candidate_area_restriction =
        reinterpret_cast<u8*>(base + SummonCandidateAreaRestrictionOffset);
    const auto actual_summon_candidate_area_restriction = std::span<const u8>{
        summon_candidate_area_restriction, SummonCandidateAreaRestriction.size()};
    if (!std::ranges::equal(actual_summon_candidate_area_restriction,
                            SummonCandidateAreaRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne summon-candidate area signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  SummonCandidateAreaRestrictionOffset, BytesToHex(SummonCandidateAreaRestriction),
                  BytesToHex(actual_summon_candidate_area_restriction));
        return;
    }

    auto* summon_build_area_restriction =
        reinterpret_cast<u8*>(base + SummonBuildAreaRestrictionOffset);
    const auto actual_summon_build_area_restriction =
        std::span<const u8>{summon_build_area_restriction, SummonBuildAreaRestriction.size()};
    if (!std::ranges::equal(actual_summon_build_area_restriction, SummonBuildAreaRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne summon-builder area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  SummonBuildAreaRestrictionOffset, BytesToHex(SummonBuildAreaRestriction),
                  BytesToHex(actual_summon_build_area_restriction));
        return;
    }

    auto* summon_build_world_state_restriction =
        reinterpret_cast<u8*>(base + SummonBuildWorldStateRestrictionOffset);
    const auto actual_summon_build_world_state_restriction = std::span<const u8>{
        summon_build_world_state_restriction, SummonBuildWorldStateRestriction.size()};
    if (!std::ranges::equal(actual_summon_build_world_state_restriction,
                            SummonBuildWorldStateRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne summon-builder world-state signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  SummonBuildWorldStateRestrictionOffset,
                  BytesToHex(SummonBuildWorldStateRestriction),
                  BytesToHex(actual_summon_build_world_state_restriction));
        return;
    }

    auto* summon_build_negative_event_restriction =
        reinterpret_cast<u8*>(base + SummonBuildNegativeEventRestrictionOffset);
    const auto actual_summon_build_negative_event_restriction = std::span<const u8>{
        summon_build_negative_event_restriction, SummonBuildNegativeEventRestriction.size()};
    if (!std::ranges::equal(actual_summon_build_negative_event_restriction,
                            SummonBuildNegativeEventRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne summon-builder negative-event signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  SummonBuildNegativeEventRestrictionOffset,
                  BytesToHex(SummonBuildNegativeEventRestriction),
                  BytesToHex(actual_summon_build_negative_event_restriction));
        return;
    }

    auto* stage_transition_stop_matching_call =
        reinterpret_cast<u8*>(base + StageTransitionStopMatchingCallOffset);
    const auto actual_stage_transition_stop_matching_call = std::span<const u8>{
        stage_transition_stop_matching_call, StageTransitionStopMatchingCall.size()};
    if (!std::ranges::equal(actual_stage_transition_stop_matching_call,
                            StageTransitionStopMatchingCall)) {
        LOG_ERROR(Debug,
                  "Bloodborne stage-transition matching-stop signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  StageTransitionStopMatchingCallOffset,
                  BytesToHex(StageTransitionStopMatchingCall),
                  BytesToHex(actual_stage_transition_stop_matching_call));
        return;
    }

    const auto actual_summon_build_entry =
        std::span<const u8>{reinterpret_cast<const u8*>(base + SummonBuildEntryOffset),
                            summon_build_entry_expected.size()};
    if (!std::ranges::equal(actual_summon_build_entry, summon_build_entry_expected)) {
        LOG_ERROR(Debug,
                  "Bloodborne SummonBuild entry signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  SummonBuildEntryOffset, BytesToHex(summon_build_entry_expected),
                  BytesToHex(actual_summon_build_entry));
        return;
    }
    const auto actual_cross_map_guest_handoff =
        std::span<const u8>{reinterpret_cast<const u8*>(base + CrossMapGuestHandoffOffset),
                            cross_map_guest_handoff_expected.size()};
    if (!std::ranges::equal(actual_cross_map_guest_handoff, cross_map_guest_handoff_expected)) {
        LOG_ERROR(Debug,
                  "Bloodborne cross-map guest-handoff signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  CrossMapGuestHandoffOffset, BytesToHex(cross_map_guest_handoff_expected),
                  BytesToHex(actual_cross_map_guest_handoff));
        return;
    }
    const auto actual_deferred_summon_reload =
        std::span<const u8>{reinterpret_cast<const u8*>(base + SosStatusUpdateOffset),
                            deferred_summon_reload_expected.size()};
    if (!std::ranges::equal(actual_deferred_summon_reload, deferred_summon_reload_expected)) {
        LOG_ERROR(Debug,
                  "Bloodborne deferred summon-reload signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  SosStatusUpdateOffset, BytesToHex(deferred_summon_reload_expected),
                  BytesToHex(actual_deferred_summon_reload));
        return;
    }
    const auto actual_summon_reload_state =
        std::span<const u8>{reinterpret_cast<const u8*>(base + StageWarpDescriptorFinalizeOffset),
                            summon_reload_state_expected.size()};
    if (!std::ranges::equal(actual_summon_reload_state, summon_reload_state_expected)) {
        LOG_ERROR(Debug,
                  "Bloodborne summon-reload state signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  StageWarpDescriptorFinalizeOffset, BytesToHex(summon_reload_state_expected),
                  BytesToHex(actual_summon_reload_state));
        return;
    }
    const auto actual_healing_fountain_host_availability =
        std::span<const u8>{reinterpret_cast<const u8*>(base + HealingFountainAvailabilityOffset),
                            healing_fountain_host_availability_expected.size()};
    if (!std::ranges::equal(actual_healing_fountain_host_availability,
                            healing_fountain_host_availability_expected)) {
        LOG_ERROR(Debug,
                  "Bloodborne healing-fountain host-availability signature mismatch at {:#x}: "
                  "expected={} actual={}",
                  HealingFountainAvailabilityOffset,
                  BytesToHex(healing_fountain_host_availability_expected),
                  BytesToHex(actual_healing_fountain_host_availability));
        return;
    }
    for (const auto& site : CrossMapNativeCalls) {
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        const auto actual =
            std::span<const u8>{reinterpret_cast<const u8*>(base + site.offset), expected.size()};
        if (!std::ranges::equal(actual, expected)) {
            LOG_ERROR(Debug,
                      "Bloodborne cross-map native call {} signature mismatch at {:#x}: "
                      "expected={} actual={}",
                      site.name, site.offset, BytesToHex(expected), BytesToHex(actual));
            return;
        }
    }
    image_base = base;
    const size_t summon_build_entry_index =
        static_cast<size_t>(std::distance(Sites.begin(), summon_build_entry));
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(base + SummonBuildEntryOffset),
                              summon_build_entry_expected, summon_build_entry_index, TraceEntry)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne cross-map host-placement hook at {:#x}",
                  SummonBuildEntryOffset);
        return;
    }
    summon_build_host_placement_hook_installed = true;
    const size_t cross_map_guest_handoff_index =
        static_cast<size_t>(std::distance(Sites.begin(), cross_map_guest_handoff));
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(base + CrossMapGuestHandoffOffset),
                              cross_map_guest_handoff_expected, cross_map_guest_handoff_index,
                              TraceEntry)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne cross-map guest-handoff hook at {:#x}",
                  CrossMapGuestHandoffOffset);
        return;
    }
    cross_map_guest_handoff_hook_installed = true;
    const size_t summon_reload_state_index =
        static_cast<size_t>(std::distance(Sites.begin(), summon_reload_state));
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(base + StageWarpDescriptorFinalizeOffset),
                              summon_reload_state_expected, summon_reload_state_index,
                              TraceEntry)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne summon-reload state hook at {:#x}",
                  StageWarpDescriptorFinalizeOffset);
        return;
    }
    summon_reload_state_hook_installed = true;
    const size_t deferred_summon_reload_index =
        static_cast<size_t>(std::distance(Sites.begin(), deferred_summon_reload));
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(base + SosStatusUpdateOffset),
                              deferred_summon_reload_expected, deferred_summon_reload_index,
                              TraceEntry)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne deferred summon-reload hook at {:#x}",
                  SosStatusUpdateOffset);
        return;
    }
    deferred_summon_reload_hook_installed = true;
    const size_t healing_fountain_host_availability_index =
        static_cast<size_t>(std::distance(Sites.begin(), healing_fountain_host_availability));
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(base + HealingFountainAvailabilityOffset),
                              healing_fountain_host_availability_expected,
                              healing_fountain_host_availability_index, TraceEntry)) {
        LOG_ERROR(Debug,
                  "Failed to install Bloodborne healing-fountain host-availability hook at "
                  "{:#x}",
                  HealingFountainAvailabilityOffset);
        return;
    }
    healing_fountain_host_availability_hook_installed = true;

    std::memcpy(area_comparison, BeckoningSignedAreaComparison.data(),
                BeckoningSignedAreaComparison.size());
    GuestCodeWritten(area_comparison, BeckoningSignedAreaComparison.size());
    std::memcpy(area_flag_result, BeckoningAllowedAreaFlagResult.data(),
                BeckoningAllowedAreaFlagResult.size());
    GuestCodeWritten(area_flag_result, BeckoningAllowedAreaFlagResult.size());
    std::memcpy(responder_bell_area_result, ResponderBellAllowedAreaResult.data(),
                ResponderBellAllowedAreaResult.size());
    GuestCodeWritten(responder_bell_area_result, ResponderBellAllowedAreaResult.size());
    std::memcpy(responder_bell_common_result, ResponderBellAllowedCommonResult.data(),
                ResponderBellAllowedCommonResult.size());
    GuestCodeWritten(responder_bell_common_result, ResponderBellAllowedCommonResult.size());
    std::memcpy(active_bell_area_comparison, ActiveBellSignedAreaComparison.data(),
                ActiveBellSignedAreaComparison.size());
    GuestCodeWritten(active_bell_area_comparison, ActiveBellSignedAreaComparison.size());
    std::memcpy(active_bell_area_flag_result, ActiveBellAllowedAreaFlagResult.data(),
                ActiveBellAllowedAreaFlagResult.size());
    GuestCodeWritten(active_bell_area_flag_result, ActiveBellAllowedAreaFlagResult.size());
    std::memcpy(responder_search_area_range_result, ResponderSearchSignedAreaRangeResult.data(),
                ResponderSearchSignedAreaRangeResult.size());
    GuestCodeWritten(responder_search_area_range_result, ResponderSearchSignedAreaRangeResult.size());
    std::memcpy(sos_status_area_restriction, SosStatusAllowedArea.data(),
                SosStatusAllowedArea.size());
    GuestCodeWritten(sos_status_area_restriction, SosStatusAllowedArea.size());
    std::memcpy(summon_candidate_area_restriction, SummonCandidateAllowedCrossMap.data(),
                SummonCandidateAllowedCrossMap.size());
    GuestCodeWritten(summon_candidate_area_restriction, SummonCandidateAllowedCrossMap.size());
    std::memcpy(summon_build_area_restriction, SummonBuildAllowedCrossMap.data(),
                SummonBuildAllowedCrossMap.size());
    GuestCodeWritten(summon_build_area_restriction, SummonBuildAllowedCrossMap.size());
    std::memcpy(summon_build_world_state_restriction, SummonBuildAllowedWorldState.data(),
                SummonBuildAllowedWorldState.size());
    GuestCodeWritten(summon_build_world_state_restriction, SummonBuildAllowedWorldState.size());
    std::memcpy(summon_build_negative_event_restriction, SummonBuildAllowedNegativeEvent.data(),
                SummonBuildAllowedNegativeEvent.size());
    GuestCodeWritten(summon_build_negative_event_restriction, SummonBuildAllowedNegativeEvent.size());
    std::memcpy(stage_transition_stop_matching_call, StageTransitionKeepMatchingCall.data(),
                StageTransitionKeepMatchingCall.size());
    GuestCodeWritten(stage_transition_stop_matching_call, StageTransitionKeepMatchingCall.size());
    negative_area_patch_installed = true;
    area_flag_patch_installed = true;
    responder_bell_area_patch_installed = true;
    responder_bell_common_patch_installed = true;
    active_bell_negative_area_patch_installed = true;
    active_bell_area_flag_patch_installed = true;
    responder_search_negative_area_patch_installed = true;
    sos_status_area_patch_installed = true;
    summon_candidate_area_patch_installed = true;
    summon_build_area_patch_installed = true;
    summon_build_world_state_patch_installed = true;
    summon_build_negative_event_patch_installed = true;
    stage_transition_keep_matching_patch_installed = true;
    LOG_INFO(Debug,
             "Bloodborne seamless patches enabled Beckoning, Small Resonant, and Sinister Bell "
             "use, active search, cross-map candidate selection, and native summon construction "
             "with host-placement transport, guest stage handoff, retained matching during "
             "stage transitions, deferred native reload, and host lantern actions in negative "
             "and area-complete SOS regions at "
             "{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/{:#x}/"
             "{:#x}/{:#x}/{:#x}/{:#x}/{:#x}",
             BeckoningAreaComparisonOffset, BeckoningAreaFlagResultOffset,
             ResponderBellAreaResultOffset, ResponderBellCommonResultOffset,
             ActiveBellAreaComparisonOffset, ActiveBellAreaFlagResultOffset,
             ResponderSearchAreaRangeResultOffset, SosStatusAreaRestrictionOffset,
             SummonCandidateAreaRestrictionOffset, SummonBuildAreaRestrictionOffset,
             SummonBuildWorldStateRestrictionOffset, SummonBuildNegativeEventRestrictionOffset,
             StageTransitionStopMatchingCallOffset, SummonBuildEntryOffset,
             CrossMapGuestHandoffOffset, SosStatusUpdateOffset, SetSummonReloadStateOffset,
             HealingFountainAvailabilityOffset);
}

void InstallReverseEngineeringTrace() {
    if (installed || !EnvFlagEnabled("SHADPS4_BLOODBORNE_RE_TRACE")) {
        return;
    }
    if (MemoryPatcher::g_game_serial != "CUSA03173") {
        LOG_WARNING(Debug, "Bloodborne RE trace ignored for title {}",
                    MemoryPatcher::g_game_serial);
        return;
    }

    const auto* param_sfo = Common::Singleton<PSF>::Instance();
    const std::string_view app_version = param_sfo->GetString("APP_VER").value_or("Unknown");
    if (app_version != "01.09") {
        LOG_ERROR(Debug, "Bloodborne RE trace requires CUSA03173 01.09; loaded version is {}",
                  app_version);
        return;
    }

    image_base = MemoryPatcher::g_eboot_address;
    if (image_base == 0 || !VerifySites()) {
        LOG_ERROR(Debug, "Bloodborne RE trace was not installed");
        return;
    }

    const auto capture_dir =
        Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) / "bloodborne-re";
    std::error_code error;
    std::filesystem::create_directories(capture_dir, error);
    if (error) {
        LOG_ERROR(Debug, "Could not create Bloodborne RE capture directory {}: {}",
                  Common::FS::PathToUTF8String(capture_dir), error.message());
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const int process_id = Debugger::GetCurrentPid();
    capture_path = capture_dir / ("cusa03173-109-p" + std::to_string(process_id) + '-' +
                                  std::to_string(timestamp) + ".jsonl");
    capture_file.open(capture_path, std::ios::out | std::ios::trunc);
    if (!capture_file) {
        LOG_ERROR(Debug, "Could not open Bloodborne RE capture {}",
                  Common::FS::PathToUTF8String(capture_path));
        return;
    }
    capture_file << "{\"type\":\"header\",\"title\":\"CUSA03173\","
                    "\"app_version\":\"01.09\",\"image_base\":\"0x"
                 << std::hex << image_base << std::dec << "\",\"process_id\":" << process_id
                 << ",\"site_count\":" << Sites.size() << ",\"negative_area_patch\":"
                 << (negative_area_patch_installed ? "true" : "false")
                 << ",\"area_flag_patch\":" << (area_flag_patch_installed ? "true" : "false")
                 << ",\"responder_bell_area_patch\":"
                 << (responder_bell_area_patch_installed ? "true" : "false")
                 << ",\"responder_bell_common_patch\":"
                 << (responder_bell_common_patch_installed ? "true" : "false")
                 << ",\"active_bell_negative_area_patch\":"
                 << (active_bell_negative_area_patch_installed ? "true" : "false")
                 << ",\"active_bell_area_flag_patch\":"
                 << (active_bell_area_flag_patch_installed ? "true" : "false")
                 << ",\"responder_search_negative_area_patch\":"
                 << (responder_search_negative_area_patch_installed ? "true" : "false")
                 << ",\"sos_status_area_patch\":"
                 << (sos_status_area_patch_installed ? "true" : "false")
                 << ",\"summon_candidate_area_patch\":"
                 << (summon_candidate_area_patch_installed ? "true" : "false")
                 << ",\"summon_build_area_patch\":"
                 << (summon_build_area_patch_installed ? "true" : "false")
                 << ",\"summon_build_world_state_patch\":"
                 << (summon_build_world_state_patch_installed ? "true" : "false")
                 << ",\"summon_build_negative_event_patch\":"
                 << (summon_build_negative_event_patch_installed ? "true" : "false")
                 << ",\"stage_transition_keep_matching_patch\":"
                 << (stage_transition_keep_matching_patch_installed ? "true" : "false")
                 << ",\"summon_build_host_placement_hook\":"
                 << (summon_build_host_placement_hook_installed ? "true" : "false")
                 << ",\"cross_map_guest_handoff_hook\":"
                 << (cross_map_guest_handoff_hook_installed ? "true" : "false")
                 << ",\"summon_reload_state_hook\":"
                 << (summon_reload_state_hook_installed ? "true" : "false")
                 << ",\"deferred_summon_reload_hook\":"
                 << (deferred_summon_reload_hook_installed ? "true" : "false")
                 << ",\"healing_fountain_host_availability_hook\":"
                 << (healing_fountain_host_availability_hook_installed ? "true" : "false") << "}\n";
    capture_file.flush();

    size_t hook_count = 0;
    for (size_t index = 0; index < Sites.size(); ++index) {
        const auto& site = Sites[index];
        if ((summon_build_host_placement_hook_installed && site.offset == SummonBuildEntryOffset) ||
            (cross_map_guest_handoff_hook_installed && site.offset == CrossMapGuestHandoffOffset) ||
            (summon_reload_state_hook_installed &&
             site.offset == StageWarpDescriptorFinalizeOffset) ||
            (deferred_summon_reload_hook_installed && site.offset == SosStatusUpdateOffset) ||
            (healing_fountain_host_availability_hook_installed &&
             site.offset == HealingFountainAvailabilityOffset)) {
            ++hook_count;
            continue;
        }
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + site.offset), expected,
                                  index, TraceEntry)) {
            LOG_ERROR(Debug, "Failed to install Bloodborne RE hook {} at {:#x}", site.name,
                      site.offset);
            continue;
        }
        ++hook_count;
    }

    installed = hook_count == Sites.size();
    LOG_INFO(Debug, "Bloodborne RE trace installed {}/{} hooks; capture={}", hook_count,
             Sites.size(), Common::FS::PathToUTF8String(capture_path));
    InstallBbscSpectateHooks();
    InstallBbscWallHooks();
    InstallBbscEchoPathHooks();
    InstallBbscDreamHooks();
    InstallBbscFloorGuardAndPickupTrace();
    InstallBbscCinematicHook();
    InstallBbscQuitHook();
    InstallBbscDeathTraceHooks();
    InstallBbscBannerHook();
}

BbscPresence GetBbscPresence() {
    std::scoped_lock lock{bbsc_presence_mutex};
    return bbsc_presence;
}

} // namespace Core::Bloodborne
