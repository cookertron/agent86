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
    enum Type { TRACE_START, TRACE_STOP, BREAKPOINT, ASSERT_EQ, VRAMOUT, REGS, LOG, LOG_ONCE };
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
    int64_t expected = 0;

    // VRAMOUT params (used by VRAMOUT, BREAKPOINT:VRAMOUT, ASSERT_EQ:VRAMOUT)
    VramOutParams vramout;

    // REGS modifier (used by BREAKPOINT:REGS, ASSERT_EQ:REGS, standalone REGS)
    bool regs = false;

    // LOG/LOG_ONCE fields (reuses check_kind, reg_index, reg_name, mem_addr for operand)
    std::string message;          // LOG message string
    std::string log_once_label;   // LOG_ONCE dedup label
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
