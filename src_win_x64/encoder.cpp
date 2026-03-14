#include "encoder.h"
#include "lexer.h"
#include <algorithm>

uint8_t Encoder::modRM(uint8_t mod, uint8_t reg, uint8_t rm) {
    return ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);
}

OpSize Encoder::operandSize(const Operand& op) {
    if (op.size_override != OpSize::UNKNOWN) return op.size_override;
    if (op.type == OperandType::REG) return op.is_reg8 ? OpSize::BYTE : OpSize::WORD;
    return OpSize::UNKNOWN;
}

std::vector<uint8_t> Encoder::encodeMemOperand(const MemOperand& mem, uint8_t reg_field) {
    std::vector<uint8_t> bytes;

    // Direct address [disp16]
    if (!mem.has_bx && !mem.has_bp && !mem.has_si && !mem.has_di) {
        bytes.push_back(modRM(0x00, reg_field, 0x06));
        bytes.push_back((uint8_t)(mem.disp & 0xFF));
        bytes.push_back((uint8_t)((mem.disp >> 8) & 0xFF));
        return bytes;
    }

    // Determine r/m field
    uint8_t rm = 0;
    if (mem.has_bx && mem.has_si) rm = 0;
    else if (mem.has_bx && mem.has_di) rm = 1;
    else if (mem.has_bp && mem.has_si) rm = 2;
    else if (mem.has_bp && mem.has_di) rm = 3;
    else if (mem.has_si) rm = 4;
    else if (mem.has_di) rm = 5;
    else if (mem.has_bp) rm = 6;
    else if (mem.has_bx) rm = 7;

    // Determine mod based on displacement
    // Special case: [BP] with no disp must be encoded as [BP+0] (mod=01, disp8=0)
    // because mod=00 rm=110 means [disp16] (direct address), not [BP].
    bool bp_only = (rm == 6) && !mem.has_disp;
    bool bp_zero = (rm == 6) && mem.has_disp && mem.disp == 0 && !mem.disp_unresolved;
    if (bp_only || bp_zero) {
        bytes.push_back(modRM(0x01, reg_field, rm));
        bytes.push_back(0x00);
    } else if (!mem.has_disp || (mem.disp == 0 && !mem.disp_unresolved)) {
        // No displacement (mod=00)
        bytes.push_back(modRM(0x00, reg_field, rm));
    } else if (mem.disp_unresolved || mem.disp < -128 || mem.disp > 127) {
        // 16-bit displacement (mod=10) - also used for unresolved
        bytes.push_back(modRM(0x02, reg_field, rm));
        bytes.push_back((uint8_t)(mem.disp & 0xFF));
        bytes.push_back((uint8_t)((mem.disp >> 8) & 0xFF));
    } else {
        // 8-bit displacement (mod=01)
        bytes.push_back(modRM(0x01, reg_field, rm));
        bytes.push_back((uint8_t)(mem.disp & 0xFF));
    }

    return bytes;
}

// ALU group: ADD=0, OR=1, ADC=2, SBB=3, AND=4, SUB=5, XOR=6, CMP=7
static int aluIndex(const std::string& mnem) {
    std::string m = Lexer::toUpper(mnem);
    if (m == "ADD") return 0;
    if (m == "OR")  return 1;
    if (m == "ADC") return 2;
    if (m == "SBB") return 3;
    if (m == "AND") return 4;
    if (m == "SUB") return 5;
    if (m == "XOR") return 6;
    if (m == "CMP") return 7;
    return -1;
}

std::vector<uint8_t> Encoder::encodeALU(const std::string& mnem, const Operand& dst, const Operand& src) {
    std::vector<uint8_t> bytes;
    int idx = aluIndex(mnem);
    if (idx < 0) return bytes;

    // REG, REG
    if (dst.type == OperandType::REG && src.type == OperandType::REG) {
        bool is8 = dst.is_reg8;
        uint8_t opcode = (uint8_t)((idx << 3) | (is8 ? 0x00 : 0x01));
        bytes.push_back(opcode);
        bytes.push_back(modRM(0x03, regNum(src.reg), regNum(dst.reg)));
        return bytes;
    }

    // REG, IMM
    if (dst.type == OperandType::REG && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        bool is8 = dst.is_reg8;
        int64_t imm = src.imm;

        // Short form for AL/AX: opcode + imm
        if (regNum(dst.reg) == 0) { // AL or AX
            uint8_t opcode = (uint8_t)((idx << 3) | 0x04 | (is8 ? 0 : 1));
            bytes.push_back(opcode);
            bytes.push_back((uint8_t)(imm & 0xFF));
            if (!is8) bytes.push_back((uint8_t)((imm >> 8) & 0xFF));
            return bytes;
        }

        // General form: 80/81/83 + ModR/M
        if (is8) {
            bytes.push_back(0x80);
            bytes.push_back(modRM(0x03, (uint8_t)idx, regNum(dst.reg)));
            bytes.push_back((uint8_t)(imm & 0xFF));
        } else {
            // Check if sign-extended byte works
            if (!src.imm_unresolved && imm >= -128 && imm <= 127) {
                bytes.push_back(0x83);
                bytes.push_back(modRM(0x03, (uint8_t)idx, regNum(dst.reg)));
                bytes.push_back((uint8_t)(imm & 0xFF));
            } else {
                bytes.push_back(0x81);
                bytes.push_back(modRM(0x03, (uint8_t)idx, regNum(dst.reg)));
                bytes.push_back((uint8_t)(imm & 0xFF));
                bytes.push_back((uint8_t)((imm >> 8) & 0xFF));
            }
        }
        return bytes;
    }

    // REG, [MEM]
    if (dst.type == OperandType::REG && src.type == OperandType::MEM) {
        bool is8 = dst.is_reg8;
        uint8_t opcode = (uint8_t)((idx << 3) | 0x02 | (is8 ? 0 : 1));
        bytes.push_back(opcode);
        auto mem_bytes = encodeMemOperand(src.mem, regNum(dst.reg));
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        return bytes;
    }

    // [MEM], REG
    if (dst.type == OperandType::MEM && src.type == OperandType::REG) {
        bool is8 = src.is_reg8;
        uint8_t opcode = (uint8_t)((idx << 3) | 0x00 | (is8 ? 0 : 1));
        bytes.push_back(opcode);
        auto mem_bytes = encodeMemOperand(dst.mem, regNum(src.reg));
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        return bytes;
    }

    // [MEM], IMM
    if (dst.type == OperandType::MEM && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        OpSize sz = dst.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD; // default
        bool is8 = (sz == OpSize::BYTE);
        int64_t imm = src.imm;

        if (is8) {
            bytes.push_back(0x80);
            auto mem_bytes = encodeMemOperand(dst.mem, (uint8_t)idx);
            bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
            bytes.push_back((uint8_t)(imm & 0xFF));
        } else {
            // For CMP with sign-extendable, use 83
            if (!src.imm_unresolved && imm >= -128 && imm <= 127) {
                bytes.push_back(0x83);
                auto mem_bytes = encodeMemOperand(dst.mem, (uint8_t)idx);
                bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
                bytes.push_back((uint8_t)(imm & 0xFF));
            } else {
                bytes.push_back(0x81);
                auto mem_bytes = encodeMemOperand(dst.mem, (uint8_t)idx);
                bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
                bytes.push_back((uint8_t)(imm & 0xFF));
                bytes.push_back((uint8_t)((imm >> 8) & 0xFF));
            }
        }
        return bytes;
    }

    return bytes;
}

std::vector<uint8_t> Encoder::encodeTEST(const Operand& dst, const Operand& src) {
    std::vector<uint8_t> bytes;

    // TEST reg, reg
    if (dst.type == OperandType::REG && src.type == OperandType::REG) {
        bool is8 = dst.is_reg8;
        bytes.push_back(is8 ? 0x84 : 0x85);
        bytes.push_back(modRM(0x03, regNum(src.reg), regNum(dst.reg)));
        return bytes;
    }

    // TEST reg, imm
    if (dst.type == OperandType::REG && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        bool is8 = dst.is_reg8;
        if (regNum(dst.reg) == 0) { // AL/AX short form
            bytes.push_back(is8 ? 0xA8 : 0xA9);
        } else {
            bytes.push_back(is8 ? 0xF6 : 0xF7);
            bytes.push_back(modRM(0x03, 0, regNum(dst.reg)));
        }
        bytes.push_back((uint8_t)(src.imm & 0xFF));
        if (!is8) bytes.push_back((uint8_t)((src.imm >> 8) & 0xFF));
        return bytes;
    }

    // TEST BYTE/WORD [mem], imm
    if (dst.type == OperandType::MEM && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        OpSize sz = dst.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::BYTE;
        bool is8 = (sz == OpSize::BYTE);
        bytes.push_back(is8 ? 0xF6 : 0xF7);
        auto mem_bytes = encodeMemOperand(dst.mem, 0);
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        bytes.push_back((uint8_t)(src.imm & 0xFF));
        if (!is8) bytes.push_back((uint8_t)((src.imm >> 8) & 0xFF));
        return bytes;
    }

    return bytes;
}

std::vector<uint8_t> Encoder::encodeMOV(const Operand& dst, const Operand& src) {
    std::vector<uint8_t> bytes;

    // MOV reg, imm
    if (dst.type == OperandType::REG && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        bool is8 = dst.is_reg8;
        uint8_t opcode = is8 ? (uint8_t)(0xB0 + regNum(dst.reg))
                             : (uint8_t)(0xB8 + regNum(dst.reg));
        bytes.push_back(opcode);
        bytes.push_back((uint8_t)(src.imm & 0xFF));
        if (!is8) bytes.push_back((uint8_t)((src.imm >> 8) & 0xFF));
        return bytes;
    }

    // MOV reg, reg
    if (dst.type == OperandType::REG && src.type == OperandType::REG) {
        bool is8 = dst.is_reg8;
        bytes.push_back(is8 ? 0x88 : 0x89);
        bytes.push_back(modRM(0x03, regNum(src.reg), regNum(dst.reg)));
        return bytes;
    }

    // MOV reg, [mem]
    if (dst.type == OperandType::REG && src.type == OperandType::MEM) {
        bool is8 = dst.is_reg8;
        bytes.push_back(is8 ? 0x8A : 0x8B);
        auto mem_bytes = encodeMemOperand(src.mem, regNum(dst.reg));
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        return bytes;
    }

    // MOV [mem], reg
    if (dst.type == OperandType::MEM && src.type == OperandType::REG) {
        bool is8 = src.is_reg8;
        bytes.push_back(is8 ? 0x88 : 0x89);
        auto mem_bytes = encodeMemOperand(dst.mem, regNum(src.reg));
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        return bytes;
    }

    // MOV [mem], imm
    if (dst.type == OperandType::MEM && (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM)) {
        OpSize sz = dst.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        bool is8 = (sz == OpSize::BYTE);
        bytes.push_back(is8 ? 0xC6 : 0xC7);
        auto mem_bytes = encodeMemOperand(dst.mem, 0);
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        bytes.push_back((uint8_t)(src.imm & 0xFF));
        if (!is8) bytes.push_back((uint8_t)((src.imm >> 8) & 0xFF));
        return bytes;
    }

    return bytes;
}

std::vector<uint8_t> Encoder::encodeShift(const std::string& mnem, const Operand& dst, const Operand& src) {
    std::vector<uint8_t> bytes;
    std::string m = Lexer::toUpper(mnem);

    // Extension field: ROL=0, ROR=1, RCL=2, RCR=3, SHL=4, SHR=5, SAR=7
    uint8_t ext = 0;
    if (m == "ROL") ext = 0;
    else if (m == "ROR") ext = 1;
    else if (m == "RCL") ext = 2;
    else if (m == "RCR") ext = 3;
    else if (m == "SHL") ext = 4;
    else if (m == "SHR") ext = 5;
    else if (m == "SAR") ext = 7;

    // Register operand
    if (dst.type == OperandType::REG) {
        bool is8 = dst.is_reg8;
        if (src.type == OperandType::IMM && src.imm == 1) {
            bytes.push_back(is8 ? 0xD0 : 0xD1);
            bytes.push_back(modRM(0x03, ext, regNum(dst.reg)));
        } else if (src.type == OperandType::REG && src.is_reg8 && regNum(src.reg) == 1) {
            bytes.push_back(is8 ? 0xD2 : 0xD3);
            bytes.push_back(modRM(0x03, ext, regNum(dst.reg)));
        }
        return bytes;
    }

    // Memory operand
    if (dst.type == OperandType::MEM) {
        OpSize sz = dst.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        bool is8 = (sz == OpSize::BYTE);
        if (src.type == OperandType::IMM && src.imm == 1) {
            bytes.push_back(is8 ? 0xD0 : 0xD1);
            auto mb = encodeMemOperand(dst.mem, ext);
            bytes.insert(bytes.end(), mb.begin(), mb.end());
        } else if (src.type == OperandType::REG && src.is_reg8 && regNum(src.reg) == 1) {
            bytes.push_back(is8 ? 0xD2 : 0xD3);
            auto mb = encodeMemOperand(dst.mem, ext);
            bytes.insert(bytes.end(), mb.begin(), mb.end());
        }
        return bytes;
    }

    return bytes;
}

std::vector<uint8_t> Encoder::encodeUnary(const std::string& mnem, const Operand& op) {
    std::vector<uint8_t> bytes;
    std::string m = Lexer::toUpper(mnem);

    // INC/DEC reg16 — short form
    if ((m == "INC" || m == "DEC") && op.type == OperandType::REG && !op.is_reg8) {
        bytes.push_back((uint8_t)((m == "INC" ? 0x40 : 0x48) + regNum(op.reg)));
        return bytes;
    }

    // INC/DEC WORD [mem] or BYTE [mem]
    if ((m == "INC" || m == "DEC") && op.type == OperandType::MEM) {
        OpSize sz = op.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        bool is8 = (sz == OpSize::BYTE);
        uint8_t ext = (m == "INC") ? 0 : 1;
        bytes.push_back(is8 ? 0xFE : 0xFF);
        auto mem_bytes = encodeMemOperand(op.mem, ext);
        bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
        return bytes;
    }

    // INC/DEC reg8
    if ((m == "INC" || m == "DEC") && op.type == OperandType::REG && op.is_reg8) {
        uint8_t ext = (m == "INC") ? 0 : 1;
        bytes.push_back(0xFE);
        bytes.push_back(modRM(0x03, ext, regNum(op.reg)));
        return bytes;
    }

    // NOT, NEG on register
    if ((m == "NOT" || m == "NEG") && op.type == OperandType::REG) {
        bool is8 = op.is_reg8;
        uint8_t ext = (m == "NOT") ? 2 : 3;
        bytes.push_back(is8 ? 0xF6 : 0xF7);
        bytes.push_back(modRM(0x03, ext, regNum(op.reg)));
        return bytes;
    }

    // NOT, NEG on memory
    if ((m == "NOT" || m == "NEG") && op.type == OperandType::MEM) {
        OpSize sz = op.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        bool is8 = (sz == OpSize::BYTE);
        uint8_t ext = (m == "NOT") ? 2 : 3;
        bytes.push_back(is8 ? 0xF6 : 0xF7);
        auto mb = encodeMemOperand(op.mem, ext);
        bytes.insert(bytes.end(), mb.begin(), mb.end());
        return bytes;
    }

    // MUL, IMUL, DIV, IDIV on register
    if ((m == "MUL" || m == "IMUL" || m == "DIV" || m == "IDIV") && op.type == OperandType::REG) {
        bool is8 = op.is_reg8;
        uint8_t ext;
        if (m == "MUL") ext = 4;
        else if (m == "IMUL") ext = 5;
        else if (m == "DIV") ext = 6;
        else ext = 7; // IDIV
        bytes.push_back(is8 ? 0xF6 : 0xF7);
        bytes.push_back(modRM(0x03, ext, regNum(op.reg)));
        return bytes;
    }

    // MUL, IMUL, DIV, IDIV on memory
    if ((m == "MUL" || m == "IMUL" || m == "DIV" || m == "IDIV") && op.type == OperandType::MEM) {
        OpSize sz = op.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        bool is8 = (sz == OpSize::BYTE);
        uint8_t ext;
        if (m == "MUL") ext = 4;
        else if (m == "IMUL") ext = 5;
        else if (m == "DIV") ext = 6;
        else ext = 7; // IDIV
        bytes.push_back(is8 ? 0xF6 : 0xF7);
        auto mb = encodeMemOperand(op.mem, ext);
        bytes.insert(bytes.end(), mb.begin(), mb.end());
        return bytes;
    }

    return bytes;
}

std::vector<uint8_t> Encoder::encodeJcc(const std::string& mnem, int64_t target, int64_t current_addr) {
    std::vector<uint8_t> bytes;
    std::string m = Lexer::toUpper(mnem);

    // JMP — try SHORT (EB rel8, 2 bytes), fall back to NEAR (E9 rel16, 3 bytes)
    if (m == "JMP") {
        int64_t rel_short = target - (current_addr + 2);
        if (rel_short >= -128 && rel_short <= 127) {
            bytes.push_back(0xEB);
            bytes.push_back((uint8_t)(rel_short & 0xFF));
        } else {
            int64_t rel = target - (current_addr + 3);
            bytes.push_back(0xE9);
            bytes.push_back((uint8_t)(rel & 0xFF));
            bytes.push_back((uint8_t)((rel >> 8) & 0xFF));
        }
        return bytes;
    }

    // CALL near (always 3 bytes)
    if (m == "CALL") {
        int64_t rel = target - (current_addr + 3);
        bytes.push_back(0xE8);
        bytes.push_back((uint8_t)(rel & 0xFF));
        bytes.push_back((uint8_t)((rel >> 8) & 0xFF));
        return bytes;
    }

    // LOOP / LOOPE / LOOPNE — short only (rel8, 2 bytes)
    if (m == "LOOP" || m == "LOOPE" || m == "LOOPZ" ||
        m == "LOOPNE" || m == "LOOPNZ") {
        uint8_t opcode;
        if (m == "LOOP") opcode = 0xE2;
        else if (m == "LOOPE" || m == "LOOPZ") opcode = 0xE1;
        else opcode = 0xE0; // LOOPNE/LOOPNZ
        int64_t rel = target - (current_addr + 2);
        bytes.push_back(opcode);
        bytes.push_back((uint8_t)(rel & 0xFF));
        return bytes;
    }

    // JCXZ — short only (rel8, 2 bytes)
    if (m == "JCXZ") {
        int64_t rel = target - (current_addr + 2);
        bytes.push_back(0xE3);
        bytes.push_back((uint8_t)(rel & 0xFF));
        return bytes;
    }

    // Conditional jumps: try short (rel8) first, else emit reverse+JMP near
    uint8_t short_opcode = 0;
    if (m == "JO")                          short_opcode = 0x70;
    else if (m == "JNO")                    short_opcode = 0x71;
    else if (m == "JB" || m == "JC" || m == "JNAE")  short_opcode = 0x72;
    else if (m == "JAE" || m == "JNC" || m == "JNB")  short_opcode = 0x73;
    else if (m == "JZ" || m == "JE")        short_opcode = 0x74;
    else if (m == "JNZ" || m == "JNE")      short_opcode = 0x75;
    else if (m == "JBE" || m == "JNA")      short_opcode = 0x76;
    else if (m == "JA" || m == "JNBE")      short_opcode = 0x77;
    else if (m == "JS")                     short_opcode = 0x78;
    else if (m == "JNS")                    short_opcode = 0x79;
    else if (m == "JP" || m == "JPE")       short_opcode = 0x7A;
    else if (m == "JNP" || m == "JPO")      short_opcode = 0x7B;
    else if (m == "JL" || m == "JNGE")      short_opcode = 0x7C;
    else if (m == "JGE" || m == "JNL")      short_opcode = 0x7D;
    else if (m == "JLE" || m == "JNG")      short_opcode = 0x7E;
    else if (m == "JG" || m == "JNLE")      short_opcode = 0x7F;
    else return bytes;

    int64_t rel = target - (current_addr + 2);
    if (rel >= -128 && rel <= 127) {
        bytes.push_back(short_opcode);
        bytes.push_back((uint8_t)(rel & 0xFF));
    } else {
        // Reverse condition + JMP near (5 bytes total)
        // XOR with 0x01 flips the condition bit for all 70-7F opcodes
        bytes.push_back(short_opcode ^ 0x01); // inverted condition
        bytes.push_back(0x03);                  // skip over the JMP near (3 bytes)
        int64_t rel16 = target - (current_addr + 5);
        bytes.push_back(0xE9);
        bytes.push_back((uint8_t)(rel16 & 0xFF));
        bytes.push_back((uint8_t)((rel16 >> 8) & 0xFF));
    }
    return bytes;
}

std::vector<uint8_t> Encoder::encodePushPop(const std::string& mnem, const Operand& op) {
    std::vector<uint8_t> bytes;
    std::string m = Lexer::toUpper(mnem);
    if (op.type == OperandType::REG && !op.is_reg8) {
        bytes.push_back((uint8_t)((m == "PUSH" ? 0x50 : 0x58) + regNum(op.reg)));
    }
    return bytes;
}

std::vector<uint8_t> Encoder::encodeLEA(const Operand& dst, const Operand& src) {
    std::vector<uint8_t> bytes;
    if (dst.type != OperandType::REG || src.type != OperandType::MEM) return bytes;
    bytes.push_back(0x8D);
    auto mem_bytes = encodeMemOperand(src.mem, regNum(dst.reg));
    bytes.insert(bytes.end(), mem_bytes.begin(), mem_bytes.end());
    return bytes;
}

std::vector<uint8_t> Encoder::encode(const ParsedLine& line, bool pass1) {
    std::vector<uint8_t> bytes;
    std::string m = Lexer::toUpper(line.mnemonic);

    // Handle prefix
    if (!line.prefix.empty()) {
        std::string pfx = Lexer::toUpper(line.prefix);
        if (pfx == "REP" || pfx == "REPE" || pfx == "REPZ") bytes.push_back(0xF3);
        else if (pfx == "REPNE" || pfx == "REPNZ") bytes.push_back(0xF2);
    }

    // Segment override prefix (ES=0x26, CS=0x2E, SS=0x36, DS=0x3E)
    uint8_t seg_pfx_byte = 0;
    for (auto& op : line.operands) {
        if (op.seg_prefix != 0xFF) {
            static const uint8_t seg_pfx_tbl[] = { 0x26, 0x2E, 0x36, 0x3E };
            seg_pfx_byte = seg_pfx_tbl[op.seg_prefix & 3];
            bytes.push_back(seg_pfx_byte);
            break; // only one segment override per instruction
        }
    }
    // Helper: prepend segment prefix to sub-encoder results
    auto withSegPrefix = [seg_pfx_byte](std::vector<uint8_t> v) {
        if (seg_pfx_byte) v.insert(v.begin(), seg_pfx_byte);
        return v;
    };

    // RET/RETF with operand (must check before zero-operand RET)
    if (m == "RET" && line.operands.size() >= 1) {
        bytes.push_back(0xC2);
        int64_t imm = line.operands[0].imm;
        bytes.push_back((uint8_t)(imm & 0xFF));
        bytes.push_back((uint8_t)((imm >> 8) & 0xFF));
        return bytes;
    }
    if (m == "RETF" && line.operands.size() >= 1) {
        bytes.push_back(0xCA);
        int64_t imm = line.operands[0].imm;
        bytes.push_back((uint8_t)(imm & 0xFF));
        bytes.push_back((uint8_t)((imm >> 8) & 0xFF));
        return bytes;
    }

    // Zero-operand instructions
    if (m == "RET") { bytes.push_back(0xC3); return bytes; }
    if (m == "RETF") { bytes.push_back(0xCB); return bytes; }
    if (m == "IRET") { bytes.push_back(0xCF); return bytes; }
    if (m == "CLD") { bytes.push_back(0xFC); return bytes; }
    if (m == "CLC") { bytes.push_back(0xF8); return bytes; }
    if (m == "STC") { bytes.push_back(0xF9); return bytes; }
    if (m == "STD") { bytes.push_back(0xFD); return bytes; }
    if (m == "CLI") { bytes.push_back(0xFA); return bytes; }
    if (m == "STI") { bytes.push_back(0xFB); return bytes; }
    if (m == "CMC") { bytes.push_back(0xF5); return bytes; }
    if (m == "NOP") { bytes.push_back(0x90); return bytes; }
    if (m == "HLT") { bytes.push_back(0xF4); return bytes; }
    if (m == "WAIT") { bytes.push_back(0x9B); return bytes; }
    if (m == "PUSHA") { bytes.push_back(0x60); return bytes; }
    if (m == "POPA") { bytes.push_back(0x61); return bytes; }
    if (m == "PUSHF") { bytes.push_back(0x9C); return bytes; }
    if (m == "POPF") { bytes.push_back(0x9D); return bytes; }
    if (m == "LAHF") { bytes.push_back(0x9F); return bytes; }
    if (m == "SAHF") { bytes.push_back(0x9E); return bytes; }
    if (m == "CBW") { bytes.push_back(0x98); return bytes; }
    if (m == "CWD") { bytes.push_back(0x99); return bytes; }
    if (m == "INTO") { bytes.push_back(0xCE); return bytes; }
    if (m == "XLAT" || m == "XLATB") { bytes.push_back(0xD7); return bytes; }
    if (m == "DAA") { bytes.push_back(0x27); return bytes; }
    if (m == "DAS") { bytes.push_back(0x2F); return bytes; }
    if (m == "AAA") { bytes.push_back(0x37); return bytes; }
    if (m == "AAS") { bytes.push_back(0x3F); return bytes; }
    if (m == "LOCK") { bytes.push_back(0xF0); return bytes; }

    // String operations
    if (m == "MOVSB") { bytes.push_back(0xA4); return bytes; }
    if (m == "MOVSW") { bytes.push_back(0xA5); return bytes; }
    if (m == "STOSB") { bytes.push_back(0xAA); return bytes; }
    if (m == "STOSW") { bytes.push_back(0xAB); return bytes; }
    if (m == "LODSB") { bytes.push_back(0xAC); return bytes; }
    if (m == "LODSW") { bytes.push_back(0xAD); return bytes; }
    if (m == "CMPSB") { bytes.push_back(0xA6); return bytes; }
    if (m == "CMPSW") { bytes.push_back(0xA7); return bytes; }
    if (m == "SCASB") { bytes.push_back(0xAE); return bytes; }
    if (m == "SCASW") { bytes.push_back(0xAF); return bytes; }

    // AAM/AAD — two-byte with fixed second byte 0Ah
    if (m == "AAM") { bytes.push_back(0xD4); bytes.push_back(0x0A); return bytes; }
    if (m == "AAD") { bytes.push_back(0xD5); bytes.push_back(0x0A); return bytes; }

    // INT imm8 (special case: INT 3 = CC)
    if (m == "INT" && line.operands.size() >= 1) {
        if (line.operands[0].imm == 3) {
            bytes.push_back(0xCC);
        } else {
            bytes.push_back(0xCD);
            bytes.push_back((uint8_t)(line.operands[0].imm & 0xFF));
        }
        return bytes;
    }

    // MOV — including segment register forms
    if (m == "MOV" && line.operands.size() >= 2) {
        const Operand& dst = line.operands[0];
        const Operand& src = line.operands[1];

        // MOV Sreg, r/m16 (8E /r)
        if (dst.type == OperandType::SREG && src.type == OperandType::REG) {
            bytes.push_back(0x8E);
            bytes.push_back(modRM(0x03, (uint8_t)dst.sreg, regNum(src.reg)));
            return bytes;
        }
        if (dst.type == OperandType::SREG && src.type == OperandType::MEM) {
            bytes.push_back(0x8E);
            auto mb = encodeMemOperand(src.mem, (uint8_t)dst.sreg);
            bytes.insert(bytes.end(), mb.begin(), mb.end());
            return bytes;
        }
        // MOV r/m16, Sreg (8C /r)
        if (src.type == OperandType::SREG && dst.type == OperandType::REG) {
            bytes.push_back(0x8C);
            bytes.push_back(modRM(0x03, (uint8_t)src.sreg, regNum(dst.reg)));
            return bytes;
        }
        if (src.type == OperandType::SREG && dst.type == OperandType::MEM) {
            bytes.push_back(0x8C);
            auto mb = encodeMemOperand(dst.mem, (uint8_t)src.sreg);
            bytes.insert(bytes.end(), mb.begin(), mb.end());
            return bytes;
        }

        return withSegPrefix(encodeMOV(dst, src));
    }

    // XCHG
    if (m == "XCHG" && line.operands.size() >= 2) {
        const Operand& op1 = line.operands[0];
        const Operand& op2 = line.operands[1];
        // XCHG AX, r16 or XCHG r16, AX — short form 90+r
        if (op1.type == OperandType::REG && op2.type == OperandType::REG &&
            !op1.is_reg8 && !op2.is_reg8) {
            if (regNum(op1.reg) == 0) {
                bytes.push_back((uint8_t)(0x90 + regNum(op2.reg)));
                return bytes;
            }
            if (regNum(op2.reg) == 0) {
                bytes.push_back((uint8_t)(0x90 + regNum(op1.reg)));
                return bytes;
            }
            // General reg,reg
            bytes.push_back(0x87);
            bytes.push_back(modRM(0x03, regNum(op1.reg), regNum(op2.reg)));
            return bytes;
        }
        // XCHG r8, r8
        if (op1.type == OperandType::REG && op2.type == OperandType::REG &&
            op1.is_reg8 && op2.is_reg8) {
            bytes.push_back(0x86);
            bytes.push_back(modRM(0x03, regNum(op1.reg), regNum(op2.reg)));
            return bytes;
        }
        // XCHG reg, [mem]
        if (op1.type == OperandType::REG && op2.type == OperandType::MEM) {
            bytes.push_back(op1.is_reg8 ? 0x86 : 0x87);
            auto mb = encodeMemOperand(op2.mem, regNum(op1.reg));
            bytes.insert(bytes.end(), mb.begin(), mb.end());
            return bytes;
        }
        // XCHG [mem], reg
        if (op1.type == OperandType::MEM && op2.type == OperandType::REG) {
            bytes.push_back(op2.is_reg8 ? 0x86 : 0x87);
            auto mb = encodeMemOperand(op1.mem, regNum(op2.reg));
            bytes.insert(bytes.end(), mb.begin(), mb.end());
            return bytes;
        }
        return bytes;
    }

    // IN
    if (m == "IN" && line.operands.size() >= 2) {
        const Operand& dst = line.operands[0];
        const Operand& src = line.operands[1];
        bool is8 = dst.is_reg8; // AL vs AX
        if (src.type == OperandType::IMM || src.type == OperandType::LABEL_IMM) {
            bytes.push_back(is8 ? 0xE4 : 0xE5);
            bytes.push_back((uint8_t)(src.imm & 0xFF));
        } else if (src.type == OperandType::REG) { // DX
            bytes.push_back(is8 ? 0xEC : 0xED);
        }
        return bytes;
    }

    // OUT
    if (m == "OUT" && line.operands.size() >= 2) {
        const Operand& dst = line.operands[0];
        const Operand& src = line.operands[1];
        bool is8 = src.is_reg8; // AL vs AX
        if (dst.type == OperandType::IMM || dst.type == OperandType::LABEL_IMM) {
            bytes.push_back(is8 ? 0xE6 : 0xE7);
            bytes.push_back((uint8_t)(dst.imm & 0xFF));
        } else if (dst.type == OperandType::REG) { // DX
            bytes.push_back(is8 ? 0xEE : 0xEF);
        }
        return bytes;
    }

    // LDS/LES
    if ((m == "LDS" || m == "LES") && line.operands.size() >= 2) {
        const Operand& dst = line.operands[0];
        const Operand& src = line.operands[1];
        if (dst.type == OperandType::REG && src.type == OperandType::MEM) {
            bytes.push_back(m == "LES" ? 0xC4 : 0xC5);
            auto mb = encodeMemOperand(src.mem, regNum(dst.reg));
            bytes.insert(bytes.end(), mb.begin(), mb.end());
        }
        return bytes;
    }

    // ALU: ADD, ADC, SUB, SBB, AND, OR, XOR, CMP
    if (aluIndex(m) >= 0 && line.operands.size() >= 2) {
        return withSegPrefix(encodeALU(m, line.operands[0], line.operands[1]));
    }

    // TEST
    if (m == "TEST" && line.operands.size() >= 2) {
        return withSegPrefix(encodeTEST(line.operands[0], line.operands[1]));
    }

    // Shift/Rotate (all 7 variants)
    if ((m == "SHL" || m == "SHR" || m == "SAR" ||
         m == "ROL" || m == "ROR" || m == "RCL" || m == "RCR") && line.operands.size() >= 2) {
        return withSegPrefix(encodeShift(m, line.operands[0], line.operands[1]));
    }

    // Unary: NOT, NEG, INC, DEC, MUL, IMUL, DIV, IDIV
    if ((m == "NOT" || m == "NEG" || m == "INC" || m == "DEC" ||
         m == "MUL" || m == "IMUL" || m == "DIV" || m == "IDIV")
        && line.operands.size() >= 1) {
        return withSegPrefix(encodeUnary(m, line.operands[0]));
    }

    // PUSH/POP — reg16, segment reg, memory
    if ((m == "PUSH" || m == "POP") && line.operands.size() >= 1) {
        const Operand& op = line.operands[0];
        // Segment register
        if (op.type == OperandType::SREG) {
            uint8_t base = (m == "PUSH") ? 0x06 : 0x07;
            bytes.push_back((uint8_t)(((uint8_t)op.sreg << 3) | base));
            return bytes;
        }
        // Memory operand: PUSH [mem] = FF /6, POP [mem] = 8F /0
        if (op.type == OperandType::MEM) {
            if (m == "PUSH") {
                bytes.push_back(0xFF);
                auto mb = encodeMemOperand(op.mem, 6);
                bytes.insert(bytes.end(), mb.begin(), mb.end());
            } else {
                bytes.push_back(0x8F);
                auto mb = encodeMemOperand(op.mem, 0);
                bytes.insert(bytes.end(), mb.begin(), mb.end());
            }
            return bytes;
        }
        return withSegPrefix(encodePushPop(m, op));
    }

    // LEA
    if (m == "LEA" && line.operands.size() >= 2) {
        return withSegPrefix(encodeLEA(line.operands[0], line.operands[1]));
    }

    // Indirect JMP/CALL — JMP reg, JMP [mem], CALL reg, CALL [mem]
    if ((m == "JMP" || m == "CALL") && line.operands.size() >= 1) {
        const Operand& op = line.operands[0];
        uint8_t ext = (m == "JMP") ? 4 : 2; // FF /4 = JMP near indirect, FF /2 = CALL near indirect
        if (op.type == OperandType::REG) {
            bytes.push_back(0xFF);
            bytes.push_back(modRM(0x03, ext, regNum(op.reg)));
            return bytes;
        }
        if (op.type == OperandType::MEM) {
            bytes.push_back(0xFF);
            auto mb = encodeMemOperand(op.mem, ext);
            bytes.insert(bytes.end(), mb.begin(), mb.end());
            return bytes;
        }
        // Relative jumps (IMM/LABEL_IMM) handled via encodeJcc
        int64_t target = op.imm;
        return encodeJcc(m, target, 0);
    }

    // Relative control flow: Jcc, LOOP variants, JCXZ
    if ((m == "LOOP" || m == "LOOPE" || m == "LOOPZ" ||
         m == "LOOPNE" || m == "LOOPNZ" || m == "JCXZ" ||
         m == "JO" || m == "JNO" ||
         m == "JZ" || m == "JNZ" || m == "JE" || m == "JNE" ||
         m == "JB" || m == "JAE" || m == "JNC" || m == "JC" ||
         m == "JBE" || m == "JA" || m == "JL" || m == "JGE" ||
         m == "JLE" || m == "JG" || m == "JNS" || m == "JS" ||
         m == "JP" || m == "JPE" || m == "JNP" || m == "JPO" ||
         m == "JNAE" || m == "JNB" || m == "JNBE" || m == "JNGE" ||
         m == "JNL" || m == "JNG" || m == "JNLE" || m == "JNA")
        && line.operands.size() >= 1) {
        int64_t target = line.operands[0].imm;
        return encodeJcc(m, target, 0);
    }

    return bytes;
}

// Helper: compute the modrm+displacement byte count for a memory operand.
// Returns the number of bytes for the modrm + displacement portion (no opcode).
static int estimateMemBytes(const MemOperand& mem) {
    // Direct address [disp16]: modrm + disp16 = 3 bytes
    if (!mem.has_bx && !mem.has_bp && !mem.has_si && !mem.has_di) return 3;
    // Base register(s) with displacement
    if (mem.has_disp || mem.disp_unresolved) {
        if (!mem.disp_unresolved && mem.disp >= -128 && mem.disp <= 127)
            return 2; // modrm + disp8
        return 3; // modrm + disp16
    }
    // [BP] with no disp: encoded as [BP+0] (modrm + disp8=0)
    bool bp_only = mem.has_bp && !mem.has_bx && !mem.has_si && !mem.has_di;
    if (bp_only) return 2;
    // Base register(s) only, no displacement: modrm only
    return 1;
}

static int segPrefixSize(const ParsedLine& line) {
    for (auto& op : line.operands)
        if (op.seg_prefix != 0xFF) return 1;
    return 0;
}

int Encoder::estimateSize(const ParsedLine& line) {
    int base = estimateSizeBase(line);
    return base + segPrefixSize(line);
}

int Encoder::estimateSizeBase(const ParsedLine& line) {
    std::string m = Lexer::toUpper(line.mnemonic);

    // RET/RETF with imm16 operand = 3 bytes
    if ((m == "RET" || m == "RETF") && !line.operands.empty()) return 3;

    // 1-byte instructions (with optional prefix)
    if (m == "RET" || m == "RETF" || m == "IRET" ||
        m == "CLD" || m == "CLC" || m == "STC" || m == "STD" ||
        m == "CLI" || m == "STI" || m == "CMC" ||
        m == "NOP" || m == "HLT" || m == "WAIT" || m == "LOCK" ||
        m == "PUSHA" || m == "POPA" ||
        m == "PUSHF" || m == "POPF" || m == "LAHF" || m == "SAHF" ||
        m == "CBW" || m == "CWD" || m == "INTO" ||
        m == "XLAT" || m == "XLATB" ||
        m == "DAA" || m == "DAS" || m == "AAA" || m == "AAS" ||
        m == "MOVSB" || m == "MOVSW" || m == "STOSB" || m == "STOSW" ||
        m == "LODSB" || m == "LODSW" || m == "CMPSB" || m == "CMPSW" ||
        m == "SCASB" || m == "SCASW") {
        int sz = 1;
        if (!line.prefix.empty()) sz++;
        return sz;
    }
    // 2-byte fixed instructions
    if (m == "AAM" || m == "AAD") return 2;

    if (m == "INT") return 2;

    // JMP/CALL — check for indirect (reg/mem) vs direct (imm)
    if (m == "JMP" || m == "CALL") {
        if (!line.operands.empty()) {
            const Operand& op = line.operands[0];
            if (op.type == OperandType::REG) return 2;   // FF /r modrm
            if (op.type == OperandType::MEM) {
                return 1 + estimateMemBytes(op.mem); // opcode + modrm+disp
            }
        }
        return 3; // direct: pessimistic NEAR
    }
    if (m == "LOOP" || m == "LOOPE" || m == "LOOPZ" ||
        m == "LOOPNE" || m == "LOOPNZ" || m == "JCXZ") return 2;

    // Conditional jumps: pessimistic worst case = 5 bytes (inverted Jcc + JMP NEAR).
    // Pass 2 may emit only 2 bytes if short range suffices — the padding bytes
    // are filled with NOPs to keep addresses stable.
    if (m == "JO" || m == "JNO" ||
        m == "JB" || m == "JAE" || m == "JNC" || m == "JC" || m == "JNAE" || m == "JNB" ||
        m == "JZ" || m == "JNZ" || m == "JE" || m == "JNE" ||
        m == "JBE" || m == "JA" || m == "JNBE" || m == "JNA" ||
        m == "JS" || m == "JNS" ||
        m == "JP" || m == "JPE" || m == "JNP" || m == "JPO" ||
        m == "JL" || m == "JGE" || m == "JNGE" || m == "JNL" ||
        m == "JLE" || m == "JG" || m == "JNG" || m == "JNLE") {
        return 5; // pessimistic: inverted Jcc SHORT + JMP NEAR
    }

    if (m == "PUSH" || m == "POP") {
        if (!line.operands.empty()) {
            const Operand& op = line.operands[0];
            if (op.type == OperandType::SREG) return 1;
            if (op.type == OperandType::REG) return 1;
            if (op.type == OperandType::MEM) {
                return 1 + estimateMemBytes(op.mem); // opcode + modrm+disp
            }
        }
        return 1;
    }

    // For instructions with operands, try encoding with dummy values to get size
    // Use a simpler heuristic
    if (line.operands.size() == 0) return 0;

    const Operand& op1 = line.operands[0];
    const Operand& op2 = line.operands.size() > 1 ? line.operands[1] : line.operands[0];

    // INC/DEC reg16 = 1 byte
    if ((m == "INC" || m == "DEC") && op1.type == OperandType::REG && !op1.is_reg8) return 1;
    // INC/DEC reg8 = 2 bytes
    if ((m == "INC" || m == "DEC") && op1.type == OperandType::REG && op1.is_reg8) return 2;
    // INC/DEC [mem] = opcode + modrm + disp
    if ((m == "INC" || m == "DEC") && op1.type == OperandType::MEM) {
        return 1 + estimateMemBytes(op1.mem);
    }

    // NOT/NEG/MUL/IMUL/DIV/IDIV reg = 2 bytes
    if ((m == "NOT" || m == "NEG" || m == "MUL" || m == "IMUL" || m == "DIV" || m == "IDIV")
        && op1.type == OperandType::REG) return 2;
    // NOT/NEG/MUL/IMUL/DIV/IDIV [mem]
    if ((m == "NOT" || m == "NEG" || m == "MUL" || m == "IMUL" || m == "DIV" || m == "IDIV")
        && op1.type == OperandType::MEM) {
        return 1 + estimateMemBytes(op1.mem);
    }

    // MOV with segment registers: 2 bytes (reg) or opcode + modrm+disp (mem)
    if (m == "MOV" && (op1.type == OperandType::SREG || op2.type == OperandType::SREG)) {
        const Operand& other = (op1.type == OperandType::SREG) ? op2 : op1;
        if (other.type == OperandType::REG) return 2;
        if (other.type == OperandType::MEM) {
            return 1 + estimateMemBytes(other.mem);
        }
        return 2;
    }

    // RET/RETF with imm16 = 3 bytes
    if ((m == "RET" || m == "RETF") && op1.type == OperandType::IMM) return 3;

    // XCHG
    if (m == "XCHG") {
        if (op1.type == OperandType::REG && op2.type == OperandType::REG) {
            if (!op1.is_reg8 && (regNum(op1.reg) == 0 || regNum(op2.reg) == 0))
                return 1; // XCHG AX, r16
            return 2;
        }
        if (op1.type == OperandType::MEM || op2.type == OperandType::MEM) {
            const MemOperand& mem = (op1.type == OperandType::MEM) ? op1.mem : op2.mem;
            return 1 + estimateMemBytes(mem);
        }
        return 2;
    }

    // IN/OUT: 2 bytes (imm port) or 1 byte (DX port)
    if (m == "IN" || m == "OUT") {
        bool has_imm = false;
        for (auto& op : line.operands)
            if (op.type == OperandType::IMM || op.type == OperandType::LABEL_IMM) has_imm = true;
        return has_imm ? 2 : 1;
    }

    // LDS/LES: opcode + modrm + displacement
    if (m == "LDS" || m == "LES") {
        if (op2.type == OperandType::MEM) {
            return 1 + estimateMemBytes(op2.mem);
        }
        return 4;
    }

    // MOV reg, imm: 2 bytes (8-bit) or 3 bytes (16-bit)
    if (m == "MOV" && op1.type == OperandType::REG && (op2.type == OperandType::IMM || op2.type == OperandType::LABEL_IMM)) {
        return op1.is_reg8 ? 2 : 3;
    }

    // MOV reg, reg: 2 bytes
    if (m == "MOV" && op1.type == OperandType::REG && op2.type == OperandType::REG) return 2;

    // MOV reg, [mem] or MOV [mem], reg
    if (m == "MOV" && ((op1.type == OperandType::REG && op2.type == OperandType::MEM) ||
                       (op1.type == OperandType::MEM && op2.type == OperandType::REG))) {
        const MemOperand& mem = (op1.type == OperandType::MEM) ? op1.mem : op2.mem;
        return 1 + estimateMemBytes(mem);
    }

    // MOV [mem], imm
    if (m == "MOV" && op1.type == OperandType::MEM && (op2.type == OperandType::IMM || op2.type == OperandType::LABEL_IMM)) {
        OpSize sz = op1.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        int imm_size = (sz == OpSize::BYTE) ? 1 : 2;
        return 1 + estimateMemBytes(op1.mem) + imm_size; // opcode + modrm+disp + imm
    }

    // LEA reg, [mem]
    if (m == "LEA") {
        return 1 + estimateMemBytes(op2.mem);
    }

    // TEST [mem], imm
    if (m == "TEST" && op1.type == OperandType::MEM) {
        OpSize sz = op1.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::BYTE;
        int imm_size = (sz == OpSize::BYTE) ? 1 : 2;
        return 1 + estimateMemBytes(op1.mem) + imm_size;
    }

    // TEST reg, reg = 2 bytes
    if (m == "TEST" && op1.type == OperandType::REG && op2.type == OperandType::REG) return 2;

    // Shift/rotate reg, 1 or CL = 2 bytes; mem = opcode + modrm+disp
    if (m == "SHL" || m == "SHR" || m == "SAR" ||
        m == "ROL" || m == "ROR" || m == "RCL" || m == "RCR") {
        if (op1.type == OperandType::REG) return 2;
        if (op1.type == OperandType::MEM) {
            return 1 + estimateMemBytes(op1.mem);
        }
        return 2;
    }

    // ALU reg, reg = 2 bytes
    if (op1.type == OperandType::REG && op2.type == OperandType::REG) return 2;

    // ALU reg, imm
    if (op1.type == OperandType::REG && (op2.type == OperandType::IMM || op2.type == OperandType::LABEL_IMM)) {
        if (op1.is_reg8) {
            return (regNum(op1.reg) == 0) ? 2 : 3; // AL short vs general
        }
        // 16-bit: check for sign-ext byte optimization
        if (!op2.imm_unresolved && op2.imm >= -128 && op2.imm <= 127 && regNum(op1.reg) != 0) return 3;
        if (regNum(op1.reg) == 0) return 3; // AX short form always 3
        return 4;
    }

    // ALU reg, [mem]
    if (op1.type == OperandType::REG && op2.type == OperandType::MEM) {
        return 1 + estimateMemBytes(op2.mem);
    }

    // ALU [mem], reg
    if (op1.type == OperandType::MEM && op2.type == OperandType::REG) {
        return 1 + estimateMemBytes(op1.mem);
    }

    // ALU [mem], imm
    if (op1.type == OperandType::MEM && (op2.type == OperandType::IMM || op2.type == OperandType::LABEL_IMM)) {
        OpSize sz = op1.size_override;
        if (sz == OpSize::UNKNOWN) sz = OpSize::WORD;
        int imm_size;
        if (sz == OpSize::BYTE) {
            imm_size = 1;
        } else {
            if (!op2.imm_unresolved && op2.imm >= -128 && op2.imm <= 127)
                imm_size = 1; // sign-extended byte
            else
                imm_size = 2;
        }
        return 1 + estimateMemBytes(op1.mem) + imm_size;
    }

    return 0;
}
