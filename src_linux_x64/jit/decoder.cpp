#include "decoder.h"
#include <cstring>
#include <utility>

const char* opTypeName(OpType op) {
    switch (op) {
        case OpType::INVALID: return "???";
        case OpType::MOV: return "MOV"; case OpType::XCHG: return "XCHG";
        case OpType::LEA: return "LEA"; case OpType::LDS: return "LDS";
        case OpType::LES: return "LES"; case OpType::PUSH: return "PUSH";
        case OpType::POP: return "POP"; case OpType::PUSHA: return "PUSHA";
        case OpType::POPA: return "POPA"; case OpType::PUSHF: return "PUSHF";
        case OpType::POPF: return "POPF";
        case OpType::ADD: return "ADD"; case OpType::ADC: return "ADC";
        case OpType::SUB: return "SUB"; case OpType::SBB: return "SBB";
        case OpType::AND: return "AND"; case OpType::OR: return "OR";
        case OpType::XOR: return "XOR"; case OpType::CMP: return "CMP";
        case OpType::TEST: return "TEST"; case OpType::NEG: return "NEG";
        case OpType::NOT: return "NOT"; case OpType::INC: return "INC";
        case OpType::DEC: return "DEC";
        case OpType::MUL: return "MUL"; case OpType::IMUL: return "IMUL";
        case OpType::DIV: return "DIV"; case OpType::IDIV: return "IDIV";
        case OpType::SHL: return "SHL"; case OpType::SHR: return "SHR";
        case OpType::SAR: return "SAR"; case OpType::ROL: return "ROL";
        case OpType::ROR: return "ROR"; case OpType::RCL: return "RCL";
        case OpType::RCR: return "RCR";
        case OpType::JMP: return "JMP"; case OpType::CALL: return "CALL";
        case OpType::RET: return "RET"; case OpType::RETF: return "RETF";
        case OpType::IRET: return "IRET"; case OpType::INT: return "INT";
        case OpType::INTO: return "INTO"; case OpType::HLT: return "HLT";
        case OpType::JO: return "JO"; case OpType::JNO: return "JNO";
        case OpType::JB: return "JB"; case OpType::JNB: return "JNB";
        case OpType::JZ: return "JZ"; case OpType::JNZ: return "JNZ";
        case OpType::JBE: return "JBE"; case OpType::JNBE: return "JNBE";
        case OpType::JSS: return "JS"; case OpType::JNS: return "JNS";
        case OpType::JP: return "JP"; case OpType::JNP: return "JNP";
        case OpType::JL: return "JL"; case OpType::JNL: return "JNL";
        case OpType::JLE: return "JLE"; case OpType::JNLE: return "JNLE";
        case OpType::JCXZ: return "JCXZ"; case OpType::LOOP: return "LOOP";
        case OpType::LOOPE: return "LOOPE"; case OpType::LOOPNE: return "LOOPNE";
        case OpType::MOVSB: return "MOVSB"; case OpType::MOVSW: return "MOVSW";
        case OpType::STOSB: return "STOSB"; case OpType::STOSW: return "STOSW";
        case OpType::LODSB: return "LODSB"; case OpType::LODSW: return "LODSW";
        case OpType::CMPSB: return "CMPSB"; case OpType::CMPSW: return "CMPSW";
        case OpType::SCASB: return "SCASB"; case OpType::SCASW: return "SCASW";
        case OpType::CLC: return "CLC"; case OpType::STC: return "STC";
        case OpType::CLI: return "CLI"; case OpType::STI: return "STI";
        case OpType::CLD: return "CLD"; case OpType::STD: return "STD";
        case OpType::CMC: return "CMC"; case OpType::LAHF: return "LAHF";
        case OpType::SAHF: return "SAHF";
        case OpType::DAA: return "DAA"; case OpType::DAS: return "DAS";
        case OpType::AAA: return "AAA"; case OpType::AAS: return "AAS";
        case OpType::AAM: return "AAM"; case OpType::AAD: return "AAD";
        case OpType::CBW: return "CBW"; case OpType::CWD: return "CWD";
        case OpType::XLAT: return "XLAT"; case OpType::NOP: return "NOP";
        case OpType::WAIT: return "WAIT"; case OpType::LOCK: return "LOCK";
        case OpType::IN: return "IN"; case OpType::OUT: return "OUT";
        case OpType::REP: return "REP"; case OpType::REPE: return "REPE";
        case OpType::REPNE: return "REPNE";
        default: return "???";
    }
}

// =====================================================================
// Helper: read bytes from memory with 16-bit wrapping
// =====================================================================

static inline uint8_t rb(const uint8_t* mem, uint16_t addr, int off) {
    return mem[(uint16_t)(addr + off)];
}

static inline uint16_t rw(const uint8_t* mem, uint16_t addr, int off) {
    uint8_t lo = mem[(uint16_t)(addr + off)];
    uint8_t hi = mem[(uint16_t)(addr + off + 1)];
    return lo | (hi << 8);
}

// =====================================================================
// ModR/M decoder: fills dst/src OpdDesc based on mod/reg/rm fields
// Returns number of extra bytes consumed (modrm byte + displacement)
// =====================================================================

static int decodeModRM(const uint8_t* mem, uint16_t addr, int offset,
                       OpdDesc& rm_opd, uint8_t& reg_field, bool is_word)
{
    uint8_t modrm = rb(mem, addr, offset);
    uint8_t mod = (modrm >> 6) & 3;
    reg_field   = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;
    int extra = 1; // modrm byte itself

    if (mod == 3) {
        // Register mode
        rm_opd.kind = is_word ? OpdKind::REG16 : OpdKind::REG8;
        rm_opd.reg = rm;
    } else {
        // Memory mode
        rm_opd.kind = OpdKind::MEM;
        rm_opd.direct = false;

        if (mod == 0 && rm == 6) {
            // Special: direct address [disp16]
            rm_opd.direct = true;
            rm_opd.disp = (int16_t)rw(mem, addr, offset + 1);
            rm_opd.has_disp = true;
            rm_opd.base = -1;
            rm_opd.index = -1;
            extra += 2;
        } else {
            // Set base/index from rm field
            switch (rm) {
                case 0: rm_opd.base = 3; rm_opd.index = 6; break; // [BX+SI]
                case 1: rm_opd.base = 3; rm_opd.index = 7; break; // [BX+DI]
                case 2: rm_opd.base = 5; rm_opd.index = 6; break; // [BP+SI]
                case 3: rm_opd.base = 5; rm_opd.index = 7; break; // [BP+DI]
                case 4: rm_opd.base = -1; rm_opd.index = 6; break; // [SI]
                case 5: rm_opd.base = -1; rm_opd.index = 7; break; // [DI]
                case 6: rm_opd.base = 5; rm_opd.index = -1; break; // [BP+disp]
                case 7: rm_opd.base = 3; rm_opd.index = -1; break; // [BX]
            }

            if (mod == 1) {
                rm_opd.disp = (int8_t)rb(mem, addr, offset + 1);
                rm_opd.has_disp = true;
                extra += 1;
            } else if (mod == 2) {
                rm_opd.disp = (int16_t)rw(mem, addr, offset + 1);
                rm_opd.has_disp = true;
                extra += 2;
            } else {
                rm_opd.disp = 0;
                rm_opd.has_disp = false;
            }
        }
    }

    return extra;
}

// ALU optype from /reg field in group1 (80-83)
static OpType aluFromReg(uint8_t reg) {
    static const OpType table[] = {
        OpType::ADD, OpType::OR, OpType::ADC, OpType::SBB,
        OpType::AND, OpType::SUB, OpType::XOR, OpType::CMP
    };
    return table[reg & 7];
}

// Shift optype from /reg field
static OpType shiftFromReg(uint8_t reg) {
    static const OpType table[] = {
        OpType::ROL, OpType::ROR, OpType::RCL, OpType::RCR,
        OpType::SHL, OpType::SHR, OpType::INVALID, OpType::SAR
    };
    return table[reg & 7];
}

// Jcc optype from opcode 70-7F
static OpType jccFromOpcode(uint8_t op) {
    static const OpType table[] = {
        OpType::JO, OpType::JNO, OpType::JB, OpType::JNB,
        OpType::JZ, OpType::JNZ, OpType::JBE, OpType::JNBE,
        OpType::JSS, OpType::JNS, OpType::JP, OpType::JNP,
        OpType::JL, OpType::JNL, OpType::JLE, OpType::JNLE
    };
    return table[op & 0x0F];
}

// =====================================================================
// Main decoder
// =====================================================================

DecodedInstr decode8086(const uint8_t* mem, uint16_t addr) {
    DecodedInstr instr;
    int pos = 0;
    uint8_t op = rb(mem, addr, pos);
    instr.opcode = op;

    // Handle prefixes: segment override, REP, LOCK
    bool has_prefix = true;
    while (has_prefix) {
        switch (op) {
            case 0x26: instr.seg_override = 0; pos++; op = rb(mem, addr, pos); break; // ES:
            case 0x2E: instr.seg_override = 1; pos++; op = rb(mem, addr, pos); break; // CS:
            case 0x36: instr.seg_override = 2; pos++; op = rb(mem, addr, pos); break; // SS:
            case 0x3E: instr.seg_override = 3; pos++; op = rb(mem, addr, pos); break; // DS:
            case 0xF0: pos++; op = rb(mem, addr, pos); break; // LOCK
            case 0xF2: instr.has_rep = true; instr.rep_z = false; pos++; op = rb(mem, addr, pos); break; // REPNE
            case 0xF3: instr.has_rep = true; instr.rep_z = true; pos++; op = rb(mem, addr, pos); break;  // REP/REPE
            default: has_prefix = false; break;
        }
    }

    instr.opcode = op;
    int opStart = pos;
    pos++; // consume opcode byte

    uint8_t reg_field = 0;

    switch (op) {
    // =================================================================
    // 00-05: ADD
    // 08-0D: OR
    // 10-15: ADC
    // 18-1D: SBB
    // 20-25: AND
    // 28-2D: SUB
    // 30-35: XOR
    // 38-3D: CMP
    // Pattern: base + 0: r/m8,  reg8     (d=0, w=0)
    //          base + 1: r/m16, reg16    (d=0, w=1)
    //          base + 2: reg8,  r/m8     (d=1, w=0)
    //          base + 3: reg16, r/m16    (d=1, w=1)
    //          base + 4: AL,    imm8
    //          base + 5: AX,    imm16
    // =================================================================
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D:
    {
        uint8_t base = op & 0xF8;
        uint8_t sub  = op & 0x07;
        instr.op = aluFromReg(base >> 3);
        instr.is_word = (sub & 1) != 0;

        if (sub < 4) {
            // ModR/M forms
            int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, instr.is_word);
            pos += extra;
            instr.src.kind = instr.is_word ? OpdKind::REG16 : OpdKind::REG8;
            instr.src.reg = reg_field;

            if (sub >= 2) {
                // d=1: swap dst/src (reg is dst)
                std::swap(instr.dst, instr.src);
            }
        } else {
            // AL/AX, imm
            instr.dst.kind = instr.is_word ? OpdKind::REG16 : OpdKind::REG8;
            instr.dst.reg = 0; // AL/AX
            if (instr.is_word) {
                instr.src.kind = OpdKind::IMM16;
                instr.src.imm = rw(mem, addr, pos);
                pos += 2;
            } else {
                instr.src.kind = OpdKind::IMM8;
                instr.src.imm = rb(mem, addr, pos);
                pos += 1;
            }
        }
        break;
    }

    // =================================================================
    // 06/07: PUSH/POP ES
    // 0E:    PUSH CS
    // 16/17: PUSH/POP SS
    // 1E/1F: PUSH/POP DS
    // =================================================================
    case 0x06: instr.op = OpType::PUSH; instr.dst.kind = OpdKind::SREG; instr.dst.reg = 0; break; // ES
    case 0x07: instr.op = OpType::POP;  instr.dst.kind = OpdKind::SREG; instr.dst.reg = 0; break;
    case 0x0E: instr.op = OpType::PUSH; instr.dst.kind = OpdKind::SREG; instr.dst.reg = 1; break; // CS
    case 0x16: instr.op = OpType::PUSH; instr.dst.kind = OpdKind::SREG; instr.dst.reg = 2; break; // SS
    case 0x17: instr.op = OpType::POP;  instr.dst.kind = OpdKind::SREG; instr.dst.reg = 2; break;
    case 0x1E: instr.op = OpType::PUSH; instr.dst.kind = OpdKind::SREG; instr.dst.reg = 3; break; // DS
    case 0x1F: instr.op = OpType::POP;  instr.dst.kind = OpdKind::SREG; instr.dst.reg = 3; break;

    // =================================================================
    // BCD: 27=DAA, 2F=DAS, 37=AAA, 3F=AAS
    // =================================================================
    case 0x27: instr.op = OpType::DAA; break;
    case 0x2F: instr.op = OpType::DAS; break;
    case 0x37: instr.op = OpType::AAA; break;
    case 0x3F: instr.op = OpType::AAS; break;

    // =================================================================
    // 40-47: INC reg16
    // 48-4F: DEC reg16
    // =================================================================
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        instr.op = OpType::INC;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = op - 0x40;
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        instr.op = OpType::DEC;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = op - 0x48;
        break;

    // =================================================================
    // 50-57: PUSH reg16
    // 58-5F: POP reg16
    // =================================================================
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        instr.op = OpType::PUSH;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = op - 0x50;
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        instr.op = OpType::POP;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = op - 0x58;
        break;

    // =================================================================
    // 60/61: PUSHA/POPA (186+)
    // =================================================================
    case 0x60: instr.op = OpType::PUSHA; break;
    case 0x61: instr.op = OpType::POPA;  break;

    // =================================================================
    // 70-7F: Jcc rel8
    // =================================================================
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        instr.op = jccFromOpcode(op);
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;

    // =================================================================
    // 80-83: ALU r/m, imm (Group 1)
    // 80: r/m8, imm8    81: r/m16, imm16
    // 82: r/m8, imm8    83: r/m16, sign-ext imm8
    // =================================================================
    case 0x80: case 0x82:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.op = aluFromReg(reg_field);
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;

    case 0x81:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = aluFromReg(reg_field);
        instr.src.kind = OpdKind::IMM16;
        instr.src.imm = rw(mem, addr, pos);
        pos += 2;
        break;

    case 0x83:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = aluFromReg(reg_field);
        instr.src.kind = OpdKind::IMM16;
        instr.src.imm = (uint16_t)(int16_t)(int8_t)rb(mem, addr, pos); // sign-extend
        pos += 1;
        break;

    // =================================================================
    // 84-85: TEST r/m, reg
    // =================================================================
    case 0x84:
        instr.op = OpType::TEST;
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = reg_field;
        break;
    case 0x85:
        instr.op = OpType::TEST;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = reg_field;
        break;

    // =================================================================
    // 86-87: XCHG r/m, reg
    // =================================================================
    case 0x86:
        instr.op = OpType::XCHG;
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = reg_field;
        break;
    case 0x87:
        instr.op = OpType::XCHG;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = reg_field;
        break;

    // =================================================================
    // 88-8B: MOV r/m, reg / reg, r/m
    // =================================================================
    case 0x88: // MOV r/m8, reg8
        instr.op = OpType::MOV;
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = reg_field;
        break;
    case 0x89: // MOV r/m16, reg16
        instr.op = OpType::MOV;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = reg_field;
        break;
    case 0x8A: // MOV reg8, r/m8
        instr.op = OpType::MOV;
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, false); pos += extra; }
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = reg_field;
        break;
    case 0x8B: // MOV reg16, r/m16
        instr.op = OpType::MOV;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, true); pos += extra; }
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = reg_field;
        break;

    // =================================================================
    // 8C: MOV r/m16, sreg
    // 8E: MOV sreg, r/m16
    // =================================================================
    case 0x8C:
        instr.op = OpType::MOV;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.src.kind = OpdKind::SREG;
        instr.src.reg = reg_field & 3;
        break;
    case 0x8E:
        instr.op = OpType::MOV;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, true); pos += extra; }
        instr.dst.kind = OpdKind::SREG;
        instr.dst.reg = reg_field & 3;
        break;

    // =================================================================
    // 8D: LEA reg16, mem
    // =================================================================
    case 0x8D:
        instr.op = OpType::LEA;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, true); pos += extra; }
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = reg_field;
        break;

    // =================================================================
    // 8F /0: POP r/m16
    // =================================================================
    case 0x8F:
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = OpType::POP;
        instr.is_word = true;
        break;

    // =================================================================
    // 90: NOP (XCHG AX,AX)
    // 91-97: XCHG AX, reg16
    // =================================================================
    case 0x90:
        instr.op = OpType::NOP;
        break;
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
        instr.op = OpType::XCHG;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 0; // AX
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = op - 0x90;
        break;

    // =================================================================
    // 98: CBW, 99: CWD
    // =================================================================
    case 0x98: instr.op = OpType::CBW; break;
    case 0x99: instr.op = OpType::CWD; break;

    // =================================================================
    // 9A: CALL far ptr (seg:off) — treat as far call
    // =================================================================
    case 0x9A:
        instr.op = OpType::CALL;
        instr.dst.kind = OpdKind::FAR_PTR;
        instr.dst.off = rw(mem, addr, pos); pos += 2;
        instr.dst.seg = rw(mem, addr, pos); pos += 2;
        break;

    // =================================================================
    // 9B: WAIT/FWAIT
    // =================================================================
    case 0x9B: instr.op = OpType::WAIT; break;

    // =================================================================
    // 9C: PUSHF, 9D: POPF
    // =================================================================
    case 0x9C: instr.op = OpType::PUSHF; break;
    case 0x9D: instr.op = OpType::POPF;  break;

    // =================================================================
    // 9E: SAHF, 9F: LAHF
    // =================================================================
    case 0x9E: instr.op = OpType::SAHF; break;
    case 0x9F: instr.op = OpType::LAHF; break;

    // =================================================================
    // A0-A3: MOV AL/AX, moffs / moffs, AL/AX
    // =================================================================
    case 0xA0: // MOV AL, [moffs8]
        instr.op = OpType::MOV;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = 0; // AL
        instr.src.kind = OpdKind::MEM;
        instr.src.direct = true;
        instr.src.disp = (int16_t)rw(mem, addr, pos);
        instr.src.has_disp = true;
        instr.src.base = -1; instr.src.index = -1;
        pos += 2;
        break;
    case 0xA1: // MOV AX, [moffs16]
        instr.op = OpType::MOV;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 0; // AX
        instr.src.kind = OpdKind::MEM;
        instr.src.direct = true;
        instr.src.disp = (int16_t)rw(mem, addr, pos);
        instr.src.has_disp = true;
        instr.src.base = -1; instr.src.index = -1;
        pos += 2;
        break;
    case 0xA2: // MOV [moffs8], AL
        instr.op = OpType::MOV;
        instr.is_word = false;
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = 0; // AL
        instr.dst.kind = OpdKind::MEM;
        instr.dst.direct = true;
        instr.dst.disp = (int16_t)rw(mem, addr, pos);
        instr.dst.has_disp = true;
        instr.dst.base = -1; instr.dst.index = -1;
        pos += 2;
        break;
    case 0xA3: // MOV [moffs16], AX
        instr.op = OpType::MOV;
        instr.is_word = true;
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = 0; // AX
        instr.dst.kind = OpdKind::MEM;
        instr.dst.direct = true;
        instr.dst.disp = (int16_t)rw(mem, addr, pos);
        instr.dst.has_disp = true;
        instr.dst.base = -1; instr.dst.index = -1;
        pos += 2;
        break;

    // =================================================================
    // A4-A7: String ops (MOVSB, MOVSW, CMPSB, CMPSW)
    // =================================================================
    case 0xA4: instr.op = OpType::MOVSB; break;
    case 0xA5: instr.op = OpType::MOVSW; break;
    case 0xA6: instr.op = OpType::CMPSB; break;
    case 0xA7: instr.op = OpType::CMPSW; break;

    // =================================================================
    // A8-A9: TEST AL/AX, imm
    // =================================================================
    case 0xA8:
        instr.op = OpType::TEST;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = 0; // AL
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xA9:
        instr.op = OpType::TEST;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 0; // AX
        instr.src.kind = OpdKind::IMM16;
        instr.src.imm = rw(mem, addr, pos);
        pos += 2;
        break;

    // =================================================================
    // AA-AF: String ops (STOSB, STOSW, LODSB, LODSW, SCASB, SCASW)
    // =================================================================
    case 0xAA: instr.op = OpType::STOSB; break;
    case 0xAB: instr.op = OpType::STOSW; break;
    case 0xAC: instr.op = OpType::LODSB; break;
    case 0xAD: instr.op = OpType::LODSW; break;
    case 0xAE: instr.op = OpType::SCASB; break;
    case 0xAF: instr.op = OpType::SCASW; break;

    // =================================================================
    // B0-B7: MOV reg8, imm8
    // B8-BF: MOV reg16, imm16
    // =================================================================
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        instr.op = OpType::MOV;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = op - 0xB0;
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        instr.op = OpType::MOV;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = op - 0xB8;
        instr.src.kind = OpdKind::IMM16;
        instr.src.imm = rw(mem, addr, pos);
        pos += 2;
        break;

    // =================================================================
    // C0/C1: Shift r/m, imm8 (186+)
    // =================================================================
    case 0xC0:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xC1:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;

    // =================================================================
    // C2: RET imm16, C3: RET
    // =================================================================
    case 0xC2:
        instr.op = OpType::RET;
        instr.dst.kind = OpdKind::IMM16;
        instr.dst.imm = rw(mem, addr, pos);
        pos += 2;
        break;
    case 0xC3:
        instr.op = OpType::RET;
        break;

    // =================================================================
    // C4: LES, C5: LDS
    // =================================================================
    case 0xC4:
        instr.op = OpType::LES;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, true); pos += extra; }
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = reg_field;
        break;
    case 0xC5:
        instr.op = OpType::LDS;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.src, reg_field, true); pos += extra; }
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = reg_field;
        break;

    // =================================================================
    // C6: MOV r/m8, imm8    C7: MOV r/m16, imm16
    // =================================================================
    case 0xC6:
        instr.op = OpType::MOV;
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xC7:
        instr.op = OpType::MOV;
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.src.kind = OpdKind::IMM16;
        instr.src.imm = rw(mem, addr, pos);
        pos += 2;
        break;

    // =================================================================
    // CA: RETF imm16, CB: RETF
    // =================================================================
    case 0xCA:
        instr.op = OpType::RETF;
        instr.dst.kind = OpdKind::IMM16;
        instr.dst.imm = rw(mem, addr, pos);
        pos += 2;
        break;
    case 0xCB:
        instr.op = OpType::RETF;
        break;

    // =================================================================
    // CC: INT 3, CD: INT imm8, CE: INTO
    // =================================================================
    case 0xCC:
        instr.op = OpType::INT;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = 3;
        break;
    case 0xCD:
        instr.op = OpType::INT;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xCE:
        instr.op = OpType::INTO;
        break;

    // =================================================================
    // CF: IRET
    // =================================================================
    case 0xCF:
        instr.op = OpType::IRET;
        break;

    // =================================================================
    // D0: Shift r/m8, 1    D1: Shift r/m16, 1
    // D2: Shift r/m8, CL   D3: Shift r/m16, CL
    // =================================================================
    case 0xD0:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = 1;
        break;
    case 0xD1:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = 1;
        break;
    case 0xD2:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = 1; // CL
        break;
    case 0xD3:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        instr.op = shiftFromReg(reg_field);
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = 1; // CL
        break;

    // =================================================================
    // D4: AAM, D5: AAD
    // =================================================================
    case 0xD4:
        instr.op = OpType::AAM;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xD5:
        instr.op = OpType::AAD;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = rb(mem, addr, pos);
        pos += 1;
        break;

    // =================================================================
    // D7: XLAT
    // =================================================================
    case 0xD7:
        instr.op = OpType::XLAT;
        break;

    // =================================================================
    // E0: LOOPNE, E1: LOOPE, E2: LOOP, E3: JCXZ
    // =================================================================
    case 0xE0:
        instr.op = OpType::LOOPNE;
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xE1:
        instr.op = OpType::LOOPE;
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xE2:
        instr.op = OpType::LOOP;
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xE3:
        instr.op = OpType::JCXZ;
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;

    // =================================================================
    // E4-E7: IN/OUT imm8
    // =================================================================
    case 0xE4: // IN AL, imm8
        instr.op = OpType::IN;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = 0; // AL
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xE5: // IN AX, imm8
        instr.op = OpType::IN;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 0; // AX
        instr.src.kind = OpdKind::IMM8;
        instr.src.imm = rb(mem, addr, pos);
        pos += 1;
        break;
    case 0xE6: // OUT imm8, AL
        instr.op = OpType::OUT;
        instr.is_word = false;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = rb(mem, addr, pos);
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = 0; // AL
        pos += 1;
        break;
    case 0xE7: // OUT imm8, AX
        instr.op = OpType::OUT;
        instr.is_word = true;
        instr.dst.kind = OpdKind::IMM8;
        instr.dst.imm = rb(mem, addr, pos);
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = 0; // AX
        pos += 1;
        break;

    // =================================================================
    // E8: CALL rel16, E9: JMP rel16
    // =================================================================
    case 0xE8:
        instr.op = OpType::CALL;
        instr.dst.kind = OpdKind::REL16;
        instr.dst.rel = (int16_t)rw(mem, addr, pos);
        pos += 2;
        break;
    case 0xE9:
        instr.op = OpType::JMP;
        instr.dst.kind = OpdKind::REL16;
        instr.dst.rel = (int16_t)rw(mem, addr, pos);
        pos += 2;
        break;

    // =================================================================
    // EA: JMP far ptr
    // =================================================================
    case 0xEA:
        instr.op = OpType::JMP;
        instr.dst.kind = OpdKind::FAR_PTR;
        instr.dst.off = rw(mem, addr, pos); pos += 2;
        instr.dst.seg = rw(mem, addr, pos); pos += 2;
        break;

    // =================================================================
    // EB: JMP rel8
    // =================================================================
    case 0xEB:
        instr.op = OpType::JMP;
        instr.dst.kind = OpdKind::REL8;
        instr.dst.rel = (int8_t)rb(mem, addr, pos);
        pos += 1;
        break;

    // =================================================================
    // EC-EF: IN/OUT DX
    // =================================================================
    case 0xEC: // IN AL, DX
        instr.op = OpType::IN;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG8;
        instr.dst.reg = 0;
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = 2; // DX
        break;
    case 0xED: // IN AX, DX
        instr.op = OpType::IN;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 0;
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = 2;
        break;
    case 0xEE: // OUT DX, AL
        instr.op = OpType::OUT;
        instr.is_word = false;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 2; // DX
        instr.src.kind = OpdKind::REG8;
        instr.src.reg = 0;
        break;
    case 0xEF: // OUT DX, AX
        instr.op = OpType::OUT;
        instr.is_word = true;
        instr.dst.kind = OpdKind::REG16;
        instr.dst.reg = 2;
        instr.src.kind = OpdKind::REG16;
        instr.src.reg = 0;
        break;

    // =================================================================
    // F4: HLT
    // =================================================================
    case 0xF4: instr.op = OpType::HLT; break;

    // =================================================================
    // F5: CMC
    // =================================================================
    case 0xF5: instr.op = OpType::CMC; break;

    // =================================================================
    // F6: Unary group 3 byte
    // F7: Unary group 3 word
    // /0=TEST, /2=NOT, /3=NEG, /4=MUL, /5=IMUL, /6=DIV, /7=IDIV
    // =================================================================
    case 0xF6:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        switch (reg_field) {
            case 0: case 1:
                instr.op = OpType::TEST;
                instr.src.kind = OpdKind::IMM8;
                instr.src.imm = rb(mem, addr, pos);
                pos += 1;
                break;
            case 2: instr.op = OpType::NOT;  break;
            case 3: instr.op = OpType::NEG;  break;
            case 4: instr.op = OpType::MUL;  break;
            case 5: instr.op = OpType::IMUL; break;
            case 6: instr.op = OpType::DIV;  break;
            case 7: instr.op = OpType::IDIV; break;
        }
        break;
    case 0xF7:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        switch (reg_field) {
            case 0: case 1:
                instr.op = OpType::TEST;
                instr.src.kind = OpdKind::IMM16;
                instr.src.imm = rw(mem, addr, pos);
                pos += 2;
                break;
            case 2: instr.op = OpType::NOT;  break;
            case 3: instr.op = OpType::NEG;  break;
            case 4: instr.op = OpType::MUL;  break;
            case 5: instr.op = OpType::IMUL; break;
            case 6: instr.op = OpType::DIV;  break;
            case 7: instr.op = OpType::IDIV; break;
        }
        break;

    // =================================================================
    // F8-FD: Flag manipulation
    // =================================================================
    case 0xF8: instr.op = OpType::CLC; break;
    case 0xF9: instr.op = OpType::STC; break;
    case 0xFA: instr.op = OpType::CLI; break;
    case 0xFB: instr.op = OpType::STI; break;
    case 0xFC: instr.op = OpType::CLD; break;
    case 0xFD: instr.op = OpType::STD; break;

    // =================================================================
    // FE: Group 4 byte (INC/DEC r/m8)
    // =================================================================
    case 0xFE:
        instr.is_word = false;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, false); pos += extra; }
        switch (reg_field) {
            case 0: instr.op = OpType::INC; break;
            case 1: instr.op = OpType::DEC; break;
            default: instr.op = OpType::INVALID; break;
        }
        break;

    // =================================================================
    // FF: Group 5 word
    // /0=INC, /1=DEC, /2=CALL indirect, /3=CALL far indirect
    // /4=JMP indirect, /5=JMP far indirect, /6=PUSH
    // =================================================================
    case 0xFF:
        instr.is_word = true;
        { int extra = decodeModRM(mem, addr, pos, instr.dst, reg_field, true); pos += extra; }
        switch (reg_field) {
            case 0: instr.op = OpType::INC;  break;
            case 1: instr.op = OpType::DEC;  break;
            case 2: instr.op = OpType::CALL; break; // indirect near
            case 3: instr.op = OpType::CALL; break; // indirect far (dst is MEM)
            case 4: instr.op = OpType::JMP;  break; // indirect near
            case 5: instr.op = OpType::JMP;  break; // indirect far
            case 6: instr.op = OpType::PUSH; break;
            default: instr.op = OpType::INVALID; break;
        }
        break;

    default:
        // Unrecognized opcode
        instr.op = OpType::INVALID;
        break;
    }

    instr.len = pos - opStart;
    // If there were prefixes, add them to total length
    instr.len = pos;

    return instr;
}
