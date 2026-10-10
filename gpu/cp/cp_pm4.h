// SPDX-License-Identifier: GPL-2.0-or-later
// bbport cp: the PM4 packet format of the PS4's command processor (GCN, CIK generation), as the
// translator's own code: headers, opcodes, register banks. Only hardware facts live here; what a
// packet does to the translator is the decoder's (cp_decoder.h).
#pragma once

#include <cstdint>

namespace BbCp {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Type-3 opcodes (the IT_* numbers of the CIK command processor).
enum class Op : std::uint8_t {
    Nop = 0x10,
    SetBase = 0x11,
    ClearState = 0x12,
    IndexBufferSize = 0x13,
    DispatchDirect = 0x15,
    DispatchIndirect = 0x16,
    AtomicGds = 0x1D,
    Atomic = 0x1E,
    OcclusionQuery = 0x1F,
    SetPredication = 0x20,
    RegRmw = 0x21,
    CondExec = 0x22,
    PredExec = 0x23,
    DrawIndirect = 0x24,
    DrawIndexIndirect = 0x25,
    IndexBase = 0x26,
    DrawIndex2 = 0x27,
    ContextControl = 0x28,
    IndexType = 0x2A,
    DrawIndirectMulti = 0x2C,
    DrawIndexAuto = 0x2D,
    NumInstances = 0x2F,
    DrawIndexMultiAuto = 0x30,
    IndirectBufferConst = 0x33,
    StrmoutBufferUpdate = 0x34,
    DrawIndexOffset2 = 0x35,
    WriteData = 0x37,
    DrawIndexIndirectMulti = 0x38,
    MemSemaphore = 0x39,
    WaitRegMem = 0x3C,
    IndirectBuffer = 0x3F,
    CopyData = 0x40,
    CpDma = 0x41,
    PfpSyncMe = 0x42,
    SurfaceSync = 0x43,
    CondWrite = 0x45,
    EventWrite = 0x46,
    EventWriteEop = 0x47,
    EventWriteEos = 0x48,
    ReleaseMem = 0x49,
    PreambleCntl = 0x4A,
    DmaData = 0x50,
    ContextRegRmw = 0x51,
    AcquireMem = 0x58,
    Rewind = 0x59,
    LoadShReg = 0x5F,
    LoadConfigReg = 0x60,
    LoadContextReg = 0x61,
    SetConfigReg = 0x68,
    SetContextReg = 0x69,
    SetContextRegIndirect = 0x73,
    SetShReg = 0x76,
    SetShRegOffset = 0x77,
    SetQueueReg = 0x78,
    SetUconfigReg = 0x79,
    LoadConstRam = 0x80,
    WriteConstRam = 0x81,
    DumpConstRam = 0x83,
    IncrementCeCounter = 0x84,
    IncrementDeCounter = 0x85,
    WaitOnCeCounter = 0x86,
    WaitOnDeCounterDiff = 0x88,
    GetLodStats = 0x8E,
    DrawIndexIndirectCountMulti = 0x9D,
};

/// The first dword of a packet.
struct Header {
    u32 raw;

    constexpr u32 Type() const {
        return raw >> 30;
    }
    /// Type 3: dwords after the header.
    constexpr u32 BodyWords() const {
        return ((raw >> 16) & 0x3FFF) + 1;
    }
    constexpr Op Opcode() const {
        return static_cast<Op>((raw >> 8) & 0xFF);
    }
    /// Type 3: runs only while the predicate holds (SET_PREDICATION).
    constexpr bool Predicated() const {
        return raw & 1;
    }
    /// Type 3: for the compute pipe (1) or graphics (0).
    constexpr bool ComputeShaderType() const {
        return (raw >> 1) & 1;
    }
    /// Type 0: the first register written, and how many.
    constexpr u32 Type0Base() const {
        return raw & 0xFFFF;
    }
};

/// Register banks: word indices of the register file (register byte address / 4).
namespace Bank {
inline constexpr u32 Config = 0x2000;
inline constexpr u32 Sh = 0x2C00;
inline constexpr u32 Context = 0xA000;
inline constexpr u32 Uconfig = 0xC000;
inline constexpr u32 End = 0xD000; ///< words in the register file
} // namespace Bank

/// Registers the draw packets write besides their own payload (word indices).
namespace Reg {
inline constexpr u32 CpStrmoutCntl = 0xC03F;  ///< CP_STRMOUT_CNTL (uconfig on CIK)
inline constexpr u32 VgtDmaBaseHi = 0xA1F9;   ///< VGT_DMA_BASE_HI (context)
inline constexpr u32 VgtDmaBase = 0xA1FA;     ///< VGT_DMA_BASE
inline constexpr u32 VgtDrawInitiator = 0xA1FC;
inline constexpr u32 VgtDmaMaxSize = 0xA29E;
inline constexpr u32 VgtDmaIndexType = 0xA29F;
inline constexpr u32 VgtNumIndices = 0xC24C;   ///< VGT_NUM_INDICES (uconfig)
inline constexpr u32 VgtNumInstances = 0xC24D; ///< VGT_NUM_INSTANCES (uconfig)
} // namespace Reg

/// EVENT_WRITE event types the decoder looks at.
namespace Event {
inline constexpr u32 SoVgtStreamoutFlush = 0x1F;
} // namespace Event

} // namespace BbCp
