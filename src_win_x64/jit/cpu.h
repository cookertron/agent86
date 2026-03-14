#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// 8086 register indices (matching standard encoding)
enum Reg86 : uint8_t {
    R_AX = 0, R_CX = 1, R_DX = 2, R_BX = 3,
    R_SP = 4, R_BP = 5, R_SI = 6, R_DI = 7
};
// 8-bit: AL=0, CL=1, DL=2, BL=3, AH=4, CH=5, DH=6, BH=7

enum SReg86 : uint8_t {
    S_ES = 0, S_CS = 1, S_SS = 2, S_DS = 3
};

// 8086 flag bit positions (same as x64 RFLAGS)
enum Flag86 : uint16_t {
    F_CF = 0x0001,  // bit 0
    F_PF = 0x0004,  // bit 2
    F_AF = 0x0010,  // bit 4
    F_ZF = 0x0040,  // bit 6
    F_SF = 0x0080,  // bit 7
    F_TF = 0x0100,  // bit 8
    F_IF = 0x0200,  // bit 9
    F_DF = 0x0400,  // bit 10
    F_OF = 0x0800   // bit 11
};

static constexpr uint16_t FLAGS_MASK = 0x0FD5; // all arithmetic flags

struct CPU8086 {
    uint16_t regs[8];       // offset 0:  AX,CX,DX,BX,SP,BP,SI,DI
    uint16_t sregs[4];      // offset 16: ES,CS,SS,DS
    uint16_t ip;            // offset 24
    uint16_t flags;         // offset 26
    uint8_t  memory[1048576]; // offset 28 — 1MB for full 20-bit addressing
    int32_t  pending_int;     // offset 1048604 (-1 = none)
    bool     halted;          // offset 1048608
    uint64_t instr_count;     // (approx, after padding)

    void reset() {
        memset(regs, 0, sizeof(regs));
        memset(sregs, 0, sizeof(sregs));
        ip = 0x0100;
        flags = 0x0002; // bit 1 always set on 8086
        memset(memory, 0, sizeof(memory));
        pending_int = -1;
        halted = false;
        instr_count = 0;
        regs[R_SP] = 0xFFFE;
        sregs[S_CS] = 0;
        sregs[S_DS] = 0;
        sregs[S_SS] = 0;
        sregs[S_ES] = 0;
    }

    bool loadCOM(const std::vector<uint8_t>& data) {
        if (data.size() > 65536 - 0x100) return false;
        reset();
        memcpy(&memory[0x0100], data.data(), data.size());
        return true;
    }

    bool loadCOM(const uint8_t* data, size_t size) {
        if (size > 65536 - 0x100) return false;
        reset();
        memcpy(&memory[0x0100], data, size);
        return true;
    }
};

// Struct offset constants for x64 code emission
static constexpr int OFF_REGS     = 0;
static constexpr int OFF_SREGS    = 16;
static constexpr int OFF_IP       = 24;
static constexpr int OFF_FLAGS    = 26;
static constexpr int OFF_MEMORY   = 28;
static constexpr int OFF_PENDING  = 1048604;
static constexpr int OFF_HALTED   = 1048608;

// Compile-time layout checks
static_assert(offsetof(CPU8086, regs)        == OFF_REGS,    "regs offset");
static_assert(offsetof(CPU8086, sregs)       == OFF_SREGS,   "sregs offset");
static_assert(offsetof(CPU8086, ip)          == OFF_IP,      "ip offset");
static_assert(offsetof(CPU8086, flags)       == OFF_FLAGS,   "flags offset");
static_assert(offsetof(CPU8086, memory)      == OFF_MEMORY,  "memory offset");
static_assert(offsetof(CPU8086, pending_int) == OFF_PENDING, "pending_int offset");
static_assert(offsetof(CPU8086, halted)      == OFF_HALTED,  "halted offset");

// Helper: offset of 16-bit register n within CPU struct
inline constexpr int regOff16(int n) { return OFF_REGS + n * 2; }

// Helper: offset of 8-bit register n (0-3=low, 4-7=high)
inline constexpr int regOff8(int n) {
    return OFF_REGS + (n < 4 ? n * 2 : (n - 4) * 2 + 1);
}

// Helper: offset of segment register
inline constexpr int sregOff(int n) { return OFF_SREGS + n * 2; }
