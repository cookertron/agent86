#pragma once
#include "types.h"
#include "lexer.h"
#include "symtab.h"
#include "expr.h"
#include "encoder.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <cstdint>

struct SourceOrigin {
    std::string file;
    int line;
};

struct AsmError {
    int line;
    std::string file;
    std::string source;
    std::string message;
};

struct DebugEntry {
    uint16_t addr;
    std::string file;
    int line;
    std::string source;
};

struct VramOutParams {
    bool active = false;
    bool full = true;      // true=full screen, false=partial region
    bool attrs = false;    // include attribute bytes
    int x = 0, y = 0, w = 0, h = 0;  // region (only when !full)
};

struct DebugDirective {
    enum Type { TRACE_START, TRACE_STOP, BREAKPOINT, ASSERT_EQ, VRAMOUT, REGS, LOG, LOG_ONCE, DOS_FAIL, DOS_PARTIAL, MEM_SNAPSHOT, MEM_ASSERT };
    Type type;
    uint16_t addr;
    uint32_t count;      // breakpoint: passes before stop (0 = immediate)
    std::string label;   // breakpoint: optional label name

    // ASSERT_EQ fields
    enum CheckKind { CHECK_NONE, CHECK_REG, CHECK_MEM_BYTE, CHECK_MEM_WORD };
    CheckKind check_kind = CHECK_NONE;
    int reg_index = -1;          // 0=AX..7=DI
    std::string reg_name;        // "AX" etc.
    uint16_t mem_addr = 0;       // for CHECK_MEM_*
    int mem_seg = -1;            // segment override: -1=none, 0=ES, 1=CS, 2=SS, 3=DS
    int64_t expected = 0;

    // VRAMOUT params (used by VRAMOUT, BREAKPOINT:VRAMOUT, ASSERT_EQ:VRAMOUT)
    VramOutParams vramout;

    // REGS modifier (used by BREAKPOINT:REGS, ASSERT_EQ:REGS, standalone REGS)
    bool regs = false;

    // LOG/LOG_ONCE fields (reuses check_kind, reg_index, reg_name, mem_addr for operand)
    std::string message;          // LOG message string
    std::string log_once_label;   // LOG_ONCE dedup label

    // DOS_FAIL/DOS_PARTIAL fields
    uint8_t int_num = 0;          // interrupt number to intercept
    uint8_t ah_func = 0;          // AH subfunction to match
    uint16_t fail_code = 5;       // DOS_FAIL: error code for AX (default: access denied)
    uint16_t partial_count = 0;   // DOS_PARTIAL: byte count to return in AX

    // MEM_SNAPSHOT / MEM_ASSERT fields
    std::string snap_name;       // snapshot identifier (matches SNAPSHOT to ASSERT)
    int snap_seg = -1;           // segment register: 0=ES, 1=CS, 2=SS, 3=DS
    uint16_t snap_offset = 0;    // offset within segment
    uint16_t snap_length = 0;    // number of bytes
};

struct CompilePrint {
    int line;
    std::string file;
    std::string text;
};

struct HexDump {
    uint16_t start_addr;
    uint16_t end_addr;
};

class Assembler {
public:
    bool assemble(const std::string& source, std::vector<uint8_t>& output,
                  const std::string& source_file = "");

    // Accessors for structured output
    const std::vector<AsmError>& errors() const { return errors_; }
    const SymbolTable& symbols() const { return symtab_; }
    const std::vector<DebugEntry>& debugEntries() const { return debug_entries_; }
    const std::vector<DebugDirective>& debugDirectives() const { return debug_directives_; }
    const std::vector<CompilePrint>& prints() const { return prints_; }
    const std::vector<HexDump>& hexDumps() const { return hex_dumps_; }
    const std::string& screenMode() const { return screen_mode_; }
    int64_t origin() const { return origin_; }
    int64_t bssStart() const { return bss_start_; }
    int64_t bssEnd() const { return in_bss_ ? current_addr_ : -1; }

private:
    // Parsing
    ParsedLine parseLine(const std::string& line, int line_num);
    Operand parseOperand(const std::vector<Token>& tokens, size_t& pos, bool pass2);
    Operand parseMemoryOperand(const std::vector<Token>& tokens, size_t& pos, OpSize size_override, bool pass2);

    // Expression evaluation
    ExprResult evalExpr(const std::vector<Token>& tokens, size_t& pos);
    ExprResult evalTokensAsExpr(const std::vector<Token>& tokens, size_t start);

    // Passes
    bool pass1(const std::vector<std::string>& lines);
    bool pass2(const std::vector<std::string>& lines, std::vector<uint8_t>& output);

    // Include expansion
    bool expandIncludes(const std::vector<std::string>& lines,
                        std::vector<std::string>& out_lines,
                        std::vector<SourceOrigin>& out_origins,
                        const std::string& base_dir,
                        const std::string& current_file,
                        std::set<std::string>& seen_files,
                        int depth);

    // Macro expansion (MACRO/ENDM, IRP/ENDM)
    struct MacroDef {
        std::string name;
        std::vector<std::string> params;  // parameter names
        std::vector<std::string> body;    // raw source lines
    };
    std::unordered_map<std::string, MacroDef> macros_;
    bool expandMacros(std::vector<std::string>& lines,
                      std::vector<SourceOrigin>& origins);
    std::vector<std::string> expandIRP(const std::vector<std::string>& body_lines,
                                       const std::string& var,
                                       const std::vector<std::string>& items);
    std::vector<std::string> expandMacroBody(const MacroDef& macro,
                                              const std::vector<std::string>& args);

    // State
    Lexer lexer_;
    SymbolTable symtab_;
    Encoder encoder_;
    int64_t current_addr_ = 0;
    int64_t origin_ = 0;
    bool pass2_ = false;
    std::vector<AsmError> errors_;
    std::vector<SourceOrigin> source_origins_;
    std::string source_file_;
    std::vector<DebugEntry> debug_entries_;
    std::vector<DebugDirective> debug_directives_;
    std::vector<CompilePrint> prints_;
    std::vector<HexDump> hex_dumps_;
    uint16_t hex_start_addr_ = 0;
    bool in_hex_region_ = false;
    std::string screen_mode_;
    std::vector<int> pass1_sizes_;  // per-line estimated sizes from pass 1
    bool directive_pending_ = false; // true when runtime directive at current_addr_ needs NOP before next label
    bool in_bss_ = false;            // true after SECTION .bss
    int64_t bss_start_ = -1;         // address where BSS begins (-1 = no BSS)

    void error(int line, const std::string& source, const std::string& msg);
    void recordDebug(int line_index, const std::string& source_line);

    // Modifier parsing helpers
    struct Modifiers {
        VramOutParams vramout;
        bool regs = false;
    };
    int findModifierColon(const std::vector<Token>& args);
    Modifiers parseModifiers(const std::vector<Token>& args, size_t start,
                             int line, const std::string& source);
    VramOutParams parseVramOutArgs(const std::vector<Token>& args, size_t start,
                                   int line, const std::string& source);

    // Expression diagnostics accumulated between parseLine calls
    std::vector<std::string> expr_diags_;
    void reportExprDiags(int line, const std::string& source);
};
