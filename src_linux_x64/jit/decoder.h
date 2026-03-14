#pragma once
#include <cstdint>

// Instruction operation types
enum class OpType {
    INVALID,
    // Data movement
    MOV, XCHG, LEA, LDS, LES, PUSH, POP, PUSHA, POPA, PUSHF, POPF,
    // ALU
    ADD, ADC, SUB, SBB, AND, OR, XOR, CMP, TEST, NEG, NOT, INC, DEC,
    // Multiply/divide
    MUL, IMUL, DIV, IDIV,
    // Shifts/rotates
    SHL, SHR, SAR, ROL, ROR, RCL, RCR,
    // Control flow
    JMP, CALL, RET, RETF, IRET, INT, INTO, HLT,
    // Conditional jumps
    JO, JNO, JB, JNB, JZ, JNZ, JBE, JNBE,
    JSS, JNS, JP, JNP, JL, JNL, JLE, JNLE,  // JSS to avoid conflict with JS keyword
    JCXZ, LOOP, LOOPE, LOOPNE,
    // String ops
    MOVSB, MOVSW, STOSB, STOSW, LODSB, LODSW,
    CMPSB, CMPSW, SCASB, SCASW,
    // Flag ops
    CLC, STC, CLI, STI, CLD, STD, CMC,
    LAHF, SAHF,
    // BCD
    DAA, DAS, AAA, AAS, AAM, AAD,
    // Misc
    CBW, CWD, XLAT, NOP, WAIT, LOCK,
    // I/O
    IN, OUT,
    // REP prefix (handled specially)
    REP, REPE, REPNE
};

// Operand descriptor
enum class OpdKind {
    NONE,
    REG8,     // 8-bit register
    REG16,    // 16-bit register
    SREG,     // segment register
    MEM,      // memory [EA]
    IMM8,     // 8-bit immediate
    IMM16,    // 16-bit immediate
    REL8,     // relative offset (8-bit)
    REL16,    // relative offset (16-bit)
    FAR_PTR   // seg:off
};

struct OpdDesc {
    OpdKind kind = OpdKind::NONE;
    uint8_t reg  = 0;     // register index (for REG8/REG16/SREG)

    // Memory addressing
    int8_t  base = -1;    // base register index (-1=none) [BX=3,BP=5]
    int8_t  index = -1;   // index register index (-1=none) [SI=6,DI=7]
    int16_t disp = 0;     // displacement
    bool    has_disp = false;
    bool    direct = false; // direct memory [disp16] (mod=00, rm=110)

    // Immediate / relative
    uint16_t imm = 0;
    int16_t  rel = 0;

    // Far pointer
    uint16_t seg = 0;
    uint16_t off = 0;
};

struct DecodedInstr {
    OpType  op = OpType::INVALID;
    OpdDesc dst;
    OpdDesc src;
    uint8_t opcode = 0;   // original opcode byte
    int     len = 0;       // total instruction length in bytes
    bool    is_word = false; // 0=byte, 1=word
    bool    has_rep = false;
    bool    rep_z = true;  // true=REP/REPE, false=REPNE
    uint8_t seg_override = 0xFF; // 0xFF=none, else S_ES/CS/SS/DS
};

// Decode one 8086 instruction from memory at given address
// Returns DecodedInstr with op==INVALID if unrecognized
DecodedInstr decode8086(const uint8_t* mem, uint16_t addr);

// Get a disassembly string for a decoded instruction
const char* opTypeName(OpType op);
