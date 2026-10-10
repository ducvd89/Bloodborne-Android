// SPDX-License-Identifier: GPL-2.0-or-later
// The translator's PM4 decoder (gpu/cp): register state, draws, dispatches, indirect buffers,
// COND_EXEC and malformed buffers, on hand-built command buffers (no GPU, no game).
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "cp_decoder.h"

using namespace BbCp;

namespace {

u32 Type3(Op op, u32 body_words, bool predicated = false) {
    return 3u << 30 | (body_words - 1) << 16 | u32(op) << 8 | (predicated ? 1u : 0u);
}

struct Stream {
    std::vector<u32> words;
    Stream& Packet(Op op, std::vector<u32> body) {
        words.push_back(Type3(op, static_cast<u32>(body.size())));
        words.insert(words.end(), body.begin(), body.end());
        return *this;
    }
    Stream& Set(Op op, u32 offset, std::vector<u32> values) {
        values.insert(values.begin(), offset);
        return Packet(op, values);
    }
    Stream& Filler() {
        words.push_back(2u << 30);
        return *this;
    }
};

struct Recorder : Sink {
    std::vector<std::string> events;
    const Decoder* decoder = nullptr;
    u32 skip = 0; ///< what ConditionalSkip answers

    void OnPacket(const Packet& packet) override {
        events.push_back("packet " + std::to_string(u32(packet.Opcode())) + " depth " +
                         std::to_string(packet.depth));
    }
    void OnDraw(const Draw& draw) override {
        events.push_back("draw " + std::to_string(u32(draw.kind)) + " count " +
                         std::to_string(draw.index_count));
    }
    void OnDispatch(const Dispatch& dispatch) override {
        events.push_back("dispatch " + std::to_string(dispatch.x) + "x" +
                         std::to_string(dispatch.y) + "x" + std::to_string(dispatch.z));
    }
    std::span<const u32> ResolveIndirectBuffer(u64 address, u32 dwords) override {
        return {reinterpret_cast<const u32*>(address), dwords};
    }
    u32 ConditionalSkip(u64, u32 exec_count, const Packet&) override {
        return skip ? exec_count : 0;
    }
    void OnBadPacket(std::span<const u32>, const char* what, int depth) override {
        events.push_back(std::string("bad ") + what + " depth " + std::to_string(depth));
    }
};

void RegistersAndDraws() {
    Decoder decoder;
    Recorder sink;
    sink.decoder = &decoder;
    // An indirect buffer with uconfig state and an indexed draw.
    Stream nested;
    nested.Set(Op::SetUconfigReg, 0x20, {0xabc})
        .Packet(Op::DrawIndex2, {0x100, 0x1000, 0x12, 36, 0x1});
    Stream top;
    top.Set(Op::SetContextReg, 0x10, {1, 2, 3})
        .Filler()
        .Set(Op::SetShReg, 0x8, {0x55})
        .Packet(Op::NumInstances, {4})
        .Packet(Op::DrawIndexAuto, {6, 0x2})
        .Packet(Op::IndirectBuffer, {u32(reinterpret_cast<u64>(nested.words.data())),
                                     u32(reinterpret_cast<u64>(nested.words.data()) >> 32),
                                     u32(nested.words.size())})
        .Packet(Op::DispatchDirect, {8, 4, 1, 0x1});
    decoder.Decode(top.words, sink);
    const auto& r = decoder.Registers();
    assert(r[Bank::Context + 0x10] == 1 && r[Bank::Context + 0x12] == 3);
    assert(r[Bank::Sh + 0x8] == 0x55 && r[Bank::Uconfig + 0x20] == 0xabc);
    assert(r[Reg::VgtNumInstances] == 4);
    // DRAW_INDEX_2 wrote the index registers (the base's high bits: 8 of them).
    assert(r[Reg::VgtDmaMaxSize] == 0x100 && r[Reg::VgtDmaBase] == 0x1000);
    assert(r[Reg::VgtDmaBaseHi] == 0x12 && r[Reg::VgtNumIndices] == 36);
    assert(r[Reg::VgtDrawInitiator] == 0x1);
    const auto& s = decoder.Stats();
    assert(s.draws == 2 && s.dispatches == 1 && s.indirect_buffers == 1 && s.filler == 1);
    assert(s.packets == 8 && s.bad == 0);
    // In stream order, the nested buffer's packets between the indirect buffer and the dispatch.
    const std::vector<std::string> expected = {
        "packet 105 depth 0", "packet 118 depth 0", "packet 47 depth 0", "packet 45 depth 0",
        "draw 2 count 6",     "packet 63 depth 0",  "packet 121 depth 1", "packet 39 depth 1",
        "draw 0 count 36",    "packet 21 depth 0",  "dispatch 8x4x1"};
    assert(sink.events == expected);
}

void PartialWritesClearStateAndEvents() {
    Decoder decoder;
    Recorder sink;
    sink.decoder = &decoder;
    std::vector<u32> defaults(Bank::End, 0);
    defaults[Bank::Context + 0x10] = 0x77;
    decoder.Registers().SetDefaults(defaults);
    // INDEX_BASE keeps the high word's upper bits; a streamout flush sets bit 0 only.
    decoder.Registers().Write(Reg::VgtDmaBaseHi, 0xAB00);
    decoder.Registers().Write(Reg::CpStrmoutCntl, 0x10);
    Stream s;
    s.Set(Op::SetContextReg, 0x10, {5})
        .Packet(Op::IndexBase, {0x2000, 0x1FF})
        .Packet(Op::EventWrite, {Event::SoVgtStreamoutFlush})
        .Packet(Op::EventWrite, {0x14}); // another event: no register
    decoder.Decode(s.words, sink);
    const auto& r = decoder.Registers();
    assert(r[Reg::VgtDmaBase] == 0x2000 && r[Reg::VgtDmaBaseHi] == 0xABFF);
    assert(r[Reg::CpStrmoutCntl] == 0x11 && r[Bank::Context + 0x10] == 5);
    assert(decoder.Stats().register_packets == 3);
    Stream clear;
    clear.Packet(Op::ClearState, {0});
    decoder.Decode(clear.words, sink);
    assert(r[Bank::Context + 0x10] == 0x77 && r[Reg::VgtDmaBase] == 0);
}

void ConditionalExecution() {
    for (const u32 skip : {0u, 1u}) {
        Decoder decoder;
        Recorder sink;
        sink.decoder = &decoder;
        sink.skip = skip;
        Stream s;
        // COND_EXEC over the next 4 dwords: a 3-dword SET_CONTEXT_REG (header + offset + value)
        // and a filler.
        s.Packet(Op::CondExec, {0x1000, 0, 4}).Set(Op::SetContextReg, 0x30, {9}).Filler();
        s.Packet(Op::DrawIndexAuto, {3, 0x2});
        decoder.Decode(s.words, sink);
        assert(decoder.Registers()[Bank::Context + 0x30] == (skip ? 0u : 9u));
        assert(decoder.Stats().draws == 1);
    }
}

void MalformedBuffers() {
    Decoder decoder;
    Recorder sink;
    sink.decoder = &decoder;
    // A packet claiming 5 body dwords with 2 left; a type-0 packet.
    std::vector<u32> truncated = {Type3(Op::SetContextReg, 5), 0x10, 1};
    decoder.Decode(truncated, sink);
    std::vector<u32> type0 = {0x00010010, 1, 2};
    decoder.Decode(type0, sink, 1);
    assert(decoder.Stats().bad == 2 && decoder.Stats().packets == 0);
    assert(sink.events.size() == 2 &&
           sink.events[0] == "bad packet past the end of its buffer depth 0" &&
           sink.events[1] == "bad type-0 packet depth 1");
    assert(decoder.Registers()[Bank::Context + 0x10] == 0);
}

void CursorObserverAndOwnerStorage() {
    struct Observer : RegisterObserver {
        std::vector<RegisterWrite> writes;
        u32 packets = 0, clears = 0;
        void OnRegisterPacket(std::span<const u32> words, std::span<const RegisterWrite> written,
                              bool clear_state) override {
            writes.insert(writes.end(), written.begin(), written.end());
            clears += clear_state;
            assert(Header{words[0]}.Type() == 3 && words.size() == Header{words[0]}.BodyWords() + 1);
            ++packets;
        }
    };
    std::vector<u32> storage(Bank::End, 0);
    Decoder decoder{std::span<u32, Bank::End>{storage.data(), Bank::End}, {true, 4}};
    Observer observer;
    decoder.SetObserver(&observer);
    Recorder sink;
    Stream nested;
    nested.Set(Op::SetContextReg, 0x40, {7});
    Stream s;
    s.Set(Op::SetContextReg, 0x20, {1, 2})
        .Set(Op::SetShReg, 0x200, {0x99}) // the compute program: left to the owner
        .Set(Op::SetShReg, 0x10, {0x42})
        .Packet(Op::IndirectBuffer, {u32(reinterpret_cast<u64>(nested.words.data())),
                                     u32(reinterpret_cast<u64>(nested.words.data()) >> 32),
                                     u32(nested.words.size())})
        .Packet(Op::CondExec, {0x1000, 0, 3})
        .Set(Op::SetContextReg, 0x21, {5}) // skipped by the caller
        .Packet(Op::ClearState, {0});
    // Not descending: the IB packet comes back, its buffer is the caller's.
    Cursor cursor{decoder, s.words, sink, 0, false};
    std::vector<Op> seen;
    while (const Packet* packet = cursor.Next()) {
        seen.push_back(packet->Opcode());
        if (packet->Opcode() == Op::CondExec) {
            cursor.Skip(packet->Word(2));
        }
        if (packet->Opcode() == Op::ClearState) {
            break;
        }
        assert(storage[Bank::Context + 0x20] == 1); // the owner's storage holds the state
    }
    const std::vector<Op> expected = {Op::SetContextReg, Op::SetShReg, Op::SetShReg,
                                      Op::IndirectBuffer, Op::CondExec, Op::ClearState};
    assert(seen == expected);
    assert(storage[Bank::Context + 0x40] == 0 && decoder.Stats().indirect_buffers == 1);
    // Written before CLEAR_STATE: the context pair and the SH register, not the compute one.
    const std::vector<RegisterWrite> writes = {{Bank::Context + 0x20, 2}, {Bank::Sh + 0x10, 1}};
    assert(observer.writes == writes);
    assert(observer.packets == 4 && observer.clears == 1);
    assert(storage[Bank::Sh + 0x200] == 0 && storage[Bank::Context + 0x20] == 0);
}

} // namespace

int main() {
    RegistersAndDraws();
    PartialWritesClearStateAndEvents();
    ConditionalExecution();
    MalformedBuffers();
    CursorObserverAndOwnerStorage();
    std::puts("PASS: cp decoder: registers, draws, dispatches, indirect buffers, COND_EXEC, "
              "malformed buffers, cursor, observer");
}
