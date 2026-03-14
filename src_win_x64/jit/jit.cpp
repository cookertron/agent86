#include "jit.h"
#include "dos.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <sstream>

// x64 register encoding constants
enum X64 : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13
};

// REX prefix bits
static constexpr uint8_t REX_W = 0x48;  // 64-bit operand
static constexpr uint8_t REX_R = 0x44;  // ext MODRM.reg
static constexpr uint8_t REX_B = 0x41;  // ext MODRM.rm or SIB.base

JitEngine::JitEngine()
    : cpu_storage_(std::make_unique<CPU8086>()),
      cpu_(*cpu_storage_),
      code_(8192) {}
JitEngine::~JitEngine() {}

void JitEngine::setEvents(std::vector<KeyEvent> triggered, std::vector<InputEvent> sequential) {
    kbd_.setEvents(std::move(triggered), std::move(sequential), &mouse_);
    has_events_ = true;
}

void JitEngine::setScreen(const std::string& mode) {
    video_.active = true;
    video_.mode_name = mode;
    if (mode == "MDA") {
        video_.cols = 80; video_.rows = 25;
        video_.vram_base = 0xB0000;
    } else if (mode == "CGA40") {
        video_.cols = 40; video_.rows = 25;
        video_.vram_base = 0xB8000;
    } else if (mode == "CGA80") {
        video_.cols = 80; video_.rows = 25;
        video_.vram_base = 0xB8000;
    } else if (mode == "VGA50") {
        video_.cols = 80; video_.rows = 50;
        video_.vram_base = 0xB8000;
    }
}

void JitEngine::setArgs(const std::string& args) {
    program_args_ = args;
}

// CP437 → Unicode codepoint table (all 256 entries)
static const uint32_t cp437_to_unicode[256] = {
    // 0x00-0x1F: control chars → visible CP437 glyphs
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    // 0x20-0x7E: standard ASCII (identity mapping)
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    // 0x80-0xFF: extended CP437
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// Encode a Unicode codepoint as UTF-8 and append to string
static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

// JSON-escape a UTF-8 string (handles ", \, and control chars)
static void jsonEscapeAppend(std::string& json, const std::string& s) {
    for (unsigned char c : s) {
        if (c == '"') json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else json += (char)c;
    }
}

std::string JitEngine::renderScreenJson(const JitVramOutParams& params) {
    // Determine region bounds
    int start_row = 0, start_col = 0;
    int end_row = video_.rows, end_col = video_.cols;
    bool is_partial = params.active && !params.full;
    if (is_partial) {
        start_col = std::max(0, std::min(params.x, (int)video_.cols));
        start_row = std::max(0, std::min(params.y, (int)video_.rows));
        end_col = std::max(start_col, std::min(params.x + params.w, (int)video_.cols));
        end_row = std::max(start_row, std::min(params.y + params.h, (int)video_.rows));
    }

    std::string json = "{\"mode\":\"" + video_.mode_name + "\"";
    json += ",\"cols\":" + std::to_string(video_.cols);
    json += ",\"rows\":" + std::to_string(video_.rows);
    json += ",\"cursor\":[" + std::to_string(video_.cursor_row) + ","
            + std::to_string(video_.cursor_col) + "]";
    if (video_.cursor_hidden)
        json += ",\"cursor_hidden\":true";
    if (is_partial) {
        json += ",\"region\":[" + std::to_string(params.x) + ","
                + std::to_string(params.y) + ","
                + std::to_string(params.w) + ","
                + std::to_string(params.h) + "]";
    }
    json += ",\"lines\":[";
    bool show_attrs = params.active && params.attrs;
    std::string attrs_json;
    if (show_attrs) attrs_json = ",\"attrs\":[";

    for (int r = start_row; r < end_row; r++) {
        if (r > start_row) { json += ","; if (show_attrs) attrs_json += ","; }
        std::string line;
        std::string attr_hex;
        for (int c = start_col; c < end_col; c++) {
            uint32_t addr = video_.vram_base + (uint32_t)(r * video_.cols + c) * 2;
            uint8_t ch = cpu_.memory[addr];
            uint8_t at = cpu_.memory[addr + 1];
            // CP437 → Unicode → UTF-8
            uint32_t cp = cp437_to_unicode[ch];
            appendUtf8(line, cp);
            if (show_attrs) {
                static const char hex[] = "0123456789ABCDEF";
                attr_hex += hex[(at >> 4) & 0xF];
                attr_hex += hex[at & 0xF];
            }
        }
        // Right-trim spaces (U+0020 and U+00A0/NBSP from CP437 0xFF)
        size_t end = line.find_last_not_of(' ');
        if (end != std::string::npos) line = line.substr(0, end + 1);
        else line.clear();
        // JSON-escape
        json += "\"";
        jsonEscapeAppend(json, line);
        json += "\"";
        if (show_attrs) {
            attrs_json += "\"" + attr_hex + "\"";
        }
    }
    json += "]";
    if (show_attrs) {
        json += attrs_json + "]";
    }
    json += "}";
    return json;
}


// =====================================================================
// x64 Emission Helpers
// =====================================================================

// Emit ModR/M byte with register and displacement from RCX base
// [RCX + disp32] or [RCX + disp8]
static void emitModRMDisp(CodeBuffer& c, uint8_t reg, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        c.emit8(0x40 | (reg << 3) | RCX); // mod=01, rm=RCX
        c.emit8((uint8_t)(int8_t)disp);
    } else {
        c.emit8(0x80 | (reg << 3) | RCX); // mod=10, rm=RCX
        c.emit32((uint32_t)disp);
    }
}

void JitEngine::emitPrologue() {
    // RCX = CPU8086* (Win64 ABI first arg)
    // We keep RCX as our base pointer throughout
    // Save RBX (callee-saved) — we use it as scratch
    code_.emit8(0x53); // push rbx
    // Save RBP (callee-saved)
    code_.emit8(0x55); // push rbp
    // Save R12 (callee-saved) — used as scratch
    code_.emit8(REX_B); code_.emit8(0x50 | (R12 & 7)); // push r12
}

void JitEngine::emitEpilogue() {
    // Restore callee-saved and return
    code_.emit8(REX_B); code_.emit8(0x58 | (R12 & 7)); // pop r12
    code_.emit8(0x5D); // pop rbp
    code_.emit8(0x5B); // pop rbx
    code_.emit8(0xC3); // ret
}

void JitEngine::emitSetIP(uint16_t newIP) {
    // mov word [rcx + OFF_IP], newIP
    code_.emit8(0x66); // operand size prefix for 16-bit
    code_.emit8(0xC7); // MOV r/m16, imm16
    emitModRMDisp(code_, 0, OFF_IP);
    code_.emit16(newIP);
}

// Load 16-bit reg from CPU struct into x64 register (zero-extended to 64-bit)
// movzx x64reg, word [rcx + regOff16(reg86)]
void JitEngine::emitLoadReg16(int x64reg, int reg86) {
    int off = regOff16(reg86);
    if (x64reg >= 8) {
        code_.emit8(REX_R); // REX.R for extended reg
    }
    code_.emit8(0x0F);
    code_.emit8(0xB7); // MOVZX r, r/m16
    emitModRMDisp(code_, x64reg & 7, off);
}

// Store 16-bit from x64 register to CPU struct
// mov word [rcx + regOff16(reg86)], x64reg
void JitEngine::emitStoreReg16(int reg86, int x64reg) {
    int off = regOff16(reg86);
    code_.emit8(0x66); // 16-bit operand
    if (x64reg >= 8) {
        code_.emit8(REX_R);
    }
    code_.emit8(0x89); // MOV r/m16, r16
    emitModRMDisp(code_, x64reg & 7, off);
}

// Load 8-bit reg from CPU struct into x64 register (zero-extended)
// movzx x64reg, byte [rcx + regOff8(reg86)]
void JitEngine::emitLoadReg8(int x64reg, int reg86) {
    int off = regOff8(reg86);
    if (x64reg >= 8) {
        code_.emit8(REX_R);
    }
    code_.emit8(0x0F);
    code_.emit8(0xB6); // MOVZX r, r/m8
    emitModRMDisp(code_, x64reg & 7, off);
}

// Store 8-bit from x64 register to CPU struct
// mov byte [rcx + regOff8(reg86)], x64reg_low8
void JitEngine::emitStoreReg8(int reg86, int x64reg) {
    int off = regOff8(reg86);
    // Need REX prefix for uniform access to low bytes of all registers
    uint8_t rex = 0x40;
    if (x64reg >= 8) rex |= 0x04; // REX.R
    code_.emit8(rex);
    code_.emit8(0x88); // MOV r/m8, r8
    emitModRMDisp(code_, x64reg & 7, off);
}

// Add segment_reg * 16 to EAX, mask to 20 bits. Uses RDX as scratch.
void JitEngine::emitApplySegment(int seg_reg) {
    // movzx edx, word [rcx + sregOff(seg_reg)]
    code_.emit8(0x0F); code_.emit8(0xB7);
    emitModRMDisp(code_, RDX, sregOff(seg_reg));
    // shl edx, 4
    code_.emit8(0xC1); code_.emit8(0xE2); code_.emit8(0x04);
    // add eax, edx
    code_.emit8(0x01); code_.emit8(0xD0);
    // and eax, 0xFFFFF
    code_.emit8(0x25); code_.emit32(0x000FFFFF);
}

// Compute seg*16 + reg_value → EAX. Uses RDX as scratch.
void JitEngine::emitSegAddr(int seg_reg, int offset_reg) {
    emitLoadReg16(RAX, offset_reg);
    emitApplySegment(seg_reg);
}

// Compute effective address into RAX (physical 20-bit address with segmentation)
void JitEngine::emitComputeEA(const OpdDesc& opd) {
    if (opd.direct) {
        // Direct address: mov eax, disp (16-bit offset)
        code_.emit8(0xB8); // MOV EAX, imm32
        code_.emit32((uint16_t)opd.disp);
    } else {
        bool has_base = (opd.base >= 0);
        bool has_index = (opd.index >= 0);

        if (!has_base && !has_index) {
            code_.emit8(0xB8);
            code_.emit32((uint16_t)opd.disp);
        } else {
            if (has_base) {
                emitLoadReg16(RAX, opd.base);
                if (has_index) {
                    emitLoadReg16(RDX, opd.index);
                    code_.emit8(0x01); code_.emit8(0xD0); // ADD EAX, EDX
                }
            } else {
                emitLoadReg16(RAX, opd.index);
            }

            if (opd.has_disp && opd.disp != 0) {
                if (opd.disp >= -128 && opd.disp <= 127) {
                    code_.emit8(0x83); code_.emit8(0xC0);
                    code_.emit8((uint8_t)(int8_t)opd.disp);
                } else {
                    code_.emit8(0x05);
                    code_.emit32((uint32_t)(int32_t)opd.disp);
                }
            }

            // Mask to 16-bit offset: movzx eax, ax
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
        }
    }

    // Apply segmentation (unless LEA — indicated by seg_override_ == SEG_NONE)
    if (seg_override_ != SEG_NONE) {
        int seg;
        if (seg_override_ != 0xFF) {
            seg = seg_override_;  // explicit segment override prefix
        } else {
            seg = (opd.base == R_BP) ? S_SS : S_DS;  // default: SS for BP, DS otherwise
        }
        emitApplySegment(seg);
    }
}

// Load operand value into x64reg
void JitEngine::emitLoadOperand(int x64reg, const OpdDesc& opd, bool is_word) {
    switch (opd.kind) {
    case OpdKind::REG16:
        emitLoadReg16(x64reg, opd.reg);
        break;
    case OpdKind::REG8:
        emitLoadReg8(x64reg, opd.reg);
        break;
    case OpdKind::SREG:
        // movzx x64reg, word [rcx + sregOff(opd.reg)]
        if (x64reg >= 8) code_.emit8(REX_R);
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, x64reg & 7, sregOff(opd.reg));
        break;
    case OpdKind::IMM8:
        // mov x64reg, imm32 (zero extended)
        if (x64reg >= 8) { code_.emit8(REX_B); }
        code_.emit8(0xB8 | (x64reg & 7));
        code_.emit32(opd.imm);
        break;
    case OpdKind::IMM16:
        if (x64reg >= 8) { code_.emit8(REX_B); }
        code_.emit8(0xB8 | (x64reg & 7));
        code_.emit32(opd.imm);
        break;
    case OpdKind::MEM: {
        // Compute EA into RAX (or save/restore if x64reg==RAX)
        if (x64reg == RAX) {
            emitComputeEA(opd);
            // Now load from [rcx + rax + OFF_MEMORY]
            if (is_word) {
                // movzx eax, word [rcx + rax + OFF_MEMORY]
                code_.emit8(0x0F); code_.emit8(0xB7);
                // Need SIB for [rcx + rax + disp32]
                code_.emit8(0x84); // mod=10 if disp32, mod=00 if disp fits... use mod=10 for safety
                code_.emit8(0x01); // SIB: scale=0, index=RAX, base=RCX
                code_.emit32(OFF_MEMORY);
            } else {
                // movzx eax, byte [rcx + rax + OFF_MEMORY]
                code_.emit8(0x0F); code_.emit8(0xB6);
                code_.emit8(0x84);
                code_.emit8(0x01);
                code_.emit32(OFF_MEMORY);
            }
        } else {
            // Save x64reg, compute EA in RAX, then load into x64reg
            emitComputeEA(opd);
            if (is_word) {
                uint8_t rex = 0;
                if (x64reg >= 8) rex = REX_R;
                if (rex) code_.emit8(rex);
                code_.emit8(0x0F); code_.emit8(0xB7);
                code_.emit8(0x84 | ((x64reg & 7) << 3));
                code_.emit8(0x01); // SIB
                code_.emit32(OFF_MEMORY);
            } else {
                uint8_t rex = 0;
                if (x64reg >= 8) rex = REX_R;
                if (rex) code_.emit8(rex);
                code_.emit8(0x0F); code_.emit8(0xB6);
                code_.emit8(0x84 | ((x64reg & 7) << 3));
                code_.emit8(0x01);
                code_.emit32(OFF_MEMORY);
            }
        }
        break;
    }
    default:
        break;
    }
}

// Store x64 register value to operand location
void JitEngine::emitStoreOperand(const OpdDesc& opd, int x64reg, bool is_word) {
    switch (opd.kind) {
    case OpdKind::REG16:
        emitStoreReg16(opd.reg, x64reg);
        break;
    case OpdKind::REG8:
        emitStoreReg8(opd.reg, x64reg);
        break;
    case OpdKind::SREG:
        // mov word [rcx + sregOff(opd.reg)], x64reg_16
        code_.emit8(0x66);
        if (x64reg >= 8) code_.emit8(REX_R);
        code_.emit8(0x89);
        emitModRMDisp(code_, x64reg & 7, sregOff(opd.reg));
        break;
    case OpdKind::MEM: {
        // We need EA in a register that's not x64reg. Use R10.
        // Save value to R10 first, compute EA in RAX, then store from R10
        // mov r10, x64reg
        if (x64reg != R10) {
            uint8_t rex = REX_W | 0x01; // REX.W + REX.B (R10 in r/m field)
            if (x64reg >= 8) rex |= 0x04; // REX.R for source
            code_.emit8(rex);
            code_.emit8(0x89); // MOV r/m64, r64
            code_.emit8(0xC0 | ((x64reg & 7) << 3) | (R10 & 7));
        }
        emitComputeEA(opd);
        // Store: mov [rcx + rax + OFF_MEMORY], r10b/r10w
        if (is_word) {
            code_.emit8(0x66); // 16-bit operand
            code_.emit8(REX_R); // REX.R for R10
            code_.emit8(0x89); // MOV r/m16, r16
            code_.emit8(0x84 | ((R10 & 7) << 3)); // ModR/M
            code_.emit8(0x01); // SIB: RAX + RCX
            code_.emit32(OFF_MEMORY);
        } else {
            code_.emit8(REX_R); // REX.R for R10
            code_.emit8(0x88); // MOV r/m8, r8
            code_.emit8(0x84 | ((R10 & 7) << 3));
            code_.emit8(0x01);
            code_.emit32(OFF_MEMORY);
        }
        break;
    }
    default:
        break;
    }
}

// Capture RFLAGS after ALU op → merge into cpu.flags
// pushfq; pop rax; and eax, FLAGS_MASK; mov [rcx+OFF_FLAGS], ax
void JitEngine::emitCaptureFlags() {
    code_.emit8(0x9C); // pushfq
    code_.emit8(0x58); // pop rax
    code_.emit8(0x25); // AND EAX, imm32
    code_.emit32(FLAGS_MASK);
    code_.emit8(0x66); // 16-bit
    code_.emit8(0x89); // MOV r/m16, r16
    emitModRMDisp(code_, RAX, OFF_FLAGS);
}

// Capture flags but preserve CF (for INC/DEC)
void JitEngine::emitCaptureFlagsPreserveCF() {
    // Load old CF (MOVZX doesn't modify flags — safe before pushfq)
    code_.emit8(0x0F); code_.emit8(0xB7);
    emitModRMDisp(code_, RBX, OFF_FLAGS);

    // Save ALU flags BEFORE any flag-modifying ops
    code_.emit8(0x9C); // pushfq

    // Now safe to modify flags
    code_.emit8(0x83); code_.emit8(0xE3); code_.emit8(0x01); // AND EBX, 1

    // Get saved ALU flags
    code_.emit8(0x58); // pop rax
    code_.emit8(0x25); // AND EAX, imm32
    code_.emit32(FLAGS_MASK & ~F_CF); // all flags except CF

    // OR in old CF
    code_.emit8(0x09); code_.emit8(0xD8); // OR EAX, EBX

    // Store
    code_.emit8(0x66);
    code_.emit8(0x89);
    emitModRMDisp(code_, RAX, OFF_FLAGS);
}

// Push cpu.flags to native stack and popfq (for Jcc evaluation)
// CRITICAL: mask out TF (bit 8) and IF (bit 9) to avoid native traps
void JitEngine::emitRestoreFlags() {
    // movzx rax, word [rcx+OFF_FLAGS]
    code_.emit8(0x0F); code_.emit8(0xB7);
    emitModRMDisp(code_, RAX, OFF_FLAGS);
    // Mask to only arithmetic flags — never set TF/IF on native CPU
    code_.emit8(0x25); code_.emit32(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF); // AND EAX, 0x08D5
    code_.emit8(0x50); // push rax
    code_.emit8(0x9D); // popfq
}

// =====================================================================
// Main dispatch loop
// =====================================================================

int JitEngine::run(const uint8_t* comData, size_t comSize, RunMode mode,
                   const std::string& dbg_path, uint64_t max_cycles) {
    if (!dbg_path.empty()) {
        loadDebugInfo(dbg_path);
    }

    if (!cpu_.loadCOM(comData, comSize)) {
        std::cout << "{\"executed\":\"FAILED\",\"error\":\"COM too large\"}" << std::endl;
        return 1;
    }

    // Write command-line args to PSP command tail (offset 0x80-0xFF)
    // Format: [0x80] = length byte, [0x81..] = " args..." + 0x0D
    if (!program_args_.empty()) {
        // DOS convention: command tail starts with a space
        size_t tail_len = 1 + program_args_.size(); // space + args
        if (tail_len > 126) tail_len = 126;         // max 126 chars (+ 0x0D fits in 127 bytes)
        cpu_.memory[0x80] = (uint8_t)tail_len;
        cpu_.memory[0x81] = ' ';
        size_t copy_len = tail_len - 1; // chars of args to copy
        memcpy(&cpu_.memory[0x82], program_args_.data(), copy_len);
        cpu_.memory[0x81 + tail_len] = 0x0D;        // CR terminator
    } else {
        cpu_.memory[0x80] = 0;
        cpu_.memory[0x81] = 0x0D;
    }

    // Initialize VRAM if screen mode is active
    if (video_.active) {
        for (int i = 0; i < video_.rows * video_.cols; i++) {
            cpu_.memory[video_.vram_base + (uint32_t)i * 2] = ' ';
            cpu_.memory[video_.vram_base + (uint32_t)i * 2 + 1] = 0x07;
        }
    }

    dos_output_.clear();
    tracing_ = false;
    idle_polls_ = 0;

    while (!cpu_.halted) {
        if (cpu_.instr_count > max_cycles) {
            std::string fail_json = "{\"executed\":\"FAILED\",\"error\":\"instruction limit exceeded\"";
            if (!vram_dumps_.empty()) {
                fail_json += ",\"vram_dumps\":[";
                for (size_t vi = 0; vi < vram_dumps_.size(); vi++) {
                    if (vi > 0) fail_json += ",";
                    fail_json += vram_dumps_[vi];
                }
                fail_json += "]";
            }
            if (!reg_dumps_.empty()) {
                fail_json += ",\"reg_dumps\":[";
                for (size_t ri = 0; ri < reg_dumps_.size(); ri++) {
                    if (ri > 0) fail_json += ",";
                    fail_json += reg_dumps_[ri];
                }
                fail_json += "]";
            }
            if (!log_dumps_.empty()) {
                fail_json += ",\"log\":[";
                for (size_t li = 0; li < log_dumps_.size(); li++) {
                    if (li > 0) fail_json += ",";
                    fail_json += log_dumps_[li];
                }
                fail_json += "]";
            }
            if (video_.active) {
                fail_json += ",\"screen\":" + renderScreenJson();
            }
            fail_json += "}";
            std::cout << fail_json << std::endl;
            return 1;
        }

        DecodedInstr instr = decode8086(cpu_.memory, cpu_.ip);

        if (instr.op == OpType::INVALID) {
            if (tracing_) {
                fprintf(stderr, "Invalid opcode %02Xh at IP=%04X\n",
                        cpu_.memory[cpu_.ip], cpu_.ip);
                dumpRegs();
            }
            std::string fail = "{\"executed\":\"FAILED\",\"error\":\"invalid opcode 0x";
            {
                std::ostringstream oss;
                oss << std::hex << (int)cpu_.memory[cpu_.ip]
                    << " at IP=0x" << cpu_.ip << "\"";
                fail += oss.str();
            }
            if (video_.active) fail += ",\"screen\":" + renderScreenJson();
            fail += "}";
            std::cout << fail << std::endl;
            return 1;
        }

        // Directive state machine (only in TRACE mode)
        if (mode == RunMode::TRACE) {
            uint16_t ip = cpu_.ip;
            if (trace_stop_addrs_.count(ip)) tracing_ = false;
            if (trace_start_addrs_.count(ip)) tracing_ = true;

            // Standalone VRAMOUT snapshot check (before breakpoints so dumps accumulate)
            auto vo_it = vramout_addr_map_.find(ip);
            if (vo_it != vramout_addr_map_.end() && video_.active) {
                for (size_t vi : vo_it->second) {
                    if (vram_dumps_.size() < MAX_VRAM_DUMPS) {
                        auto& vo = vramouts_[vi];
                        std::string snap = "{\"addr\":" + std::to_string(ip)
                            + ",\"instr\":" + std::to_string(cpu_.instr_count)
                            + ",\"screen\":" + renderScreenJson(vo.params) + "}";
                        vram_dumps_.push_back(snap);
                    }
                }
            }

            // Standalone REGS snapshot check
            auto rg_it = regs_addr_map_.find(ip);
            if (rg_it != regs_addr_map_.end()) {
                for (size_t ri : rg_it->second) {
                    if (reg_dumps_.size() < MAX_REG_DUMPS) {
                        std::string snap = "{\"addr\":" + std::to_string(ip)
                            + ",\"instr\":" + std::to_string(cpu_.instr_count)
                            + ",\"regs\":" + dumpRegsJson() + "}";
                        reg_dumps_.push_back(snap);
                    }
                }
            }

            // LOG / LOG_ONCE check
            auto log_it = log_addr_map_.find(ip);
            if (log_it != log_addr_map_.end()) {
                for (size_t li : log_it->second) {
                    auto& lg = log_entries_[li];
                    // LOG_ONCE: skip if already fired
                    if (!lg.once_label.empty()) {
                        if (log_once_fired_.count(lg.once_label)) continue;
                        log_once_fired_.insert(lg.once_label);
                    }
                    if (log_dumps_.size() < MAX_LOG_DUMPS) {
                        std::string entry = "{\"addr\":" + std::to_string(ip)
                            + ",\"instr\":" + std::to_string(cpu_.instr_count)
                            + ",\"message\":\"";
                        // JSON-escape the message inline
                        for (char c : lg.message) {
                            switch (c) {
                                case '"':  entry += "\\\""; break;
                                case '\\': entry += "\\\\"; break;
                                case '\n': entry += "\\n"; break;
                                case '\t': entry += "\\t"; break;
                                default:   entry += c; break;
                            }
                        }
                        entry += "\"";
                        if (lg.check_kind != DbgLog::CHECK_NONE) {
                            int64_t val = 0;
                            switch (lg.check_kind) {
                                case DbgLog::CHECK_REG:
                                    val = cpu_.regs[lg.reg_index];
                                    entry += ",\"reg\":\"" + lg.reg_name + "\",\"value\":" + std::to_string(val);
                                    break;
                                case DbgLog::CHECK_MEM_BYTE:
                                    val = cpu_.memory[lg.mem_addr];
                                    entry += ",\"mem_addr\":" + std::to_string(lg.mem_addr)
                                           + ",\"size\":\"byte\",\"value\":" + std::to_string(val);
                                    break;
                                case DbgLog::CHECK_MEM_WORD:
                                    val = cpu_.memory[lg.mem_addr] | (cpu_.memory[lg.mem_addr + 1] << 8);
                                    entry += ",\"mem_addr\":" + std::to_string(lg.mem_addr)
                                           + ",\"size\":\"word\",\"value\":" + std::to_string(val);
                                    break;
                                default: break;
                            }
                        }
                        entry += "}";
                        log_dumps_.push_back(entry);
                    }
                }
            }

            auto bp_it = bp_addr_map_.find(ip);
            if (bp_it != bp_addr_map_.end()) {
                auto& bp = breakpoints_[bp_it->second];
                if (bp.hits >= bp.count) {
                    std::string bp_json = "{\"executed\":\"BREAKPOINT\",\"addr\":" + std::to_string(ip);
                    if (!bp.name.empty()) {
                        bp_json += ",\"name\":\"" + bp.name + "\"";
                    }
                    bp_json += ",\"instructions\":" + std::to_string(cpu_.instr_count);
                    if (bp.vramout.active && video_.active) {
                        bp_json += ",\"screen\":" + renderScreenJson(bp.vramout);
                    }
                    if (bp.regs) {
                        bp_json += ",\"regs\":" + dumpRegsJson();
                    }
                    if (!vram_dumps_.empty()) {
                        bp_json += ",\"vram_dumps\":[";
                        for (size_t vi = 0; vi < vram_dumps_.size(); vi++) {
                            if (vi > 0) bp_json += ",";
                            bp_json += vram_dumps_[vi];
                        }
                        bp_json += "]";
                    }
                    if (!reg_dumps_.empty()) {
                        bp_json += ",\"reg_dumps\":[";
                        for (size_t ri = 0; ri < reg_dumps_.size(); ri++) {
                            if (ri > 0) bp_json += ",";
                            bp_json += reg_dumps_[ri];
                        }
                        bp_json += "]";
                    }
                    if (!log_dumps_.empty()) {
                        bp_json += ",\"log\":[";
                        for (size_t li = 0; li < log_dumps_.size(); li++) {
                            if (li > 0) bp_json += ",";
                            bp_json += log_dumps_[li];
                        }
                        bp_json += "]";
                    }
                    bp_json += "}";
                    std::cout << bp_json << std::endl;
                    return 0;
                }
                bp.hits++;
            }

            // Runtime ASSERT_EQ checks
            auto aeq_it = assert_addr_map_.find(ip);
            if (aeq_it != assert_addr_map_.end()) {
                for (size_t ai : aeq_it->second) {
                    auto& a = asserts_[ai];
                    int64_t actual = 0;
                    std::string assert_desc;

                    switch (a.check_kind) {
                    case DbgAssertEq::CHECK_REG:
                        actual = cpu_.regs[a.reg_index];
                        assert_desc = a.reg_name + " == " + std::to_string(a.expected);
                        break;
                    case DbgAssertEq::CHECK_MEM_BYTE:
                        actual = cpu_.memory[a.mem_addr];
                        assert_desc = "BYTE [" + std::to_string(a.mem_addr) + "] == " + std::to_string(a.expected);
                        break;
                    case DbgAssertEq::CHECK_MEM_WORD:
                        actual = cpu_.memory[a.mem_addr] | (cpu_.memory[a.mem_addr + 1] << 8);
                        assert_desc = "WORD [" + std::to_string(a.mem_addr) + "] == " + std::to_string(a.expected);
                        break;
                    }

                    int64_t expected_masked = a.expected;
                    if (a.check_kind == DbgAssertEq::CHECK_MEM_BYTE)
                        expected_masked &= 0xFF;
                    else
                        expected_masked &= 0xFFFF;

                    if (actual != expected_masked) {
                        fprintf(stderr, "ASSERT_EQ FAILED at %04X: %s (actual=%lld)\n",
                                ip, assert_desc.c_str(), (long long)actual);
                        dumpRegs();
                        // Dump 4 rows of memory around relevant address
                        uint16_t dump_base = (a.check_kind == DbgAssertEq::CHECK_REG)
                                             ? ip : a.mem_addr;
                        dump_base &= 0xFFF0; // align to 16
                        for (int row = 0; row < 4; row++) {
                            uint16_t raddr = dump_base + row * 16;
                            fprintf(stderr, "%04X:", raddr);
                            for (int col = 0; col < 16; col++) {
                                fprintf(stderr, " %02X", cpu_.memory[(raddr + col) & 0xFFFF]);
                            }
                            fprintf(stderr, "\n");
                        }
                        std::string af_json = "{\"executed\":\"ASSERT_FAILED\",\"addr\":" + std::to_string(ip)
                            + ",\"assert\":\"" + assert_desc
                            + "\",\"actual\":" + std::to_string(actual)
                            + ",\"expected\":" + std::to_string(expected_masked)
                            + ",\"instructions\":" + std::to_string(cpu_.instr_count);
                        if (a.regs) {
                            af_json += ",\"regs\":" + dumpRegsJson();
                        }
                        if (a.vramout.active && video_.active) {
                            af_json += ",\"screen\":" + renderScreenJson(a.vramout);
                        }
                        if (!vram_dumps_.empty()) {
                            af_json += ",\"vram_dumps\":[";
                            for (size_t vi = 0; vi < vram_dumps_.size(); vi++) {
                                if (vi > 0) af_json += ",";
                                af_json += vram_dumps_[vi];
                            }
                            af_json += "]";
                        }
                        if (!reg_dumps_.empty()) {
                            af_json += ",\"reg_dumps\":[";
                            for (size_t ri = 0; ri < reg_dumps_.size(); ri++) {
                                if (ri > 0) af_json += ",";
                                af_json += reg_dumps_[ri];
                            }
                            af_json += "]";
                        }
                        if (!log_dumps_.empty()) {
                            af_json += ",\"log\":[";
                            for (size_t li = 0; li < log_dumps_.size(); li++) {
                                if (li > 0) af_json += ",";
                                af_json += log_dumps_[li];
                            }
                            af_json += "]";
                        }
                        af_json += "}";
                        std::cout << af_json << std::endl;
                        return 1;
                    }
                }
            }

        }

        if (tracing_) {
            dumpInstr(instr);
        }

        // Handle REP prefix in the dispatch loop
        if (instr.has_rep) {
            uint16_t nextIP = cpu_.ip + instr.len;
            while (cpu_.regs[R_CX] != 0) {
                cpu_.regs[R_CX]--;
                code_.reset();
                if (!emitInstruction(instr)) {
                    std::cout << "{\"executed\":\"FAILED\",\"error\":\"emit failed\"}" << std::endl;
                    return 1;
                }
                auto fn = code_.getFunc<void(*)(CPU8086*)>();
                fn(&cpu_);
                cpu_.instr_count++;

                if (instr.op == OpType::CMPSB || instr.op == OpType::CMPSW ||
                    instr.op == OpType::SCASB || instr.op == OpType::SCASW) {
                    bool zf = (cpu_.flags & F_ZF) != 0;
                    if (instr.rep_z && !zf) break;
                    if (!instr.rep_z && zf) break;
                }
            }
            cpu_.ip = nextIP;
        } else {
            code_.reset();
            if (!emitInstruction(instr)) {
                if (tracing_) {
                    fprintf(stderr, "Failed to emit x64 for %s at IP=%04X\n",
                            opTypeName(instr.op), cpu_.ip);
                }
                std::cout << "{\"executed\":\"FAILED\",\"error\":\"emit failed for "
                          << opTypeName(instr.op) << "\"}" << std::endl;
                return 1;
            }

            auto fn = code_.getFunc<void(*)(CPU8086*)>();
            fn(&cpu_);
            cpu_.instr_count++;

            if (cpu_.pending_int != -1) {
                int marker = cpu_.pending_int;
                cpu_.pending_int = -1;

                if (marker >= 0) {
                    // DOS/BIOS interrupt
                    if (!handleDOSInt(cpu_, marker, dos_output_, dos_state_, video_, has_events_ ? &kbd_ : nullptr, &mouse_)) {
                        if (marker == 0x16) {
                            uint8_t ah = (cpu_.regs[R_AX] >> 8) & 0xFF;
                            if (ah == 0x00) {
                                // Blocking read with no keys — terminate
                                std::string fail = "{\"executed\":\"FAILED\",\"error\":\"INT 16h AH=00: blocking read with no keys available\"";
                                if (has_events_) fail += std::string(",\"detail\":\"read #") + std::to_string(kbd_.readCount()) + "\"";
                                if (video_.active) fail += ",\"screen\":" + renderScreenJson();
                                fail += "}";
                                std::cout << fail << std::endl;
                                return 1;
                            }
                        }
                        if (tracing_) {
                            fprintf(stderr, "Unhandled INT %02Xh at IP=%04X\n", marker, cpu_.ip);
                        }
                    }

                    // Idle detection: track keyboard polls returning "no key"
                    // Covers INT 16h AH=01h (BIOS poll) and INT 21h AH=06h DL=FFh (DOS poll)
                    bool is_idle_poll = false;
                    if (marker == 0x16) {
                        uint8_t ah16 = (cpu_.regs[R_AX] >> 8) & 0xFF;
                        if (ah16 == 0x01 && (cpu_.flags & F_ZF))
                            is_idle_poll = true;
                        else if (ah16 == 0x00)
                            idle_polls_ = 0;  // blocking read consumed a key
                    } else if (marker == 0x21) {
                        uint8_t ah21 = (cpu_.regs[R_AX] >> 8) & 0xFF;
                        if (ah21 == 0x06 && (cpu_.flags & F_ZF))
                            is_idle_poll = true;
                        else if (ah21 == 0x01 || ah21 == 0x08)
                            idle_polls_ = 0;  // blocking read
                    }
                    if (is_idle_poll) {
                        idle_polls_++;
                        if (idle_polls_ >= IDLE_THRESHOLD) {
                            std::string json = "{\"executed\":\"IDLE\",\"instructions\":"
                                + std::to_string(cpu_.instr_count)
                                + ",\"idle_polls\":" + std::to_string(idle_polls_);
                            if (!vram_dumps_.empty()) {
                                json += ",\"vram_dumps\":[";
                                for (size_t vi = 0; vi < vram_dumps_.size(); vi++) {
                                    if (vi > 0) json += ",";
                                    json += vram_dumps_[vi];
                                }
                                json += "]";
                            }
                            if (!reg_dumps_.empty()) {
                                json += ",\"reg_dumps\":[";
                                for (size_t ri = 0; ri < reg_dumps_.size(); ri++) {
                                    if (ri > 0) json += ",";
                                    json += reg_dumps_[ri];
                                }
                                json += "]";
                            }
                            if (!log_dumps_.empty()) {
                                json += ",\"log\":[";
                                for (size_t li = 0; li < log_dumps_.size(); li++) {
                                    if (li > 0) json += ",";
                                    json += log_dumps_[li];
                                }
                                json += "]";
                            }
                            if (video_.active) json += ",\"screen\":" + renderScreenJson();
                            json += "}";
                            std::cout << json << std::endl;
                            return 0;  // success — program reached stable idle state
                        }
                    }
                } else {
                    // BCD operations handled in C++
                    uint8_t al = cpu_.regs[R_AX] & 0xFF;
                    uint8_t ah = (cpu_.regs[R_AX] >> 8) & 0xFF;
                    uint16_t flags = cpu_.flags;
                    bool cf = (flags & F_CF) != 0;
                    bool af = (flags & F_AF) != 0;

                    switch (marker) {
                    case -2: { // DAA
                        uint8_t old_al = al;
                        bool old_cf = cf;
                        cf = false;
                        if ((al & 0x0F) > 9 || af) {
                            uint16_t t = al + 6;
                            al = t & 0xFF;
                            cf = old_cf || (t > 0xFF);
                            af = true;
                        } else { af = false; }
                        if (old_al > 0x99 || old_cf) {
                            al += 0x60;
                            cf = true;
                        }
                        cpu_.regs[R_AX] = (cpu_.regs[R_AX] & 0xFF00) | al;
                        flags = (flags & ~(F_CF|F_AF|F_ZF|F_SF|F_PF)) |
                                (cf ? F_CF : 0) | (af ? F_AF : 0) |
                                (al == 0 ? F_ZF : 0) | ((al & 0x80) ? F_SF : 0) |
                                (__popcnt(al & 0xFF) % 2 == 0 ? F_PF : 0);
                        cpu_.flags = flags;
                        break;
                    }
                    case -3: { // DAS
                        uint8_t old_al = al;
                        bool old_cf = cf;
                        cf = false;
                        if ((al & 0x0F) > 9 || af) {
                            uint16_t t = al - 6;
                            al = t & 0xFF;
                            cf = old_cf || (t > 0xFF);
                            af = true;
                        } else { af = false; }
                        if (old_al > 0x99 || old_cf) {
                            al -= 0x60;
                            cf = true;
                        }
                        cpu_.regs[R_AX] = (cpu_.regs[R_AX] & 0xFF00) | al;
                        flags = (flags & ~(F_CF|F_AF|F_ZF|F_SF|F_PF)) |
                                (cf ? F_CF : 0) | (af ? F_AF : 0) |
                                (al == 0 ? F_ZF : 0) | ((al & 0x80) ? F_SF : 0) |
                                (__popcnt(al & 0xFF) % 2 == 0 ? F_PF : 0);
                        cpu_.flags = flags;
                        break;
                    }
                    case -4: { // AAA
                        if ((al & 0x0F) > 9 || af) {
                            al = (al + 6) & 0x0F;
                            ah++;
                            af = true; cf = true;
                        } else {
                            al &= 0x0F;
                            af = false; cf = false;
                        }
                        cpu_.regs[R_AX] = (ah << 8) | al;
                        cpu_.flags = (flags & ~(F_CF|F_AF)) | (cf?F_CF:0) | (af?F_AF:0);
                        break;
                    }
                    case -5: { // AAS
                        if ((al & 0x0F) > 9 || af) {
                            al = (al - 6) & 0x0F;
                            ah--;
                            af = true; cf = true;
                        } else {
                            al &= 0x0F;
                            af = false; cf = false;
                        }
                        cpu_.regs[R_AX] = (ah << 8) | al;
                        cpu_.flags = (flags & ~(F_CF|F_AF)) | (cf?F_CF:0) | (af?F_AF:0);
                        break;
                    }
                    case -6: { // AAM (base 10)
                        ah = al / 10;
                        al = al % 10;
                        cpu_.regs[R_AX] = (ah << 8) | al;
                        flags = (flags & ~(F_ZF|F_SF|F_PF)) |
                                (al == 0 ? F_ZF : 0) | ((al & 0x80) ? F_SF : 0) |
                                (__popcnt(al & 0xFF) % 2 == 0 ? F_PF : 0);
                        cpu_.flags = flags;
                        break;
                    }
                    case -7: { // AAD (base 10)
                        al = ah * 10 + al;
                        ah = 0;
                        cpu_.regs[R_AX] = (ah << 8) | al;
                        flags = (flags & ~(F_ZF|F_SF|F_PF)) |
                                (al == 0 ? F_ZF : 0) | ((al & 0x80) ? F_SF : 0) |
                                (__popcnt(al & 0xFF) % 2 == 0 ? F_PF : 0);
                        cpu_.flags = flags;
                        break;
                    }
                    }
                }
            }
        }

        if (tracing_) {
            dumpRegs();
        }
    }

    if (!dos_output_.empty()) {
        fprintf(stderr, "%s", dos_output_.c_str());
    }

    std::cout << "{\"executed\":\"OK\",\"instructions\":"
              << cpu_.instr_count;
    if (!vram_dumps_.empty()) {
        std::cout << ",\"vram_dumps\":[";
        for (size_t vi = 0; vi < vram_dumps_.size(); vi++) {
            if (vi > 0) std::cout << ",";
            std::cout << vram_dumps_[vi];
        }
        std::cout << "]";
    }
    if (!reg_dumps_.empty()) {
        std::cout << ",\"reg_dumps\":[";
        for (size_t ri = 0; ri < reg_dumps_.size(); ri++) {
            if (ri > 0) std::cout << ",";
            std::cout << reg_dumps_[ri];
        }
        std::cout << "]";
    }
    if (!log_dumps_.empty()) {
        std::cout << ",\"log\":[";
        for (size_t li = 0; li < log_dumps_.size(); li++) {
            if (li > 0) std::cout << ",";
            std::cout << log_dumps_[li];
        }
        std::cout << "]";
    }
    if (video_.active) {
        std::cout << ",\"screen\":" << renderScreenJson();
    }
    std::cout << "}" << std::endl;

    return 0;
}

// =====================================================================
// Debug info loading
// =====================================================================

void JitEngine::loadDebugInfo(const std::string& dbg_path) {
    std::ifstream ifs(dbg_path);
    if (!ifs) return; // graceful degradation

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Hand-rolled JSON parser for source_map array
    // Find "source_map":[ ... ]
    size_t pos = content.find("\"source_map\"");
    if (pos == std::string::npos) return;
    pos = content.find('[', pos);
    if (pos == std::string::npos) return;
    pos++; // skip '['

    // Parse each entry: {"addr":N,"file":"...","line":N,"source":"..."}
    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == ']') break;
        if (content[pos] != '{') break;
        pos++; // skip '{'

        SourceLine sl = {};
        // Parse key-value pairs
        while (pos < content.size() && content[pos] != '}') {
            // Skip whitespace/commas
            while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
                   content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
                pos++;
            if (pos >= content.size() || content[pos] == '}') break;

            // Parse key (quoted string)
            if (content[pos] != '"') break;
            pos++;
            size_t key_start = pos;
            while (pos < content.size() && content[pos] != '"') pos++;
            std::string key = content.substr(key_start, pos - key_start);
            pos++; // skip closing quote

            // Skip ':'
            while (pos < content.size() && content[pos] != ':') pos++;
            pos++;

            // Skip whitespace
            while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

            if (key == "addr" || key == "line") {
                // Parse number
                size_t num_start = pos;
                while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') pos++;
                int val = std::atoi(content.substr(num_start, pos - num_start).c_str());
                if (key == "addr") sl.addr = (uint16_t)val;
                else sl.line = val;
            } else {
                // Parse string value
                if (pos >= content.size() || content[pos] != '"') break;
                pos++; // skip opening quote
                std::string val;
                while (pos < content.size() && content[pos] != '"') {
                    if (content[pos] == '\\' && pos + 1 < content.size()) {
                        pos++;
                        switch (content[pos]) {
                            case '"': val += '"'; break;
                            case '\\': val += '\\'; break;
                            case 'n': val += '\n'; break;
                            case 't': val += '\t'; break;
                            default: val += content[pos]; break;
                        }
                    } else {
                        val += content[pos];
                    }
                    pos++;
                }
                pos++; // skip closing quote
                if (key == "file") sl.file = val;
                else if (key == "source") sl.source = val;
            }
        }
        if (pos < content.size() && content[pos] == '}') pos++;
        source_map_.push_back(sl);
    }

    // Sort by addr for binary search
    std::sort(source_map_.begin(), source_map_.end(),
              [](const SourceLine& a, const SourceLine& b) { return a.addr < b.addr; });

    // Parse symbols for reverse lookup
    pos = content.find("\"symbols\"");
    if (pos != std::string::npos) {
        pos = content.find('{', pos + 9);
        if (pos != std::string::npos) {
            pos++; // skip '{'
            while (pos < content.size() && content[pos] != '}') {
                while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
                       content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
                    pos++;
                if (pos >= content.size() || content[pos] == '}') break;
                if (content[pos] != '"') break;
                pos++;
                size_t name_start = pos;
                while (pos < content.size() && content[pos] != '"') pos++;
                std::string name = content.substr(name_start, pos - name_start);
                pos++; // skip closing quote

                // Find addr value in the nested object
                size_t obj_start = content.find('{', pos);
                size_t obj_end = content.find('}', obj_start);
                if (obj_start == std::string::npos || obj_end == std::string::npos) break;
                std::string obj = content.substr(obj_start, obj_end - obj_start + 1);
                size_t addr_pos = obj.find("\"addr\":");
                if (addr_pos != std::string::npos) {
                    addr_pos += 7;
                    int addr = std::atoi(obj.c_str() + addr_pos);
                    // Only store labels (not EQU)
                    if (obj.find("\"label\"") != std::string::npos) {
                        addr_to_symbol_[(uint16_t)addr] = name;
                    }
                }
                pos = obj_end + 1;
            }
        }
    }

    // Parse directives array
    pos = content.find("\"directives\"");
    if (pos != std::string::npos) {
        pos = content.find('[', pos);
        if (pos != std::string::npos) {
            pos++; // skip '['
            while (pos < content.size()) {
                // Skip whitespace and commas
                while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
                       content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
                    pos++;
                if (pos >= content.size() || content[pos] == ']') break;
                if (content[pos] != '{') break;
                pos++; // skip '{'

                std::string dtype;
                uint16_t daddr = 0;
                uint32_t dcount = 0;
                std::string dname;
                // ASSERT_EQ fields
                std::string dcheck, dreg_name;
                int dreg_index = -1;
                uint16_t dmem_addr = 0;
                int64_t dexpected = 0;
                // VRAMOUT fields
                JitVramOutParams dvramout;
                // REGS flag
                bool dregs = false;
                // LOG fields
                std::string dmessage;
                std::string donce_label;

                while (pos < content.size() && content[pos] != '}') {
                    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
                           content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
                        pos++;
                    if (pos >= content.size() || content[pos] == '}') break;

                    if (content[pos] != '"') break;
                    pos++;
                    size_t key_start = pos;
                    while (pos < content.size() && content[pos] != '"') pos++;
                    std::string key = content.substr(key_start, pos - key_start);
                    pos++; // skip closing quote

                    while (pos < content.size() && content[pos] != ':') pos++;
                    pos++;
                    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

                    if (key == "vramout") {
                        // Parse nested vramout object: {"full":bool,"attrs":bool[,"x":N,"y":N,"w":N,"h":N]}
                        if (pos < content.size() && content[pos] == '{') {
                            pos++; // skip '{'
                            dvramout.active = true;
                            while (pos < content.size() && content[pos] != '}') {
                                while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
                                       content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
                                    pos++;
                                if (pos >= content.size() || content[pos] == '}') break;
                                if (content[pos] != '"') break;
                                pos++;
                                size_t vk_start = pos;
                                while (pos < content.size() && content[pos] != '"') pos++;
                                std::string vkey = content.substr(vk_start, pos - vk_start);
                                pos++; // skip closing quote
                                while (pos < content.size() && content[pos] != ':') pos++;
                                pos++;
                                while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
                                if (vkey == "full" || vkey == "attrs") {
                                    // Parse boolean: true/false
                                    bool bval = false;
                                    if (pos < content.size() && content[pos] == 't') {
                                        bval = true; pos += 4; // skip "true"
                                    } else if (pos < content.size() && content[pos] == 'f') {
                                        bval = false; pos += 5; // skip "false"
                                    }
                                    if (vkey == "full") dvramout.full = bval;
                                    else dvramout.attrs = bval;
                                } else {
                                    // numeric: x, y, w, h
                                    bool neg = false;
                                    if (pos < content.size() && content[pos] == '-') { neg = true; pos++; }
                                    size_t ns = pos;
                                    while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') pos++;
                                    int vv = std::atoi(content.substr(ns, pos - ns).c_str());
                                    if (neg) vv = -vv;
                                    if (vkey == "x") dvramout.x = vv;
                                    else if (vkey == "y") dvramout.y = vv;
                                    else if (vkey == "w") dvramout.w = vv;
                                    else if (vkey == "h") dvramout.h = vv;
                                }
                            }
                            if (pos < content.size() && content[pos] == '}') pos++; // skip nested '}'
                        }
                    } else if (key == "regs") {
                        // Parse boolean: true/false
                        if (pos < content.size() && content[pos] == 't') {
                            dregs = true; pos += 4; // skip "true"
                        } else if (pos < content.size() && content[pos] == 'f') {
                            dregs = false; pos += 5; // skip "false"
                        }
                    } else if (key == "type" || key == "name" || key == "label" || key == "check" || key == "reg" ||
                               key == "message" || key == "once_label") {
                        if (pos >= content.size() || content[pos] != '"') break;
                        pos++;
                        std::string val;
                        while (pos < content.size() && content[pos] != '"') {
                            if (content[pos] == '\\' && pos + 1 < content.size()) {
                                pos++;
                                val += content[pos];
                            } else {
                                val += content[pos];
                            }
                            pos++;
                        }
                        pos++; // skip closing quote
                        if (key == "type") dtype = val;
                        else if (key == "name" || key == "label") dname = val;
                        else if (key == "check") dcheck = val;
                        else if (key == "reg") dreg_name = val;
                        else if (key == "message") dmessage = val;
                        else if (key == "once_label") donce_label = val;
                    } else {
                        // numeric: addr, count, reg_index, mem_addr, expected
                        // Support negative numbers
                        bool neg = false;
                        if (pos < content.size() && content[pos] == '-') {
                            neg = true;
                            pos++;
                        }
                        size_t num_start = pos;
                        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') pos++;
                        int64_t val = std::atoll(content.substr(num_start, pos - num_start).c_str());
                        if (neg) val = -val;
                        if (key == "addr") daddr = (uint16_t)val;
                        else if (key == "count") dcount = (uint32_t)val;
                        else if (key == "reg_index") dreg_index = (int)val;
                        else if (key == "mem_addr") dmem_addr = (uint16_t)val;
                        else if (key == "expected") dexpected = val;
                    }
                }
                if (pos < content.size() && content[pos] == '}') pos++;

                if (dtype == "trace_start") {
                    trace_start_addrs_.insert(daddr);
                } else if (dtype == "trace_stop") {
                    trace_stop_addrs_.insert(daddr);
                } else if (dtype == "breakpoint") {
                    size_t idx = breakpoints_.size();
                    DbgBreakpoint bp;
                    bp.addr = daddr;
                    bp.count = dcount;
                    bp.hits = 0;
                    bp.name = dname;
                    bp.vramout = dvramout;
                    bp.regs = dregs;
                    breakpoints_.push_back(bp);
                    bp_addr_map_[daddr] = idx;
                } else if (dtype == "assert_eq") {
                    DbgAssertEq a;
                    a.addr = daddr;
                    a.reg_index = dreg_index;
                    a.reg_name = dreg_name;
                    a.mem_addr = dmem_addr;
                    a.expected = dexpected;
                    a.vramout = dvramout;
                    a.regs = dregs;
                    if (dcheck == "reg") a.check_kind = DbgAssertEq::CHECK_REG;
                    else if (dcheck == "mem_byte") a.check_kind = DbgAssertEq::CHECK_MEM_BYTE;
                    else if (dcheck == "mem_word") a.check_kind = DbgAssertEq::CHECK_MEM_WORD;
                    else a.check_kind = DbgAssertEq::CHECK_REG; // fallback
                    size_t idx = asserts_.size();
                    asserts_.push_back(a);
                    assert_addr_map_[daddr].push_back(idx);
                } else if (dtype == "vramout") {
                    DbgVramOut vo;
                    vo.addr = daddr;
                    vo.params = dvramout;
                    size_t idx = vramouts_.size();
                    vramouts_.push_back(vo);
                    vramout_addr_map_[daddr].push_back(idx);
                } else if (dtype == "regs") {
                    DbgRegs dr;
                    dr.addr = daddr;
                    size_t idx = regs_snapshots_.size();
                    regs_snapshots_.push_back(dr);
                    regs_addr_map_[daddr].push_back(idx);
                } else if (dtype == "log" || dtype == "log_once") {
                    DbgLog dl;
                    dl.addr = daddr;
                    dl.message = dmessage;
                    dl.once_label = donce_label;
                    if (dcheck == "reg") {
                        dl.check_kind = DbgLog::CHECK_REG;
                        dl.reg_index = dreg_index;
                        dl.reg_name = dreg_name;
                    } else if (dcheck == "mem_byte") {
                        dl.check_kind = DbgLog::CHECK_MEM_BYTE;
                        dl.mem_addr = dmem_addr;
                    } else if (dcheck == "mem_word") {
                        dl.check_kind = DbgLog::CHECK_MEM_WORD;
                        dl.mem_addr = dmem_addr;
                    }
                    size_t idx = log_entries_.size();
                    log_entries_.push_back(dl);
                    log_addr_map_[daddr].push_back(idx);
                }
            }
        }
    }

    // Parse screen mode (directive override, only if CLI didn't set it)
    if (!video_.active) {
        pos = content.find("\"screen\":\"");
        if (pos != std::string::npos) {
            pos += 10; // skip "screen":"
            size_t val_start = pos;
            while (pos < content.size() && content[pos] != '"') pos++;
            std::string mode = content.substr(val_start, pos - val_start);
            if (!mode.empty()) setScreen(mode);
        }
    }
}

const SourceLine* JitEngine::findSourceLine(uint16_t ip) const {
    if (source_map_.empty()) return nullptr;
    // Binary search for exact match
    auto it = std::lower_bound(source_map_.begin(), source_map_.end(), ip,
        [](const SourceLine& sl, uint16_t addr) { return sl.addr < addr; });
    if (it != source_map_.end() && it->addr == ip) return &(*it);
    return nullptr;
}

void JitEngine::dumpRegs() const {
    fprintf(stderr,
        "AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X\n"
        "DS=%04X ES=%04X SS=%04X CS=%04X IP=%04X FL=%04X [%c%c%c%c%c%c%c%c]\n",
        cpu_.regs[R_AX], cpu_.regs[R_BX], cpu_.regs[R_CX], cpu_.regs[R_DX],
        cpu_.regs[R_SP], cpu_.regs[R_BP], cpu_.regs[R_SI], cpu_.regs[R_DI],
        cpu_.sregs[S_DS], cpu_.sregs[S_ES], cpu_.sregs[S_SS], cpu_.sregs[S_CS],
        cpu_.ip, cpu_.flags,
        (cpu_.flags & F_OF) ? 'O' : '-',
        (cpu_.flags & F_DF) ? 'D' : '-',
        (cpu_.flags & F_SF) ? 'S' : '-',
        (cpu_.flags & F_ZF) ? 'Z' : '-',
        (cpu_.flags & F_AF) ? 'A' : '-',
        (cpu_.flags & F_PF) ? 'P' : '-',
        (cpu_.flags & F_CF) ? 'C' : '-',
        (cpu_.flags & F_IF) ? 'I' : '-');
}

std::string JitEngine::dumpRegsJson() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"AX\":%u,\"BX\":%u,\"CX\":%u,\"DX\":%u,"
        "\"SP\":%u,\"BP\":%u,\"SI\":%u,\"DI\":%u,"
        "\"DS\":%u,\"ES\":%u,\"SS\":%u,\"CS\":%u,"
        "\"IP\":%u,\"FL\":%u,\"flags\":\"%c%c%c%c-%c%c%c\"}",
        cpu_.regs[R_AX], cpu_.regs[R_BX], cpu_.regs[R_CX], cpu_.regs[R_DX],
        cpu_.regs[R_SP], cpu_.regs[R_BP], cpu_.regs[R_SI], cpu_.regs[R_DI],
        cpu_.sregs[S_DS], cpu_.sregs[S_ES], cpu_.sregs[S_SS], cpu_.sregs[S_CS],
        cpu_.ip, cpu_.flags,
        (cpu_.flags & F_OF) ? 'O' : '-',
        (cpu_.flags & F_DF) ? 'D' : '-',
        (cpu_.flags & F_SF) ? 'S' : '-',
        (cpu_.flags & F_ZF) ? 'Z' : '-',
        (cpu_.flags & F_AF) ? 'A' : '-',
        (cpu_.flags & F_PF) ? 'P' : '-',
        (cpu_.flags & F_CF) ? 'C' : '-');
    return std::string(buf);
}

void JitEngine::dumpInstr(const DecodedInstr& instr) const {
    const SourceLine* sl = findSourceLine(cpu_.ip);
    if (sl) {
        // Strip directory from path for compact output
        const std::string& path = sl->file;
        size_t sep = path.find_last_of("\\/");
        const char* basename = (sep != std::string::npos) ? path.c_str() + sep + 1 : path.c_str();
        fprintf(stderr, "%s:%d    %s\n", basename, sl->line, sl->source.c_str());
    }
    fprintf(stderr, "%04X: %-6s", cpu_.ip, opTypeName(instr.op));
    fprintf(stderr, " (");
    for (int i = 0; i < instr.len && i < 6; i++) {
        fprintf(stderr, "%02X", cpu_.memory[(cpu_.ip + i) & 0xFFFF]);
        if (i < instr.len - 1) fprintf(stderr, " ");
    }
    fprintf(stderr, ")\n");
}

// =====================================================================
// Emit a single x64 ALU instruction: op x64_dst, x64_src (both in regs)
// aluOp: 0=ADD, 1=OR, 2=ADC, 3=SBB, 4=AND, 5=SUB, 6=XOR, 7=CMP
// =====================================================================

static int aluOpcode(OpType op) {
    switch (op) {
        case OpType::ADD: return 0;
        case OpType::OR:  return 1;
        case OpType::ADC: return 2;
        case OpType::SBB: return 3;
        case OpType::AND: return 4;
        case OpType::SUB: return 5;
        case OpType::XOR: return 6;
        case OpType::CMP: return 7;
        default: return -1;
    }
}

// =====================================================================
// Main instruction emitter
// =====================================================================

bool JitEngine::emitInstruction(const DecodedInstr& instr) {
    uint16_t nextIP = cpu_.ip + instr.len;
    seg_override_ = instr.seg_override;

    emitPrologue();

    switch (instr.op) {
    // =================================================================
    // MOV
    // =================================================================
    case OpType::MOV: {
        // Load src into RAX, store to dst
        emitLoadOperand(RAX, instr.src, instr.is_word);
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // ALU: ADD, ADC, SUB, SBB, AND, OR, XOR, CMP, TEST
    // =================================================================
    case OpType::ADD: case OpType::ADC: case OpType::SUB: case OpType::SBB:
    case OpType::AND: case OpType::OR:  case OpType::XOR: case OpType::CMP: {
        bool needsCF = (instr.op == OpType::ADC || instr.op == OpType::SBB);
        if (needsCF) {
            emitRestoreFlags();
        }

        // Load src first if it's MEM (EA computation clobbers RAX)
        if (instr.src.kind == OpdKind::MEM) {
            emitLoadOperand(RDX, instr.src, instr.is_word);
            emitLoadOperand(RAX, instr.dst, instr.is_word);
        } else {
            emitLoadOperand(RAX, instr.dst, instr.is_word);
            emitLoadOperand(RDX, instr.src, instr.is_word);
        }

        int aluIdx = aluOpcode(instr.op);
        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0x01 + aluIdx * 8);
            code_.emit8(0xC0 | (RDX << 3) | RAX);
        } else {
            code_.emit8(0x00 + aluIdx * 8);
            code_.emit8(0xC0 | (RDX << 3) | RAX);
        }

        // Save ALU result in RBP before captureFlags (which clobbers RAX)
        code_.emit8(0x89); code_.emit8(0xC5); // MOV EBP, EAX

        emitCaptureFlags();

        if (instr.op != OpType::CMP) {
            // Restore result from RBP → store to destination
            code_.emit8(0x89); code_.emit8(0xE8); // MOV EAX, EBP
            emitStoreOperand(instr.dst, RAX, instr.is_word);
        }
        emitSetIP(nextIP);
        break;
    }

    case OpType::TEST: {
        if (instr.src.kind == OpdKind::MEM) {
            emitLoadOperand(RDX, instr.src, instr.is_word);
            emitLoadOperand(RAX, instr.dst, instr.is_word);
        } else {
            emitLoadOperand(RAX, instr.dst, instr.is_word);
            emitLoadOperand(RDX, instr.src, instr.is_word);
        }

        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0x85); // TEST r/m16, r16
            code_.emit8(0xC0 | (RDX << 3) | RAX);
        } else {
            code_.emit8(0x84); // TEST r/m8, r8
            code_.emit8(0xC0 | (RDX << 3) | RAX);
        }

        emitCaptureFlags();
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // INC/DEC — must preserve CF
    // =================================================================
    case OpType::INC: {
        emitLoadOperand(RAX, instr.dst, instr.is_word);
        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0xFF); code_.emit8(0xC0); // INC AX
        } else {
            code_.emit8(0xFE); code_.emit8(0xC0); // INC AL
        }
        code_.emit8(0x89); code_.emit8(0xC5); // save result to EBP
        emitCaptureFlagsPreserveCF();
        code_.emit8(0x89); code_.emit8(0xE8); // restore result from EBP
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    case OpType::DEC: {
        emitLoadOperand(RAX, instr.dst, instr.is_word);
        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0xFF); code_.emit8(0xC8); // DEC AX
        } else {
            code_.emit8(0xFE); code_.emit8(0xC8); // DEC AL
        }
        code_.emit8(0x89); code_.emit8(0xC5); // save result
        emitCaptureFlagsPreserveCF();
        code_.emit8(0x89); code_.emit8(0xE8); // restore result
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // NEG, NOT
    // =================================================================
    case OpType::NEG: {
        emitLoadOperand(RAX, instr.dst, instr.is_word);
        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0xF7); code_.emit8(0xD8); // NEG AX
        } else {
            code_.emit8(0xF6); code_.emit8(0xD8); // NEG AL
        }
        code_.emit8(0x89); code_.emit8(0xC5); // save result
        emitCaptureFlags();
        code_.emit8(0x89); code_.emit8(0xE8); // restore result
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    case OpType::NOT: {
        emitLoadOperand(RAX, instr.dst, instr.is_word);
        if (instr.is_word) {
            code_.emit8(0x66);
            code_.emit8(0xF7); code_.emit8(0xD0); // NOT AX
        } else {
            code_.emit8(0xF6); code_.emit8(0xD0); // NOT AL
        }
        // NOT doesn't affect flags
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // XCHG
    // =================================================================
    case OpType::XCHG: {
        emitLoadOperand(RAX, instr.dst, instr.is_word);
        emitLoadOperand(RDX, instr.src, instr.is_word);
        // Store swapped
        emitStoreOperand(instr.dst, RDX, instr.is_word);
        emitStoreOperand(instr.src, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // LEA
    // =================================================================
    case OpType::LEA: {
        uint8_t saved_seg = seg_override_;
        seg_override_ = SEG_NONE; // LEA computes offset only, no segment
        emitComputeEA(instr.src);
        seg_override_ = saved_seg;
        emitStoreReg16(instr.dst.reg, RAX);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // PUSH
    // =================================================================
    case OpType::PUSH: {
        // Load value to push into RBX (avoid RAX clobber by emitSegAddr)
        emitLoadOperand(RBX, instr.dst, true);
        // Decrement SP by 2
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xEA); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        emitStoreReg16(R_SP, RDX);
        // Compute SS:SP physical address → EAX
        emitSegAddr(S_SS, R_SP);
        // Store: mov word [rcx + rax + OFF_MEMORY], bx
        code_.emit8(0x66);
        code_.emit8(0x89);
        code_.emit8(0x9C); // ModR/M: mod=10, reg=RBX(3), rm=SIB(4)
        code_.emit8(0x01); // SIB: RAX + RCX
        code_.emit32(OFF_MEMORY);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // POP
    // =================================================================
    case OpType::POP: {
        // Compute SS:SP physical address → EAX
        emitSegAddr(S_SS, R_SP);
        // Load word from [SS:SP]: movzx ebx, word [rcx + rax + OFF_MEMORY]
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x9C); // ModR/M: mod=10, reg=RBX(3), rm=SIB(4)
        code_.emit8(0x01); // SIB: RAX + RCX
        code_.emit32(OFF_MEMORY);
        // Increment SP by 2
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        emitStoreReg16(R_SP, RDX);
        // Store popped value (in RBX)
        emitStoreOperand(instr.dst, RBX, true);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // PUSHA
    // =================================================================
    case OpType::PUSHA: {
        // Save original SP in R12
        emitLoadReg16(R12, R_SP);
        // Pre-compute SS*16 → RBP for efficiency
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBP, sregOff(S_SS));  // movzx ebp, word [rcx + sregOff(S_SS)]
        code_.emit8(0xC1); code_.emit8(0xED); code_.emit8(0x04); // shl ebp, 4

        // Push AX, CX, DX, BX, SP(original), BP, SI, DI
        for (int r = 0; r < 8; r++) {
            if (r == R_SP) {
                // Push original SP (in R12) → save in RBX
                code_.emit8(REX_R); // REX.R for R12 source
                code_.emit8(0x89); code_.emit8(0xC0 | ((R12 & 7) << 3) | RBX); // mov ebx, r12d
            } else {
                emitLoadReg16(RBX, r);
            }
            // Decrement SP
            emitLoadReg16(RDX, R_SP);
            code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xEA); code_.emit8(0x02);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
            emitStoreReg16(R_SP, RDX);
            // Compute physical address: EAX = EBP(SS*16) + EDX(new SP)
            code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A); // LEA EAX, [RDX+RBP]
            code_.emit8(0x25); code_.emit32(0x000FFFFF); // AND EAX, 0xFFFFF
            // Store: mov word [rcx + rax + OFF_MEMORY], bx
            code_.emit8(0x66); code_.emit8(0x89);
            code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        }
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // POPA
    // =================================================================
    case OpType::POPA: {
        // Pre-compute SS*16 → RBP for efficiency
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBP, sregOff(S_SS));
        code_.emit8(0xC1); code_.emit8(0xED); code_.emit8(0x04); // shl ebp, 4

        // Pop DI, SI, BP, skip SP, BX, DX, CX, AX
        for (int r = 7; r >= 0; r--) {
            // Compute SS:SP physical address
            emitLoadReg16(RDX, R_SP);
            code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A); // LEA EAX, [RDX+RBP]
            code_.emit8(0x25); code_.emit32(0x000FFFFF);
            // Load from stack: movzx ebx, word [rcx + rax + OFF_MEMORY]
            code_.emit8(0x0F); code_.emit8(0xB7);
            code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
            // Increment SP
            code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
            emitStoreReg16(R_SP, RDX);

            if (r != R_SP) {
                emitStoreReg16(r, RBX);
            }
        }
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // PUSHF / POPF
    // =================================================================
    case OpType::PUSHF: {
        // Load flags into RBX
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBX, OFF_FLAGS);
        // Decrement SP
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xEA); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        emitStoreReg16(R_SP, RDX);
        // Compute SS:SP physical address → EAX
        emitSegAddr(S_SS, R_SP);
        // Store flags: mov word [rcx + rax + OFF_MEMORY], bx
        code_.emit8(0x66); code_.emit8(0x89);
        code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        emitSetIP(nextIP);
        break;
    }

    case OpType::POPF: {
        // Compute SS:SP physical address → EAX
        emitSegAddr(S_SS, R_SP);
        // Load from stack: movzx ebx, word [rcx + rax + OFF_MEMORY]
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Increment SP
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        emitStoreReg16(R_SP, RDX);
        // Store to flags (from RBX)
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RBX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // JMP
    // =================================================================
    case OpType::JMP: {
        if (instr.dst.kind == OpdKind::REL8 || instr.dst.kind == OpdKind::REL16) {
            uint16_t target = nextIP + instr.dst.rel;
            emitSetIP(target);
        } else if (instr.dst.kind == OpdKind::REG16) {
            // JMP reg16 (indirect)
            emitLoadReg16(RAX, instr.dst.reg);
            // mov word [rcx + OFF_IP], ax
            code_.emit8(0x66); code_.emit8(0x89);
            emitModRMDisp(code_, RAX, OFF_IP);
        } else if (instr.dst.kind == OpdKind::MEM) {
            // JMP [mem] (indirect through memory)
            emitComputeEA(instr.dst);
            // movzx eax, word [rcx + rax + OFF_MEMORY]
            code_.emit8(0x0F); code_.emit8(0xB7);
            code_.emit8(0x84); code_.emit8(0x01);
            code_.emit32(OFF_MEMORY);
            code_.emit8(0x66); code_.emit8(0x89);
            emitModRMDisp(code_, RAX, OFF_IP);
        } else if (instr.dst.kind == OpdKind::FAR_PTR) {
            emitSetIP(instr.dst.off);
            // Also set CS
            code_.emit8(0x66); code_.emit8(0xC7);
            emitModRMDisp(code_, 0, sregOff(S_CS));
            code_.emit16(instr.dst.seg);
        } else {
            emitEpilogue();
            return false;
        }
        emitEpilogue();
        return true;
    }

    // =================================================================
    // Jcc (all 16 conditions)
    // =================================================================
    case OpType::JO: case OpType::JNO: case OpType::JB: case OpType::JNB:
    case OpType::JZ: case OpType::JNZ: case OpType::JBE: case OpType::JNBE:
    case OpType::JSS: case OpType::JNS: case OpType::JP: case OpType::JNP:
    case OpType::JL: case OpType::JNL: case OpType::JLE: case OpType::JNLE: {
        uint16_t takenIP = nextIP + instr.dst.rel;

        // Restore 8086 flags to native RFLAGS
        emitRestoreFlags();

        // Emit native Jcc to a label
        // The condition code maps directly: JO=0, JNO=1, JB=2, JNB=3, ...
        static const uint8_t ccMap[] = {
            0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
            0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F
        };
        int ccIdx = (int)instr.op - (int)OpType::JO;
        uint8_t ccOpcode = ccMap[ccIdx];

        // Jcc skip (2-byte relative: jcc +offset)
        // If taken: set IP = takenIP
        // If not taken: set IP = nextIP
        code_.emit8(ccOpcode);
        size_t patchPos = code_.cursor();
        code_.emit8(0); // placeholder for rel8

        // Not taken path:
        emitSetIP(nextIP);
        emitEpilogue();

        // Patch jump target
        size_t afterNotTaken = code_.cursor();
        code_.patch8(patchPos, (uint8_t)(afterNotTaken - patchPos - 1));

        // Taken path:
        emitSetIP(takenIP);
        emitEpilogue();
        return true;
    }

    // =================================================================
    // CALL near
    // =================================================================
    case OpType::CALL: {
        if (instr.dst.kind == OpdKind::REL16) {
            uint16_t target = nextIP + instr.dst.rel;
            // Decrement SP
            emitLoadReg16(RDX, R_SP);
            code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xEA); code_.emit8(0x02);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
            emitStoreReg16(R_SP, RDX);
            // Compute SS:SP → EAX, push return address
            emitSegAddr(S_SS, R_SP);
            // mov word [rcx + rax + OFF_MEMORY], nextIP
            code_.emit8(0x66); code_.emit8(0xC7);
            code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
            code_.emit16(nextIP);
            emitSetIP(target);
        } else if (instr.dst.kind == OpdKind::REG16 || instr.dst.kind == OpdKind::MEM) {
            // Indirect call: compute target first into R12
            if (instr.dst.kind == OpdKind::REG16) {
                emitLoadReg16(R12, instr.dst.reg);
            } else {
                emitComputeEA(instr.dst);
                // Load target: movzx r12d, word [rcx+rax+OFF_MEMORY]
                code_.emit8(REX_R); // REX.R for R12
                code_.emit8(0x0F); code_.emit8(0xB7);
                code_.emit8(0x84 | ((R12 & 7) << 3)); code_.emit8(0x01);
                code_.emit32(OFF_MEMORY);
            }
            // Decrement SP
            emitLoadReg16(RDX, R_SP);
            code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xEA); code_.emit8(0x02);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
            emitStoreReg16(R_SP, RDX);
            // Compute SS:SP → EAX, store return address
            emitSegAddr(S_SS, R_SP);
            code_.emit8(0x66); code_.emit8(0xC7);
            code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
            code_.emit16(nextIP);
            // Set IP to target (in R12)
            code_.emit8(0x66);
            code_.emit8(REX_R); // REX.R for R12
            code_.emit8(0x89);
            emitModRMDisp(code_, R12 & 7, OFF_IP);
        } else {
            emitEpilogue();
            return false;
        }
        emitEpilogue();
        return true;
    }

    // =================================================================
    // RET / RET imm16
    // =================================================================
    case OpType::RET: {
        // Compute SS:SP → EAX, pop return address
        emitSegAddr(S_SS, R_SP);
        // movzx ebx, word [rcx + rax + OFF_MEMORY]
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Set IP from RBX
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RBX, OFF_IP);
        // Increment SP by 2
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        // If RET imm16, add extra to SP
        if (instr.dst.kind == OpdKind::IMM16) {
            code_.emit8(0x66); code_.emit8(0x81); code_.emit8(0xC2);
            code_.emit16((uint16_t)instr.dst.imm);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        }
        emitStoreReg16(R_SP, RDX);
        emitEpilogue();
        return true;
    }

    // =================================================================
    // INT
    // =================================================================
    case OpType::INT: {
        // Store interrupt number to pending_int
        // mov dword [rcx + OFF_PENDING], imm32
        code_.emit8(0xC7);
        emitModRMDisp(code_, 0, OFF_PENDING);
        code_.emit32((uint32_t)instr.dst.imm);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // LOOP / LOOPE / LOOPNE / JCXZ
    // =================================================================
    case OpType::LOOP: {
        uint16_t takenIP = nextIP + instr.dst.rel;
        // Decrement CX
        emitLoadReg16(RAX, R_CX);
        code_.emit8(0x66); code_.emit8(0xFF); code_.emit8(0xC8); // DEC AX
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0); // MOVZX EAX, AX
        emitStoreReg16(R_CX, RAX);
        // Test if CX != 0
        code_.emit8(0x66); code_.emit8(0x85); code_.emit8(0xC0); // TEST AX, AX
        code_.emit8(0x75); // JNZ taken
        size_t patch = code_.cursor();
        code_.emit8(0);
        // Not taken
        emitSetIP(nextIP);
        emitEpilogue();
        // Taken
        code_.patch8(patch, (uint8_t)(code_.cursor() - patch - 1));
        emitSetIP(takenIP);
        emitEpilogue();
        return true;
    }

    case OpType::LOOPE: {
        uint16_t takenIP = nextIP + instr.dst.rel;
        emitLoadReg16(RAX, R_CX);
        code_.emit8(0x66); code_.emit8(0xFF); code_.emit8(0xC8);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
        emitStoreReg16(R_CX, RAX);
        // CX != 0 AND ZF == 1
        code_.emit8(0x66); code_.emit8(0x85); code_.emit8(0xC0);
        code_.emit8(0x74); // JZ (CX==0) → not taken
        size_t patchCxZ = code_.cursor();
        code_.emit8(0);
        // CX != 0, check ZF
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RDX, OFF_FLAGS);
        code_.emit8(0xF6); code_.emit8(0xC2); code_.emit8(0x40); // TEST DL, 0x40 (ZF)
        code_.emit8(0x75); // JNZ → taken (ZF set)
        size_t patchZf = code_.cursor();
        code_.emit8(0);
        // Not taken (CX==0 or ZF==0)
        code_.patch8(patchCxZ, (uint8_t)(code_.cursor() - patchCxZ - 1));
        emitSetIP(nextIP);
        emitEpilogue();
        // Taken
        code_.patch8(patchZf, (uint8_t)(code_.cursor() - patchZf - 1));
        emitSetIP(takenIP);
        emitEpilogue();
        return true;
    }

    case OpType::LOOPNE: {
        uint16_t takenIP = nextIP + instr.dst.rel;
        emitLoadReg16(RAX, R_CX);
        code_.emit8(0x66); code_.emit8(0xFF); code_.emit8(0xC8);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
        emitStoreReg16(R_CX, RAX);
        code_.emit8(0x66); code_.emit8(0x85); code_.emit8(0xC0);
        code_.emit8(0x74); // JZ → not taken (CX==0)
        size_t patchCxZ = code_.cursor();
        code_.emit8(0);
        // CX != 0, check ZF==0
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RDX, OFF_FLAGS);
        code_.emit8(0xF6); code_.emit8(0xC2); code_.emit8(0x40);
        code_.emit8(0x74); // JZ → taken (ZF clear)
        size_t patchZf = code_.cursor();
        code_.emit8(0);
        // Not taken
        code_.patch8(patchCxZ, (uint8_t)(code_.cursor() - patchCxZ - 1));
        emitSetIP(nextIP);
        emitEpilogue();
        // Taken
        code_.patch8(patchZf, (uint8_t)(code_.cursor() - patchZf - 1));
        emitSetIP(takenIP);
        emitEpilogue();
        return true;
    }

    case OpType::JCXZ: {
        uint16_t takenIP = nextIP + instr.dst.rel;
        emitLoadReg16(RAX, R_CX);
        code_.emit8(0x66); code_.emit8(0x85); code_.emit8(0xC0); // TEST AX, AX
        code_.emit8(0x74); // JZ → taken
        size_t patch = code_.cursor();
        code_.emit8(0);
        // Not taken (CX != 0)
        emitSetIP(nextIP);
        emitEpilogue();
        // Taken (CX == 0)
        code_.patch8(patch, (uint8_t)(code_.cursor() - patch - 1));
        emitSetIP(takenIP);
        emitEpilogue();
        return true;
    }

    // =================================================================
    // Shifts and Rotates
    // =================================================================
    case OpType::SHL: case OpType::SHR: case OpType::SAR:
    case OpType::ROL: case OpType::ROR: case OpType::RCL: case OpType::RCR: {
        // For RCL/RCR, restore CF first
        if (instr.op == OpType::RCL || instr.op == OpType::RCR) {
            emitRestoreFlags();
        }

        emitLoadOperand(RAX, instr.dst, instr.is_word);

        // Determine the shift /reg field for x64 encoding
        uint8_t shReg;
        switch (instr.op) {
            case OpType::ROL: shReg = 0; break;
            case OpType::ROR: shReg = 1; break;
            case OpType::RCL: shReg = 2; break;
            case OpType::RCR: shReg = 3; break;
            case OpType::SHL: shReg = 4; break;
            case OpType::SHR: shReg = 5; break;
            case OpType::SAR: shReg = 7; break;
            default: shReg = 4; break;
        }

        if (instr.src.kind == OpdKind::IMM8 && instr.src.imm == 1) {
            // Shift by 1
            if (instr.is_word) {
                code_.emit8(0x66);
                code_.emit8(0xD1);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
            } else {
                code_.emit8(0xD0);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
            }
        } else if (instr.src.kind == OpdKind::IMM8) {
            // Shift by immediate
            if (instr.is_word) {
                code_.emit8(0x66);
                code_.emit8(0xC1);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
                code_.emit8((uint8_t)instr.src.imm);
            } else {
                code_.emit8(0xC0);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
                code_.emit8((uint8_t)instr.src.imm);
            }
        } else {
            // Shift by CL — need to save RCX (our CPU pointer!) to R10
            // mov r10, rcx  (0x89 = MOV r/m, r → R10 in r/m needs REX.B)
            code_.emit8(REX_W | 0x01); // REX.WB
            code_.emit8(0x89); code_.emit8(0xC0 | (RCX << 3) | (R10 & 7)); // mov r10, rcx

            // Load CL from CPU struct (CL = reg 1 8-bit)
            // movzx ecx, byte [r10 + regOff8(1)]
            code_.emit8(0x41); // REX.B for R10 base
            code_.emit8(0x0F); code_.emit8(0xB6);
            // ModR/M: mod=01, reg=RCX(1), rm=R10&7(2)
            int clOff = regOff8(1);
            if (clOff >= -128 && clOff <= 127) {
                code_.emit8(0x40 | (RCX << 3) | (R10 & 7));
                code_.emit8((uint8_t)clOff);
            } else {
                code_.emit8(0x80 | (RCX << 3) | (R10 & 7));
                code_.emit32((uint32_t)clOff);
            }

            // Shift
            if (instr.is_word) {
                code_.emit8(0x66);
                code_.emit8(0xD3);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
            } else {
                code_.emit8(0xD2);
                code_.emit8(0xC0 | (shReg << 3) | RAX);
            }

            // Restore RCX from R10
            code_.emit8(REX_W | 0x04);
            code_.emit8(0x89); code_.emit8(0xC0 | ((R10 & 7) << 3) | RCX); // mov rcx, r10
        }

        code_.emit8(0x89); code_.emit8(0xC5); // save result in EBP
        emitCaptureFlags();
        code_.emit8(0x89); code_.emit8(0xE8); // restore result
        emitStoreOperand(instr.dst, RAX, instr.is_word);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // MUL / IMUL
    // =================================================================
    case OpType::MUL: case OpType::IMUL: {
        bool is_signed = (instr.op == OpType::IMUL);
        emitLoadOperand(RBX, instr.dst, instr.is_word); // operand in RBX

        if (instr.is_word) {
            // DX:AX = AX * operand
            emitLoadReg16(RAX, R_AX);
            if (is_signed) {
                // Need sign-extend AX to EAX for IMUL
                // movsx eax, ax
                code_.emit8(0x0F); code_.emit8(0xBF); code_.emit8(0xC0);
                // movsx ebx, bx
                code_.emit8(0x0F); code_.emit8(0xBF); code_.emit8(0xDB);
                // imul eax, ebx
                code_.emit8(0x0F); code_.emit8(0xAF); code_.emit8(0xC3);
            } else {
                // mul bx — but x64 MUL only works with EAX implicit
                // Use: movzx eax, ax (already done by emitLoadReg16)
                // mul bx → use imul for unsigned since result in EDX:EAX would need MUL r32
                // Actually, just do 32-bit multiply and extract low/high 16 bits
                // imul eax, ebx gives us only low 32 bits in eax, which is fine
                // But we need the full DX:AX. So use MUL:
                // mov edx, 0; mul ebx → EDX:EAX = EAX * EBX (unsigned)
                code_.emit8(0xF7); code_.emit8(0xE3); // MUL EBX
            }
            // AX = low 16 bits, DX = high 16 bits
            if (is_signed) {
                // Result is in EAX (32-bit). Split into AX and DX.
                // mov edx, eax; shr edx, 16; (DX = high word)
                code_.emit8(0x89); code_.emit8(0xC2); // mov edx, eax
                code_.emit8(0xC1); code_.emit8(0xEA); code_.emit8(0x10); // shr edx, 16
            }
            // Store AX (low 16 of result)
            emitStoreReg16(R_AX, RAX);
            emitStoreReg16(R_DX, RDX);
        } else {
            // AX = AL * operand (8-bit)
            emitLoadReg8(RAX, 0); // AL
            if (is_signed) {
                // movsx eax, al
                code_.emit8(0x0F); code_.emit8(0xBE); code_.emit8(0xC0);
                code_.emit8(0x0F); code_.emit8(0xBE); code_.emit8(0xDB); // movsx ebx, bl
                code_.emit8(0x0F); code_.emit8(0xAF); code_.emit8(0xC3); // imul eax, ebx
            } else {
                code_.emit8(0xF6); code_.emit8(0xE3); // MUL BL → AX = AL * BL
            }
            // Store to AX (full 16 bits)
            emitStoreReg16(R_AX, RAX);
        }
        // Set flags: CF=OF=1 if high part nonzero
        emitCaptureFlags();
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // DIV / IDIV
    // =================================================================
    case OpType::DIV: case OpType::IDIV: {
        bool is_signed = (instr.op == OpType::IDIV);
        emitLoadOperand(RBX, instr.dst, instr.is_word); // divisor in RBX

        if (instr.is_word) {
            // DX:AX / operand → AX=quotient, DX=remainder
            emitLoadReg16(RAX, R_AX);
            emitLoadReg16(RDX, R_DX);
            // Combine DX:AX into EAX
            // shl edx, 16; or eax, edx
            code_.emit8(0xC1); code_.emit8(0xE2); code_.emit8(0x10); // SHL EDX, 16
            code_.emit8(0x09); code_.emit8(0xD0); // OR EAX, EDX
            if (is_signed) {
                // CDQ to sign-extend EAX into EDX:EAX
                // But we already have the dividend in EAX as 32-bit value
                // IDIV EBX
                code_.emit8(0x99); // CDQ (sign-extend EAX to EDX:EAX) — wait, we already built DX:AX
                // Actually we need to be more careful: the dividend is the 32-bit DX:AX value
                // We have it in EAX. For signed div, sign-extend to EDX:EAX first
                // But the original value IS DX:AX as a 32-bit number, so just:
                code_.emit8(0xF7); code_.emit8(0xFB); // IDIV EBX
            } else {
                // xor edx, edx; div ebx
                code_.emit8(0x31); code_.emit8(0xD2); // XOR EDX, EDX
                code_.emit8(0xF7); code_.emit8(0xF3); // DIV EBX
            }
            // EAX = quotient, EDX = remainder
            emitStoreReg16(R_AX, RAX);
            emitStoreReg16(R_DX, RDX);
        } else {
            // AX / operand8 → AL=quotient, AH=remainder
            emitLoadReg16(RAX, R_AX); // Full AX is dividend
            if (is_signed) {
                // Sign-extend AX to EAX (already 16 bits)
                code_.emit8(0x0F); code_.emit8(0xBF); code_.emit8(0xC0); // MOVSX EAX, AX
                code_.emit8(0x0F); code_.emit8(0xBE); code_.emit8(0xDB); // MOVSX EBX, BL
                code_.emit8(0x99); // CDQ
                code_.emit8(0xF7); code_.emit8(0xFB); // IDIV EBX
            } else {
                // MOVZX EAX, AX already done
                code_.emit8(0x31); code_.emit8(0xD2);
                code_.emit8(0xF7); code_.emit8(0xF3); // DIV EBX
            }
            // AL = quotient (already in AL of EAX)
            // AH = remainder (in DL for word div, but for byte div...)
            // For byte div: AL=quotient, AH=remainder
            // x64 byte DIV: AL=quotient, AH=remainder (if dividend in AX)
            // For 8-bit: we did 32-bit div, so EAX=quotient, EDX=remainder
            // Need to combine: AL=quotient low byte, AH=remainder low byte
            // shl edx, 8; and eax, 0xFF; or eax, edx
            code_.emit8(0xC1); code_.emit8(0xE2); code_.emit8(0x08); // SHL EDX, 8
            code_.emit8(0x25); code_.emit32(0xFF); // AND EAX, 0xFF
            code_.emit8(0x09); code_.emit8(0xD0); // OR EAX, EDX
            emitStoreReg16(R_AX, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // String operations (single iteration — REP handled in dispatch loop)
    // =================================================================
    case OpType::MOVSB: case OpType::MOVSW: {
        bool isWord = (instr.op == OpType::MOVSW);
        int step = isWord ? 2 : 1;
        // Compute DS:SI (or override:SI) physical address → EAX
        int src_seg = (seg_override_ != 0xFF) ? seg_override_ : S_DS;
        emitLoadReg16(RAX, R_SI);
        emitApplySegment(src_seg);
        // Load byte/word from [rcx + rax + OFF_MEMORY]
        if (isWord) {
            code_.emit8(0x0F); code_.emit8(0xB7);
        } else {
            code_.emit8(0x0F); code_.emit8(0xB6);
        }
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Save loaded value in RBX
        code_.emit8(0x89); code_.emit8(0xC3); // MOV EBX, EAX

        // Compute ES:DI physical address → EAX
        emitLoadReg16(RAX, R_DI);
        emitApplySegment(S_ES);
        // Store RBX to [rcx + rax + OFF_MEMORY]
        if (isWord) {
            code_.emit8(0x66); code_.emit8(0x89);
            code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        } else {
            code_.emit8(0x88);
            code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        }

        // Update SI based on DF
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBX, OFF_FLAGS);
        code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400); // TEST EBX, DF
        emitLoadReg16(RAX, R_SI);
        code_.emit8(0x75); code_.emit8(0x05); // JNZ → subtract
        code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step); // ADD EAX, step
        code_.emit8(0xEB); code_.emit8(0x03); // JMP past sub
        code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step); // SUB EAX, step
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0); // MOVZX EAX, AX
        emitStoreReg16(R_SI, RAX);

        // Update DI based on DF
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBX, OFF_FLAGS);
        code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400);
        emitLoadReg16(RAX, R_DI);
        code_.emit8(0x75); code_.emit8(0x05);
        code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
        code_.emit8(0xEB); code_.emit8(0x03);
        code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
        emitStoreReg16(R_DI, RAX);
        emitSetIP(nextIP);
        break;
    }

    case OpType::STOSB: case OpType::STOSW: {
        bool isWord = (instr.op == OpType::STOSW);
        // Load value to store into RBX
        if (isWord) {
            emitLoadReg16(RBX, R_AX);
        } else {
            emitLoadReg8(RBX, 0); // AL
        }
        // Compute ES:DI physical address → EAX
        emitLoadReg16(RAX, R_DI);
        emitApplySegment(S_ES);
        // Store RBX to [rcx + rax + OFF_MEMORY]
        if (isWord) {
            code_.emit8(0x66); code_.emit8(0x89);
        } else {
            code_.emit8(0x88);
        }
        code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Update DI
        {
            int step = isWord ? 2 : 1;
            code_.emit8(0x0F); code_.emit8(0xB7);
            emitModRMDisp(code_, RBX, OFF_FLAGS);
            code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400); // TEST EBX, DF
            // If DF: DI -= step, else DI += step
            emitLoadReg16(RAX, R_DI);
            code_.emit8(0x75); // JNZ → subtract
            code_.emit8(0x05); // skip add+jmp
            code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
            code_.emit8(0xEB); code_.emit8(0x03); // JMP past sub
            // subtract:
            code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
            emitStoreReg16(R_DI, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    case OpType::LODSB: case OpType::LODSW: {
        bool isWord = (instr.op == OpType::LODSW);
        // Compute DS:SI (or override:SI) physical address → EAX
        int src_seg = (seg_override_ != 0xFF) ? seg_override_ : S_DS;
        emitLoadReg16(RAX, R_SI);
        emitApplySegment(src_seg);
        if (isWord) {
            code_.emit8(0x0F); code_.emit8(0xB7);
        } else {
            code_.emit8(0x0F); code_.emit8(0xB6);
        }
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        if (isWord) {
            emitStoreReg16(R_AX, RAX);
        } else {
            emitStoreReg8(0, RAX); // AL
        }
        // Update SI
        {
            int step = isWord ? 2 : 1;
            code_.emit8(0x0F); code_.emit8(0xB7);
            emitModRMDisp(code_, RBX, OFF_FLAGS);
            code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400);
            emitLoadReg16(RAX, R_SI);
            code_.emit8(0x75); code_.emit8(0x05);
            code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
            code_.emit8(0xEB); code_.emit8(0x03);
            code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
            emitStoreReg16(R_SI, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    case OpType::CMPSB: case OpType::CMPSW: {
        bool isWord = (instr.op == OpType::CMPSW);
        // Load DS:[SI] (or override:SI) into RAX
        int src_seg = (seg_override_ != 0xFF) ? seg_override_ : S_DS;
        emitLoadReg16(RAX, R_SI);
        emitApplySegment(src_seg);
        if (isWord) {
            code_.emit8(0x0F); code_.emit8(0xB7);
        } else {
            code_.emit8(0x0F); code_.emit8(0xB6);
        }
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Save to RBX
        code_.emit8(0x89); code_.emit8(0xC3); // MOV EBX, EAX

        // Load ES:[DI] into RAX
        emitLoadReg16(RAX, R_DI);
        emitApplySegment(S_ES);
        if (isWord) {
            code_.emit8(0x0F); code_.emit8(0xB7);
        } else {
            code_.emit8(0x0F); code_.emit8(0xB6);
        }
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Now compare: [SI] - [DI] = RBX - RAX
        // We want to set flags as if doing SUB [SI], [DI]
        if (isWord) {
            code_.emit8(0x66); code_.emit8(0x29); code_.emit8(0xC3); // SUB BX, AX
        } else {
            code_.emit8(0x28); code_.emit8(0xC3); // SUB BL, AL
        }
        emitCaptureFlags();

        // Update SI and DI
        {
            int step = isWord ? 2 : 1;
            code_.emit8(0x0F); code_.emit8(0xB7);
            emitModRMDisp(code_, RBX, OFF_FLAGS);
            code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400);

            emitLoadReg16(RAX, R_SI);
            code_.emit8(0x75); code_.emit8(0x05);
            code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
            code_.emit8(0xEB); code_.emit8(0x03);
            code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
            emitStoreReg16(R_SI, RAX);

            emitLoadReg16(RAX, R_DI);
            // Re-check DF (it's still in flags)
            code_.emit8(0x0F); code_.emit8(0xB7);
            emitModRMDisp(code_, RBX, OFF_FLAGS);
            code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400);
            code_.emit8(0x75); code_.emit8(0x05);
            code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
            code_.emit8(0xEB); code_.emit8(0x03);
            code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
            emitStoreReg16(R_DI, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    case OpType::SCASB: case OpType::SCASW: {
        bool isWord = (instr.op == OpType::SCASW);
        // Compare AL/AX with ES:[DI]
        if (isWord) {
            emitLoadReg16(RBX, R_AX);
        } else {
            emitLoadReg8(RBX, 0); // AL
        }
        emitLoadReg16(RAX, R_DI);
        emitApplySegment(S_ES);
        if (isWord) {
            code_.emit8(0x0F); code_.emit8(0xB7);
        } else {
            code_.emit8(0x0F); code_.emit8(0xB6);
        }
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // SUB BX/BL, AX/AL → sets flags
        if (isWord) {
            code_.emit8(0x66); code_.emit8(0x29); code_.emit8(0xC3);
        } else {
            code_.emit8(0x28); code_.emit8(0xC3);
        }
        emitCaptureFlags();

        // Update DI
        {
            int step = isWord ? 2 : 1;
            code_.emit8(0x0F); code_.emit8(0xB7);
            emitModRMDisp(code_, RBX, OFF_FLAGS);
            code_.emit8(0xF7); code_.emit8(0xC3); code_.emit32(0x0400);
            emitLoadReg16(RAX, R_DI);
            code_.emit8(0x75); code_.emit8(0x05);
            code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(step);
            code_.emit8(0xEB); code_.emit8(0x03);
            code_.emit8(0x83); code_.emit8(0xE8); code_.emit8(step);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0);
            emitStoreReg16(R_DI, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // Flag operations
    // =================================================================
    case OpType::CLC: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x25); code_.emit32(~(uint32_t)F_CF); // AND EAX, ~CF
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::STC: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x0D); code_.emit32(F_CF); // OR EAX, CF
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::CMC: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x35); code_.emit32(F_CF); // XOR EAX, CF
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::CLD: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x25); code_.emit32(~(uint32_t)F_DF);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::STD: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x0D); code_.emit32(F_DF);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::CLI: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x25); code_.emit32(~(uint32_t)F_IF);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    case OpType::STI: {
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x0D); code_.emit32(F_IF);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // LAHF / SAHF
    // =================================================================
    case OpType::LAHF: {
        // AH = low byte of flags (SF:ZF:0:AF:0:PF:1:CF)
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        // Store AL (flags low byte) to AH position
        emitStoreReg8(4, RAX); // AH = reg8 index 4
        emitSetIP(nextIP);
        break;
    }

    case OpType::SAHF: {
        // flags low byte = AH
        emitLoadReg8(RAX, 4); // AH
        // Merge into flags: preserve high byte of flags, replace low byte
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RDX, OFF_FLAGS);
        code_.emit8(0x81); code_.emit8(0xE2); code_.emit32(0xFF00); // AND EDX, 0xFF00
        code_.emit8(0x09); code_.emit8(0xC2); // OR EDX, EAX
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RDX, OFF_FLAGS);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // CBW / CWD
    // =================================================================
    case OpType::CBW: {
        // AX = sign-extend(AL)
        emitLoadReg8(RAX, 0); // AL
        // movsx eax, al
        code_.emit8(0x0F); code_.emit8(0xBE); code_.emit8(0xC0);
        emitStoreReg16(R_AX, RAX);
        emitSetIP(nextIP);
        break;
    }

    case OpType::CWD: {
        // DX:AX = sign-extend(AX)
        emitLoadReg16(RAX, R_AX);
        // movsx eax, ax
        code_.emit8(0x0F); code_.emit8(0xBF); code_.emit8(0xC0);
        // DX = (AX < 0) ? 0xFFFF : 0x0000
        code_.emit8(0x89); code_.emit8(0xC2); // MOV EDX, EAX
        code_.emit8(0xC1); code_.emit8(0xFA); code_.emit8(0x1F); // SAR EDX, 31
        emitStoreReg16(R_DX, RDX);
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // XLAT
    // =================================================================
    case OpType::XLAT: {
        // AL = DS:[BX + AL] (or override segment)
        emitLoadReg16(RDX, R_BX);
        emitLoadReg8(RAX, 0); // AL
        code_.emit8(0x01); code_.emit8(0xD0); // ADD EAX, EDX
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0); // MOVZX EAX, AX (16-bit offset)
        // Apply segment
        int xlat_seg = (seg_override_ != 0xFF) ? seg_override_ : S_DS;
        emitApplySegment(xlat_seg);
        // Load byte [rcx + rax + OFF_MEMORY]
        code_.emit8(0x0F); code_.emit8(0xB6);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        emitStoreReg8(0, RAX); // AL
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // NOP, HLT, WAIT
    // =================================================================
    case OpType::NOP: {
        emitSetIP(nextIP);
        break;
    }

    case OpType::HLT: {
        // Set halted flag
        // mov byte [rcx + OFF_HALTED], 1
        code_.emit8(0xC6);
        emitModRMDisp(code_, 0, OFF_HALTED);
        code_.emit8(0x01);
        emitSetIP(nextIP);
        break;
    }

    case OpType::WAIT: {
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // IN / OUT — treat as no-ops for .COM programs
    // =================================================================
    case OpType::IN: {
        // Just set result to 0
        if (instr.is_word) {
            code_.emit8(0xB8); code_.emit32(0);
            emitStoreReg16(R_AX, RAX);
        } else {
            code_.emit8(0xB8); code_.emit32(0);
            emitStoreReg8(0, RAX);
        }
        emitSetIP(nextIP);
        break;
    }

    case OpType::OUT: {
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // LDS / LES
    // =================================================================
    case OpType::LDS: case OpType::LES: {
        // Load reg16 from [mem], segment from [mem+2]
        emitComputeEA(instr.src);
        // Load offset (word at EA)
        // movzx ebx, word [rcx + rax + OFF_MEMORY]
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x9C); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Load segment (word at EA+2)
        code_.emit8(0x83); code_.emit8(0xC0); code_.emit8(0x02); // ADD EAX, 2
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xC0); // MOVZX EAX, AX
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        // Store: RAX=segment, RBX=offset
        emitStoreReg16(instr.dst.reg, RBX);
        int sreg = (instr.op == OpType::LDS) ? S_DS : S_ES;
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, sregOff(sreg));
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // BCD: DAA, DAS, AAA, AAS, AAM, AAD
    // These are complex — implement in C++ via pending mechanism
    // But actually easier to just do it directly in the JIT:
    // We'll handle them by calling back to C++ or emitting the logic
    // For now, use a simpler approach: handle in dispatcher
    // =================================================================
    case OpType::DAA: case OpType::DAS: case OpType::AAA: case OpType::AAS:
    case OpType::AAM: case OpType::AAD: {
        // For BCD ops, set pending_int to a special value that the dispatcher recognizes
        // Actually, let's just handle these in C++ after the JIT call
        // We'll store a special marker
        code_.emit8(0xC7);
        emitModRMDisp(code_, 0, OFF_PENDING);
        // Use negative numbers: -2=DAA, -3=DAS, -4=AAA, -5=AAS, -6=AAM, -7=AAD
        int32_t marker;
        switch (instr.op) {
            case OpType::DAA: marker = -2; break;
            case OpType::DAS: marker = -3; break;
            case OpType::AAA: marker = -4; break;
            case OpType::AAS: marker = -5; break;
            case OpType::AAM: marker = -6; break;
            case OpType::AAD: marker = -7; break;
            default: marker = -100; break;
        }
        code_.emit32((uint32_t)marker);
        // For AAM/AAD, store the immediate in R10 temp? No, we handle in dispatcher
        // Store immediate base for AAM/AAD (normally 0x0A)
        if (instr.op == OpType::AAM || instr.op == OpType::AAD) {
            // Store imm in memory at a known scratch location? Or just let C++ read it
            // The immediate is in instr.dst.imm, but C++ dispatcher won't have it...
            // Better: store it in a CPU scratch field. Or: just hardcode base 10.
        }
        emitSetIP(nextIP);
        break;
    }

    // =================================================================
    // IRET
    // =================================================================
    case OpType::IRET: {
        // Pre-compute SS*16 → RBP
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBP, sregOff(S_SS));
        code_.emit8(0xC1); code_.emit8(0xED); code_.emit8(0x04); // shl ebp, 4

        // Pop IP
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A); // LEA EAX, [RDX+RBP]
        code_.emit8(0x25); code_.emit32(0x000FFFFF);
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_IP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        // Pop CS
        code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A);
        code_.emit8(0x25); code_.emit32(0x000FFFFF);
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, sregOff(S_CS));
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        // Pop FLAGS
        code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A);
        code_.emit8(0x25); code_.emit32(0x000FFFFF);
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x06);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        emitStoreReg16(R_SP, RDX);
        emitEpilogue();
        return true;
    }

    case OpType::INTO: {
        // INT 4 if OF is set
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RAX, OFF_FLAGS);
        code_.emit8(0xA9); code_.emit32(F_OF); // TEST EAX, OF
        code_.emit8(0x74); // JZ → skip
        size_t patch = code_.cursor();
        code_.emit8(0);
        // OF set: trigger INT 4
        code_.emit8(0xC7);
        emitModRMDisp(code_, 0, OFF_PENDING);
        code_.emit32(4);
        code_.patch8(patch, (uint8_t)(code_.cursor() - patch - 1));
        emitSetIP(nextIP);
        break;
    }

    case OpType::RETF: {
        // Pre-compute SS*16 → RBP
        code_.emit8(0x0F); code_.emit8(0xB7);
        emitModRMDisp(code_, RBP, sregOff(S_SS));
        code_.emit8(0xC1); code_.emit8(0xED); code_.emit8(0x04);

        // Pop IP
        emitLoadReg16(RDX, R_SP);
        code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A);
        code_.emit8(0x25); code_.emit32(0x000FFFFF);
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, OFF_IP);
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        // Pop CS
        code_.emit8(0x8D); code_.emit8(0x04); code_.emit8(0x2A);
        code_.emit8(0x25); code_.emit32(0x000FFFFF);
        code_.emit8(0x0F); code_.emit8(0xB7);
        code_.emit8(0x84); code_.emit8(0x01); code_.emit32(OFF_MEMORY);
        code_.emit8(0x66); code_.emit8(0x89);
        emitModRMDisp(code_, RAX, sregOff(S_CS));
        code_.emit8(0x66); code_.emit8(0x83); code_.emit8(0xC2); code_.emit8(0x02);
        code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        // RET imm16?
        if (instr.dst.kind == OpdKind::IMM16) {
            code_.emit8(0x66); code_.emit8(0x81); code_.emit8(0xC2);
            code_.emit16((uint16_t)instr.dst.imm);
            code_.emit8(0x0F); code_.emit8(0xB7); code_.emit8(0xD2);
        }
        emitStoreReg16(R_SP, RDX);
        emitEpilogue();
        return true;
    }

    default:
        emitEpilogue();
        return false;
    }

    emitEpilogue();
    return true;
}
