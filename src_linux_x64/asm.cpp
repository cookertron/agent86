#include "asm.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <climits>

void Assembler::error(int line, const std::string& source, const std::string& msg) {
    std::string file;
    int orig_line = line;
    if (line >= 1 && line <= (int)source_origins_.size()) {
        file = source_origins_[line - 1].file;
        orig_line = source_origins_[line - 1].line;
    }
    errors_.push_back({orig_line, file, source, msg});
    if (!file.empty()) {
        std::cerr << "Error " << file << ":" << orig_line << ": " << msg << std::endl;
    } else {
        std::cerr << "Error line " << orig_line << ": " << msg << std::endl;
    }
}

void Assembler::recordDebug(int line_index, const std::string& source_line) {
    std::string file;
    int line = line_index;
    if (line_index >= 1 && line_index <= (int)source_origins_.size()) {
        file = source_origins_[line_index - 1].file;
        line = source_origins_[line_index - 1].line;
    }
    debug_entries_.push_back({(uint16_t)current_addr_, file, line, source_line});
}

ExprResult Assembler::evalExpr(const std::vector<Token>& tokens, size_t& pos) {
    ExprEvaluator ev(symtab_, symtab_.getScope(), current_addr_);
    auto result = ev.evaluate(tokens, pos);
    for (auto& e : ev.errors()) {
        expr_diags_.push_back(e);
    }
    return result;
}

void Assembler::reportExprDiags(int line, const std::string& source) {
    for (auto& diag : expr_diags_) {
        error(line, source, diag);
    }
    expr_diags_.clear();
}

ExprResult Assembler::evalTokensAsExpr(const std::vector<Token>& tokens, size_t start) {
    size_t pos = start;
    return evalExpr(tokens, pos);
}

Operand Assembler::parseMemoryOperand(const std::vector<Token>& tokens, size_t& pos,
                                        OpSize size_override, bool pass2) {
    Operand op;
    op.type = OperandType::MEM;
    op.size_override = size_override;

    // pos is right after '[', collect tokens until ']'
    std::vector<Token> inner;
    while (pos < tokens.size() && tokens[pos].type != TokenType::CLOSE_BRACKET) {
        inner.push_back(tokens[pos++]);
    }
    if (pos < tokens.size()) pos++; // skip ']'
    inner.push_back({TokenType::EOL, ""});

    // Parse inner tokens: identify registers and build expression for displacement
    // Registers that can appear: BX, BP, SI, DI
    std::vector<Token> disp_tokens;
    bool expect_plus = false;

    for (size_t i = 0; i < inner.size(); i++) {
        if (inner[i].type == TokenType::EOL) break;

        if (inner[i].type == TokenType::REGISTER) {
            std::string rname = Lexer::toUpper(inner[i].text);
            if (rname == "BX") op.mem.has_bx = true;
            else if (rname == "BP") op.mem.has_bp = true;
            else if (rname == "SI") op.mem.has_si = true;
            else if (rname == "DI") op.mem.has_di = true;
            expect_plus = true;
        } else if (inner[i].type == TokenType::PLUS && expect_plus) {
            // skip the '+' between register and rest
            expect_plus = false;
        } else {
            // Everything else is part of the displacement expression
            // Collect remaining tokens
            while (i < inner.size() && inner[i].type != TokenType::EOL) {
                disp_tokens.push_back(inner[i++]);
            }
            break;
        }
    }

    if (!disp_tokens.empty()) {
        disp_tokens.push_back({TokenType::EOL, ""});
        size_t dpos = 0;
        ExprResult r = evalExpr(disp_tokens, dpos);
        op.mem.has_disp = true;
        op.mem.disp = r.value;
        op.mem.disp_unresolved = !r.resolved;
    }

    return op;
}

Operand Assembler::parseOperand(const std::vector<Token>& tokens, size_t& pos, bool pass2) {
    Operand op;
    if (pos >= tokens.size() || tokens[pos].type == TokenType::EOL) return op;

    // Check for BYTE/WORD size prefix
    OpSize size_override = OpSize::UNKNOWN;
    if (tokens[pos].type == TokenType::SIZE_KEYWORD) {
        size_override = tokens[pos].size;
        pos++;
    }

    // Memory operand: [...] or SREG:[...]
    if (pos < tokens.size() && tokens[pos].type == TokenType::OPEN_BRACKET) {
        pos++; // skip '['
        return parseMemoryOperand(tokens, pos, size_override, pass2);
    }

    // Register (or segment override prefix for memory operand)
    if (tokens[pos].type == TokenType::REGISTER) {
        std::string rname = Lexer::toUpper(tokens[pos].text);
        // Check segment registers first
        for (auto& si : SREG_TABLE) {
            if (rname == si.name) {
                // Check for segment override: ES:[...] syntax
                if (pos + 2 < tokens.size() &&
                    tokens[pos + 1].type == TokenType::COLON &&
                    tokens[pos + 2].type == TokenType::OPEN_BRACKET) {
                    pos += 3; // skip SREG, ':', '['
                    Operand mem_op = parseMemoryOperand(tokens, pos, size_override, pass2);
                    mem_op.seg_prefix = (uint8_t)si.sreg;
                    return mem_op;
                }
                op.type = OperandType::SREG;
                op.sreg = si.sreg;
                op.size_override = size_override;
                pos++;
                return op;
            }
        }
        op.type = OperandType::REG;
        for (auto& ri : REG_TABLE) {
            if (rname == ri.name) {
                op.reg = ri.reg;
                op.is_reg8 = ri.is_8bit;
                break;
            }
        }
        op.size_override = size_override;
        pos++;
        return op;
    }

    // Immediate or label reference — evaluate as expression
    // Collect tokens until comma or EOL
    std::vector<Token> expr_tokens;
    size_t start = pos;
    while (pos < tokens.size() && tokens[pos].type != TokenType::COMMA &&
           tokens[pos].type != TokenType::EOL) {
        expr_tokens.push_back(tokens[pos++]);
    }
    expr_tokens.push_back({TokenType::EOL, ""});

    // Check if it's a single symbol (used as label reference for MOV DX, msg etc.)
    bool single_symbol = (expr_tokens.size() == 2 && expr_tokens[0].type == TokenType::NUMBER &&
                          expr_tokens[0].numval == -1);

    size_t epos = 0;
    ExprResult r = evalExpr(expr_tokens, epos);

    if (single_symbol) {
        op.type = OperandType::LABEL_IMM;
    } else {
        op.type = OperandType::IMM;
    }
    op.imm = r.value;
    op.imm_unresolved = !r.resolved;
    op.size_override = size_override;

    // Save expression text for re-evaluation
    std::string expr_text;
    for (size_t i = start; i < pos; i++) {
        if (i > start) expr_text += " ";
        expr_text += tokens[i].text;
    }
    op.expr_text = expr_text;

    return op;
}

ParsedLine Assembler::parseLine(const std::string& line, int line_num) {
    ParsedLine pl;
    pl.raw_line = line;
    pl.line_number = line_num;

    auto tokens = lexer_.tokenize(line);
    size_t pos = 0;

    // Skip to non-EOL
    if (tokens.empty() || tokens[0].type == TokenType::EOL) return pl;

    // Check for label
    if (tokens[pos].type == TokenType::LABEL) {
        pl.label = tokens[pos].text;
        // Remove trailing colon if present in text (lexer should have consumed it)
        pl.is_local_label = (!pl.label.empty() && pl.label[0] == '.');
        pos++;
    }

    // Skip if nothing left
    if (pos >= tokens.size() || tokens[pos].type == TokenType::EOL) return pl;

    // Mnemonic or directive
    std::string word = Lexer::toUpper(tokens[pos].text);

    // Check for EQU special case: label EQU value (label is not followed by colon sometimes)
    // Actually in our files, EQU is like: NUM_OPCODES EQU 1Bh + 1
    // where NUM_OPCODES was parsed as a NUMBER (symbol) token
    if (pos + 1 < tokens.size() && Lexer::toUpper(tokens[pos + 1].text) == "EQU") {
        // tokens[pos] is the name, tokens[pos+1] is EQU
        pl.label = tokens[pos].text;
        pos++; // skip name
        pos++; // skip EQU
        pl.directive = "EQU";
        // Collect remaining tokens as directive_args
        while (pos < tokens.size() && tokens[pos].type != TokenType::EOL) {
            pl.directive_args.push_back(tokens[pos++]);
        }
        return pl;
    }

    // Check for name PROC / name ENDP (MASM-style, no colon)
    if (pos + 1 < tokens.size()) {
        std::string next = Lexer::toUpper(tokens[pos + 1].text);
        if (next == "PROC") {
            pl.label = tokens[pos].text;
            pos++; // skip name
            pl.directive = "PROC";
            pos++; // skip PROC
            return pl;
        }
        if (next == "ENDP") {
            // ENDP: name is decorative, do NOT redefine the label
            pos++; // skip name
            pl.directive = "ENDP";
            pos++; // skip ENDP
            return pl;
        }
    }

    // Check for DOLLAR EQU pattern where DOLLAR was lexed as a label
    // (e.g., "DOLLAR EQU '$'" — but we handle that above since DOLLAR would be NUMBER token)

    if (Lexer::isDirective(word)) {
        pl.directive = word;
        pos++;

        if (word == "PROC" || word == "ENDP") {
            return pl;
        }

        if (word == "TRACE_START" || word == "TRACE_STOP" ||
            word == "HEX_START" || word == "HEX_END") {
            return pl;  // no arguments
        }

        // Collect directive arguments (BREAKPOINT falls through here too)
        while (pos < tokens.size() && tokens[pos].type != TokenType::EOL) {
            pl.directive_args.push_back(tokens[pos++]);
        }
        return pl;
    }

    // Handle REP prefix
    if (word == "REP" || word == "REPE" || word == "REPZ" || word == "REPNE" || word == "REPNZ") {
        pl.prefix = word;
        pos++;
        if (pos < tokens.size() && tokens[pos].type != TokenType::EOL) {
            word = Lexer::toUpper(tokens[pos].text);
        }
    }

    // Regular mnemonic
    pl.mnemonic = word;
    pos++;

    // Parse operands
    while (pos < tokens.size() && tokens[pos].type != TokenType::EOL) {
        Operand op = parseOperand(tokens, pos, pass2_);
        if (op.type != OperandType::NONE) {
            pl.operands.push_back(op);
        }
        // Skip comma
        if (pos < tokens.size() && tokens[pos].type == TokenType::COMMA) pos++;
    }

    return pl;
}

// --- INCLUDE helpers ---

static std::string canonicalizePath(const std::string& path) {
    // Normalize separators to forward slash
    std::string norm = path;
    for (auto& c : norm) {
        if (c == '\\') c = '/';
    }
    char resolved[PATH_MAX];
    if (realpath(norm.c_str(), resolved)) {
        return std::string(resolved);
    }
    return norm;
}

static std::string directoryOf(const std::string& filepath) {
    // Find last separator
    size_t pos = filepath.find_last_of("/\\");
    if (pos != std::string::npos) {
        return filepath.substr(0, pos + 1);
    }
    return "./";
}

bool Assembler::expandIncludes(const std::vector<std::string>& lines,
                               std::vector<std::string>& out_lines,
                               std::vector<SourceOrigin>& out_origins,
                               const std::string& base_dir,
                               const std::string& current_file,
                               std::set<std::string>& seen_files,
                               int depth) {
    if (depth > 16) {
        errors_.push_back({0, current_file, "", "INCLUDE depth limit exceeded (>16)"});
        std::cerr << "Error: INCLUDE depth limit exceeded (>16)" << std::endl;
        return false;
    }

    for (int i = 0; i < (int)lines.size(); i++) {
        // Detect INCLUDE directive from raw text (handles unquoted paths)
        std::string trimmed = lines[i];
        // Strip leading whitespace
        size_t ls = trimmed.find_first_not_of(" \t");
        if (ls != std::string::npos) trimmed = trimmed.substr(ls);
        else trimmed.clear();
        // Strip comment
        {
            bool in_q = false;
            for (size_t ci = 0; ci < trimmed.size(); ci++) {
                if (trimmed[ci] == '\'' || trimmed[ci] == '"') in_q = !in_q;
                if (trimmed[ci] == ';' && !in_q) { trimmed = trimmed.substr(0, ci); break; }
            }
        }

        // Check if line starts with INCLUDE (case-insensitive)
        std::string upper_trimmed;
        for (char c : trimmed) upper_trimmed += (char)toupper((unsigned char)c);

        // Skip optional label (word followed by colon)
        size_t tpos = 0;
        {
            size_t sp = upper_trimmed.find_first_of(" \t:");
            if (sp != std::string::npos && sp < upper_trimmed.size() && upper_trimmed[sp] == ':') {
                tpos = sp + 1;
                while (tpos < upper_trimmed.size() &&
                       (upper_trimmed[tpos] == ' ' || upper_trimmed[tpos] == '\t'))
                    tpos++;
            }
        }

        bool is_include = false;
        if (upper_trimmed.substr(tpos, 7) == "INCLUDE" &&
            (tpos + 7 >= upper_trimmed.size() ||
             upper_trimmed[tpos + 7] == ' ' || upper_trimmed[tpos + 7] == '\t')) {
            is_include = true;
        }

        if (is_include) {
                // Extract filename after INCLUDE keyword
                size_t fn_start = tpos + 7;
                while (fn_start < trimmed.size() &&
                       (trimmed[fn_start] == ' ' || trimmed[fn_start] == '\t'))
                    fn_start++;

                if (fn_start >= trimmed.size()) {
                    errors_.push_back({i + 1, current_file, lines[i],
                                       "INCLUDE requires a filename"});
                    std::cerr << "Error: INCLUDE requires a filename" << std::endl;
                    return false;
                }

                std::string inc_name;
                // Strip quotes if present
                if (trimmed[fn_start] == '\'' || trimmed[fn_start] == '"') {
                    char q = trimmed[fn_start];
                    fn_start++;
                    size_t fn_end = trimmed.find(q, fn_start);
                    inc_name = (fn_end != std::string::npos)
                        ? trimmed.substr(fn_start, fn_end - fn_start)
                        : trimmed.substr(fn_start);
                } else {
                    // Unquoted: take rest of line (trimmed)
                    inc_name = trimmed.substr(fn_start);
                    // Trim trailing whitespace
                    while (!inc_name.empty() &&
                           (inc_name.back() == ' ' || inc_name.back() == '\t'))
                        inc_name.pop_back();
                }

                // Resolve path relative to current file's directory
                std::string inc_path = canonicalizePath(base_dir + inc_name);

                // Check circular include
                if (seen_files.count(inc_path)) {
                    errors_.push_back({i + 1, current_file, lines[i],
                                       "circular INCLUDE detected: " + inc_path});
                    std::cerr << "Error: circular INCLUDE detected: " << inc_path << std::endl;
                    return false;
                }

                // Read the included file
                std::ifstream ifs(inc_path);
                if (!ifs) {
                    errors_.push_back({i + 1, current_file, lines[i],
                                       "cannot open INCLUDE file: " + inc_path});
                    std::cerr << "Error: cannot open INCLUDE file: " << inc_path << std::endl;
                    return false;
                }
                std::vector<std::string> inc_lines;
                std::string line;
                while (std::getline(ifs, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    inc_lines.push_back(line);
                }
                ifs.close();

                // Mark as seen before recursion (include-guard behavior)
                seen_files.insert(inc_path);

                // Recurse
                std::string inc_dir = directoryOf(inc_path);
                if (!expandIncludes(inc_lines, out_lines, out_origins,
                                    inc_dir, inc_path, seen_files, depth + 1)) {
                    return false;
                }
                continue; // don't add the INCLUDE line itself
        }

        // Normal line — pass through
        out_lines.push_back(lines[i]);
        out_origins.push_back({current_file, i + 1});
    }
    return true;
}

// Find COLON followed by VRAMOUT identifier in token vector.
// Returns index of the COLON token, or -1 if not found.
int Assembler::findModifierColon(const std::vector<Token>& args) {
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].type == TokenType::COLON && i + 1 < args.size()) {
            std::string next = Lexer::toUpper(args[i + 1].text);
            if (next == "VRAMOUT" || next == "REGS") {
                return (int)i;
            }
        }
    }
    return -1;
}

Assembler::Modifiers Assembler::parseModifiers(const std::vector<Token>& args, size_t start,
                                                int line, const std::string& source) {
    Modifiers mods;
    size_t pos = start;
    while (pos < args.size() && args[pos].type != TokenType::EOL) {
        // Skip colons between modifiers
        if (args[pos].type == TokenType::COLON) { pos++; continue; }
        std::string upper = Lexer::toUpper(args[pos].text);
        if (upper == "VRAMOUT") {
            mods.vramout = parseVramOutArgs(args, pos, line, source);
            // Advance past VRAMOUT and its arguments
            pos++; // skip "VRAMOUT"
            while (pos < args.size() && args[pos].type != TokenType::EOL &&
                   args[pos].type != TokenType::COLON) {
                // Check if next token starts a new modifier keyword
                std::string peek = Lexer::toUpper(args[pos].text);
                if (peek == "REGS") break;
                pos++;
            }
        } else if (upper == "REGS") {
            mods.regs = true;
            pos++;
        } else {
            pos++; // skip unknown
        }
    }
    return mods;
}

// Parse VRAMOUT arguments starting at position 'start' in args.
// Syntax: [FULL|PARTIAL x, y, w, h] [, ATTRS]
VramOutParams Assembler::parseVramOutArgs(const std::vector<Token>& args, size_t start,
                                           int line, const std::string& source) {
    VramOutParams vp;
    vp.active = true;
    vp.full = true;
    vp.attrs = false;

    size_t pos = start;
    // Skip past "VRAMOUT" token itself if present
    if (pos < args.size() && Lexer::toUpper(args[pos].text) == "VRAMOUT") pos++;

    while (pos < args.size() && args[pos].type != TokenType::EOL) {
        std::string upper = Lexer::toUpper(args[pos].text);
        if (upper == "FULL") {
            vp.full = true;
            pos++;
        } else if (upper == "ATTRS") {
            vp.attrs = true;
            pos++;
        } else if (upper == "PARTIAL") {
            vp.full = false;
            pos++;
            // Expect: x, y, w, h (4 numeric values separated by commas)
            int coords[4] = {0, 0, 0, 0};
            for (int ci = 0; ci < 4; ci++) {
                std::vector<Token> expr_tokens;
                while (pos < args.size() && args[pos].type != TokenType::COMMA &&
                       args[pos].type != TokenType::EOL &&
                       Lexer::toUpper(args[pos].text) != "ATTRS") {
                    expr_tokens.push_back(args[pos++]);
                }
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t ep = 0;
                ExprResult r = evalExpr(expr_tokens, ep);
                reportExprDiags(line, source);
                coords[ci] = (int)r.value;
                // Skip comma between coords
                if (ci < 3 && pos < args.size() && args[pos].type == TokenType::COMMA) pos++;
            }
            vp.x = coords[0]; vp.y = coords[1];
            vp.w = coords[2]; vp.h = coords[3];
        } else if (args[pos].type == TokenType::COMMA) {
            pos++; // skip separator commas between modifiers
        } else {
            // Unknown token, skip
            pos++;
        }
    }
    return vp;
}

bool Assembler::pass1(const std::vector<std::string>& lines) {
    pass2_ = false;
    current_addr_ = 0;
    origin_ = 0;
    directive_pending_ = false;
    in_bss_ = false;
    bss_start_ = -1;
    pass1_sizes_.assign(lines.size(), 0);

    for (int i = 0; i < (int)lines.size(); i++) {
        ParsedLine pl = parseLine(lines[i], i + 1);
        reportExprDiags(i + 1, lines[i]);

        // Handle label
        if (!pl.label.empty()) {
            // NOP separator: runtime directive at this address needs 1-byte gap before label
            if (pl.directive != "EQU" && directive_pending_) {
                current_addr_++;
                directive_pending_ = false;
            }
            std::string redef;
            if (pl.is_local_label) {
                std::string qualified = symtab_.qualify(pl.label);
                redef = symtab_.define(qualified, current_addr_);
            } else {
                // New global label → becomes scope for local labels
                // But only if it's not an EQU
                if (pl.directive != "EQU") {
                    symtab_.setScope(pl.label);
                }
                redef = symtab_.define(pl.label, current_addr_);
            }
            if (!redef.empty()) {
                error(i + 1, lines[i], redef);
            }
        }

        // Handle directives
        if (pl.directive == "ORG") {
            if (!pl.directive_args.empty()) {
                size_t p = 0;
                std::vector<Token> args = pl.directive_args;
                args.push_back({TokenType::EOL, ""});
                ExprResult r = evalExpr(args, p);
                reportExprDiags(i + 1, lines[i]);
                origin_ = r.value;
                current_addr_ = r.value;
            }
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "EQU") {
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            if (r.resolved) {
                symtab_.define(pl.label, r.value, true);
            } else {
                // May need pass 2 resolution — define with 0 for now
                symtab_.define(pl.label, 0, true);
            }
            continue;
        }

        if (pl.directive == "PROC") {
            // PROC doesn't emit bytes; scope already set by label
            continue;
        }
        if (pl.directive == "ENDP") {
            continue;
        }

        if (pl.directive == "TRACE_START" || pl.directive == "TRACE_STOP" ||
            pl.directive == "BREAKPOINT" ||
            pl.directive == "ASSERT" || pl.directive == "HEX_START" ||
            pl.directive == "HEX_END" || pl.directive == "PRINT" ||
            pl.directive == "ASSERT_EQ" || pl.directive == "SCREEN" ||
            pl.directive == "VRAMOUT" || pl.directive == "REGS" ||
            pl.directive == "LOG" || pl.directive == "LOG_ONCE" ||
            pl.directive == "DOS_FAIL" || pl.directive == "DOS_PARTIAL" ||
            pl.directive == "MEM_SNAPSHOT" || pl.directive == "MEM_ASSERT") {
            // Block runtime directives in BSS (compile-time ones are fine)
            if (in_bss_ && pl.directive != "ASSERT" && pl.directive != "PRINT" &&
                pl.directive != "HEX_START" && pl.directive != "HEX_END" &&
                pl.directive != "SCREEN") {
                error(i + 1, lines[i], "runtime directive '" + pl.directive + "' not allowed in BSS section");
                continue;
            }
            // Runtime directives need NOP separation from following labels
            if (pl.directive == "TRACE_START" || pl.directive == "TRACE_STOP" ||
                pl.directive == "BREAKPOINT" || pl.directive == "ASSERT_EQ" ||
                pl.directive == "VRAMOUT" || pl.directive == "REGS" ||
                pl.directive == "LOG" || pl.directive == "LOG_ONCE" ||
                pl.directive == "DOS_FAIL" || pl.directive == "DOS_PARTIAL" ||
                pl.directive == "MEM_SNAPSHOT" || pl.directive == "MEM_ASSERT") {
                directive_pending_ = true;
            }
            continue;
        }

        if (pl.directive == "INCLUDE") {
            error(i + 1, lines[i], "unexpected INCLUDE directive (should have been expanded)");
            continue;
        }

        if (pl.directive == "SECTION") {
            auto& args = pl.directive_args;
            if (args.empty()) {
                error(i + 1, lines[i], "SECTION requires a section name (e.g., SECTION .bss)");
                continue;
            }
            std::string secname = Lexer::toUpper(args[0].text);
            if (secname == ".BSS" || secname == "BSS") {
                if (!in_bss_) {
                    in_bss_ = true;
                    bss_start_ = current_addr_;
                }
                // Duplicate SECTION .bss is a no-op
            } else {
                error(i + 1, lines[i], "unknown section '" + args[0].text + "' (only .bss is supported)");
            }
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "RESB") {
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            current_addr_ += r.value;
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "RESW") {
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            current_addr_ += r.value * 2;
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "DB") {
            if (in_bss_) {
                error(i + 1, lines[i], "initialized data (DB) not allowed in BSS section");
                continue;
            }
            int size = 0;
            std::vector<Token>& args = pl.directive_args;
            for (size_t j = 0; j < args.size(); j++) {
                if (args[j].type == TokenType::STRING) {
                    size += (int)args[j].text.size();
                } else if (args[j].type == TokenType::COMMA) {
                    continue;
                } else if (args[j].type == TokenType::EOL) {
                    break;
                } else {
                    // Expression: collect until comma or end
                    std::vector<Token> expr;
                    while (j < args.size() && args[j].type != TokenType::COMMA) {
                        expr.push_back(args[j++]);
                    }
                    size += 1; // one byte per expression
                    if (j < args.size() && args[j].type == TokenType::COMMA) {
                        // will be skipped by outer loop
                    }
                    j--; // compensate for outer loop increment
                }
            }
            current_addr_ += size;
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "DW") {
            if (in_bss_) {
                error(i + 1, lines[i], "initialized data (DW) not allowed in BSS section");
                continue;
            }
            int count = 0;
            std::vector<Token>& args = pl.directive_args;
            for (size_t j = 0; j < args.size(); j++) {
                if (args[j].type == TokenType::COMMA) continue;
                if (args[j].type == TokenType::EOL) break;
                // Collect expression tokens until comma
                while (j + 1 < args.size() && args[j + 1].type != TokenType::COMMA &&
                       args[j + 1].type != TokenType::EOL) {
                    j++;
                }
                count++;
            }
            current_addr_ += count * 2;
            directive_pending_ = false;
            continue;
        }

        // Instruction
        if (!pl.mnemonic.empty()) {
            if (in_bss_) {
                error(i + 1, lines[i], "instructions not allowed in BSS section");
                continue;
            }
            int size = encoder_.estimateSize(pl);
            pass1_sizes_[i] = size;
            current_addr_ += size;
            directive_pending_ = false;
        }

    }

    // Segment overflow check for BSS
    if (in_bss_ && current_addr_ > 0xFFFF) {
        error((int)lines.size(), "",
              "BSS exceeds 64KB segment limit (need " +
              std::to_string(current_addr_) + " bytes, max 65536)");
    }

    return errors_.empty();
}

bool Assembler::pass2(const std::vector<std::string>& lines, std::vector<uint8_t>& output) {
    pass2_ = true;
    current_addr_ = 0;
    origin_ = 0;
    directive_pending_ = false;
    in_bss_ = false;
    bss_start_ = -1;
    debug_entries_.clear();
    debug_directives_.clear();
    prints_.clear();
    hex_dumps_.clear();
    in_hex_region_ = false;

    // Re-resolve EQU values that might have been forward references
    // We do this by re-processing pass with full symbol table

    for (int i = 0; i < (int)lines.size(); i++) {
        ParsedLine pl = parseLine(lines[i], i + 1);
        reportExprDiags(i + 1, lines[i]);

        // NOP separator: runtime directive at this address needs 1-byte gap before label
        if (!pl.label.empty() && pl.directive != "EQU" && directive_pending_) {
            output.push_back(0x90);
            current_addr_++;
            directive_pending_ = false;
        }

        // Handle label scope (same as pass 1)
        if (!pl.label.empty() && !pl.is_local_label && pl.directive != "EQU") {
            symtab_.setScope(pl.label);
        }

        // ORG
        if (pl.directive == "ORG") {
            if (!pl.directive_args.empty()) {
                std::vector<Token> args = pl.directive_args;
                args.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(args, p);
                reportExprDiags(i + 1, lines[i]);
                origin_ = r.value;
                current_addr_ = r.value;
            }
            directive_pending_ = false;
            continue;
        }

        // EQU — re-evaluate with full symbol table
        if (pl.directive == "EQU") {
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            if (r.resolved) {
                symtab_.define(pl.label, r.value, true);
            } else {
                error(i + 1, lines[i], "unresolved EQU: " + pl.label);
            }
            continue;
        }

        if (pl.directive == "PROC" || pl.directive == "ENDP") continue;

        // SECTION .bss
        if (pl.directive == "SECTION") {
            auto& args = pl.directive_args;
            if (!args.empty()) {
                std::string secname = Lexer::toUpper(args[0].text);
                if (secname == ".BSS" || secname == "BSS") {
                    if (!in_bss_) {
                        in_bss_ = true;
                        bss_start_ = current_addr_;
                    }
                } else {
                    error(i + 1, lines[i], "unknown section '" + args[0].text + "' (only .bss is supported)");
                }
            }
            directive_pending_ = false;
            continue;
        }

        // Block runtime directives in BSS section
        if (in_bss_ && !pl.directive.empty()) {
            std::string d = pl.directive;
            if (d == "TRACE_START" || d == "TRACE_STOP" || d == "BREAKPOINT" ||
                d == "ASSERT_EQ" || d == "VRAMOUT" || d == "REGS" ||
                d == "LOG" || d == "LOG_ONCE" || d == "DOS_FAIL" || d == "DOS_PARTIAL" ||
                d == "MEM_SNAPSHOT" || d == "MEM_ASSERT") {
                error(i + 1, lines[i], "runtime directive '" + d + "' not allowed in BSS section");
                continue;
            }
        }

        if (pl.directive == "TRACE_START") {
            debug_directives_.push_back({DebugDirective::TRACE_START,
                                         (uint16_t)current_addr_, 0, ""});
            directive_pending_ = true;
            continue;
        }
        if (pl.directive == "TRACE_STOP") {
            debug_directives_.push_back({DebugDirective::TRACE_STOP,
                                         (uint16_t)current_addr_, 0, ""});
            directive_pending_ = true;
            continue;
        }
        if (pl.directive == "BREAKPOINT") {
            uint32_t bp_count = 0;
            std::string bp_name;
            Modifiers bp_mods;

            auto& args = pl.directive_args;

            // Check for : VRAMOUT / : REGS modifiers
            int colon_pos = findModifierColon(args);
            std::vector<Token> main_args;
            if (colon_pos >= 0) {
                main_args.assign(args.begin(), args.begin() + colon_pos);
                bp_mods = parseModifiers(args, colon_pos, i + 1, lines[i]);
            } else {
                main_args = args;
            }

            if (!main_args.empty()) {
                size_t apos = 0;
                // First arg: if it's an identifier (not a number), it's the name
                if (main_args[0].type != TokenType::COMMA &&
                    main_args[0].numval == -1 && !main_args[0].text.empty()) {
                    bp_name = main_args[0].text;
                    apos = 1;
                }
                // Skip comma
                if (apos < main_args.size() && main_args[apos].type == TokenType::COMMA) apos++;
                // Remaining tokens: count expression
                if (apos < main_args.size() && main_args[apos].type != TokenType::EOL) {
                    std::vector<Token> count_expr;
                    while (apos < main_args.size() && main_args[apos].type != TokenType::EOL) {
                        count_expr.push_back(main_args[apos++]);
                    }
                    count_expr.push_back({TokenType::EOL, ""});
                    size_t cpos = 0;
                    ExprResult r = evalExpr(count_expr, cpos);
                    reportExprDiags(i + 1, lines[i]);
                    bp_count = (uint32_t)r.value;
                }
            }

            DebugDirective dd;
            dd.type = DebugDirective::BREAKPOINT;
            dd.addr = (uint16_t)current_addr_;
            dd.count = bp_count;
            dd.label = bp_name;
            dd.vramout = bp_mods.vramout;
            dd.regs = bp_mods.regs;
            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // ASSERT — compile-time assertion
        if (pl.directive == "ASSERT") {
            auto& args = pl.directive_args;
            // Find top-level comma (not inside parens) to split two-arg form
            int paren_depth = 0;
            int comma_pos = -1;
            for (size_t j = 0; j < args.size(); j++) {
                if (args[j].type == TokenType::OPEN_PAREN) paren_depth++;
                else if (args[j].type == TokenType::CLOSE_PAREN) paren_depth--;
                else if (args[j].type == TokenType::COMMA && paren_depth == 0) {
                    comma_pos = (int)j;
                    break;
                }
            }

            if (comma_pos >= 0) {
                // Two-arg: ASSERT expr1, expr2 — fail if expr1 != expr2
                std::vector<Token> lhs(args.begin(), args.begin() + comma_pos);
                lhs.push_back({TokenType::EOL, ""});
                std::vector<Token> rhs(args.begin() + comma_pos + 1, args.end());
                rhs.push_back({TokenType::EOL, ""});

                size_t p1 = 0;
                ExprResult r1 = evalExpr(lhs, p1);
                reportExprDiags(i + 1, lines[i]);
                size_t p2 = 0;
                ExprResult r2 = evalExpr(rhs, p2);
                reportExprDiags(i + 1, lines[i]);

                if (!r1.resolved || !r2.resolved) {
                    error(i + 1, lines[i], "ASSERT: unresolved expression");
                } else if (r1.value != r2.value) {
                    error(i + 1, lines[i], "ASSERT failed: " +
                          std::to_string(r1.value) + " != " + std::to_string(r2.value));
                }
            } else {
                // Single-arg: ASSERT expr — fail if expr == 0
                std::vector<Token> expr(args.begin(), args.end());
                expr.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], "ASSERT: unresolved expression");
                } else if (r.value == 0) {
                    error(i + 1, lines[i], "ASSERT failed: expression is zero");
                }
            }
            continue;
        }

        // HEX_START / HEX_END — hex capture regions
        if (pl.directive == "HEX_START") {
            if (in_hex_region_) {
                error(i + 1, lines[i], "nested HEX_START (missing HEX_END)");
            }
            in_hex_region_ = true;
            hex_start_addr_ = (uint16_t)current_addr_;
            continue;
        }
        if (pl.directive == "HEX_END") {
            if (!in_hex_region_) {
                error(i + 1, lines[i], "HEX_END without matching HEX_START");
            } else {
                hex_dumps_.push_back({hex_start_addr_, (uint16_t)current_addr_});
                in_hex_region_ = false;
            }
            continue;
        }

        // PRINT — compile-time message/value output
        if (pl.directive == "PRINT") {
            auto& args = pl.directive_args;
            std::string file;
            int orig_line = i + 1;
            if (orig_line >= 1 && orig_line <= (int)source_origins_.size()) {
                file = source_origins_[orig_line - 1].file;
                orig_line = source_origins_[orig_line - 1].line;
            }

            std::string text;
            if (!args.empty() && args[0].type == TokenType::STRING) {
                // First token is a string literal
                text = args[0].text;
                // Check if comma follows → string + expression
                if (args.size() > 1 && args[1].type == TokenType::COMMA) {
                    std::vector<Token> expr(args.begin() + 2, args.end());
                    expr.push_back({TokenType::EOL, ""});
                    size_t p = 0;
                    ExprResult r = evalExpr(expr, p);
                    reportExprDiags(i + 1, lines[i]);
                    if (r.resolved) {
                        text += std::to_string(r.value);
                    } else {
                        text += "?";
                    }
                }
            } else {
                // All tokens are an expression
                std::vector<Token> expr(args.begin(), args.end());
                expr.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr, p);
                reportExprDiags(i + 1, lines[i]);
                if (r.resolved) {
                    text = std::to_string(r.value);
                } else {
                    text = "?";
                }
            }
            prints_.push_back({orig_line, file, text});
            std::cerr << "PRINT: " << text << std::endl;
            continue;
        }

        // ASSERT_EQ — runtime assertion (stored in .dbg for --trace)
        if (pl.directive == "ASSERT_EQ") {
            auto& args = pl.directive_args;

            // Check for : VRAMOUT / : REGS modifiers
            int colon_pos = findModifierColon(args);
            Modifiers aeq_mods;
            // Build working args: everything before the colon (or all args if no colon)
            std::vector<Token> work_args;
            if (colon_pos >= 0) {
                work_args.assign(args.begin(), args.begin() + colon_pos);
                aeq_mods = parseModifiers(args, colon_pos, i + 1, lines[i]);
            } else {
                work_args = args;
            }

            if (work_args.empty()) {
                error(i + 1, lines[i], "ASSERT_EQ requires arguments");
                continue;
            }

            DebugDirective dd;
            dd.type = DebugDirective::ASSERT_EQ;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;
            dd.vramout = aeq_mods.vramout;
            dd.regs = aeq_mods.regs;

            size_t apos = 0;
            std::string first_upper = Lexer::toUpper(work_args[0].text);

            if (first_upper == "BYTE" || first_upper == "WORD") {
                // Memory check: ASSERT_EQ BYTE [addr], expected  or  ASSERT_EQ WORD [addr], expected
                bool is_byte = (first_upper == "BYTE");
                dd.check_kind = is_byte ? DebugDirective::CHECK_MEM_BYTE : DebugDirective::CHECK_MEM_WORD;
                apos = 1;

                // Check for segment override: BYTE ES:[addr]
                int seg_override = -1;
                if (apos < work_args.size() && work_args[apos].type == TokenType::REGISTER) {
                    std::string sreg_name = Lexer::toUpper(work_args[apos].text);
                    for (auto& si : SREG_TABLE) {
                        if (sreg_name == si.name) {
                            seg_override = (int)si.sreg;
                            break;
                        }
                    }
                    if (seg_override >= 0) {
                        apos++; // skip segment register
                        if (apos < work_args.size() && work_args[apos].type == TokenType::COLON)
                            apos++; // skip ':'
                    }
                }

                // Expect '['
                if (apos >= work_args.size() || work_args[apos].type != TokenType::OPEN_BRACKET) {
                    error(i + 1, lines[i], "ASSERT_EQ: expected '[' after " + first_upper);
                    continue;
                }
                apos++; // skip '['

                // Collect tokens until ']'
                std::vector<Token> addr_expr;
                while (apos < work_args.size() && work_args[apos].type != TokenType::CLOSE_BRACKET) {
                    addr_expr.push_back(work_args[apos++]);
                }
                if (apos < work_args.size()) apos++; // skip ']'
                addr_expr.push_back({TokenType::EOL, ""});

                size_t p = 0;
                ExprResult r = evalExpr(addr_expr, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], "ASSERT_EQ: unresolved memory address");
                    continue;
                }
                dd.mem_addr = (uint16_t)r.value;
                dd.mem_seg = seg_override;

                // Skip comma
                if (apos < work_args.size() && work_args[apos].type == TokenType::COMMA) apos++;

                // Parse expected value
                std::vector<Token> val_expr(work_args.begin() + apos, work_args.end());
                val_expr.push_back({TokenType::EOL, ""});
                size_t vp = 0;
                ExprResult rv = evalExpr(val_expr, vp);
                reportExprDiags(i + 1, lines[i]);
                if (!rv.resolved) {
                    error(i + 1, lines[i], "ASSERT_EQ: unresolved expected value");
                    continue;
                }
                dd.expected = rv.value;
            } else {
                // Check if first token is a 16-bit register
                bool found_reg = false;
                for (auto& ri : REG_TABLE) {
                    if (first_upper == ri.name && !ri.is_8bit) {
                        found_reg = true;
                        dd.check_kind = DebugDirective::CHECK_REG;
                        dd.reg_name = ri.name;
                        dd.reg_index = (int)ri.reg; // Reg enum maps to 0..7 for 16-bit
                        break;
                    }
                }
                if (!found_reg) {
                    error(i + 1, lines[i], "ASSERT_EQ: expected register or BYTE/WORD, got '" + work_args[0].text + "'");
                    continue;
                }
                apos = 1;

                // Skip comma
                if (apos < work_args.size() && work_args[apos].type == TokenType::COMMA) apos++;

                // Parse expected value
                std::vector<Token> val_expr(work_args.begin() + apos, work_args.end());
                val_expr.push_back({TokenType::EOL, ""});
                size_t vp = 0;
                ExprResult rv = evalExpr(val_expr, vp);
                reportExprDiags(i + 1, lines[i]);
                if (!rv.resolved) {
                    error(i + 1, lines[i], "ASSERT_EQ: unresolved expected value");
                    continue;
                }
                dd.expected = rv.value;
            }

            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        if (pl.directive == "SCREEN") {
            auto& args = pl.directive_args;
            if (args.empty()) {
                error(i + 1, lines[i], "SCREEN requires a mode (MDA, CGA40, CGA80, VGA50)");
                continue;
            }
            std::string mode = Lexer::toUpper(args[0].text);
            if (mode != "MDA" && mode != "CGA40" && mode != "CGA80" && mode != "VGA50") {
                error(i + 1, lines[i], "SCREEN: invalid mode '" + args[0].text + "' (expected MDA, CGA40, CGA80, or VGA50)");
                continue;
            }
            if (!screen_mode_.empty()) {
                error(i + 1, lines[i], "SCREEN: duplicate directive (already set to " + screen_mode_ + ")");
                continue;
            }
            screen_mode_ = mode;
            continue;
        }

        // VRAMOUT — standalone VRAM snapshot directive
        if (pl.directive == "VRAMOUT") {
            auto& args = pl.directive_args;
            VramOutParams vp = parseVramOutArgs(args, 0, i + 1, lines[i]);
            DebugDirective dd;
            dd.type = DebugDirective::VRAMOUT;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;
            dd.vramout = vp;
            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // REGS — standalone register snapshot directive
        if (pl.directive == "REGS") {
            DebugDirective dd;
            dd.type = DebugDirective::REGS;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;
            dd.regs = true;
            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // LOG / LOG_ONCE — runtime debug print
        // LOG "msg" [, reg_or_mem]
        // LOG_ONCE label, "msg" [, reg_or_mem]
        if (pl.directive == "LOG" || pl.directive == "LOG_ONCE") {
            auto& args = pl.directive_args;
            bool is_once = (pl.directive == "LOG_ONCE");

            DebugDirective dd;
            dd.type = is_once ? DebugDirective::LOG_ONCE : DebugDirective::LOG;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;
            dd.check_kind = DebugDirective::CHECK_NONE;

            size_t apos = 0;

            // LOG_ONCE: first token is the dedup label
            if (is_once) {
                if (args.empty() || args[0].type == TokenType::EOL) {
                    error(i + 1, lines[i], "LOG_ONCE requires a label");
                    continue;
                }
                dd.log_once_label = args[0].text;
                apos = 1;
                // Skip comma after label
                if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;
            }

            // Next token: message string
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires a message string");
                continue;
            }
            if (args[apos].type != TokenType::STRING && args[apos].type != TokenType::CHAR_LITERAL) {
                error(i + 1, lines[i], std::string(pl.directive) + ": expected string literal");
                continue;
            }
            dd.message = args[apos].text;
            apos++;

            // Optional: comma + register or BYTE/WORD [addr]
            if (apos < args.size() && args[apos].type == TokenType::COMMA) {
                apos++; // skip comma

                if (apos < args.size() && args[apos].type != TokenType::EOL) {
                    std::string first_upper = Lexer::toUpper(args[apos].text);

                    if (first_upper == "BYTE" || first_upper == "WORD") {
                        bool is_byte = (first_upper == "BYTE");
                        dd.check_kind = is_byte ? DebugDirective::CHECK_MEM_BYTE : DebugDirective::CHECK_MEM_WORD;
                        apos++;

                        // Check for segment override: BYTE ES:[addr]
                        int seg_override = -1;
                        if (apos < args.size() && args[apos].type == TokenType::REGISTER) {
                            std::string sreg_name = Lexer::toUpper(args[apos].text);
                            for (auto& si : SREG_TABLE) {
                                if (sreg_name == si.name) {
                                    seg_override = (int)si.sreg;
                                    break;
                                }
                            }
                            if (seg_override >= 0) {
                                apos++; // skip segment register
                                if (apos < args.size() && args[apos].type == TokenType::COLON)
                                    apos++; // skip ':'
                            }
                        }

                        if (apos >= args.size() || args[apos].type != TokenType::OPEN_BRACKET) {
                            error(i + 1, lines[i], std::string(pl.directive) + ": expected '[' after " + first_upper);
                            continue;
                        }
                        apos++; // skip '['

                        std::vector<Token> addr_expr;
                        while (apos < args.size() && args[apos].type != TokenType::CLOSE_BRACKET) {
                            addr_expr.push_back(args[apos++]);
                        }
                        if (apos < args.size()) apos++; // skip ']'
                        addr_expr.push_back({TokenType::EOL, ""});

                        size_t p = 0;
                        ExprResult r = evalExpr(addr_expr, p);
                        reportExprDiags(i + 1, lines[i]);
                        if (!r.resolved) {
                            error(i + 1, lines[i], std::string(pl.directive) + ": unresolved memory address");
                            continue;
                        }
                        dd.mem_addr = (uint16_t)r.value;
                        dd.mem_seg = seg_override;
                    } else {
                        // Check for 16-bit register
                        bool found_reg = false;
                        for (auto& ri : REG_TABLE) {
                            if (first_upper == ri.name && !ri.is_8bit) {
                                found_reg = true;
                                dd.check_kind = DebugDirective::CHECK_REG;
                                dd.reg_name = ri.name;
                                dd.reg_index = (int)ri.reg;
                                break;
                            }
                        }
                        if (!found_reg) {
                            error(i + 1, lines[i], std::string(pl.directive) + ": expected register or BYTE/WORD, got '" + args[apos].text + "'");
                            continue;
                        }
                        apos++;
                    }
                }
            }

            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // DOS_FAIL / DOS_PARTIAL — one-shot DOS failure injection
        // DOS_FAIL int_num, ah_func [, error_code]
        // DOS_PARTIAL int_num, ah_func, count
        if (pl.directive == "DOS_FAIL" || pl.directive == "DOS_PARTIAL") {
            auto& args = pl.directive_args;
            bool is_partial = (pl.directive == "DOS_PARTIAL");

            DebugDirective dd;
            dd.type = is_partial ? DebugDirective::DOS_PARTIAL : DebugDirective::DOS_FAIL;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;

            size_t apos = 0;

            // Parse int_num (required)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires interrupt number");
                continue;
            }
            {
                std::vector<Token> expr_tokens;
                while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                    expr_tokens.push_back(args[apos++]);
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr_tokens, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], std::string(pl.directive) + ": unresolved interrupt number");
                    continue;
                }
                dd.int_num = (uint8_t)r.value;
            }

            // Skip comma
            if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;

            // Parse ah_func (required)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires AH function number");
                continue;
            }
            {
                std::vector<Token> expr_tokens;
                while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                    expr_tokens.push_back(args[apos++]);
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr_tokens, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], std::string(pl.directive) + ": unresolved AH function number");
                    continue;
                }
                dd.ah_func = (uint8_t)r.value;
            }

            // Skip comma
            if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;

            if (is_partial) {
                // DOS_PARTIAL: count is required
                if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                    error(i + 1, lines[i], "DOS_PARTIAL requires a byte count");
                    continue;
                }
                std::vector<Token> expr_tokens;
                while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                    expr_tokens.push_back(args[apos++]);
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr_tokens, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], "DOS_PARTIAL: unresolved byte count");
                    continue;
                }
                dd.partial_count = (uint16_t)r.value;
            } else {
                // DOS_FAIL: optional error_code (default 5)
                if (apos < args.size() && args[apos].type != TokenType::EOL) {
                    std::vector<Token> expr_tokens;
                    while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                        expr_tokens.push_back(args[apos++]);
                    expr_tokens.push_back({TokenType::EOL, ""});
                    size_t p = 0;
                    ExprResult r = evalExpr(expr_tokens, p);
                    reportExprDiags(i + 1, lines[i]);
                    if (!r.resolved) {
                        error(i + 1, lines[i], "DOS_FAIL: unresolved error code");
                        continue;
                    }
                    dd.fail_code = (uint16_t)r.value;
                }
            }

            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // MEM_SNAPSHOT / MEM_ASSERT — memory region snapshot and comparison
        // MEM_SNAPSHOT name, SEG_REG, offset_expr, length_expr
        // MEM_ASSERT   name, SEG_REG, offset_expr, length_expr
        if (pl.directive == "MEM_SNAPSHOT" || pl.directive == "MEM_ASSERT") {
            auto& args = pl.directive_args;
            bool is_assert = (pl.directive == "MEM_ASSERT");

            DebugDirective dd;
            dd.type = is_assert ? DebugDirective::MEM_ASSERT : DebugDirective::MEM_SNAPSHOT;
            dd.addr = (uint16_t)current_addr_;
            dd.count = 0;

            size_t apos = 0;

            // Parse name (required — any non-EOL, non-COMMA token)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires a snapshot name");
                continue;
            }
            dd.snap_name = args[apos].text;
            apos++;

            // Skip comma
            if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;

            // Parse segment register (required: ES, CS, SS, DS)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires a segment register (ES/CS/SS/DS)");
                continue;
            }
            std::string seg_str = args[apos].text;
            for (auto& c : seg_str) c = (char)toupper((unsigned char)c);
            if (seg_str == "ES") dd.snap_seg = 0;
            else if (seg_str == "CS") dd.snap_seg = 1;
            else if (seg_str == "SS") dd.snap_seg = 2;
            else if (seg_str == "DS") dd.snap_seg = 3;
            else {
                error(i + 1, lines[i], std::string(pl.directive) + ": unknown segment register '" + args[apos].text + "' (expected ES/CS/SS/DS)");
                continue;
            }
            apos++;

            // Skip comma
            if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;

            // Parse offset expression (required)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires an offset expression");
                continue;
            }
            {
                std::vector<Token> expr_tokens;
                while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                    expr_tokens.push_back(args[apos++]);
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr_tokens, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], std::string(pl.directive) + ": unresolved offset expression");
                    continue;
                }
                dd.snap_offset = (uint16_t)r.value;
            }

            // Skip comma
            if (apos < args.size() && args[apos].type == TokenType::COMMA) apos++;

            // Parse length expression (required)
            if (apos >= args.size() || args[apos].type == TokenType::EOL) {
                error(i + 1, lines[i], std::string(pl.directive) + " requires a length expression");
                continue;
            }
            {
                std::vector<Token> expr_tokens;
                while (apos < args.size() && args[apos].type != TokenType::COMMA && args[apos].type != TokenType::EOL)
                    expr_tokens.push_back(args[apos++]);
                expr_tokens.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr_tokens, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], std::string(pl.directive) + ": unresolved length expression");
                    continue;
                }
                if (r.value < 1 || r.value > 65536) {
                    error(i + 1, lines[i], std::string(pl.directive) + ": length must be 1..65536 (got " + std::to_string(r.value) + ")");
                    continue;
                }
                dd.snap_length = (uint16_t)r.value;
            }

            debug_directives_.push_back(dd);
            directive_pending_ = true;
            continue;
        }

        // RESB — emit zero bytes
        if (pl.directive == "RESB") {
            recordDebug(i + 1, lines[i]);
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            if (!in_bss_) {
                for (int64_t j = 0; j < r.value; j++) output.push_back(0);
            }
            current_addr_ += r.value;
            directive_pending_ = false;
            continue;
        }

        if (pl.directive == "RESW") {
            recordDebug(i + 1, lines[i]);
            std::vector<Token> args = pl.directive_args;
            args.push_back({TokenType::EOL, ""});
            size_t p = 0;
            ExprResult r = evalExpr(args, p);
            reportExprDiags(i + 1, lines[i]);
            if (!in_bss_) {
                for (int64_t j = 0; j < r.value * 2; j++) output.push_back(0);
            }
            current_addr_ += r.value * 2;
            directive_pending_ = false;
            continue;
        }

        // DB
        if (pl.directive == "DB") {
            if (in_bss_) {
                error(i + 1, lines[i], "initialized data (DB) not allowed in BSS section");
                continue;
            }
            recordDebug(i + 1, lines[i]);
            std::vector<Token>& args = pl.directive_args;
            for (size_t j = 0; j < args.size(); j++) {
                if (args[j].type == TokenType::STRING) {
                    for (char c : args[j].text) {
                        output.push_back((uint8_t)c);
                        current_addr_++;
                    }
                } else if (args[j].type == TokenType::COMMA) {
                    continue;
                } else if (args[j].type == TokenType::EOL) {
                    break;
                } else {
                    // Expression
                    std::vector<Token> expr;
                    while (j < args.size() && args[j].type != TokenType::COMMA) {
                        expr.push_back(args[j++]);
                    }
                    expr.push_back({TokenType::EOL, ""});
                    size_t p = 0;
                    ExprResult r = evalExpr(expr, p);
                    reportExprDiags(i + 1, lines[i]);
                    if (!r.resolved) {
                        error(i + 1, lines[i], "unresolved DB expression");
                    }
                    output.push_back((uint8_t)(r.value & 0xFF));
                    current_addr_++;
                    if (j < args.size() && args[j].type == TokenType::COMMA) {
                        // outer loop will skip
                    }
                    j--;
                }
            }
            directive_pending_ = false;
            continue;
        }

        // DW
        if (pl.directive == "DW") {
            if (in_bss_) {
                error(i + 1, lines[i], "initialized data (DW) not allowed in BSS section");
                continue;
            }
            recordDebug(i + 1, lines[i]);
            std::vector<Token>& args = pl.directive_args;
            for (size_t j = 0; j < args.size(); j++) {
                if (args[j].type == TokenType::COMMA) continue;
                if (args[j].type == TokenType::EOL) break;

                std::vector<Token> expr;
                while (j < args.size() && args[j].type != TokenType::COMMA &&
                       args[j].type != TokenType::EOL) {
                    expr.push_back(args[j++]);
                }
                expr.push_back({TokenType::EOL, ""});
                size_t p = 0;
                ExprResult r = evalExpr(expr, p);
                reportExprDiags(i + 1, lines[i]);
                if (!r.resolved) {
                    error(i + 1, lines[i], "unresolved DW expression");
                }
                output.push_back((uint8_t)(r.value & 0xFF));
                output.push_back((uint8_t)((r.value >> 8) & 0xFF));
                current_addr_ += 2;

                if (j < args.size() && args[j].type == TokenType::COMMA) {
                    // will be skipped
                } else {
                    j--; // compensate
                }
            }
            directive_pending_ = false;
            continue;
        }

        // Instruction
        if (!pl.mnemonic.empty()) {
            if (in_bss_) {
                error(i + 1, lines[i], "instructions not allowed in BSS section");
                continue;
            }
            recordDebug(i + 1, lines[i]);
            std::string m = Lexer::toUpper(pl.mnemonic);

            // For jump/call/loop instructions with immediate targets,
            // encode with current_addr for relative offset calculation.
            // Indirect JMP/CALL (reg or mem operand) fall through to regular encoder.
            if (m == "JMP" || m == "CALL" ||
                m == "LOOP" || m == "LOOPE" || m == "LOOPZ" ||
                m == "LOOPNE" || m == "LOOPNZ" || m == "JCXZ" ||
                m == "JO" || m == "JNO" ||
                m == "JZ" || m == "JNZ" || m == "JE" || m == "JNE" ||
                m == "JB" || m == "JAE" || m == "JNC" || m == "JC" ||
                m == "JBE" || m == "JA" || m == "JL" || m == "JGE" ||
                m == "JLE" || m == "JG" || m == "JNS" || m == "JS" ||
                m == "JP" || m == "JPE" || m == "JNP" || m == "JPO" ||
                m == "JNAE" || m == "JNB" || m == "JNBE" || m == "JNGE" ||
                m == "JNL" || m == "JNG" || m == "JNLE" || m == "JNA") {
                if (!pl.operands.empty() &&
                    (pl.operands[0].type == OperandType::IMM ||
                     pl.operands[0].type == OperandType::LABEL_IMM)) {
                    int64_t target = pl.operands[0].imm;
                    if (pl.operands[0].imm_unresolved) {
                        error(i + 1, lines[i], "unresolved jump target");
                    }

                    // Use the saved pass 1 estimate for consistent NOP padding
                    int estimated = pass1_sizes_[i];

                    auto jbytes = encoder_.encodeJcc(m, target, current_addr_);

                    if (jbytes.empty()) {
                        error(i + 1, lines[i], "failed to encode jump: " + m);
                        continue;
                    }

                    int actual = (int)jbytes.size();

                    // Range validation for short-only instructions
                    if (m == "LOOP" || m == "LOOPE" || m == "LOOPZ" ||
                        m == "LOOPNE" || m == "LOOPNZ" || m == "JCXZ") {
                        int64_t rel = target - (current_addr_ + 2);
                        if (rel < -128 || rel > 127) {
                            error(i + 1, lines[i], m + " target out of short range (rel=" +
                                  std::to_string(rel) + ")");
                        }
                    }

                    // Pad with NOPs if actual encoding is smaller than pass 1 estimate.
                    for (auto b : jbytes) output.push_back(b);
                    for (int pad = actual; pad < estimated; pad++) {
                        output.push_back(0x90); // NOP padding
                    }
                    current_addr_ += estimated;
                    directive_pending_ = false;
                    continue;
                }
                // Indirect JMP/CALL (reg/mem operand) falls through to regular encoder
            }

            auto bytes = encoder_.encode(pl, false);
            if (bytes.empty() && m != "PROC" && m != "ENDP") {
                error(i + 1, lines[i], "failed to encode: " + m + " (operands: " + std::to_string(pl.operands.size()) + ")");
            }
            int estimated = pass1_sizes_[i];
            int actual = (int)bytes.size();
            if (actual > estimated && !bytes.empty()) {
                error(i + 1, lines[i],
                      "internal: encode size (" + std::to_string(actual) +
                      ") exceeds estimate (" + std::to_string(estimated) +
                      ") for " + m + " — labels after this point may be wrong");
            }
            for (auto b : bytes) output.push_back(b);
            // Pad with NOPs if actual encoding is smaller than pass 1 estimate.
            for (int pad = actual; pad < estimated; pad++) {
                output.push_back(0x90);
            }
            current_addr_ += std::max(estimated, actual);
            directive_pending_ = false;
        }
    }

    // Validate unclosed HEX region
    if (in_hex_region_) {
        error((int)lines.size(), "", "unclosed HEX_START (missing HEX_END)");
    }

    return errors_.empty();
}

// ── Macro expansion ─────────────────────────────────────────────────────────

// Helper: trim leading/trailing whitespace
static std::string trimWS(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Helper: extract first whitespace-delimited word (uppercased)
static std::string firstWord(const std::string& line) {
    std::string trimmed = trimWS(line);
    // Skip label (ends with ':')
    size_t start = 0;
    // Check if first word ends with ':'
    size_t sp = trimmed.find_first_of(" \t");
    if (sp != std::string::npos) {
        std::string first = trimmed.substr(0, sp);
        if (!first.empty() && first.back() == ':') {
            // Skip label and whitespace after it
            start = sp;
            while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t')) start++;
        }
    }
    size_t end = trimmed.find_first_of(" \t", start);
    std::string word = (end == std::string::npos) ? trimmed.substr(start) : trimmed.substr(start, end - start);
    std::string upper;
    for (char c : word) upper += (char)toupper((unsigned char)c);
    return upper;
}

// Helper: get second word (after first word + whitespace)
static std::string secondWord(const std::string& line) {
    std::string trimmed = trimWS(line);
    size_t p = trimmed.find_first_of(" \t");
    if (p == std::string::npos) return "";
    while (p < trimmed.size() && (trimmed[p] == ' ' || trimmed[p] == '\t')) p++;
    size_t end = trimmed.find_first_of(" \t,", p);
    std::string word = (end == std::string::npos) ? trimmed.substr(p) : trimmed.substr(p, end - p);
    std::string upper;
    for (char c : word) upper += (char)toupper((unsigned char)c);
    return upper;
}

// Helper: strip comment from line (respecting strings)
static std::string stripComment(const std::string& line) {
    bool in_str = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (in_str) {
            if (c == quote) in_str = false;
        } else {
            if (c == '\'' || c == '"') { in_str = true; quote = c; }
            else if (c == ';') return line.substr(0, i);
        }
    }
    return line;
}

// Expand IRP block: for each item in the list, substitute variable and produce lines
std::vector<std::string> Assembler::expandIRP(
    const std::vector<std::string>& body_lines,
    const std::string& var,
    const std::vector<std::string>& items)
{
    std::vector<std::string> result;
    for (const auto& item : items) {
        for (const auto& bline : body_lines) {
            std::string expanded = bline;
            // Replace whole-word occurrences of var with item
            size_t pos = 0;
            while (pos < expanded.size()) {
                size_t found = expanded.find(var, pos);
                if (found == std::string::npos) break;
                // Check word boundaries
                bool left_ok = (found == 0) ||
                    (!isalnum((unsigned char)expanded[found - 1]) && expanded[found - 1] != '_');
                size_t after = found + var.size();
                bool right_ok = (after >= expanded.size()) ||
                    (!isalnum((unsigned char)expanded[after]) && expanded[after] != '_');
                if (left_ok && right_ok) {
                    expanded.replace(found, var.size(), item);
                    pos = found + item.size();
                } else {
                    pos = found + 1;
                }
            }
            result.push_back(expanded);
        }
    }
    return result;
}

// Expand a macro body: substitute params, expand nested IRP blocks
std::vector<std::string> Assembler::expandMacroBody(
    const MacroDef& macro,
    const std::vector<std::string>& args)
{
    // First, substitute parameters in the body
    std::vector<std::string> substituted;
    for (const auto& bline : macro.body) {
        std::string line = bline;
        for (size_t pi = 0; pi < macro.params.size() && pi < args.size(); pi++) {
            const std::string& param = macro.params[pi];
            const std::string& arg = args[pi];
            size_t pos = 0;
            while (pos < line.size()) {
                size_t found = line.find(param, pos);
                if (found == std::string::npos) break;
                bool left_ok = (found == 0) ||
                    (!isalnum((unsigned char)line[found - 1]) && line[found - 1] != '_');
                size_t after = found + param.size();
                bool right_ok = (after >= line.size()) ||
                    (!isalnum((unsigned char)line[after]) && line[after] != '_');
                if (left_ok && right_ok) {
                    line.replace(found, param.size(), arg);
                    pos = found + arg.size();
                } else {
                    pos = found + 1;
                }
            }
        }
        substituted.push_back(line);
    }

    // Now expand any IRP blocks in the substituted body
    std::vector<std::string> result;
    size_t i = 0;
    while (i < substituted.size()) {
        std::string fw = firstWord(substituted[i]);
        if (fw == "IRP") {
            // Parse: IRP var, <item1, item2, ...>
            std::string trimmed = trimWS(stripComment(substituted[i]));
            // Find IRP keyword position
            size_t irp_pos = 0;
            std::string upper_trimmed;
            for (char c : trimmed) upper_trimmed += (char)toupper((unsigned char)c);
            irp_pos = upper_trimmed.find("IRP");
            size_t after_irp = irp_pos + 3;
            while (after_irp < trimmed.size() && (trimmed[after_irp] == ' ' || trimmed[after_irp] == '\t'))
                after_irp++;

            // Get variable name
            size_t var_end = trimmed.find_first_of(" \t,", after_irp);
            std::string var = (var_end == std::string::npos)
                ? trimmed.substr(after_irp) : trimmed.substr(after_irp, var_end - after_irp);

            // Find '<' ... '>' list
            size_t lt = trimmed.find('<', var_end != std::string::npos ? var_end : after_irp);
            size_t gt = trimmed.find('>', lt != std::string::npos ? lt : 0);
            std::vector<std::string> items;
            if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                std::string list_str = trimmed.substr(lt + 1, gt - lt - 1);
                // Split by commas
                std::istringstream ss(list_str);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    items.push_back(trimWS(item));
                }
            }

            // Collect IRP body until ENDM
            i++;
            std::vector<std::string> irp_body;
            int nesting = 1;
            while (i < substituted.size()) {
                std::string bfw = firstWord(substituted[i]);
                if (bfw == "IRP" || bfw == "MACRO") nesting++;
                else if (bfw == "ENDM") {
                    nesting--;
                    if (nesting == 0) { i++; break; }
                }
                irp_body.push_back(substituted[i]);
                i++;
            }

            // Expand
            auto expanded = expandIRP(irp_body, var, items);
            result.insert(result.end(), expanded.begin(), expanded.end());
        } else {
            result.push_back(substituted[i]);
            i++;
        }
    }
    return result;
}

bool Assembler::expandMacros(std::vector<std::string>& lines,
                             std::vector<SourceOrigin>& origins) {
    macros_.clear();

    // Phase 1: Collect MACRO definitions and remove them from lines
    std::vector<std::string> stripped_lines;
    std::vector<SourceOrigin> stripped_origins;

    size_t i = 0;
    while (i < lines.size()) {
        // Check for: Name MACRO [params]
        std::string sw = secondWord(lines[i]);
        if (sw == "MACRO") {
            // First word is the macro name
            std::string trimmed = trimWS(stripComment(lines[i]));
            size_t sp = trimmed.find_first_of(" \t");
            std::string macro_name = (sp == std::string::npos) ? trimmed : trimmed.substr(0, sp);

            // Parse optional parameters (after MACRO keyword)
            MacroDef def;
            def.name = macro_name;

            // Find params after "MACRO" keyword
            std::string upper_trimmed;
            for (char c : trimmed) upper_trimmed += (char)toupper((unsigned char)c);
            size_t macro_kw = upper_trimmed.find("MACRO");
            if (macro_kw != std::string::npos) {
                size_t after_macro = macro_kw + 5;
                while (after_macro < trimmed.size() &&
                       (trimmed[after_macro] == ' ' || trimmed[after_macro] == '\t'))
                    after_macro++;
                if (after_macro < trimmed.size()) {
                    // Parse comma-separated param names
                    std::string params_str = trimmed.substr(after_macro);
                    std::istringstream ps(params_str);
                    std::string param;
                    while (std::getline(ps, param, ',')) {
                        std::string p = trimWS(param);
                        if (!p.empty()) def.params.push_back(p);
                    }
                }
            }

            // Collect body until ENDM
            i++;
            int nesting = 1;
            while (i < lines.size()) {
                std::string fw = firstWord(lines[i]);
                if (secondWord(lines[i]) == "MACRO") nesting++;
                else if (fw == "IRP") nesting++;
                else if (fw == "ENDM") {
                    nesting--;
                    if (nesting == 0) { i++; break; }
                }
                def.body.push_back(lines[i]);
                i++;
            }

            // Store macro (case-insensitive key)
            std::string key;
            for (char c : macro_name) key += (char)toupper((unsigned char)c);
            macros_[key] = def;
            continue;
        }

        // Check for standalone IRP (not inside a macro)
        std::string fw = firstWord(lines[i]);
        if (fw == "IRP") {
            std::string trimmed = trimWS(stripComment(lines[i]));
            std::string upper_trimmed;
            for (char c : trimmed) upper_trimmed += (char)toupper((unsigned char)c);
            size_t irp_pos = upper_trimmed.find("IRP");
            size_t after_irp = irp_pos + 3;
            while (after_irp < trimmed.size() &&
                   (trimmed[after_irp] == ' ' || trimmed[after_irp] == '\t'))
                after_irp++;

            size_t var_end = trimmed.find_first_of(" \t,", after_irp);
            std::string var = (var_end == std::string::npos)
                ? trimmed.substr(after_irp) : trimmed.substr(after_irp, var_end - after_irp);

            size_t lt = trimmed.find('<', var_end != std::string::npos ? var_end : after_irp);
            size_t gt = trimmed.find('>', lt != std::string::npos ? lt : 0);
            std::vector<std::string> items;
            if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                std::string list_str = trimmed.substr(lt + 1, gt - lt - 1);
                std::istringstream ss(list_str);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    items.push_back(trimWS(item));
                }
            }

            SourceOrigin orig_origin = (i < origins.size()) ? origins[i] : SourceOrigin{"", (int)i + 1};
            i++;
            std::vector<std::string> irp_body;
            int nesting = 1;
            while (i < lines.size()) {
                std::string bfw = firstWord(lines[i]);
                if (bfw == "IRP" || secondWord(lines[i]) == "MACRO") nesting++;
                else if (bfw == "ENDM") {
                    nesting--;
                    if (nesting == 0) { i++; break; }
                }
                irp_body.push_back(lines[i]);
                i++;
            }

            auto expanded = expandIRP(irp_body, var, items);
            for (const auto& eline : expanded) {
                stripped_lines.push_back(eline);
                stripped_origins.push_back(orig_origin);
            }
            continue;
        }

        stripped_lines.push_back(lines[i]);
        if (i < origins.size())
            stripped_origins.push_back(origins[i]);
        else
            stripped_origins.push_back({"", (int)i + 1});
        i++;
    }

    // Phase 2: Expand macro invocations (iteratively until no more expansions)
    bool changed = true;
    int max_iterations = 100; // prevent infinite recursion
    while (changed && max_iterations-- > 0) {
        changed = false;
        std::vector<std::string> new_lines;
        std::vector<SourceOrigin> new_origins;

        for (size_t j = 0; j < stripped_lines.size(); j++) {
            std::string fw = firstWord(stripped_lines[j]);
            // Check if this is a macro invocation
            auto it = macros_.find(fw);
            if (it != macros_.end() && !fw.empty()) {
                // Parse arguments
                std::string trimmed = trimWS(stripComment(stripped_lines[j]));
                // Skip past label if any
                size_t start = 0;
                {
                    size_t sp = trimmed.find_first_of(" \t");
                    if (sp != std::string::npos) {
                        std::string first = trimmed.substr(0, sp);
                        if (!first.empty() && first.back() == ':') {
                            start = sp;
                            while (start < trimmed.size() &&
                                   (trimmed[start] == ' ' || trimmed[start] == '\t'))
                                start++;
                        }
                    }
                }
                // Skip past macro name
                size_t name_end = trimmed.find_first_of(" \t,", start);
                std::vector<std::string> args;
                if (name_end != std::string::npos) {
                    std::string args_str = trimWS(trimmed.substr(name_end));
                    if (!args_str.empty()) {
                        std::istringstream as(args_str);
                        std::string arg;
                        while (std::getline(as, arg, ',')) {
                            std::string a = trimWS(arg);
                            if (!a.empty()) args.push_back(a);
                        }
                    }
                }

                auto expanded = expandMacroBody(it->second, args);
                SourceOrigin orig = (j < stripped_origins.size())
                    ? stripped_origins[j] : SourceOrigin{"", (int)j + 1};
                for (const auto& eline : expanded) {
                    new_lines.push_back(eline);
                    new_origins.push_back(orig);
                }
                changed = true;
            } else {
                new_lines.push_back(stripped_lines[j]);
                if (j < stripped_origins.size())
                    new_origins.push_back(stripped_origins[j]);
                else
                    new_origins.push_back({"", (int)j + 1});
            }
        }

        stripped_lines = std::move(new_lines);
        stripped_origins = std::move(new_origins);
    }

    lines = std::move(stripped_lines);
    origins = std::move(stripped_origins);
    return true;
}

bool Assembler::assemble(const std::string& source, std::vector<uint8_t>& output,
                         const std::string& source_file) {
    source_file_ = source_file;

    // Split source into lines
    std::vector<std::string> lines;
    std::istringstream iss(source);
    std::string line;
    while (std::getline(iss, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    // Expand INCLUDE directives (only when source_file is provided)
    if (!source_file_.empty()) {
        std::string canonical = canonicalizePath(source_file_);
        std::string base_dir = directoryOf(canonical);
        std::set<std::string> seen_files;
        seen_files.insert(canonical);

        std::vector<std::string> expanded_lines;
        std::vector<SourceOrigin> origins;
        if (!expandIncludes(lines, expanded_lines, origins,
                            base_dir, canonical, seen_files, 0)) {
            return false;
        }
        lines = std::move(expanded_lines);
        source_origins_ = std::move(origins);
    } else {
        // Build trivial origins (line N -> line N, no file)
        source_origins_.clear();
        for (int i = 0; i < (int)lines.size(); i++) {
            source_origins_.push_back({"", i + 1});
        }
    }

    // Expand MACRO/IRP directives
    if (!expandMacros(lines, source_origins_)) {
        return false;
    }

    // Pass 1: build symbol table, calculate sizes
    if (!pass1(lines)) {
        return false;
    }

    // Pass 2: generate code
    output.clear();
    if (!pass2(lines, output)) {
        return false;
    }

    // Strip origin offset — .COM files start at offset 0 but addresses from ORG 100h
    // The output should start at the first byte after ORG
    // Actually, we need to remove the leading empty bytes from RESB/RESW before ORG
    // Our approach: output contains all bytes from address origin_ onward
    // But we've been building from 0... let me re-think.
    // Actually ORG sets current_addr but we emit bytes sequentially.
    // For .COM format, the binary starts at address 100h — the bytes in output
    // are already the correct content starting from the ORG address.

    return true;
}
