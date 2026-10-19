// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "shader_recompiler/frontend/instruction.h"

namespace Shader::Gcn {

struct InstFormat {
    InstClass inst_class = InstClass::Undefined;
    InstCategory inst_category = InstCategory::Undefined;
    u32 src_count = 0;
    u32 dst_count = 0;
    ScalarType src_type = ScalarType::Undefined;
    ScalarType dst_type = ScalarType::Undefined;
};

InstEncoding GetInstructionEncoding(u32 token);

u32 GetEncodingLength(InstEncoding encoding);

InstFormat InstructionFormat(InstEncoding encoding, u32 opcode);

Opcode DecodeOpcode(u32 token);

/// bbport: whether every word of a program decodes (tryDecodeInstruction); else the index of
/// the first one that does not, in `bad`.
bool ProgramDecodes(std::span<const u32> code, size_t* bad = nullptr);

class GcnCodeSlice {
public:
    GcnCodeSlice(const u32* ptr, const u32* end) : m_ptr(ptr), m_end(end) {}
    GcnCodeSlice(const GcnCodeSlice& other) = default;
    ~GcnCodeSlice() = default;

    u32 at(u32 id) const {
        return m_ptr[id];
    }

    u32 readu32() {
        return *(m_ptr++);
    }

    u64 readu64() {
        const u64 value = *(u64*)m_ptr;
        m_ptr += 2;
        return value;
    }

    bool atEnd() const {
        return m_ptr == m_end;
    }

    // bbport: current read position (fetch shader parse cache)
    const u32* position() const {
        return m_ptr;
    }

private:
    const u32* m_ptr{};
    const u32* m_end{};
};

class GcnDecodeContext {
public:
    GcnInst decodeInstruction(GcnCodeSlice& code);
    /// bbport: decodeInstruction for words that may not be a program (memory the game is still
    /// filling, or reused): false instead of an assertion for an illegal encoding, an SDWA/DPP
    /// operand (not in a PS4 program) or an opcode the format table does not know; `out` and the
    /// slice's position are valid only when true.
    bool tryDecodeInstruction(GcnCodeSlice& code, GcnInst& out);

private:
    uint32_t getEncodingLength(InstEncoding encoding);
    uint32_t getOpMapOffset(InstEncoding encoding);
    uint32_t mapEncodingOp(InstEncoding encoding, Opcode opcode);
    void updateInstructionMeta(InstEncoding encoding);
    uint32_t getMimgModifier(Opcode opcode);
    void repairOperandType();

    OperandField getOperandField(uint32_t code);

    void decodeInstruction32(InstEncoding encoding, GcnCodeSlice& code);
    void decodeInstruction64(InstEncoding encoding, GcnCodeSlice& code);
    void decodeLiteralConstant(InstEncoding encoding, GcnCodeSlice& code);
    void decodeSubDwordAddressing(InstEncoding encoding, GcnCodeSlice& code);
    void decodeDataParallelPrimitive(InstEncoding encoding, GcnCodeSlice& code);

    // 32 bits encodings
    void decodeInstructionSOP1(uint32_t hexInstruction);
    void decodeInstructionSOPP(uint32_t hexInstruction);
    void decodeInstructionSOPC(uint32_t hexInstruction);
    void decodeInstructionSOPK(uint32_t hexInstruction);
    void decodeInstructionSOP2(uint32_t hexInstruction);
    void decodeInstructionVOP1(uint32_t hexInstruction);
    void decodeInstructionVOPC(uint32_t hexInstruction);
    void decodeInstructionVOP2(uint32_t hexInstruction);
    void decodeInstructionSMRD(uint32_t hexInstruction);
    void decodeInstructionVINTRP(uint32_t hexInstruction);
    // 64 bits encodings
    void decodeInstructionVOP3(uint64_t hexInstruction);
    void decodeInstructionVOP3P(uint64_t hexInstruction);
    void decodeInstructionMUBUF(uint64_t hexInstruction);
    void decodeInstructionMTBUF(uint64_t hexInstruction);
    void decodeInstructionMIMG(uint64_t hexInstruction);
    void decodeInstructionDS(uint64_t hexInstruction);
    void decodeInstructionEXP(uint64_t hexInstruction);

private:
    GcnInst m_instruction;
};

} // namespace Shader::Gcn
