// SPDX-License-Identifier: GPL-2.0-or-later
// bbport cp: the translator's own PM4 decoder (step 2 of docs/TRANSLATOR_ARCHITECTURE.ru.md).
//
// A graphics command buffer is a stream of packets. The decoder walks it in stream order, applies
// the register packets to its register file (the command processor's state: SET_*_REG, the
// registers the draw packets write), descends into indirect buffers, and hands every packet to a
// Sink with that state already applied: draws and dispatches as typed calls, the rest as packets.
// It knows nothing of Vulkan, of the game, or of where the work runs; the sink decides (the
// translator, a test, the shadow check against the old decoder, cp_shadow.h).
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "cp_pm4.h"

namespace BbCp {

/// The command processor's registers, by word index (register byte address / 4): its own
/// storage, or the owner's (an array of Bank::End words laid out the same way).
class RegisterFile {
public:
    RegisterFile()
        : storage{std::make_unique<std::array<u32, Bank::End>>()}, words{storage->data()} {
        storage->fill(0);
    }
    explicit RegisterFile(std::span<u32, Bank::End> external) : words{external.data()} {}

    u32 operator[](u32 index) const {
        return index < Bank::End ? words[index] : 0;
    }
    std::span<const u32> Words() const {
        return {words, Bank::End};
    }

    /// `values` into [index, index + size), clipped to the file; the words written.
    u32 Write(u32 index, std::span<const u32> values) {
        if (index >= Bank::End) {
            return 0;
        }
        const u32 count = static_cast<u32>(std::min<size_t>(values.size(), Bank::End - index));
        std::memcpy(&words[index], values.data(), count * sizeof(u32));
        return count;
    }
    u32 Write(u32 index, u32 value) {
        return Write(index, std::span{&value, 1});
    }
    /// The bits of `mask` from `value`, the others kept.
    u32 WriteBits(u32 index, u32 value, u32 mask) {
        if (index >= Bank::End) {
            return 0;
        }
        words[index] = (words[index] & ~mask) | (value & mask);
        return 1;
    }

    /// The state CLEAR_STATE restores (the hardware's defaults; set by the owner).
    void SetDefaults(std::span<const u32> values) {
        if (!defaults) {
            defaults = std::make_unique<std::array<u32, Bank::End>>();
            defaults->fill(0);
        }
        const size_t count = std::min<size_t>(values.size(), Bank::End);
        std::memcpy(defaults->data(), values.data(), count * sizeof(u32));
    }
    void ClearState() {
        if (defaults) {
            std::memcpy(words, defaults->data(), Bank::End * sizeof(u32));
        } else {
            std::memset(words, 0, Bank::End * sizeof(u32));
        }
    }

private:
    std::unique_ptr<std::array<u32, Bank::End>> storage;
    u32* words;
    std::unique_ptr<std::array<u32, Bank::End>> defaults;
};

/// A packet as decoded: its header and body (the dwords after the header).
struct Packet {
    Header header;
    std::span<const u32> body;
    int depth; ///< 0: the submitted buffer, 1: an indirect buffer in it, ...

    Op Opcode() const {
        return header.Opcode();
    }
    /// Dword `i` of the body, or 0 past its end.
    u32 Word(size_t i) const {
        return i < body.size() ? body[i] : 0;
    }
};

enum class DrawKind : std::uint8_t {
    Index2,       ///< DRAW_INDEX_2: indices from an address in the packet
    IndexOffset2, ///< DRAW_INDEX_OFFSET_2: indices from the bound buffer, at an offset
    IndexAuto,    ///< DRAW_INDEX_AUTO: no index buffer
    Indirect,     ///< DRAW_INDIRECT and the indexed and multi forms: arguments in memory
    IndexIndirect,
    IndirectMulti,
    IndexIndirectMulti,
    IndexIndirectCountMulti,
};

struct Draw {
    DrawKind kind;
    u32 index_count;    ///< direct draws
    u32 index_offset;   ///< DRAW_INDEX_OFFSET_2
    u32 draw_initiator; ///< VGT_DRAW_INITIATOR
    const Packet* packet;
};

struct Dispatch {
    bool indirect;
    u32 x, y, z;     ///< thread groups (direct)
    u32 data_offset; ///< indirect: the arguments' offset from the base set by SET_BASE
    u32 dispatch_initiator;
    const Packet* packet;
};

/// What the decoder found, in stream order.
class Sink {
public:
    virtual ~Sink() = default;
    /// Every type-3 packet (draws and dispatches too, before their own call), after the register
    /// writes it makes.
    virtual void OnPacket(const Packet&) {}
    virtual void OnDraw(const Draw&) {}
    virtual void OnDispatch(const Dispatch&) {}
    /// An INDIRECT_BUFFER of `dwords` at `address`: the buffer to decode there (empty: not
    /// descended into).
    virtual std::span<const u32> ResolveIndirectBuffer(u64 /*address*/, u32 /*dwords*/) {
        return {};
    }
    /// COND_EXEC: the dwords after it to skip (`exec_count` when the 32-bit condition at
    /// `address` is zero as the command processor reads it there); 0: they run.
    virtual u32 ConditionalSkip(u64 /*address*/, u32 /*exec_count*/, const Packet&) {
        return 0;
    }
    /// A packet that cannot be decoded (`what`); decoding of this buffer stops there.
    virtual void OnBadPacket(std::span<const u32> /*rest*/, const char* /*what*/, int /*depth*/) {}
};

/// Counts of what was decoded (for checks and statistics).
struct DecodeStats {
    u64 packets = 0, draws = 0, dispatches = 0, indirect_buffers = 0, register_packets = 0;
    u64 filler = 0, bad = 0;
    std::array<u64, 256> by_opcode{};
};

/// The SH register range of the compute program (COMPUTE_*), written by graphics buffers for
/// their dispatches.
inline constexpr u32 ComputeShFirst = Bank::Sh + 0x200;

/// A range of registers a packet wrote.
struct RegisterWrite {
    u32 index, count;
    bool operator==(const RegisterWrite&) const = default;
};

/// Told of the decoder's register writes (the owner's dirty tracking and checksums): once for
/// each packet that wrote registers.
class RegisterObserver {
public:
    virtual ~RegisterObserver() = default;
    /// The whole packet (header first) and the ranges it wrote; `clear_state`: CLEAR_STATE, the
    /// whole file restored.
    virtual void OnRegisterPacket(std::span<const u32> words, std::span<const RegisterWrite> writes,
                                  bool clear_state) = 0;
};

struct DecoderOptions {
    /// SET_SH_REG packets starting in the compute program range (ComputeShFirst, `compute_words`
    /// on) are left to the owner (a per-queue compute state), not written to the file.
    bool compute_sh_to_owner = false;
    u32 compute_words = 0;
};

class Decoder {
public:
    Decoder() = default;
    /// Registers in the owner's storage.
    explicit Decoder(std::span<u32, Bank::End> storage, DecoderOptions options_ = {})
        : registers{storage}, options{options_} {}

    RegisterFile& Registers() {
        return registers;
    }
    const DecodeStats& Stats() const {
        return stats;
    }
    void ResetStats() {
        stats = {};
    }
    void SetObserver(RegisterObserver* observer_) {
        observer = observer_;
    }

    /// Decodes `buffer` (a graphics command buffer, or an indirect buffer at `depth` > 0).
    void Decode(std::span<const u32> buffer, Sink& sink, int depth = 0);

private:
    friend class Cursor;
    /// The register writes of a packet; false when it writes none.
    bool ApplyRegisters(const Packet& packet);

    RegisterFile registers;
    DecoderOptions options;
    RegisterObserver* observer = nullptr;
    DecodeStats stats;
};

/// The packets of a buffer one at a time, in stream order, each with its register writes
/// applied: for a loop that has to stop between packets (a wait that yields to other queues).
class Cursor {
public:
    /// `descend`: indirect buffers are decoded in place (the IB packet, then its packets);
    /// else the caller gets the IB packet and decodes the buffer itself.
    Cursor(Decoder& decoder, std::span<const u32> buffer, Sink& sink, int depth = 0,
           bool descend = true);

    /// The next type-3 packet (fillers skipped), valid until the next call; null at the end, or
    /// after a packet that cannot be decoded (told to the sink).
    const Packet* Next();

    /// Skips `dwords` after the packet Next returned last (COND_EXEC), clipped to its buffer.
    void Skip(u32 dwords);

    static constexpr int MaxDepth = 8; ///< indirect buffers in indirect buffers

private:
    struct Level {
        std::span<const u32> rest;
        int depth;
    };
    Decoder& decoder;
    Sink& sink;
    bool descend;
    std::vector<Level> levels;
    u64 pending_address = 0; ///< an indirect buffer to descend into on the next call
    u32 pending_dwords = 0;
    bool pending = false;
    Packet current{};
};

} // namespace BbCp
