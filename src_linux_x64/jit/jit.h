#pragma once
#include "cpu.h"
#include "decoder.h"
#include "emitter.h"
#include "kbd.h"
#include "dos_state.h"
#include "video.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Sentinel: skip segmentation (for LEA)
static constexpr uint8_t SEG_NONE = 0xFE;

enum class RunMode {
    RUN,    // execute to completion, JSON output
    TRACE   // directive-driven debug (no directives = silent like RUN)
};

struct SourceLine {
    uint16_t addr;
    std::string file;
    int line;
    std::string source;
};

// VRAM output parameters (JIT-side, mirrors asm.h VramOutParams)
struct JitVramOutParams {
    bool active = false;
    bool full = true;
    bool attrs = false;
    int x = 0, y = 0, w = 0, h = 0;
};

struct DbgBreakpoint {
    uint16_t addr;
    uint32_t count;
    uint32_t hits;       // runtime counter
    std::string name;
    JitVramOutParams vramout;
    bool regs = false;
};

struct DbgAssertEq {
    uint16_t addr;
    enum CheckKind { CHECK_REG, CHECK_MEM_BYTE, CHECK_MEM_WORD };
    CheckKind check_kind;
    int reg_index;
    std::string reg_name;
    uint16_t mem_addr;
    int64_t expected;
    JitVramOutParams vramout;
    bool regs = false;
};

struct DbgVramOut {
    uint16_t addr;
    JitVramOutParams params;
};

struct DbgRegs {
    uint16_t addr;
};

struct DbgLog {
    uint16_t addr;
    std::string message;
    enum CheckKind { CHECK_NONE, CHECK_REG, CHECK_MEM_BYTE, CHECK_MEM_WORD };
    CheckKind check_kind = CHECK_NONE;
    int reg_index = -1;
    std::string reg_name;
    uint16_t mem_addr = 0;
    std::string once_label;  // non-empty for LOG_ONCE
};

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    // Load a COM binary and execute it
    // Returns 0 on success, 1 on error
    int run(const uint8_t* comData, size_t comSize, RunMode mode,
            const std::string& dbg_path = "",
            uint64_t max_cycles = 100000000);

    // Access CPU state (for testing)
    const CPU8086& cpu() const { return cpu_; }

    // Set keyboard/mouse events for INT 16h / INT 33h
    void setEvents(std::vector<KeyEvent> triggered, std::vector<InputEvent> sequential);

    // Set screen mode for VRAM rendering
    void setScreen(const std::string& mode);

    // Set command-line arguments (written to PSP at offset 0x80)
    void setArgs(const std::string& args);

private:
    // Emit x64 code for one decoded instruction
    bool emitInstruction(const DecodedInstr& instr);

    // Register/flag dump to stderr
    void dumpRegs() const;
    // Register dump as JSON string (for structured output)
    std::string dumpRegsJson() const;
    void dumpInstr(const DecodedInstr& instr) const;

    // x64 emission helpers
    void emitPrologue();    // save callee-saved, RCX = CPU ptr
    void emitEpilogue();    // restore + ret
    void emitSetIP(uint16_t newIP);

    // Load/store 16-bit register from CPU struct into x64 register
    // x64reg: RAX=0, RCX=1, RDX=2, RBX=3, ...
    void emitLoadReg16(int x64reg, int reg86);
    void emitStoreReg16(int reg86, int x64reg);
    void emitLoadReg8(int x64reg, int reg86);
    void emitStoreReg8(int reg86, int x64reg);

    // Effective address computation → result in RAX (physical 20-bit address)
    void emitComputeEA(const OpdDesc& opd);

    // Segment helpers: add seg*16 to EAX, mask to 20 bits (uses RDX scratch)
    void emitApplySegment(int seg_reg);
    // Compute seg*16 + reg_value → EAX (uses RDX scratch)
    void emitSegAddr(int seg_reg, int offset_reg);

    // Load operand value into specified x64 register
    void emitLoadOperand(int x64reg, const OpdDesc& opd, bool is_word);
    // Store x64 register to operand location
    void emitStoreOperand(const OpdDesc& opd, int x64reg, bool is_word);

    // Capture RFLAGS after ALU op into cpu.flags
    void emitCaptureFlags();
    // Capture flags but preserve CF (for INC/DEC)
    void emitCaptureFlagsPreserveCF();
    // Push cpu.flags onto native stack and popfq (for Jcc)
    void emitRestoreFlags();

    // Debug info
    void loadDebugInfo(const std::string& dbg_path);
    const SourceLine* findSourceLine(uint16_t ip) const;
    std::vector<SourceLine> source_map_;
    std::unordered_map<uint16_t, std::string> addr_to_symbol_;

    std::unique_ptr<CPU8086> cpu_storage_;
    CPU8086&    cpu_;
    CodeBuffer  code_;
    std::string dos_output_;
    DosState    dos_state_;
    VideoState  video_;
    MouseState  mouse_;
    KeyboardBuffer kbd_;
    bool has_events_ = false;
    std::string program_args_;
    uint8_t seg_override_ = 0xFF; // current instruction's segment override

    // Screen rendering
    std::string renderScreenJson(const JitVramOutParams& params = {});

    // Directive-driven trace state
    std::unordered_set<uint16_t> trace_start_addrs_;
    std::unordered_set<uint16_t> trace_stop_addrs_;
    std::vector<DbgBreakpoint> breakpoints_;
    std::unordered_map<uint16_t, size_t> bp_addr_map_;
    std::vector<DbgAssertEq> asserts_;
    std::unordered_map<uint16_t, std::vector<size_t>> assert_addr_map_;
    std::vector<DbgVramOut> vramouts_;
    std::unordered_map<uint16_t, std::vector<size_t>> vramout_addr_map_;
    std::vector<std::string> vram_dumps_;
    static constexpr size_t MAX_VRAM_DUMPS = 32;
    std::vector<DbgRegs> regs_snapshots_;
    std::unordered_map<uint16_t, std::vector<size_t>> regs_addr_map_;
    std::vector<std::string> reg_dumps_;
    static constexpr size_t MAX_REG_DUMPS = 32;
    std::vector<DbgLog> log_entries_;
    std::unordered_map<uint16_t, std::vector<size_t>> log_addr_map_;
    std::unordered_set<std::string> log_once_fired_;
    std::vector<std::string> log_dumps_;
    static constexpr size_t MAX_LOG_DUMPS = 256;
    bool tracing_ = false;

    // Idle detection: consecutive INT 16h AH=01h polls with ZF=1 (no key)
    uint32_t idle_polls_ = 0;
    static constexpr uint32_t IDLE_THRESHOLD = 1000;
};
