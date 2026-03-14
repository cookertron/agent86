#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class TokenType {
    LABEL,          // "name:" or ".name:"
    MNEMONIC,       // MOV, ADD, etc.
    REGISTER,       // AX, BX, AL, etc.
    NUMBER,         // decimal, hex, binary
    STRING,         // 'Hello!'
    CHAR_LITERAL,   // 'A'
    COMMA,
    OPEN_BRACKET,   // [
    CLOSE_BRACKET,  // ]
    PLUS,
    MINUS,
    STAR,
    SLASH,
    COLON,
    DOLLAR,         // $
    OPEN_PAREN,     // (
    CLOSE_PAREN,    // )
    PERCENT,        // %
    AMPERSAND,      // &
    PIPE,           // |
    CARET,          // ^
    TILDE,          // ~
    SHL,            // <<
    SHR,            // >>
    SIZE_KEYWORD,   // BYTE, WORD
    EOL
};

enum class Reg : uint8_t {
    AL=0, CL, DL, BL, AH, CH, DH, BH,  // 8-bit: 0-7
    AX=0, CX, DX, BX, SP, BP, SI, DI    // 16-bit: 0-7
};

enum class SReg : uint8_t {
    ES=0, CS=1, SS=2, DS=3
};

enum class OpSize { UNKNOWN, BYTE, WORD };

struct Token {
    TokenType type;
    std::string text;
    int64_t numval = 0;
    OpSize size = OpSize::UNKNOWN; // for SIZE_KEYWORD
};

enum class OperandType {
    NONE,
    REG,
    SREG,       // segment register (ES, CS, SS, DS)
    IMM,
    MEM,        // [expression] or [BX + label] etc.
    LABEL_IMM   // label used as immediate (e.g., MOV DX, msg)
};

struct MemOperand {
    bool has_bx = false;
    bool has_bp = false;
    bool has_si = false;
    bool has_di = false;
    bool has_disp = false;
    int64_t disp = 0;
    bool disp_unresolved = false;
    std::string disp_expr; // raw expression text for re-eval in pass 2
};

struct Operand {
    OperandType type = OperandType::NONE;
    Reg reg = Reg::AX;
    SReg sreg = SReg::ES;
    bool is_reg8 = false;
    int64_t imm = 0;
    bool imm_unresolved = false;
    OpSize size_override = OpSize::UNKNOWN; // BYTE/WORD prefix
    MemOperand mem;
    std::string expr_text; // raw expression for re-evaluation
    uint8_t seg_prefix = 0xFF;  // segment override: 0=ES, 1=CS, 2=SS, 3=DS, 0xFF=none
};

struct ParsedLine {
    std::string label;          // empty if no label
    bool is_local_label = false;
    std::string mnemonic;       // uppercase
    std::vector<Operand> operands;
    std::string directive;      // DB, DW, RESB, RESW, EQU, ORG, PROC, ENDP
    std::vector<Token> directive_args; // raw tokens after directive
    std::string raw_line;       // original line text
    int line_number = 0;
    int calculated_size = 0;    // bytes this line produces
    std::string prefix;         // REP etc.
};

// Register lookup helpers
struct RegInfo {
    const char* name;
    Reg reg;
    bool is_8bit;
};

inline const RegInfo REG_TABLE[] = {
    {"AL", Reg::AL, true},  {"CL", Reg::CL, true},
    {"DL", Reg::DL, true},  {"BL", Reg::BL, true},
    {"AH", Reg::AH, true},  {"CH", Reg::CH, true},
    {"DH", Reg::DH, true},  {"BH", Reg::BH, true},
    {"AX", Reg::AX, false}, {"CX", Reg::CX, false},
    {"DX", Reg::DX, false}, {"BX", Reg::BX, false},
    {"SP", Reg::SP, false}, {"BP", Reg::BP, false},
    {"SI", Reg::SI, false}, {"DI", Reg::DI, false},
};

struct SRegInfo {
    const char* name;
    SReg sreg;
};

inline const SRegInfo SREG_TABLE[] = {
    {"ES", SReg::ES}, {"CS", SReg::CS},
    {"SS", SReg::SS}, {"DS", SReg::DS},
};
