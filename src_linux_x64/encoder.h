#pragma once
#include "types.h"
#include <vector>
#include <cstdint>

class Encoder {
public:
    // Returns encoded bytes for an instruction. Empty = error.
    std::vector<uint8_t> encode(const ParsedLine& line, bool pass1);

    // Returns size in bytes (for pass 1 estimation)
    int estimateSize(const ParsedLine& line);

private:
    int estimateSizeBase(const ParsedLine& line);
    // ModR/M helpers
    uint8_t modRM(uint8_t mod, uint8_t reg, uint8_t rm);
    std::vector<uint8_t> encodeMemOperand(const MemOperand& mem, uint8_t reg_field);

    // Instruction encoders
    std::vector<uint8_t> encodeALU(const std::string& mnem, const Operand& dst, const Operand& src);
    std::vector<uint8_t> encodeMOV(const Operand& dst, const Operand& src);
    std::vector<uint8_t> encodeShift(const std::string& mnem, const Operand& dst, const Operand& src);
    std::vector<uint8_t> encodeUnary(const std::string& mnem, const Operand& op);
    std::vector<uint8_t> encodePushPop(const std::string& mnem, const Operand& op);
    std::vector<uint8_t> encodeLEA(const Operand& dst, const Operand& src);
    std::vector<uint8_t> encodeTEST(const Operand& dst, const Operand& src);

    OpSize operandSize(const Operand& op);
    uint8_t regNum(Reg r) { return static_cast<uint8_t>(r) & 0x07; }

public:
    // Exposed for asm.cpp to call directly with current_addr
    std::vector<uint8_t> encodeJcc(const std::string& mnem, int64_t target, int64_t current_addr);
};
