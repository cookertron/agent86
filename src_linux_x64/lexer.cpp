#include "lexer.h"
#include <algorithm>
#include <cctype>

std::string Lexer::toUpper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

bool Lexer::isRegister(const std::string& name) {
    std::string u = toUpper(name);
    for (auto& r : REG_TABLE)
        if (u == r.name) return true;
    // Segment registers
    for (auto& s : SREG_TABLE)
        if (u == s.name) return true;
    return false;
}

bool Lexer::isMnemonic(const std::string& name) {
    static const char* mnemonics[] = {
        "MOV","ADD","ADC","SUB","SBB","AND","OR","XOR","NOT","NEG","INC","DEC",
        "CMP","TEST","SHL","SHR","SAR","ROL","ROR","RCL","RCR",
        "MUL","IMUL","DIV","IDIV",
        "PUSH","POP","PUSHA","POPA","PUSHF","POPF",
        "JMP","JZ","JNZ","JB","JAE","JNS","JNC","JCXZ",
        "JE","JNE","JBE","JA","JL","JLE","JG","JGE",
        "JO","JNO","JP","JPE","JNP","JPO","JS",
        "JNAE","JNB","JNBE","JNGE","JNL","JNG","JNLE","JNA","JC",
        "LOOP","LOOPE","LOOPZ","LOOPNE","LOOPNZ",
        "CALL","RET","RETF","IRET","INT","INTO",
        "LEA","LDS","LES","XCHG","XLAT","XLATB",
        "CBW","CWD",
        "CLD","CLC","STC","STD","CLI","STI","CMC",
        "LAHF","SAHF","NOP","HLT","WAIT",
        "IN","OUT",
        "REP","REPE","REPZ","REPNE","REPNZ",
        "MOVSB","MOVSW","STOSB","STOSW",
        "LODSB","LODSW","CMPSB","CMPSW","SCASB","SCASW",
        "DAA","DAS","AAA","AAS","AAM","AAD",
        "LOCK",
        nullptr
    };
    std::string u = toUpper(name);
    for (int i = 0; mnemonics[i]; i++)
        if (u == mnemonics[i]) return true;
    return false;
}

bool Lexer::isDirective(const std::string& name) {
    static const char* dirs[] = {
        "DB","DW","RESB","RESW","EQU","ORG","PROC","ENDP","INCLUDE",
        "SECTION",
        "TRACE_START","TRACE_STOP","BREAKPOINT",
        "ASSERT","HEX_START","HEX_END","PRINT","ASSERT_EQ","SCREEN","VRAMOUT","REGS",
        "LOG","LOG_ONCE","DOS_FAIL","DOS_PARTIAL",
        "MEM_SNAPSHOT","MEM_ASSERT", nullptr
    };
    std::string u = toUpper(name);
    for (int i = 0; dirs[i]; i++)
        if (u == dirs[i]) return true;
    return false;
}

bool Lexer::isSizeKeyword(const std::string& name) {
    std::string u = toUpper(name);
    return u == "BYTE" || u == "WORD";
}

Token Lexer::readNumber(const std::string& line, size_t& pos) {
    Token t;
    t.type = TokenType::NUMBER;
    size_t start = pos;

    // Collect alphanumeric + 'x' for hex prefix
    std::string num;
    while (pos < line.size() && (isalnum((unsigned char)line[pos]))) {
        num += line[pos++];
    }

    std::string upper = toUpper(num);
    t.text = num;

    // 0xNN hex
    if (upper.size() > 2 && upper[0] == '0' && upper[1] == 'X') {
        t.numval = std::stoll(upper.substr(2), nullptr, 16);
    }
    // NNNNh hex
    else if (upper.back() == 'H') {
        t.numval = std::stoll(upper.substr(0, upper.size()-1), nullptr, 16);
    }
    // NNNNb binary
    else if (upper.back() == 'B' && upper.find_first_not_of("01bB") == std::string::npos) {
        // Check it's actually binary (all 0s and 1s before 'b')
        bool is_binary = true;
        for (size_t i = 0; i < upper.size()-1; i++) {
            if (upper[i] != '0' && upper[i] != '1') { is_binary = false; break; }
        }
        if (is_binary) {
            t.numval = std::stoll(upper.substr(0, upper.size()-1), nullptr, 2);
        } else {
            // It's a hex number ending in B (like 0FB)
            t.numval = std::stoll(upper.substr(0, upper.size()-1), nullptr, 16);
        }
    }
    // Decimal
    else {
        t.numval = std::stoll(num, nullptr, 10);
    }

    return t;
}

Token Lexer::readString(const std::string& line, size_t& pos) {
    Token t;
    char quote = line[pos++]; // skip opening quote
    std::string val;
    while (pos < line.size() && line[pos] != quote) {
        val += line[pos++];
    }
    if (pos < line.size()) pos++; // skip closing quote

    if (val.size() == 1) {
        t.type = TokenType::CHAR_LITERAL;
        t.numval = (unsigned char)val[0];
    } else {
        t.type = TokenType::STRING;
    }
    t.text = val;
    return t;
}

Token Lexer::readIdentifier(const std::string& line, size_t& pos) {
    Token t;
    size_t start = pos;
    // Allow leading dot for local labels
    if (pos < line.size() && line[pos] == '.') pos++;
    while (pos < line.size() && (isalnum((unsigned char)line[pos]) || line[pos] == '_')) {
        pos++;
    }
    t.text = line.substr(start, pos - start);

    std::string upper = toUpper(t.text);

    // Check if followed by colon → label (but not segment override like ES:[...])
    if (pos < line.size() && line[pos] == ':') {
        // Segment override: ES:[...] — emit as REGISTER, don't consume colon
        if (isRegister(t.text)) {
            std::string ru = toUpper(t.text);
            bool is_sreg = (ru == "ES" || ru == "CS" || ru == "SS" || ru == "DS");
            if (is_sreg) {
                // Look ahead past colon for '[' (skip whitespace)
                size_t peek = pos + 1;
                while (peek < line.size() && (line[peek] == ' ' || line[peek] == '\t')) peek++;
                if (peek < line.size() && line[peek] == '[') {
                    t.type = TokenType::REGISTER;
                    // Don't consume colon — it stays for the parser
                    return t;
                }
            }
        }
        t.type = TokenType::LABEL;
        pos++; // consume colon
    } else if (isRegister(t.text)) {
        t.type = TokenType::REGISTER;
    } else if (isSizeKeyword(t.text)) {
        t.type = TokenType::SIZE_KEYWORD;
        t.size = (upper == "BYTE") ? OpSize::BYTE : OpSize::WORD;
    } else if (isDirective(t.text)) {
        t.type = TokenType::MNEMONIC; // treat directives as mnemonics initially
    } else if (isMnemonic(t.text)) {
        t.type = TokenType::MNEMONIC;
    } else {
        // Symbol name (label reference, EQU name, etc.)
        t.type = TokenType::NUMBER; // will be resolved by expression evaluator
        t.numval = -1; // sentinel: needs symbol resolution
    }
    return t;
}

std::vector<Token> Lexer::tokenize(const std::string& line) {
    std::vector<Token> tokens;
    size_t pos = 0;

    // Strip comments
    std::string stripped = line;
    // Find semicolon not inside quotes
    bool in_quote = false;
    for (size_t i = 0; i < stripped.size(); i++) {
        if (stripped[i] == '\'' || stripped[i] == '"') in_quote = !in_quote;
        if (stripped[i] == ';' && !in_quote) {
            stripped = stripped.substr(0, i);
            break;
        }
    }

    while (pos < stripped.size()) {
        // Skip whitespace
        while (pos < stripped.size() && isspace((unsigned char)stripped[pos])) pos++;
        if (pos >= stripped.size()) break;

        char c = stripped[pos];

        if (c == ',') { tokens.push_back({TokenType::COMMA, ","}); pos++; }
        else if (c == '[') { tokens.push_back({TokenType::OPEN_BRACKET, "["}); pos++; }
        else if (c == ']') { tokens.push_back({TokenType::CLOSE_BRACKET, "]"}); pos++; }
        else if (c == '+') { tokens.push_back({TokenType::PLUS, "+"}); pos++; }
        else if (c == '-') { tokens.push_back({TokenType::MINUS, "-"}); pos++; }
        else if (c == '*') { tokens.push_back({TokenType::STAR, "*"}); pos++; }
        else if (c == '/') { tokens.push_back({TokenType::SLASH, "/"}); pos++; }
        else if (c == ':') { tokens.push_back({TokenType::COLON, ":"}); pos++; }
        else if (c == '$') { tokens.push_back({TokenType::DOLLAR, "$"}); pos++; }
        else if (c == '(') { tokens.push_back({TokenType::OPEN_PAREN, "("}); pos++; }
        else if (c == ')') { tokens.push_back({TokenType::CLOSE_PAREN, ")"}); pos++; }
        else if (c == '%') { tokens.push_back({TokenType::PERCENT, "%"}); pos++; }
        else if (c == '&') { tokens.push_back({TokenType::AMPERSAND, "&"}); pos++; }
        else if (c == '|') { tokens.push_back({TokenType::PIPE, "|"}); pos++; }
        else if (c == '^') { tokens.push_back({TokenType::CARET, "^"}); pos++; }
        else if (c == '~') { tokens.push_back({TokenType::TILDE, "~"}); pos++; }
        else if (c == '<' && pos + 1 < stripped.size() && stripped[pos+1] == '<') {
            tokens.push_back({TokenType::SHL, "<<"}); pos += 2;
        }
        else if (c == '>' && pos + 1 < stripped.size() && stripped[pos+1] == '>') {
            tokens.push_back({TokenType::SHR, ">>"}); pos += 2;
        }
        else if (c == '\'' || c == '"') {
            tokens.push_back(readString(stripped, pos));
        }
        else if (isdigit((unsigned char)c)) {
            tokens.push_back(readNumber(stripped, pos));
        }
        else if (isalpha((unsigned char)c) || c == '_' || c == '.') {
            tokens.push_back(readIdentifier(stripped, pos));
        }
        else {
            pos++; // skip unknown
        }
    }

    tokens.push_back({TokenType::EOL, ""});
    return tokens;
}
