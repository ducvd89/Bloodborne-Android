// SPDX-License-Identifier: GPL-2.0-or-later
#include "cp_decoder.h"

namespace BbCp {

bool Decoder::ApplyRegisters(const Packet& packet) {
    const auto& body = packet.body;
    std::array<RegisterWrite, 5> writes;
    u32 num_writes = 0;
    const auto noted = [&](u32 index, u32 count) {
        if (count != 0) {
            writes[num_writes++] = {index, count};
        }
    };
    const auto write = [&](u32 index, u32 value) { noted(index, registers.Write(index, value)); };
    const auto set = [&](u32 bank) {
        if (body.empty()) {
            return;
        }
        const u32 index = bank + (body[0] & 0xFFFF);
        noted(index, registers.Write(index, body.subspan(1)));
    };
    switch (packet.Opcode()) {
    case Op::ClearState:
        registers.ClearState();
        break;
    case Op::SetConfigReg:
        set(Bank::Config);
        break;
    case Op::SetContextReg:
        set(Bank::Context);
        break;
    case Op::SetShReg: {
        const u32 offset = packet.Word(0) & 0xFFFF;
        const u32 first = ComputeShFirst - Bank::Sh;
        if (!(options.compute_sh_to_owner && offset >= first &&
              offset <= first + options.compute_words)) {
            set(Bank::Sh);
        }
        break;
    }
    case Op::SetUconfigReg:
        set(Bank::Uconfig);
        break;
    case Op::IndexType:
        write(Reg::VgtDmaIndexType, packet.Word(0));
        break;
    case Op::DrawIndex2:
        // max size, base low, base high (8 bits), index count, draw initiator.
        write(Reg::VgtDmaMaxSize, packet.Word(0));
        write(Reg::VgtDmaBase, packet.Word(1));
        noted(Reg::VgtDmaBaseHi, registers.WriteBits(Reg::VgtDmaBaseHi, packet.Word(2), 0xFF));
        write(Reg::VgtNumIndices, packet.Word(3));
        write(Reg::VgtDrawInitiator, packet.Word(4));
        break;
    case Op::DrawIndexOffset2:
        // max size, index offset, index count, draw initiator.
        write(Reg::VgtDmaMaxSize, packet.Word(0));
        write(Reg::VgtNumIndices, packet.Word(2));
        write(Reg::VgtDrawInitiator, packet.Word(3));
        break;
    case Op::DrawIndexAuto:
        write(Reg::VgtNumIndices, packet.Word(0));
        write(Reg::VgtDrawInitiator, packet.Word(1));
        break;
    case Op::NumInstances:
        write(Reg::VgtNumInstances, packet.Word(0));
        break;
    case Op::IndexBase:
        write(Reg::VgtDmaBase, packet.Word(0));
        noted(Reg::VgtDmaBaseHi, registers.WriteBits(Reg::VgtDmaBaseHi, packet.Word(1), 0xFF));
        break;
    case Op::IndexBufferSize:
        write(Reg::VgtNumIndices, packet.Word(0));
        break;
    case Op::EventWrite:
        // A streamout flush: the offsets are updated at once (no streamout here).
        if ((packet.Word(0) & 0x3F) != Event::SoVgtStreamoutFlush) {
            return false;
        }
        noted(Reg::CpStrmoutCntl, registers.WriteBits(Reg::CpStrmoutCntl, 1, 1));
        break;
    default:
        return false;
    }
    if (observer) {
        observer->OnRegisterPacket({packet.body.data() - 1, packet.body.size() + 1},
                                   {writes.data(), num_writes}, packet.Opcode() == Op::ClearState);
    }
    return true;
}

Cursor::Cursor(Decoder& decoder_, std::span<const u32> buffer, Sink& sink_, int depth,
               bool descend_)
    : decoder{decoder_}, sink{sink_}, descend{descend_} {
    levels.push_back({buffer, depth});
}

const Packet* Cursor::Next() {
    auto& stats = decoder.stats;
    if (pending) {
        pending = false;
        const int depth = levels.back().depth + 1;
        if (depth >= MaxDepth) {
            ++stats.bad;
            sink.OnBadPacket({}, "indirect buffers nested too deep", depth);
        } else if (const auto nested = sink.ResolveIndirectBuffer(pending_address, pending_dwords);
                   !nested.empty()) {
            levels.push_back({nested, depth});
        }
    }
    while (!levels.empty()) {
        auto& level = levels.back();
        if (level.rest.empty()) {
            levels.pop_back();
            continue;
        }
        const Header header{level.rest[0]};
        if (header.Type() == 2) { // filler
            ++stats.filler;
            level.rest = level.rest.subspan(1);
            continue;
        }
        if (header.Type() != 3) {
            ++stats.bad;
            sink.OnBadPacket(level.rest, header.Type() == 0 ? "type-0 packet" : "type-1 packet",
                             level.depth);
            levels.clear();
            return nullptr;
        }
        const u32 words = header.BodyWords();
        if (words + 1 > level.rest.size()) {
            ++stats.bad;
            sink.OnBadPacket(level.rest, "packet past the end of its buffer", level.depth);
            levels.clear();
            return nullptr;
        }
        current = {header, level.rest.subspan(1, words), level.depth};
        const Packet& packet = current;
        level.rest = level.rest.subspan(words + 1);
        ++stats.packets;
        ++stats.by_opcode[static_cast<u32>(packet.Opcode())];
        if (decoder.ApplyRegisters(packet)) {
            ++stats.register_packets;
        }
        if (packet.Opcode() == Op::IndirectBuffer) {
            ++stats.indirect_buffers;
            if (descend) {
                pending = true;
                pending_address = packet.Word(0) | u64(packet.Word(1) & 0xFFFF) << 32;
                pending_dwords = packet.Word(2) & 0xFFFFF;
            }
        }
        return &current;
    }
    return nullptr;
}

void Cursor::Skip(u32 dwords) {
    if (levels.empty()) {
        return;
    }
    auto& rest = levels.back().rest;
    rest = rest.subspan(std::min<size_t>(dwords, rest.size()));
}

void Decoder::Decode(std::span<const u32> buffer, Sink& sink, int depth) {
    Cursor cursor{*this, buffer, sink, depth, true};
    while (const Packet* packet = cursor.Next()) {
        sink.OnPacket(*packet);
        switch (packet->Opcode()) {
        case Op::DrawIndex2:
            ++stats.draws;
            sink.OnDraw({DrawKind::Index2, packet->Word(3), 0, packet->Word(4), packet});
            break;
        case Op::DrawIndexOffset2:
            ++stats.draws;
            sink.OnDraw({DrawKind::IndexOffset2, packet->Word(2), packet->Word(1),
                         packet->Word(3), packet});
            break;
        case Op::DrawIndexAuto:
            ++stats.draws;
            sink.OnDraw({DrawKind::IndexAuto, packet->Word(0), 0, packet->Word(1), packet});
            break;
        case Op::DrawIndirect:
        case Op::DrawIndexIndirect:
        case Op::DrawIndirectMulti:
        case Op::DrawIndexIndirectMulti:
        case Op::DrawIndexIndirectCountMulti: {
            static constexpr auto kind = [](Op op) {
                switch (op) {
                case Op::DrawIndirect:
                    return DrawKind::Indirect;
                case Op::DrawIndexIndirect:
                    return DrawKind::IndexIndirect;
                case Op::DrawIndirectMulti:
                    return DrawKind::IndirectMulti;
                case Op::DrawIndexIndirectMulti:
                    return DrawKind::IndexIndirectMulti;
                default:
                    return DrawKind::IndexIndirectCountMulti;
                }
            };
            ++stats.draws;
            // The draw initiator is the last dword of each of these.
            sink.OnDraw({kind(packet->Opcode()), 0, 0, packet->body.back(), packet});
            break;
        }
        case Op::DispatchDirect:
            ++stats.dispatches;
            sink.OnDispatch({false, packet->Word(0), packet->Word(1), packet->Word(2), 0,
                             packet->Word(3), packet});
            break;
        case Op::DispatchIndirect:
            ++stats.dispatches;
            sink.OnDispatch({true, 0, 0, 0, packet->Word(0), packet->Word(1), packet});
            break;
        case Op::CondExec: {
            const u64 address = (packet->Word(0) & ~3u) | u64(packet->Word(1) & 0xFFFF) << 32;
            cursor.Skip(sink.ConditionalSkip(address, packet->Word(2) & 0x3FFF, *packet));
            break;
        }
        default:
            break;
        }
    }
}

} // namespace BbCp
