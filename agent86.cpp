#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <cstdio>
#include <cctype>
#include <set>
#include <filesystem>

#define AGENT86_VERSION "0.9.1"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace std;

// --- Windows Output Encoding Fix (Manual Declaration to avoid pollution) ---
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
#define CP_UTF8 65001
#endif

// --- Agentic Protocol Definitions ---

struct Diagnostic {
    string level;       // "ERROR", "WARNING", "INFO"
    int line;
    string message;
    string hint;        // Agent-specific context (e.g., "Range: -128 to 127")
};

struct BinaryMap {
    int address;
    int sourceLine;
    vector<uint8_t> bytes;
    string sourceCode;
    // --- New Fields ---
    int size;           // The physical size in bytes
    string decoded;     // e.g., "MOV REG(AX), IMM(5)" - How we understood it
};

struct AssemblerState {
    bool success;
    vector<Diagnostic> diagnostics;
    map<string, int> symbols;
    vector<BinaryMap> listing; // The visual "debug" view
};

// --- Help System ---

struct HelpEntry {
    string topic, group, brief, content;
    vector<string> related;
    vector<string> examples;
};

// --- Lexer Types ---
enum class TokenType {
    LabelDef, // label:
    Identifier, // MOV, AX, msg
    Number,   // 100h, 9
    String,   // 'Hello'
    Comma,    // ,
    Plus,     // +
    Minus,    // -
    Star,     // *
    Slash,    // /
    LParen,   // (
    RParen,   // )
    LBracket, // [
    RBracket, // ]
    Colon,    // :
    Unknown
};

struct Token {
    TokenType type;
    string value;
    int line;
};

// --- Assembler Context ---
struct SymbolInfo {
    int value;
    bool isConstant; // true = EQU, false = Label
    int definedLine;
};

struct StructField {
    string name;                   // uppercase field name
    int offset;                    // byte offset within struct
    int size;                      // size in bytes
    vector<uint8_t> defaultValue;  // default bytes, little-endian
};

struct StructDef {
    string name;                   // uppercase struct name
    vector<StructField> fields;
    int totalSize;
    int definedLine;
};

struct AssemblerContext {
    AssemblerState agentState;
    vector<uint8_t> currentLineBytes;
    map<string, SymbolInfo> symbolTable;
    int currentAddress = 0;
    vector<uint8_t> machineCode;
    bool isPass1 = true;
    string currentProcedureName = "";
    bool globalError = false;
    bool encounteredSymbol = false; // NEW: Track if current expression involved a symbol
    // STRUC support
    map<string, StructDef> structDefs;
    string currentStructName = "";
    int currentStructOffset = 0;
    vector<StructField> currentStructFields;
    // Jcc auto-promotion: set of source line numbers whose Jcc must be promoted to near
    set<int> promotedJumps;
    bool detectPromotions = false; // only true after first pass 1 establishes symbol table
};

// --- Source Location Tracking (for INCLUDE directive) ---

struct SourceLocation {
    string file;  // path of source file
    int line;     // 1-based line number within that file
};

static const int MAX_INCLUDE_DEPTH = 16;

// --- Helper Functions ---

void logError(AssemblerContext& ctx, int line, const string& msg, const string& hint = "") {
    ctx.agentState.diagnostics.push_back({ "ERROR", line, msg, hint });
    ctx.globalError = true;
}

void logWarning(AssemblerContext& ctx, int line, const string& msg, const string& hint = "") {
    ctx.agentState.diagnostics.push_back({ "WARNING", line, msg, hint });
}

string jsonEscape(const string& s) {
    string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c >= 0x7F) {
            // Escape all non-printable and non-ASCII bytes as \u00XX
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04X", c);
            out += buf;
        }
        else out += (char)c;
    }
    return out;
}

string toUpper(string s) {
    transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// --- ISA Knowledge Base ---

struct OperandRule {
    string type1; // e.g., "REG8", "MEM16", "IMM"
    string type2; // e.g., "REG8", "IMM", "NONE"
    string constraints; // e.g., "Sizes must match"
};

struct ISAEntry {
    string mnemonic;
    string description;
    vector<OperandRule> validForms;
};

// The Knowledge Base
// WARNING: This database MUST be kept in sync with the assembleLine() function.
// If you add support for a new instruction in assembleLine, you MUST add it here.
const vector<ISAEntry> isaDB = {
    { "MOV", "Move Data",  { { "REG", "REG", "" }, { "REG", "IMM", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "MEM", "IMM", "" }, { "REG", "SREG", "" }, { "SREG", "REG", "" } }},
    { "ADD", "Arithmetic Add", { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "SUB", "Arithmetic Sub", { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "CMP", "Compare", { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "AND", "Logical AND", { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "OR",  "Logical OR",  { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "XOR", "Logical XOR", { { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    { "TEST","Logical TEST",{ { "REG", "REG", "" }, { "REG", "MEM", "" }, { "MEM", "REG", "" }, { "REG", "IMM", "" }, { "MEM", "IMM", "" } }},
    
    { "INC", "Increment", { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    { "DEC", "Decrement", { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    { "NOT", "One's Compl", { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    { "NEG", "Two's Compl", { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    
    { "MUL", "Unsigned Multiply", { { "REG", "NONE", "Accumulator * Src" }, { "MEM", "NONE", "" } }},
    { "IMUL","Signed Multiply",   { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    { "DIV", "Unsigned Divide",   { { "REG", "NONE", "Accumulator / Src" }, { "MEM", "NONE", "" } }},
    { "IDIV","Signed Divide",     { { "REG", "NONE", "" }, { "MEM", "NONE", "" } }},
    
    { "SHL", "Shift Left",  { { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "SHR", "Shift Right", { { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "ROL", "Rotate Left", { { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "ROR", "Rotate Right",{ { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "RCL", "Rotate thru Carry Left", { { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "RCR", "Rotate thru Carry Right",{ { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    { "SAR", "Shift Arith Right", { { "REG", "1", "8086" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "8086" }, { "MEM", "1", "8086" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "8086" } }},
    
    { "PUSH","Push to Stack",{ { "REG16", "NONE", "" }, { "MEM16", "NONE", "" }, { "SEG", "NONE", "" } }},
    { "POP", "Pop from Stack",{ { "REG16", "NONE", "" }, { "MEM16", "NONE", "" }, { "SEG", "NONE", "" } }},
    
    { "IN",  "Input from Port", { { "AL/AX", "IMM", "Fixed port" }, { "AL/AX", "DX", "Variable port" } }},
    { "OUT", "Output to Port",  { { "IMM", "AL/AX", "Fixed port" }, { "DX", "AL/AX", "Variable port" } }},
    
    { "LEA", "Load Eff. Addr", { { "REG16", "MEM", "" } }},
    
    { "JMP", "Unconditional Jump", { { "LABEL", "NONE", "Short/Near" }, { "IMM", "NONE", "Abs" }, { "REG16", "NONE", "Indirect" }, { "MEM", "NONE", "Indirect" } }},
    { "JZ",  "Jump if Zero",         { { "LABEL", "NONE", "Short only (-128 to +127)" } }},
    { "JE",  "Jump if Equal",         { { "LABEL", "NONE", "Short only" } }},
    { "JNZ", "Jump if Not Zero",      { { "LABEL", "NONE", "Short only" } }},
    { "JNE", "Jump if Not Equal",     { { "LABEL", "NONE", "Short only" } }},
    { "JL",  "Jump if Less (signed)",        { { "LABEL", "NONE", "Short only" } }},
    { "JNGE","Jump if Not Greater/Equal",    { { "LABEL", "NONE", "Short only" } }},
    { "JG",  "Jump if Greater (signed)",     { { "LABEL", "NONE", "Short only" } }},
    { "JNLE","Jump if Not Less/Equal",       { { "LABEL", "NONE", "Short only" } }},
    { "JLE", "Jump if Less/Equal (signed)",  { { "LABEL", "NONE", "Short only" } }},
    { "JNG", "Jump if Not Greater",          { { "LABEL", "NONE", "Short only" } }},
    { "JGE", "Jump if Greater/Equal (signed)",{ { "LABEL", "NONE", "Short only" } }},
    { "JNL", "Jump if Not Less",             { { "LABEL", "NONE", "Short only" } }},
    { "JA",  "Jump if Above (unsigned)",     { { "LABEL", "NONE", "Short only" } }},
    { "JNBE","Jump if Not Below/Equal",      { { "LABEL", "NONE", "Short only" } }},
    { "JB",  "Jump if Below (unsigned)",     { { "LABEL", "NONE", "Short only" } }},
    { "JNAE","Jump if Not Above/Equal",      { { "LABEL", "NONE", "Short only" } }},
    { "JAE", "Jump if Above/Equal (unsigned)",{ { "LABEL", "NONE", "Short only" } }},
    { "JNB", "Jump if Not Below",            { { "LABEL", "NONE", "Short only" } }},
    { "JBE", "Jump if Below/Equal (unsigned)",{ { "LABEL", "NONE", "Short only" } }},
    { "JNA", "Jump if Not Above",            { { "LABEL", "NONE", "Short only" } }},
    { "JC",  "Jump if Carry",         { { "LABEL", "NONE", "Short only" } }},
    { "JNC", "Jump if No Carry",      { { "LABEL", "NONE", "Short only" } }},
    { "JS",  "Jump if Sign",          { { "LABEL", "NONE", "Short only" } }},
    { "JNS", "Jump if No Sign",       { { "LABEL", "NONE", "Short only" } }},
    { "JO",  "Jump if Overflow",      { { "LABEL", "NONE", "Short only" } }},
    { "JNO", "Jump if No Overflow",   { { "LABEL", "NONE", "Short only" } }},
    { "JP",  "Jump if Parity (even)", { { "LABEL", "NONE", "Short only" } }},
    { "JPE", "Jump if Parity Even",   { { "LABEL", "NONE", "Short only" } }},
    { "JNP", "Jump if No Parity (odd)",{ { "LABEL", "NONE", "Short only" } }},
    { "JPO", "Jump if Parity Odd",    { { "LABEL", "NONE", "Short only" } }},
    { "LOOP","Loop CX times",      { { "LABEL", "NONE", "Short only" } }},
    { "LOOPE","Loop if Equal",     { { "LABEL", "NONE", "Short only" } }},
    { "LOOPZ","Loop if Zero",      { { "LABEL", "NONE", "Short only" } }},
    { "LOOPNE","Loop if Not Equal",{ { "LABEL", "NONE", "Short only" } }},
    { "LOOPNZ","Loop if Not Zero", { { "LABEL", "NONE", "Short only" } }},
    { "JCXZ", "Jump if CX Zero",   { { "LABEL", "NONE", "Short only" } }},
    
    { "MOVSB","Move String Byte",   { { "NONE", "NONE", "DS:[SI] -> ES:[DI]" } }},
    { "MOVSW","Move String Word",   { { "NONE", "NONE", "DS:[SI] -> ES:[DI]" } }},
    { "CMPSB","Compare String Byte",{ { "NONE", "NONE", "DS:[SI] - ES:[DI]" } }},
    { "CMPSW","Compare String Word",{ { "NONE", "NONE", "DS:[SI] - ES:[DI]" } }},
    { "STOSB","Store String Byte",  { { "NONE", "NONE", "AL -> ES:[DI]" } }},
    { "STOSW","Store String Word",  { { "NONE", "NONE", "AX -> ES:[DI]" } }},
    { "LODSB","Load String Byte",   { { "NONE", "NONE", "DS:[SI] -> AL" } }},
    { "LODSW","Load String Word",   { { "NONE", "NONE", "DS:[SI] -> AX" } }},
    { "SCASB","Scan String Byte",   { { "NONE", "NONE", "AL - ES:[DI]" } }},
    { "SCASW","Scan String Word",   { { "NONE", "NONE", "AX - ES:[DI]" } }},
    
    { "CALL","Call Procedure", { { "LABEL", "NONE", "Near" }, { "IMM", "NONE", "Abs" }, { "REG16", "NONE", "Indirect" }, { "MEM", "NONE", "Indirect" } }},
    { "RET", "Return", { { "NONE", "NONE", "" } }},
    { "INT", "Interrupt", { { "IMM", "NONE", "0-255" } }},
    
    { "CLD", "Clear Dir Flag", { { "NONE", "NONE", "" } }},
    { "STD", "Set Dir Flag",   { { "NONE", "NONE", "" } }},
    { "CLI", "Clear Int Flag", { { "NONE", "NONE", "" } }},
    { "STI", "Set Int Flag",   { { "NONE", "NONE", "" } }},
    { "CMC", "Compl Carry",    { { "NONE", "NONE", "" } }},
    { "CLC", "Clear Carry",    { { "NONE", "NONE", "" } }},
    { "STC", "Set Carry",      { { "NONE", "NONE", "" } }},

    { "NOP", "No Operation",   { { "NONE", "NONE", "" } }},
    { "XCHG","Exchange",       { { "REG", "REG", "Same size" }, { "REG", "MEM", "" }, { "MEM", "REG", "" } }},
    { "CBW", "Byte to Word",   { { "NONE", "NONE", "Sign-extend AL into AX" } }},
    { "CWD", "Word to DWord",  { { "NONE", "NONE", "Sign-extend AX into DX:AX" } }},
    { "LAHF","Load Flags to AH",{ { "NONE", "NONE", "" } }},
    { "SAHF","Store AH to Flags",{ { "NONE", "NONE", "" } }},
    { "PUSHF","Push Flags",    { { "NONE", "NONE", "" } }},
    { "POPF","Pop Flags",      { { "NONE", "NONE", "" } }},
    { "XLAT", "Table Lookup",  { { "NONE", "NONE", "AL = DS:[BX + AL]" } }},
    { "HLT",  "Halt CPU",     { { "NONE", "NONE", "" } }},
    { "PUSHA","Push All Regs", { { "NONE", "NONE", "80186+" } }},
    { "POPA", "Pop All Regs",  { { "NONE", "NONE", "80186+" } }},
    { "SAL", "Shift Arith Left", { { "REG", "1", "= SHL" }, { "REG", "IMM", "186+ only" }, { "REG", "CL", "= SHL" }, { "MEM", "1", "= SHL" }, { "MEM", "IMM", "186+ only" }, { "MEM", "CL", "= SHL" } }},
};

void printInstructionHelp(const string& targetMnemonic) {
    string search = toUpper(targetMnemonic);
    bool found = false;
    
    cout << "{ \"mnemonic\": \"" << search << "\", \"forms\": [";
    
    for (const auto& entry : isaDB) {
        if (entry.mnemonic == search) {
            found = true;
            for (size_t i = 0; i < entry.validForms.size(); ++i) {
                const auto& form = entry.validForms[i];
                cout << "{ \"op1\": \"" << form.type1 << "\", "
                     << "\"op2\": \"" << form.type2 << "\", "
                     << "\"notes\": \"" << form.constraints << "\" }";
                if (i < entry.validForms.size() - 1) cout << ",";
            }
        }
    }
    cout << "], \"found\": " << (found ? "true" : "false") << " }" << endl;
}

bool isRegister(const string& s, int& regCode, int& size) {
    string u = toUpper(s);
    static map<string, pair<int, int>> regs = {
        {"AL", {0, 8}}, {"CL", {1, 8}}, {"DL", {2, 8}}, {"BL", {3, 8}},
        {"AH", {4, 8}}, {"CH", {5, 8}}, {"DH", {6, 8}}, {"BH", {7, 8}},
        {"AX", {0, 16}}, {"CX", {1, 16}}, {"DX", {2, 16}}, {"BX", {3, 16}},
        {"SP", {4, 16}}, {"BP", {5, 16}}, {"SI", {6, 16}}, {"DI", {7, 16}}
    };
    if (regs.count(u)) {
        regCode = regs[u].first;
        size = regs[u].second;
        return true;
    }
    return false;
}

int parseNumber(string s, bool& ok, string& reason) {
    ok = true;
    reason = "";
    if (s.empty()) { ok = false; reason = "Empty numeric literal."; return 0; }
    string original = s;
    string u = toUpper(s);
    int base = 10;
    string baseLabel = "decimal";

    // Check suffixes first
    char suffix = u.back();
    if (suffix == 'H') {
        base = 16; baseLabel = "hex";
        s.pop_back(); u.pop_back();
    } else if (suffix == 'B') {
        base = 2; baseLabel = "binary";
        s.pop_back(); u.pop_back();
    } else if (suffix == 'O' || suffix == 'Q') {
        base = 8; baseLabel = "octal";
        s.pop_back(); u.pop_back();
    } else if (suffix == 'D') {
        base = 10; baseLabel = "decimal";
        s.pop_back(); u.pop_back();
    } else {
        // Check prefixes if no suffix
        if (u.size() > 2 && u.substr(0, 2) == "0X") {
            base = 16; baseLabel = "hex";
            s = s.substr(2); u = u.substr(2);
        } else if (u.size() > 2 && u.substr(0, 2) == "0B") {
            base = 2; baseLabel = "binary";
            s = s.substr(2); u = u.substr(2);
        }
    }

    if (s.empty()) {
        ok = false;
        reason = "Numeric prefix with no digits following in '" + original + "'.";
        return 0;
    }

    // Validate digits before calling stoi for better diagnostics
    for (size_t i = 0; i < u.size(); i++) {
        char c = u[i];
        if (base == 2 && c != '0' && c != '1') {
            ok = false;
            reason = "Binary literal '" + original + "' contains non-binary digit '" + string(1, c) + "'. Valid binary digits: 0, 1.";
            return 0;
        }
        if (base == 8 && (c < '0' || c > '7')) {
            ok = false;
            reason = "Octal literal '" + original + "' contains non-octal digit '" + string(1, c) + "'. Valid octal digits: 0-7.";
            return 0;
        }
        if (base == 16 && !isxdigit(c)) {
            ok = false;
            reason = "Hex literal '" + original + "' contains non-hex character '" + string(1, c) + "'. Valid hex digits: 0-9, A-F.";
            return 0;
        }
        if (base == 10 && !isdigit(c)) {
            ok = false;
            reason = "Decimal literal '" + original + "' contains non-digit character '" + string(1, c) + "'.";
            return 0;
        }
    }

    try {
        size_t pos = 0;
        long long result = stoll(s, &pos, base);
        if (pos != s.size()) {
            ok = false;
            reason = "Not all characters consumed in '" + original + "'.";
            return 0;
        }
        if (result > 65535 || result < -32768) {
            ok = false;
            reason = "Numeric literal '" + original + "' overflows. Maximum value is 65535 (FFFFh) for 16-bit.";
            return 0;
        }
        return (int)result;
    } catch (...) {
        ok = false;
        reason = "Invalid " + baseLabel + " literal '" + original + "'.";
        return 0;
    }
}

// Backward-compatible overload
int parseNumber(string s, bool& ok) {
    string reason;
    return parseNumber(s, ok, reason);
}

// --- Lexer ---
vector<Token> tokenize(const string& line, int lineNum) {
    vector<Token> tokens;
    string cur;
    bool inString = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inString) {
            if (c == '\'') {
                inString = false;
                tokens.push_back({TokenType::String, cur, lineNum});
                cur = "";
            } else {
                cur += c;
            }
            continue;
        }
        if (c == ';') break;
        if (c == '\'') { inString = true; continue; }
        
        if (c == '[' || c == ']' || c == ',' || c == ':' || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
            if (!cur.empty()) {
                tokens.push_back({TokenType::Identifier, cur, lineNum});
                cur = "";
            }
            if (c != ' ') { 
                string s(1, c);
                TokenType type = TokenType::Unknown;
                if (c == ',') type = TokenType::Comma;
                else if (c == '+') type = TokenType::Plus;
                else if (c == '-') type = TokenType::Minus;
                else if (c == '*') type = TokenType::Star;
                else if (c == '/') type = TokenType::Slash;
                else if (c == '(') type = TokenType::LParen;
                else if (c == ')') type = TokenType::RParen;
                else if (c == '[') type = TokenType::LBracket;
                else if (c == ']') type = TokenType::RBracket;
                else if (c == ':') type = TokenType::Colon;

                if (type == TokenType::Colon) {
                    if (!tokens.empty() && tokens.back().type == TokenType::Identifier) {
                        tokens.back().type = TokenType::LabelDef;
                    } else {
                        tokens.push_back({type, s, lineNum});
                    }
                } else {
                    tokens.push_back({type, s, lineNum}); 
                }
            }
            continue;
        }

        if (isspace(c)) {
            if (!cur.empty()) {
                 tokens.push_back({TokenType::Identifier, cur, lineNum});
                 cur = "";
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) tokens.push_back({TokenType::Identifier, cur, lineNum});

    for (auto& t : tokens) {
        if (t.type == TokenType::Identifier && isdigit(t.value[0])) t.type = TokenType::Number;
    }
    return tokens;
}



// --- Diagnostic Helpers (needed by parseExpression) ---

int editDistance(const string& a, const string& b) {
    int m = (int)a.size(), n = (int)b.size();
    vector<vector<int>> dp(m + 1, vector<int>(n + 1));
    for (int i = 0; i <= m; i++) dp[i][0] = i;
    for (int j = 0; j <= n; j++) dp[0][j] = j;
    for (int i = 1; i <= m; i++) {
        for (int j = 1; j <= n; j++) {
            if (toupper(a[i-1]) == toupper(b[j-1])) dp[i][j] = dp[i-1][j-1];
            else dp[i][j] = 1 + min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
        }
    }
    return dp[m][n];
}

string findClosestSymbol(const map<string, SymbolInfo>& table, const string& target, int maxDist = 2) {
    string best;
    int bestDist = maxDist + 1;
    for (const auto& kv : table) {
        int d = editDistance(kv.first, target);
        if (d > 0 && d < bestDist) {
            bestDist = d;
            best = kv.first;
        }
    }
    return best;
}

// --- Expression Parser ---

int parseExpression(AssemblerContext& ctx, const vector<Token>& tokens, int& idx, int minPrec = 0, bool insideBrackets = false) {
    if (idx >= tokens.size()) return 0;

    int lhs = 0;
    
    // Unary operators
    if (tokens[idx].type == TokenType::Plus || tokens[idx].type == TokenType::Minus) {
        TokenType opType = tokens[idx].type;
        idx++;
        int val = parseExpression(ctx, tokens, idx, 100, insideBrackets); // High precedence for unary
        if (opType == TokenType::Minus) lhs = -val;
        else lhs = val;
    } else if (tokens[idx].type == TokenType::LParen) {
        idx++;
        lhs = parseExpression(ctx, tokens, idx, 0, insideBrackets);
        if (idx < tokens.size() && tokens[idx].type == TokenType::RParen) idx++;
        else logError(ctx, tokens[idx-1].line, "Expected ')'", "Check for unmatched parentheses in your expression.");
    } else if (tokens[idx].type == TokenType::Number) {
        bool numOk;
        string numReason;
        lhs = parseNumber(tokens[idx].value, numOk, numReason);
        if (!numOk) logError(ctx, tokens[idx].line, "Invalid numeric literal: " + tokens[idx].value, numReason);
        idx++;
    } else if (tokens[idx].type == TokenType::Identifier && (isalpha(tokens[idx].value[0]) || tokens[idx].value[0] == '.' || tokens[idx].value[0] == '_' || tokens[idx].value[0] == '?')) { // Label, $, or ??xxxx macro-local
        if (tokens[idx].value == "$") {
             lhs = ctx.currentAddress;
         } else {
             string label = tokens[idx].value;
             // Handle local labels
             if (label[0] == '.') {
                 if (!ctx.currentProcedureName.empty()) label = ctx.currentProcedureName + label;
             }
             
             string uml = toUpper(label);
             ctx.encounteredSymbol = true; // Mark that we used a symbol

             if (ctx.symbolTable.count(uml)) {
                 lhs = ctx.symbolTable[uml].value;
             } else {
                 if (!ctx.isPass1) {
                     string msg = "Undefined label " + uml;
                     string hint = "";
                     // Check if it looks like a hex number (ends in H, all hex digits)
                     if (uml.size() > 1 && uml.back() == 'H') {
                         bool isHex = true;
                         for(size_t k=0; k<uml.size()-1; ++k) {
                             if (!isxdigit(uml[k])) { isHex = false; break; }
                         }
                         if (isHex) hint = "Did you mean 0" + uml + "? Hex literals starting with A-F must be prefixed with 0.";
                     }
                     // Check if it's a register name used in expression
                     if (hint.empty()) {
                         int dummyReg, dummySize;
                         if (isRegister(uml, dummyReg, dummySize)) {
                             hint = "'" + uml + "' is a register, not a label. Registers cannot be used in expressions directly.";
                         }
                     }
                     // Check if local label used outside PROC
                     if (hint.empty() && !uml.empty() && uml[0] == '.' && ctx.currentProcedureName.empty()) {
                         hint = "Local label '" + uml + "' used outside any PROC. Wrap your code in PROC/ENDP, or use a global label.";
                     }
                     // Fuzzy match against symbol table
                     if (hint.empty()) {
                         string closest = findClosestSymbol(ctx.symbolTable, uml);
                         if (!closest.empty()) {
                             hint = "Did you mean '" + closest + "'?";
                             if (ctx.symbolTable.count(closest)) {
                                 hint += " (defined at line " + to_string(ctx.symbolTable.at(closest).definedLine) + ")";
                             }
                         }
                     }
                     logError(ctx, tokens[idx].line, msg, hint);
                 }
                 lhs = 0;
             }
        }
        idx++;
    } else if (tokens[idx].type == TokenType::String) {
        if (tokens[idx].value.size() > 0) lhs = (uint8_t)tokens[idx].value[0];
        idx++;
    } else {
        // Assume 0 if unknown (or let it fail later?)
        // For registers in expression? No, registers are handled by parseOperand before expression.
        // If we hit a register token here, it might be part of [BX+SI+Disp] logic which is separate?
        // Actually, parseOperand handles [ ... ] specially.
        // But what about MOV AX, BX+2 ? Invalid.
        // So we just return 0.
        if (!ctx.isPass1) {
            string tok = tokens[idx].value;
            string upper = toUpper(tok);
            string hint;
            int dummyReg, dummySize;
            if (isRegister(upper, dummyReg, dummySize)) {
                hint = "'" + tok + "' is a register and cannot appear in an arithmetic expression. "
                       "If you meant a memory operand, use [" + tok + "]. "
                       "If you meant the value in the register, this must be computed at runtime, not assembly time.";
            } else if (upper == "DB" || upper == "DW" || upper == "DD" || upper == "EQU" ||
                       upper == "PROC" || upper == "ENDP" || upper == "ORG" ||
                       upper == "STRUC" || upper == "STRUCT") {
                hint = "'" + tok + "' is a directive and cannot be used as a value in an expression.";
            } else if (tok == "[" || tok == "]") {
                hint = "Brackets indicate a memory operand and cannot appear inside an arithmetic expression.";
            } else {
                hint = "'" + tok + "' is not a recognized number, label, or operator.";
            }
            logError(ctx, tokens[idx].line, "Unexpected token in expression: " + tok, hint);
        }
        idx++; // Prevent infinite loop
        return 0;
    }

    while (idx < tokens.size()) {
        TokenType opType = tokens[idx].type;
        int prec = -1;
        if (opType == TokenType::Plus || opType == TokenType::Minus) prec = 1;
        else if (opType == TokenType::Star || opType == TokenType::Slash) prec = 2;
        else break; // Not an operator

        if (prec < minPrec) break;

        // Inside brackets, stop before +/- if the next token is a register name
        // so the bracket loop can handle it as a base/index register
        if (insideBrackets && (opType == TokenType::Plus || opType == TokenType::Minus)) {
            if (idx + 1 < (int)tokens.size()) {
                int dummyReg, dummySize;
                if (isRegister(tokens[idx + 1].value, dummyReg, dummySize)) {
                    break;
                }
            }
        }

        idx++;
        int rhs = parseExpression(ctx, tokens, idx, prec + 1, insideBrackets);
        
        if (opType == TokenType::Plus) lhs += rhs;
        else if (opType == TokenType::Minus) lhs -= rhs;
        else if (opType == TokenType::Star) lhs *= rhs;
        else if (opType == TokenType::Slash) {
            if (rhs != 0) lhs /= rhs;
            else logError(ctx, tokens[idx-1].line, "Division by zero", "Expression contains division by zero. Check the divisor value or EQU constant.");
        }
    }
    return lhs;
}

// --- Instructions ---



struct Operand {
    enum Type { REGISTER, IMMEDIATE, MEMORY, SEGREG } type;
    int reg; // Register code
    int size; // 8 or 16
    int val; // Immediate or Displacement
    int memReg; // Base register for memory: -1=Direct, 3=BX, 6=SI, 7=DI, 5=BP
    bool isLabel;
    int segmentPrefix; // -1=None, 0x26=ES, 0x2E=CS, 0x36=SS, 0x3E=DS
    bool present; // NEW: Explicitly track if operand exists (vs val=0)
    bool involvesSymbol; // NEW: Did calculation involve a label?
    bool hasExplicitSize; // Whether BYTE/WORD prefix was given explicitly

    Operand() : type(IMMEDIATE), reg(0), size(0), val(0), memReg(-1), isLabel(false), segmentPrefix(-1), present(false), involvesSymbol(false), hasExplicitSize(false) {}
};

Operand parseOperand(AssemblerContext& ctx, const vector<Token>& tokens, int& idx) {
    Operand op;
    op.type = Operand::IMMEDIATE;
    op.reg = 0; op.size = 0; op.val = 0; op.memReg = -1; op.isLabel = false;
    op.segmentPrefix = -1;
    op.present = false;
    op.involvesSymbol = false;

    int startIdx = idx;

    if (idx >= tokens.size()) return op;
    
    // Check for Segment Override (e.g., ES: [BX])
    if (tokens[idx].type == TokenType::LabelDef) {
        string s = toUpper(tokens[idx].value);
        int prefix = -1;
        if (s == "ES") prefix = 0x26;
        else if (s == "CS") prefix = 0x2E;
        else if (s == "SS") prefix = 0x36;
        else if (s == "DS") prefix = 0x3E;

        if (prefix != -1) {
            op.segmentPrefix = prefix;
            idx++; // Skip LabelDef
        }
    }

    if (idx >= tokens.size()) {
        op.present = (idx > startIdx);
        return op;
    }

    Token t = tokens[idx];

    // Check for BYTE/WORD size prefix (e.g., BYTE [BX], WORD [100h])
    int sizeOverride = 0;
    if (t.type == TokenType::Identifier) {
        string upper = toUpper(t.value);
        if (upper == "BYTE") { sizeOverride = 8; idx++; }
        else if (upper == "WORD") { sizeOverride = 16; idx++; }
    }

    if (idx >= tokens.size()) {
        if (sizeOverride) { op.present = true; } // BYTE/WORD alone is odd but consumed tokens
        else { op.present = (idx > startIdx); }
        return op;
    }

    // Check for [ ]
    if (tokens[idx].type == TokenType::LBracket) {
        op.type = Operand::MEMORY;
        op.hasExplicitSize = (sizeOverride != 0);
        op.size = sizeOverride ? sizeOverride : 16;
        idx++;
        
        // Parse contents of [ ... ]
        bool hasBX = false, hasBP = false, hasSI = false, hasDI = false;
        int displacement = 0;
        
        while (idx < tokens.size() && tokens[idx].type != TokenType::RBracket) {
            string val = tokens[idx].value; // Peek
            TokenType tType = tokens[idx].type;
            
            if (tType == TokenType::Plus) { idx++; continue; } 
            
            // Check for Segment Override inside [] (e.g. [ES:BX])
            if (tokens[idx].type == TokenType::LabelDef) {
                string s = toUpper(val);
                int prefix = -1;
                if (s == "ES") prefix = 0x26;
                else if (s == "CS") prefix = 0x2E;
                else if (s == "SS") prefix = 0x36;
                else if (s == "DS") prefix = 0x3E;

                if (prefix != -1) {
                    op.segmentPrefix = prefix;
                    idx++; // Skip LabelDef
                    continue;
                }
            }

            int r, s;
            if (isRegister(val, r, s)) {
                string u = toUpper(val);
                if (u == "BX") hasBX = true;
                else if (u == "BP") hasBP = true;
                else if (u == "SI") hasSI = true;
                else if (u == "DI") hasDI = true;
                else logError(ctx, tokens[idx].line, "Invalid register in memory operand: " + val, "Only BX, BP, SI, and DI can be used inside []. AX, CX, DX, SP are not valid base/index registers on 8086.");
                idx++;
            } else {
                // Parse as expression/number/label
                ctx.encounteredSymbol = false; // Reset before expression
                displacement += parseExpression(ctx, tokens, idx, 0, true);
                if (ctx.encounteredSymbol) op.involvesSymbol = true;
            }
        }
        
        if (idx < tokens.size() && tokens[idx].type == TokenType::RBracket) idx++;

        // Calculate memReg (R/M field)
        op.val = displacement;
        op.memReg = -1; // Default direct
        
        if (hasBX && hasSI) op.memReg = 0;
        else if (hasBX && hasDI) op.memReg = 1;
        else if (hasBP && hasSI) op.memReg = 2;
        else if (hasBP && hasDI) op.memReg = 3;
        else if (hasSI && !hasBX && !hasBP) op.memReg = 4;
        else if (hasDI && !hasBX && !hasBP) op.memReg = 5;
        else if (hasBP && !hasSI && !hasDI) op.memReg = 6;
        else if (hasBX && !hasSI && !hasDI) op.memReg = 7;
        else if (!hasBX && !hasBP && !hasSI && !hasDI) op.memReg = -1; // Direct
        else {
             logError(ctx, tokens[idx].line, "Invalid addressing mode combination", "Valid 8086 addressing modes: [BX+SI], [BX+DI], [BP+SI], [BP+DI], [SI], [DI], [BP], [BX], or [direct_address]. You cannot combine SI+DI, BX+BP, or use AX/CX/DX/SP inside brackets.");
        }
    }
    else if (isRegister(t.value, op.reg, op.size)) {
        op.type = Operand::REGISTER;
        idx++;
    }
    // Segment register as operand (ES, CS, SS, DS) — NOT followed by ':' (that's a prefix)
    else {
        string upper = toUpper(t.value);
        int segCode = -1;
        if (upper == "ES") segCode = 0;
        else if (upper == "CS") segCode = 1;
        else if (upper == "SS") segCode = 2;
        else if (upper == "DS") segCode = 3;

        if (segCode != -1 && t.type == TokenType::Identifier) {
            op.type = Operand::SEGREG;
            op.reg = segCode;  // ES=0, CS=1, SS=2, DS=3
            op.size = 16;
            idx++;
        } else {
            op.type = Operand::IMMEDIATE;
            op.val = parseExpression(ctx, tokens, idx, 0);
        }
    }
    
    op.present = (idx > startIdx);
    return op;
}

// Helper to get register name from ID
string getRegName(int reg, int size) {
    static const string regs8[] = { "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH" };
    static const string regs16[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    if (size == 8 && reg >= 0 && reg < 8) return regs8[reg];
    if (size == 16 && reg >= 0 && reg < 8) return regs16[reg];
    return "?";
}

// --- Diagnostic Helper: describe operand type for ISA hints ---
string describeOperandType(const Operand& op) {
    if (!op.present) return "NONE";
    switch (op.type) {
        case Operand::REGISTER:
            return "REG" + to_string(op.size) + "(" + getRegName(op.reg, op.size) + ")";
        case Operand::MEMORY:
            return "MEM" + to_string(op.size);
        case Operand::IMMEDIATE:
            return "IMM(" + to_string(op.val) + ")";
        case Operand::SEGREG: {
            static const string sregs[] = { "ES", "CS", "SS", "DS" };
            return "SREG(" + (op.reg >= 0 && op.reg < 4 ? sregs[op.reg] : string("?")) + ")";
        }
    }
    return "UNKNOWN";
}

// Helper to format operand for the Listing
string formatOperand(const Operand& op) {
    stringstream ss;
    if (op.type == Operand::REGISTER) {
        ss << "REG(" << getRegName(op.reg, op.size) << ")";
    } else if (op.type == Operand::SEGREG) {
        static const string sregs[] = { "ES", "CS", "SS", "DS" };
        ss << "SREG(" << (op.reg >= 0 && op.reg < 4 ? sregs[op.reg] : "?") << ")";
    } else if (op.type == Operand::IMMEDIATE) {
        ss << "IMM(" << op.val << ")";
    } else if (op.type == Operand::MEMORY) {
        ss << "MEM(";
        if (op.size == 8) ss << "BYTE ";
        else ss << "WORD ";
        
        if (op.segmentPrefix != -1) ss << "SEG:"; // Simplified for now
        
        if (op.memReg == -1) ss << "[" << op.val << "]";
        else {
            // Decode R/M field back to registers
            ss << "[";
            switch(op.memReg) {
                case 0: ss << "BX+SI"; break;
                case 1: ss << "BX+DI"; break;
                case 2: ss << "BP+SI"; break;
                case 3: ss << "BP+DI"; break;
                case 4: ss << "SI"; break;
                case 5: ss << "DI"; break;
                case 6: ss << "BP"; break;
                case 7: ss << "BX"; break;
            }
            if (op.val != 0) ss << (op.val > 0 ? "+" : "") << op.val;
            ss << "]";
        }
        ss << ")";
    }
    return ss.str();
}

// Validation Helper
bool validateInstruction(AssemblerContext& ctx, const string& mnemonic, const Operand& op1, const Operand& op2, int line) {
    bool inDB = false;
    for (const auto& entry : isaDB) {
        if (entry.mnemonic == mnemonic) {
            inDB = true;
            for (const auto& form : entry.validForms) {
                bool match1 = false, match2 = false;

                // Helper lambda for matching
                auto matches = [&](const Operand& op, const string& rule) -> bool {
                    if (rule == "NONE") return !op.present;
                    if (!op.present) return false;
                    
                    if (rule == "REG") return op.type == Operand::REGISTER;
                    if (rule == "REG8") return op.type == Operand::REGISTER && op.size == 8;
                    if (rule == "REG16") return op.type == Operand::REGISTER && op.size == 16;
                    if (rule == "MEM") return op.type == Operand::MEMORY;
                    if (rule == "MEM16") return op.type == Operand::MEMORY; // Assume 16-bit
                    if (rule == "IMM") return op.type == Operand::IMMEDIATE;
                    if (rule == "LABEL") return op.type == Operand::IMMEDIATE; 
                    if (rule == "AL/AX") return op.type == Operand::REGISTER && op.reg == 0;
                    if (rule == "1") return op.type == Operand::IMMEDIATE && op.val == 1;
                    if (rule == "CL") return op.type == Operand::REGISTER && op.reg == 1 && op.size == 8;
                    if (rule == "DX") return op.type == Operand::REGISTER && op.reg == 2 && op.size == 16;
                    if (rule == "SEG" || rule == "SREG") return op.type == Operand::SEGREG;
                    return false;
                };

                
                match1 = matches(op1, form.type1);
                match2 = matches(op2, form.type2);

                if (match1 && match2) return true;
            }
            
            // Build hint from ISA DB showing valid forms
            string hint = "Valid forms: ";
            for (size_t i = 0; i < entry.validForms.size(); ++i) {
                const auto& form = entry.validForms[i];
                hint += mnemonic + " " + form.type1;
                if (form.type2 != "NONE") hint += ", " + form.type2;
                if (!form.constraints.empty()) hint += " (" + form.constraints + ")";
                if (i < entry.validForms.size() - 1) hint += " | ";
            }
            // Tell the agent what it actually provided
            hint += ". You provided: " + describeOperandType(op1);
            if (op2.present) hint += ", " + describeOperandType(op2);
            hint += ".";

            logError(ctx, line, "Invalid operands for " + mnemonic, hint);
            return false;
        }
    }
    return true; // Not in DB (Directive or Label?), ignore
}

// Helper functions
void emitByte(AssemblerContext& ctx, uint8_t byte) {
    if (!ctx.isPass1) {
        ctx.machineCode.push_back(byte);
        ctx.currentLineBytes.push_back(byte);
    }
    ctx.currentAddress++;
}

void emitWord(AssemblerContext& ctx, uint16_t word) {
    if (!ctx.isPass1) {
        ctx.machineCode.push_back(word & 0xFF);
        ctx.machineCode.push_back((word >> 8) & 0xFF);
        ctx.currentLineBytes.push_back(word & 0xFF);
        ctx.currentLineBytes.push_back((word >> 8) & 0xFF);
    }
    ctx.currentAddress += 2;
}

// Emit ModR/M byte + displacement for a memory operand.
// regField: the 3-bit reg/opcode extension field (bits 5-3 of ModR/M).
// mem: the memory Operand (must be Operand::MEMORY).
void emitModRM(AssemblerContext& ctx, int regField, const Operand& mem) {
    if (mem.memReg == -1) {
        // Direct address: Mod=00, R/M=110, followed by disp16
        emitByte(ctx, 0x06 | (regField << 3));
        emitWord(ctx, mem.val);
    } else {
        int mod = 0;
        
        // Optimize displacement size
        // RULES:
        // 1. If symbol involved, FORCE 16-bit (mod=2) to prevent phase errors between passes
        // 2. If val=0 (and not BP dim rect), mod=00 (no disp)
        // 3. If signed 8-bit, mod=01
        // 4. Else mod=10 (16-bit)
        
        if (mem.involvesSymbol) {
             // cerr << "Force16: " << mem.val << " Pass1=" << ctx.isPass1 << endl;
             mod = 2; // Fixed 16-bit displacement for safety
        } else if (mem.val == 0 && mem.memReg != 6) {
             mod = 0;
        } else if (mem.val >= -128 && mem.val <= 127) {
             mod = 1;
        } else {
             mod = 2;
        }

        // BP (R/M=110) with mod=00 encodes direct address, so [BP] must use mod=01
        if (mem.memReg == 6 && mod == 0) mod = 1;

        emitByte(ctx, (mod << 6) | (regField << 3) | mem.memReg);
        if (mod == 1) emitByte(ctx, mem.val & 0xFF);
        else if (mod == 2) emitWord(ctx, mem.val & 0xFFFF);
    }
}

void assembleLine(AssemblerContext& ctx, const vector<Token>& tokens, int lineNum, string sourceLine) {
    if (tokens.empty()) return;
    int idx = 0;
    
    int startAddr = ctx.currentAddress;
    ctx.currentLineBytes.clear();

    // --- STRUC mode intercept: when inside a STRUC definition, intercept all lines ---
    if (!ctx.currentStructName.empty()) {
        // Check for ENDS (either "STRUCTNAME ENDS" or bare "ENDS")
        bool isEnds = false;
        if (tokens.size() >= 2 && tokens[0].type == TokenType::Identifier &&
            toUpper(tokens[1].value) == "ENDS") {
            string endsName = toUpper(tokens[0].value);
            if (endsName != ctx.currentStructName) {
                logWarning(ctx, lineNum, "ENDS name '" + endsName + "' does not match STRUC name '" + ctx.currentStructName + "'",
                    "The name after ENDS should match the STRUC name.");
            }
            isEnds = true;
        } else if (tokens.size() >= 1 && toUpper(tokens[0].value) == "ENDS") {
            isEnds = true;
        }

        if (isEnds) {
            // Finalize the struct definition
            StructDef sd;
            sd.name = ctx.currentStructName;
            sd.fields = ctx.currentStructFields;
            sd.totalSize = ctx.currentStructOffset;
            sd.definedLine = lineNum;

            if (ctx.isPass1 && ctx.structDefs.count(sd.name)
                && ctx.structDefs[sd.name].definedLine != lineNum) {
                logWarning(ctx, lineNum, "Structure '" + sd.name + "' redefined",
                    "Previous definition will be overwritten.");
            }
            ctx.structDefs[sd.name] = sd;

            // Populate symbol table: STRUCTNAME = totalSize, STRUCTNAME.FIELD = offset
            ctx.symbolTable[sd.name] = { sd.totalSize, true, lineNum };
            for (const auto& f : sd.fields) {
                string qualName = sd.name + "." + f.name;
                ctx.symbolTable[qualName] = { f.offset, true, lineNum };
            }

            ctx.currentStructName = "";
            ctx.currentStructFields.clear();
            ctx.currentStructOffset = 0;
            return;
        }

        // Check for nested STRUC/STRUCT
        if (tokens.size() >= 2 && tokens[0].type == TokenType::Identifier) {
            string secondUpper = toUpper(tokens[1].value);
            if (secondUpper == "STRUC" || secondUpper == "STRUCT") {
                logError(ctx, lineNum, "Nested STRUC not allowed",
                    "Already inside STRUC '" + ctx.currentStructName + "'. Close it with ENDS first.");
                return;
            }
        }

        // Parse field line: FIELDNAME DB/DW/DD/RESB/RESW [value]
        if (tokens.size() >= 2 && tokens[0].type == TokenType::Identifier) {
            string fieldName = toUpper(tokens[0].value);
            string fieldDir = toUpper(tokens[1].value);
            int fieldSize = 0;
            vector<uint8_t> defaultValue;

            if (fieldDir == "DB") {
                fieldSize = 1;
                if (tokens.size() >= 3) {
                    if (tokens[2].type == TokenType::String) {
                        // Single character default
                        if (!tokens[2].value.empty())
                            defaultValue.push_back((uint8_t)tokens[2].value[0]);
                        else
                            defaultValue.push_back(0);
                    } else {
                        int tIdx = 2;
                        int val = parseExpression(ctx, tokens, tIdx, 0);
                        defaultValue.push_back((uint8_t)(val & 0xFF));
                    }
                } else {
                    defaultValue.push_back(0);
                }
            } else if (fieldDir == "DW") {
                fieldSize = 2;
                if (tokens.size() >= 3) {
                    int tIdx = 2;
                    int val = parseExpression(ctx, tokens, tIdx, 0);
                    defaultValue.push_back((uint8_t)(val & 0xFF));
                    defaultValue.push_back((uint8_t)((val >> 8) & 0xFF));
                } else {
                    defaultValue.push_back(0);
                    defaultValue.push_back(0);
                }
            } else if (fieldDir == "DD") {
                fieldSize = 4;
                if (tokens.size() >= 3) {
                    int tIdx = 2;
                    int val = parseExpression(ctx, tokens, tIdx, 0);
                    defaultValue.push_back((uint8_t)(val & 0xFF));
                    defaultValue.push_back((uint8_t)((val >> 8) & 0xFF));
                    defaultValue.push_back((uint8_t)((val >> 16) & 0xFF));
                    defaultValue.push_back((uint8_t)((val >> 24) & 0xFF));
                } else {
                    for (int k = 0; k < 4; k++) defaultValue.push_back(0);
                }
            } else if (fieldDir == "RESB") {
                int count = 1;
                if (tokens.size() >= 3) {
                    int tIdx = 2;
                    count = parseExpression(ctx, tokens, tIdx, 0);
                }
                fieldSize = count;
                for (int k = 0; k < count; k++) defaultValue.push_back(0);
            } else if (fieldDir == "RESW") {
                int count = 1;
                if (tokens.size() >= 3) {
                    int tIdx = 2;
                    count = parseExpression(ctx, tokens, tIdx, 0);
                }
                fieldSize = count * 2;
                for (int k = 0; k < fieldSize; k++) defaultValue.push_back(0);
            } else {
                logError(ctx, lineNum, "Invalid field directive '" + fieldDir + "' in STRUC '" + ctx.currentStructName + "'",
                    "Fields inside a STRUC must use DB, DW, DD, RESB, or RESW.");
                return;
            }

            StructField sf;
            sf.name = fieldName;
            sf.offset = ctx.currentStructOffset;
            sf.size = fieldSize;
            sf.defaultValue = defaultValue;
            ctx.currentStructFields.push_back(sf);
            ctx.currentStructOffset += fieldSize;
        } else if (tokens.size() == 1 && toUpper(tokens[0].value) != "ENDS") {
            logError(ctx, lineNum, "Invalid line inside STRUC '" + ctx.currentStructName + "'",
                "Expected a field definition (e.g., FIELDNAME DW 0) or ENDS.");
        }
        return; // No bytes emitted during STRUC definition
    }

    // 0. Check for EQU (Label EQU Value)
    // Structure: Identifier EQU Expression
    if (tokens.size() >= 3 && tokens[0].type == TokenType::Identifier) {
        if (toUpper(tokens[1].value) == "EQU") {
            string label = toUpper(tokens[0].value);
            int valIdx = 2; // Value starts at index 2
            int val = parseExpression(ctx, tokens, valIdx, 0);

            if (ctx.symbolTable.count(label) && !ctx.symbolTable[label].isConstant
                && ctx.symbolTable[label].definedLine != tokens[0].line) {
                int prevLine = ctx.symbolTable[label].definedLine;
                logError(ctx, tokens[0].line,
                    "EQU '" + label + "' conflicts with existing label defined at line " + to_string(prevLine),
                    "A symbol can only be defined once. Rename the EQU or the label to avoid this collision.");
            }
            if (ctx.isPass1) {
                ctx.symbolTable[label] = { val, true, tokens[0].line };
            }

            // Debug output if needed
            // if (ctx.isPass1) cout << "EQU: " << label << " = " << val << endl;
            return;
        }
    }

    // 0b. STRUC/STRUCT definition start (Identifier STRUC or Identifier STRUCT)
    if (tokens.size() >= 2 && tokens[0].type == TokenType::Identifier) {
        string secondUpper = toUpper(tokens[1].value);
        if (secondUpper == "STRUC" || secondUpper == "STRUCT") {
            if (!ctx.currentStructName.empty()) {
                logError(ctx, lineNum, "Nested STRUC not allowed",
                    "Already inside STRUC '" + ctx.currentStructName + "'. Close it with ENDS first.");
                return;
            }
            if (!ctx.currentProcedureName.empty()) {
                logError(ctx, lineNum, "STRUC inside PROC not allowed",
                    "STRUC definitions must be outside any PROC/ENDP block.");
                return;
            }
            ctx.currentStructName = toUpper(tokens[0].value);
            ctx.currentStructOffset = 0;
            ctx.currentStructFields.clear();
            return;
        }
    }

    // 0c. Instance allocation (Identifier STRUCTNAME <...>)
    if (tokens.size() >= 2 && tokens[0].type == TokenType::Identifier) {
        string possibleStructName = toUpper(tokens[1].value);
        if (ctx.structDefs.count(possibleStructName)) {
            const StructDef& sd = ctx.structDefs[possibleStructName];
            string instanceLabel = toUpper(tokens[0].value);

            // Define label at current address
            if (ctx.isPass1) {
                ctx.symbolTable[instanceLabel] = { ctx.currentAddress, false, lineNum };
            }

            // Parse angle-bracket overrides from raw sourceLine
            vector<int> overrides; // INT_MIN = use default
            size_t ltPos = sourceLine.find('<');
            size_t gtPos = sourceLine.rfind('>');
            if (ltPos != string::npos && gtPos != string::npos && gtPos > ltPos) {
                string inside = sourceLine.substr(ltPos + 1, gtPos - ltPos - 1);
                // Split by commas
                vector<string> parts;
                string current;
                for (char c : inside) {
                    if (c == ',') {
                        parts.push_back(current);
                        current.clear();
                    } else {
                        current += c;
                    }
                }
                parts.push_back(current);

                for (const auto& part : parts) {
                    // Trim whitespace
                    string trimmed;
                    for (char c : part) {
                        if (!isspace((unsigned char)c)) trimmed += c;
                    }
                    if (trimmed.empty()) {
                        overrides.push_back(INT_MIN); // Use default
                    } else {
                        // Tokenize and evaluate the override expression
                        vector<Token> valTokens = tokenize(trimmed, lineNum);
                        if (!valTokens.empty()) {
                            int vIdx = 0;
                            int val = parseExpression(ctx, valTokens, vIdx, 0);
                            overrides.push_back(val);
                        } else {
                            overrides.push_back(INT_MIN);
                        }
                    }
                }
            }

            // Emit bytes for each field
            for (size_t fi = 0; fi < sd.fields.size(); fi++) {
                const StructField& f = sd.fields[fi];
                if (fi < overrides.size() && overrides[fi] != INT_MIN) {
                    // Use override value
                    int val = overrides[fi];
                    if (f.size == 1) {
                        emitByte(ctx, (uint8_t)(val & 0xFF));
                    } else if (f.size == 2) {
                        emitByte(ctx, (uint8_t)(val & 0xFF));
                        emitByte(ctx, (uint8_t)((val >> 8) & 0xFF));
                    } else if (f.size == 4) {
                        emitByte(ctx, (uint8_t)(val & 0xFF));
                        emitByte(ctx, (uint8_t)((val >> 8) & 0xFF));
                        emitByte(ctx, (uint8_t)((val >> 16) & 0xFF));
                        emitByte(ctx, (uint8_t)((val >> 24) & 0xFF));
                    } else {
                        // RESB-style: fill with val for first byte, 0 for rest
                        emitByte(ctx, (uint8_t)(val & 0xFF));
                        for (int k = 1; k < f.size; k++) emitByte(ctx, 0);
                    }
                } else {
                    // Use default value
                    for (uint8_t b : f.defaultValue) {
                        emitByte(ctx, b);
                    }
                }
            }

            // Record in listing
            if (!ctx.isPass1) {
                BinaryMap bm;
                bm.address = startAddr;
                bm.sourceLine = lineNum;
                bm.bytes = ctx.currentLineBytes;
                bm.size = (int)ctx.currentLineBytes.size();
                bm.sourceCode = sourceLine;
                bm.decoded = instanceLabel + " " + possibleStructName;
                ctx.agentState.listing.push_back(bm);
            }
            return;
        }
    }

    // 1. Check for Label FIRST
    if (tokens.size() > 0 && tokens[0].type == TokenType::LabelDef) {
        string label = tokens[0].value;
        if (!label.empty() && label.back() == ':') label.pop_back(); // Remove : if present

        if (label[0] == '.') {
             if (!ctx.currentProcedureName.empty()) label = ctx.currentProcedureName + label;
             else logWarning(ctx, tokens[0].line, "Local label " + label + " outside procedure", "Local labels (starting with '.') must be inside a PROC/ENDP block. Either wrap your code in a PROC or use a global label (no '.' prefix).");
        }

        label = toUpper(label);
        if (ctx.symbolTable.count(label) && ctx.symbolTable[label].definedLine != tokens[0].line) {
            int prevLine = ctx.symbolTable[label].definedLine;
            if (ctx.symbolTable[label].isConstant) {
                logError(ctx, tokens[0].line,
                    "Label '" + label + "' conflicts with EQU constant defined at line " + to_string(prevLine),
                    "A symbol can only be defined once. Rename the label or the EQU to avoid this collision.");
            } else {
                logWarning(ctx, tokens[0].line,
                    "Label '" + label + "' redefined (previous definition at line " + to_string(prevLine) + ")",
                    "Each label should be defined once. If you need the same name in different scopes, use local labels with '.' prefix inside PROC/ENDP blocks.");
            }
        }
        if (ctx.isPass1) {
            ctx.symbolTable[label] = { ctx.currentAddress, false, tokens[0].line };
        }
        idx++;
    }

    if (idx >= tokens.size()) return;

    string mnemonic = toUpper(tokens[idx].value);
    idx++;

    // 0. Handle Prefixes (REP, REPE, REPNE)
    if (mnemonic == "REP" || mnemonic == "REPE" || mnemonic == "REPZ") {
        emitByte(ctx, 0xF3);
        if (idx < tokens.size()) {
            mnemonic = toUpper(tokens[idx].value);
            idx++;
        }
    } else if (mnemonic == "REPNE" || mnemonic == "REPNZ") {
        emitByte(ctx, 0xF2);
        if (idx < tokens.size()) {
            mnemonic = toUpper(tokens[idx].value);
            idx++;
        }
    }

    // 2. Directives
    if (mnemonic == "ORG") {
        // F4: Warn if ORG appears after code has been emitted
        if (ctx.currentAddress > 0 && !ctx.isPass1) {
            logWarning(ctx, tokens[0].line,
                "ORG directive after code has been emitted",
                "ORG sets the address counter but does not move existing code. Place ORG at the start of your source, before any instructions or data.");
        }
        vector<Token> args;
        for (; idx < tokens.size(); ++idx) {
            if (tokens[idx].type != TokenType::Comma) args.push_back(tokens[idx]);
        }
        if (args.size() == 1 && args[0].type == TokenType::Number) {
            bool numOk;
            string numReason;
            ctx.currentAddress = parseNumber(args[0].value, numOk, numReason);
            if (!numOk) logError(ctx, args[0].line, "Invalid numeric literal in ORG: " + args[0].value, numReason.empty() ? "ORG requires a numeric value. Common usage: ORG 100h (for .COM files)." : numReason);
        }
        return;
    }

    if (mnemonic == "DB") {
        while (idx < tokens.size()) {
            bool isExpr = false;
            if (tokens[idx].type == TokenType::String) {
                // Check if next token is an operator
                if (idx + 1 < tokens.size()) {
                    string nextVal = tokens[idx+1].value;
                    if (nextVal == "+" || nextVal == "-" || nextVal == "*" || nextVal == "/") {
                        isExpr = true;
                    }
                }
            } else {
                isExpr = true;
            }

            if (!isExpr && tokens[idx].type == TokenType::String) {
                for (char c : tokens[idx].value) emitByte(ctx, c);
                idx++;
            } else {
                int val = parseExpression(ctx, tokens, idx, 0);
                emitByte(ctx, (uint8_t)val);
            }

            if (idx < tokens.size()) {
                 if (tokens[idx].type == TokenType::Comma) {
                     idx++;
                 } else {
                     if (!ctx.isPass1) logError(ctx, tokens[idx].line, "Expected comma in DB", "DB values must be comma-separated. Example: DB 'Hello', 0Dh, 0Ah, '$'");
                     idx++;
                 }
            }
        }
        return;
    }

    if (mnemonic == "DW") {
        while (idx < tokens.size()) {
             int val = parseExpression(ctx, tokens, idx, 0);
             emitWord(ctx, (uint16_t)val);

             if (idx < tokens.size()) {
                 if (tokens[idx].type == TokenType::Comma) {
                     idx++;
                 } else {
                     if (!ctx.isPass1) logError(ctx, tokens[idx].line, "Expected comma in DW", "DW values must be comma-separated. Example: DW 1234h, 5678h");
                     idx++;
                 }
            }
        }
        return;
    }

    if (mnemonic == "DD") {
        while (idx < tokens.size()) {
             int val = parseExpression(ctx, tokens, idx, 0);
             // Emit 32-bit little endian
             emitWord(ctx, (uint16_t)(val & 0xFFFF));
             emitWord(ctx, (uint16_t)((val >> 16) & 0xFFFF));

             if (idx < tokens.size()) {
                 if (tokens[idx].type == TokenType::Comma) {
                     idx++;
                 } else {
                     if (!ctx.isPass1) logError(ctx, tokens[idx].line, "Expected comma in DD", "DD values must be comma-separated. Example: DD 12345678h");
                     idx++;
                 }
            }
        }
        return;
    }




    if (mnemonic == "RESB" || mnemonic == "RESW") {
        if (idx < tokens.size()) {
            int count = parseExpression(ctx, tokens, idx, 0);
            // No increment needed here as parseExpression consumes tokens
            
            // Check for trailing comma or tokens not consumed by parseExpression?
            // parseExpression advances idx.
            
            if (mnemonic == "RESW") count *= 2;
            for (int k=0; k<count; ++k) emitByte(ctx, 0);
        }
        return;
    }


    // 3. PROC / ENDP
    if (mnemonic == "PROC") {
        // Look for the label defined on this line
        string procName = "";
        for(int i=0; i<idx-1; ++i) {
            if (tokens[i].type == TokenType::LabelDef) {
                procName = tokens[i].value;
                if (!procName.empty() && procName.back() == ':') procName.pop_back();
            }
        }
        if (!procName.empty()) {
            ctx.currentProcedureName = toUpper(procName);
        } else {
            logError(ctx, tokens[0].line, "PROC without label", "PROC must be on the same line as a label. Example: myproc: PROC");
        }
        return;
    }
    if (mnemonic == "ENDP") {
        ctx.currentProcedureName = "";
        return;
    }

    // --- Instructions ---
    int p = idx;
    Operand op1 = parseOperand(ctx, tokens, p);
    Operand op2;
    if (p < tokens.size() && tokens[p].value == ",") {
        p++;
        op2 = parseOperand(ctx, tokens, p);
    }
    
    // Check for extra tokens
    if (p < tokens.size()) {
         logError(ctx, tokens[p].line, "Extra tokens at end of line", "Unexpected content after instruction. Check for missing commas, stray characters, or a comment that doesn't start with ';'.");
    }

    // Validate Instructions against ISA DB
    if (!validateInstruction(ctx, mnemonic, op1, op2, tokens[0].line)) return;

    // --- NEW: Generate Decoded String ---
    string decodedStr = mnemonic;
    if (op1.present) { 
        decodedStr += " " + formatOperand(op1);
        if (op2.present) { 
             decodedStr += ", " + formatOperand(op2);
        }
    }

    // Emit Segment Prefix if present (only one operand can be memory usually)
    if (op1.segmentPrefix != -1) emitByte(ctx, op1.segmentPrefix);
    if (op2.segmentPrefix != -1) emitByte(ctx, op2.segmentPrefix);

    // 1. MOV
    if (mnemonic == "MOV") {
        // MOV Reg, Reg
        if (op1.type == Operand::REGISTER && op2.type == Operand::REGISTER) {
            if (op1.size != op2.size) {
                string hint = "Op1 is " + to_string(op1.size) + "-bit (" + getRegName(op1.reg, op1.size)
                            + "), Op2 is " + to_string(op2.size) + "-bit (" + getRegName(op2.reg, op2.size)
                            + "). Both operands must be the same width.";
                logError(ctx, tokens[0].line, "Size mismatch between operands", hint); return;
            }
            if (op1.size == 8) { emitByte(ctx, 0x88); emitByte(ctx, 0xC0 | (op2.reg << 3) | op1.reg); }
            else { emitByte(ctx, 0x89); emitByte(ctx, 0xC0 | (op2.reg << 3) | op1.reg); }
        }
        // MOV Reg, Imm
        else if (op1.type == Operand::REGISTER && op2.type == Operand::IMMEDIATE) {
             if (op1.size == 8) {
                 if (!ctx.isPass1 && (op2.val < -128 || op2.val > 255)) {
                     logWarning(ctx, tokens[0].line,
                         "Immediate value " + to_string(op2.val) + " truncated to 8-bit (result: " + to_string(op2.val & 0xFF) + ")",
                         "Value exceeds 8-bit range (0-255 unsigned, -128 to 127 signed). The low 8 bits will be used.");
                 }
                 emitByte(ctx, 0xB0 + op1.reg); emitByte(ctx, op2.val & 0xFF);
             } else {
                 if (!ctx.isPass1 && (op2.val < -32768 || op2.val > 65535)) {
                     logWarning(ctx, tokens[0].line,
                         "Immediate value " + to_string(op2.val) + " truncated to 16-bit (result: " + to_string(op2.val & 0xFFFF) + ")",
                         "Value exceeds 16-bit range (0-65535 unsigned, -32768 to 32767 signed).");
                 }
                 emitByte(ctx, 0xB8 + op1.reg); emitWord(ctx, op2.val & 0xFFFF);
             }
        }
        // MOV Reg, Mem
        else if (op1.type == Operand::REGISTER && op2.type == Operand::MEMORY) {
            if (op1.size == 8) emitByte(ctx, 0x8A); else emitByte(ctx, 0x8B);
            
            emitModRM(ctx, op1.reg, op2);
        }
        // MOV Mem, Reg
        else if (op1.type == Operand::MEMORY && op2.type == Operand::REGISTER) {
            if (op2.size == 8) emitByte(ctx, 0x88); else emitByte(ctx, 0x89);
            
            emitModRM(ctx, op2.reg, op1);
        }
        // MOV Mem, Imm  (C6 /0 ib  or  C7 /0 iw)
        else if (op1.type == Operand::MEMORY && op2.type == Operand::IMMEDIATE) {
            int opSize = op1.size; // Size comes from BYTE/WORD prefix on memory operand
            if (!op1.hasExplicitSize && !ctx.isPass1) {
                logWarning(ctx, tokens[0].line,
                    "No size prefix on memory-immediate operation, defaulting to WORD",
                    "Add BYTE or WORD before the memory operand to be explicit. Example: MOV BYTE [BX], 5 or MOV WORD [BX], 5");
            }

            if (opSize == 8) emitByte(ctx, 0xC6); else emitByte(ctx, 0xC7);

            emitModRM(ctx, 0, op1);

            // Emit the immediate
            if (opSize == 8) emitByte(ctx, op2.val & 0xFF);
            else emitWord(ctx, op2.val & 0xFFFF);
        }
        // MOV Reg, SReg  (8C /r) — e.g. MOV AX, DS
        else if (op1.type == Operand::REGISTER && op2.type == Operand::SEGREG) {
            emitByte(ctx, 0x8C);
            emitByte(ctx, 0xC0 | (op2.reg << 3) | op1.reg);
        }
        // MOV SReg, Reg  (8E /r) — e.g. MOV DS, AX
        else if (op1.type == Operand::SEGREG && op2.type == Operand::REGISTER) {
            emitByte(ctx, 0x8E);
            emitByte(ctx, 0xC0 | (op1.reg << 3) | op2.reg);
        }
    }
    // 2. Arithmetic & Logic
    else if (mnemonic == "ADD" || mnemonic == "ADC" || mnemonic == "SUB" || mnemonic == "SBB" ||
             mnemonic == "CMP" || mnemonic == "AND" || mnemonic == "OR" || mnemonic == "XOR" || mnemonic == "TEST") {
        
        if (op1.type == Operand::REGISTER && op2.type == Operand::REGISTER) {
            uint8_t base = 0;
            if (mnemonic == "ADD") base = 0x00;
            else if (mnemonic == "OR")  base = 0x08;
            else if (mnemonic == "ADC") base = 0x10;
            else if (mnemonic == "SBB") base = 0x18;
            else if (mnemonic == "AND") base = 0x20;
            else if (mnemonic == "SUB") base = 0x28;
            else if (mnemonic == "XOR") base = 0x30;
            else if (mnemonic == "CMP") base = 0x38;
            else if (mnemonic == "TEST") base = 0x84;

            if (op1.size == 16) base += 1;
            emitByte(ctx, base);
            emitByte(ctx, 0xC0 | (op2.reg << 3) | op1.reg); 
        }
        else if (op1.type == Operand::REGISTER && op2.type == Operand::IMMEDIATE) { 
             bool isTest = (mnemonic == "TEST");
             if (isTest) {
                 if (op1.size == 8) {
                     emitByte(ctx, 0xF6); emitByte(ctx, 0xC0 | (0 << 3) | op1.reg); emitByte(ctx, op2.val & 0xFF);
                 } else {
                     emitByte(ctx, 0xF7); emitByte(ctx, 0xC0 | (0 << 3) | op1.reg); emitWord(ctx, op2.val & 0xFFFF);
                 }
                 return;
             }
             int ext = 0;
             if (mnemonic == "ADD") ext = 0;
             else if (mnemonic == "OR")  ext = 1;
             else if (mnemonic == "ADC") ext = 2;
             else if (mnemonic == "SBB") ext = 3;
             else if (mnemonic == "AND") ext = 4;
             else if (mnemonic == "SUB") ext = 5;
             else if (mnemonic == "XOR") ext = 6;
             else if (mnemonic == "CMP") ext = 7;

             if (op1.size == 8) {
                 if (!ctx.isPass1 && (op2.val < -128 || op2.val > 255)) {
                     logWarning(ctx, tokens[0].line,
                         "Immediate value " + to_string(op2.val) + " truncated to 8-bit (result: " + to_string(op2.val & 0xFF) + ")",
                         "Value exceeds 8-bit range (0-255 unsigned, -128 to 127 signed). The low 8 bits will be used.");
                 }
                 emitByte(ctx, 0x80); emitByte(ctx, 0xC0 | (ext << 3) | op1.reg); emitByte(ctx, op2.val & 0xFF);
             } else {
                 if (!ctx.isPass1 && (op2.val < -32768 || op2.val > 65535)) {
                     logWarning(ctx, tokens[0].line,
                         "Immediate value " + to_string(op2.val) + " truncated to 16-bit (result: " + to_string(op2.val & 0xFFFF) + ")",
                         "Value exceeds 16-bit range (0-65535 unsigned, -32768 to 32767 signed).");
                 }
                 emitByte(ctx, 0x81); emitByte(ctx, 0xC0 | (ext << 3) | op1.reg); emitWord(ctx, op2.val & 0xFFFF);
             }
        }
        // REG, MEM  (e.g. ADD AX, [BX])  — base+2 (8-bit) / base+3 (16-bit)
        else if (op1.type == Operand::REGISTER && op2.type == Operand::MEMORY) {
            uint8_t base = 0;
            if (mnemonic == "ADD") base = 0x02;
            else if (mnemonic == "OR")  base = 0x0A;
            else if (mnemonic == "ADC") base = 0x12;
            else if (mnemonic == "SBB") base = 0x1A;
            else if (mnemonic == "AND") base = 0x22;
            else if (mnemonic == "SUB") base = 0x2A;
            else if (mnemonic == "XOR") base = 0x32;
            else if (mnemonic == "CMP") base = 0x3A;
            else if (mnemonic == "TEST") base = 0x84; // TEST has no direction bit

            if (op1.size == 16 || (mnemonic == "TEST" && op1.size == 16)) base += 1;
            emitByte(ctx, base);

            emitModRM(ctx, op1.reg, op2);
        }
        // MEM, REG  (e.g. ADD [BX], AX)  — base+0 (8-bit) / base+1 (16-bit)
        else if (op1.type == Operand::MEMORY && op2.type == Operand::REGISTER) {
            uint8_t base = 0;
            if (mnemonic == "ADD") base = 0x00;
            else if (mnemonic == "OR")  base = 0x08;
            else if (mnemonic == "ADC") base = 0x10;
            else if (mnemonic == "SBB") base = 0x18;
            else if (mnemonic == "AND") base = 0x20;
            else if (mnemonic == "SUB") base = 0x28;
            else if (mnemonic == "XOR") base = 0x30;
            else if (mnemonic == "CMP") base = 0x38;
            else if (mnemonic == "TEST") base = 0x84;

            if (op2.size == 16 || (mnemonic == "TEST" && op2.size == 16)) base += 1;
            emitByte(ctx, base);

            emitModRM(ctx, op2.reg, op1);
        }
        // MEM, IMM  (e.g. ADD WORD [BX], 5)  — 80/81 group
        else if (op1.type == Operand::MEMORY && op2.type == Operand::IMMEDIATE) {
            if (mnemonic == "TEST") {
                // TEST r/m, imm: F6 /0 ib or F7 /0 iw
                int opSize = op1.size ? op1.size : 16;
                if (!op1.hasExplicitSize && !ctx.isPass1) {
                    logWarning(ctx, tokens[0].line,
                        "No size prefix on memory-immediate operation, defaulting to WORD",
                        "Add BYTE or WORD before the memory operand to be explicit. Example: " + mnemonic + " BYTE [BX], 5 or " + mnemonic + " WORD [BX], 5");
                }
                if (opSize == 8) emitByte(ctx, 0xF6); else emitByte(ctx, 0xF7);

                emitModRM(ctx, 0, op1);
                if (opSize == 8) emitByte(ctx, op2.val & 0xFF);
                else emitWord(ctx, op2.val & 0xFFFF);
            } else {
                int ext = 0;
                if (mnemonic == "ADD") ext = 0;
                else if (mnemonic == "OR")  ext = 1;
                else if (mnemonic == "ADC") ext = 2;
                else if (mnemonic == "SBB") ext = 3;
                else if (mnemonic == "AND") ext = 4;
                else if (mnemonic == "SUB") ext = 5;
                else if (mnemonic == "XOR") ext = 6;
                else if (mnemonic == "CMP") ext = 7;

                int opSize = op1.size ? op1.size : 16;
                if (!op1.hasExplicitSize && !ctx.isPass1) {
                    logWarning(ctx, tokens[0].line,
                        "No size prefix on memory-immediate operation, defaulting to WORD",
                        "Add BYTE or WORD before the memory operand to be explicit. Example: " + mnemonic + " BYTE [BX], 5 or " + mnemonic + " WORD [BX], 5");
                }
                if (opSize == 8) emitByte(ctx, 0x80); else emitByte(ctx, 0x81);

                emitModRM(ctx, ext, op1);
                if (opSize == 8) emitByte(ctx, op2.val & 0xFF);
                else emitWord(ctx, op2.val & 0xFFFF);
            }
        }
    }
    // 3. Unary (INC, DEC, NOT, NEG)
    else if (mnemonic == "INC" || mnemonic == "DEC" || mnemonic == "NOT" || mnemonic == "NEG") {
        int ext = 0;
        if (mnemonic == "INC") ext = 0;
        else if (mnemonic == "DEC") ext = 1;
        else if (mnemonic == "NOT") ext = 2;
        else if (mnemonic == "NEG") ext = 3;

        if (op1.type == Operand::REGISTER) {
             // INC/DEC have short forms for 16-bit regs
             if ((mnemonic == "INC" || mnemonic == "DEC") && op1.size == 16) {
                 if (mnemonic == "INC") emitByte(ctx, 0x40 + op1.reg);
                 else emitByte(ctx, 0x48 + op1.reg);
                 return;
             }
        }
        
        // Group 4 (FE): INC (0), DEC (1) byte
        // Group 5 (FF): INC (0), DEC (1) word
        // Group 3 (F6/F7): NOT (2), NEG (3)

        if (mnemonic == "INC" || mnemonic == "DEC") {
            if (op1.size == 8) emitByte(ctx, 0xFE); else emitByte(ctx, 0xFF);
        } else {
            if (op1.size == 8) emitByte(ctx, 0xF6); else emitByte(ctx, 0xF7);
        }
        
        // ModR/M
        if (op1.type == Operand::REGISTER) {
            emitByte(ctx, 0xC0 | (ext << 3) | op1.reg);
        } else if (op1.type == Operand::MEMORY) {
            emitModRM(ctx, ext, op1);
        }
    }
    // 5. Multiplication / Division
    else if (mnemonic == "MUL" || mnemonic == "IMUL" || mnemonic == "DIV" || mnemonic == "IDIV") {
        int ext = 0;
        if (mnemonic == "MUL") ext = 4;
        else if (mnemonic == "IMUL") ext = 5;
        else if (mnemonic == "DIV") ext = 6;
        else if (mnemonic == "IDIV") ext = 7;

        if (op1.size == 8) emitByte(ctx, 0xF6); else emitByte(ctx, 0xF7);

        if (op1.type == Operand::REGISTER) {
            emitByte(ctx, (3 << 6) | (ext << 3) | op1.reg);
        } else if (op1.type == Operand::MEMORY) {
            emitModRM(ctx, ext, op1);
        }
    }
    // 6. Interrupts
    else if (mnemonic == "INT") {
        if (op1.type == Operand::IMMEDIATE) {
            emitByte(ctx, 0xCD); 
            emitByte(ctx, op1.val & 0xFF);
        }
    }
    // 5. Shift & Rotate
    else if (mnemonic == "SHL" || mnemonic == "SHR" || mnemonic == "SAR" || mnemonic == "SAL" || mnemonic == "ROL" || mnemonic == "ROR" || mnemonic == "RCL" || mnemonic == "RCR") {
        if (op1.type != Operand::REGISTER && op1.type != Operand::MEMORY) return;

        int ext = 0;
        if (mnemonic == "ROL") ext = 0;
        else if (mnemonic == "ROR") ext = 1;
        else if (mnemonic == "RCL") ext = 2;
        else if (mnemonic == "RCR") ext = 3;
        else if (mnemonic == "SHL" || mnemonic == "SAL") ext = 4;
        else if (mnemonic == "SHR") ext = 5;
        else if (mnemonic == "SAR") ext = 7;

        bool isMem = (op1.type == Operand::MEMORY);
        bool isCL = (op2.type == Operand::REGISTER && op2.reg == 1 && op2.size == 8);

        if (isMem && !op1.hasExplicitSize && !ctx.isPass1) {
            logWarning(ctx, tokens[0].line,
                "No size prefix on memory shift/rotate, defaulting to WORD",
                "Add BYTE or WORD before the memory operand to be explicit. Example: " + mnemonic + " BYTE [BX], 1 or " + mnemonic + " WORD [BX], 1");
        }
        int opSize = isMem ? (op1.size ? op1.size : 16) : op1.size;

        // Helper lambda to emit ModR/M byte for reg or memory
        auto emitModRMByte = [&]() {
            if (isMem) {
                emitModRM(ctx, ext, op1);
            } else {
                emitByte(ctx, 0xC0 | (ext << 3) | op1.reg);
            }
        };

        if (op2.type == Operand::IMMEDIATE && op2.val == 1) {
            if (opSize == 8) emitByte(ctx, 0xD0); else emitByte(ctx, 0xD1);
            emitModRMByte();
        }
        else if (op2.type == Operand::IMMEDIATE) {
            // 0xC0/0xC1 encodings are 80186+ only
            if (!ctx.isPass1) {
                string op1Name = isMem ? ("memory operand") : getRegName(op1.reg, op1.size);
                logWarning(ctx, tokens[0].line, mnemonic + " with immediate count >1 uses 80186+ encoding (0xC0/0xC1)", "For strict 8086 compatibility, load the count into CL first: MOV CL, " + to_string(op2.val) + " / " + mnemonic + " " + op1Name + ", CL. The immediate form (" + mnemonic + " dest, N where N>1) generates an 80186-only opcode.");
            }
            if (opSize == 8) emitByte(ctx, 0xC0); else emitByte(ctx, 0xC1);
            emitModRMByte();
            emitByte(ctx, op2.val & 0xFF);
        }
        else if (isCL) {
            if (opSize == 8) emitByte(ctx, 0xD2); else emitByte(ctx, 0xD3);
            emitModRMByte();
        }
    }
    // 6. I/O Instructions
    else if (mnemonic == "IN") {
        if (op1.type == Operand::REGISTER && op1.reg == 0) { // Dest AL/AX
             if (op2.type == Operand::IMMEDIATE) {
                 if (op1.size == 8) emitByte(ctx, 0xE4); else emitByte(ctx, 0xE5);
                 emitByte(ctx, op2.val & 0xFF);
             } else if (op2.type == Operand::REGISTER && op2.reg == 2 && op2.size == 16) { // DX
                 if (op1.size == 8) emitByte(ctx, 0xEC); else emitByte(ctx, 0xED);
             } else {
                 logError(ctx, tokens[0].line, "Invalid IN operands", "IN requires: IN AL, imm8 | IN AX, imm8 | IN AL, DX | IN AX, DX");
             }
        } else {
             logError(ctx, tokens[0].line, "IN dest must be AL/AX", "The destination of IN must be AL (byte) or AX (word). Example: IN AL, 60h");
        }
    }
    else if (mnemonic == "OUT") {
        if (op2.type == Operand::REGISTER && op2.reg == 0) { // Src AL/AX
             if (op1.type == Operand::IMMEDIATE) {
                 if (op2.size == 8) emitByte(ctx, 0xE6); else emitByte(ctx, 0xE7);
                 emitByte(ctx, op1.val & 0xFF);
             } else if (op1.type == Operand::REGISTER && op1.reg == 2 && op1.size == 16) { // DX
                 if (op2.size == 8) emitByte(ctx, 0xEE); else emitByte(ctx, 0xEF);
             } else {
                 logError(ctx, tokens[0].line, "Invalid OUT operands", "OUT requires: OUT imm8, AL | OUT imm8, AX | OUT DX, AL | OUT DX, AX");
             }
        } else {
             logError(ctx, tokens[0].line, "OUT src must be AL/AX", "The source of OUT must be AL (byte) or AX (word). Example: OUT 60h, AL");
        }
    }
    // 7. LEA
    else if (mnemonic == "LEA") {
        if (op1.type == Operand::REGISTER && op2.type == Operand::MEMORY) {
            if (op1.size != 16) { logError(ctx, tokens[0].line, "LEA requires 16-bit register", "LEA only works with 16-bit registers (AX, BX, CX, DX, SI, DI, BP, SP). Use a 16-bit register as the destination."); return; }
            
            emitByte(ctx, 0x8D); // LEA Opcode
            
            emitModRM(ctx, op1.reg, op2);
        } else {
             logError(ctx, tokens[0].line, "Invalid operands for LEA", "LEA requires a 16-bit register and a memory operand. Example: LEA DI, [BX+SI+10h]");
        }
    }

    // 7. Jumps
    else if (mnemonic == "JMP") {
        if (op1.type == Operand::REGISTER && op1.size == 16) {
            // JMP r16: FF /4 with mod=11
            emitByte(ctx, 0xFF);
            emitByte(ctx, 0xC0 | (4 << 3) | op1.reg);
        } else if (op1.type == Operand::MEMORY) {
            // JMP [mem]: FF /4
            emitByte(ctx, 0xFF);
            emitModRM(ctx, 4, op1);
        } else {
            // Near Jump (E9) — label/immediate target
            int targetAddr = 0;
            if (op1.type == Operand::IMMEDIATE) targetAddr = op1.val;
            int offset = targetAddr - (ctx.currentAddress + 3);
            emitByte(ctx, 0xE9);
            emitWord(ctx, offset & 0xFFFF);
        }
    }
    else if (mnemonic.size() >= 2 && mnemonic[0] == 'J' && mnemonic != "JMP" && mnemonic != "JCXZ") {
        // Full Jcc opcode table (all short conditional jumps)
        static const map<string, uint8_t> jccOpcodes = {
            {"JO",   0x70}, {"JNO",  0x71},
            {"JB",   0x72}, {"JNAE", 0x72}, {"JC",   0x72},
            {"JNB",  0x73}, {"JAE",  0x73}, {"JNC",  0x73},
            {"JZ",   0x74}, {"JE",   0x74},
            {"JNZ",  0x75}, {"JNE",  0x75},
            {"JBE",  0x76}, {"JNA",  0x76},
            {"JA",   0x77}, {"JNBE", 0x77},
            {"JS",   0x78}, {"JNS",  0x79},
            {"JP",   0x7A}, {"JPE",  0x7A},
            {"JNP",  0x7B}, {"JPO",  0x7B},
            {"JL",   0x7C}, {"JNGE", 0x7C},
            {"JGE",  0x7D}, {"JNL",  0x7D},
            {"JLE",  0x7E}, {"JNG",  0x7E},
            {"JG",   0x7F}, {"JNLE", 0x7F}
        };

        // Inversion table: each Jcc maps to its opposite condition
        static const map<string, string> jccInversions = {
            {"JZ", "JNZ"}, {"JE", "JNE"}, {"JNZ", "JZ"}, {"JNE", "JE"},
            {"JL", "JGE"}, {"JNGE", "JGE"}, {"JG", "JLE"}, {"JNLE", "JLE"},
            {"JLE", "JG"}, {"JNG", "JG"}, {"JGE", "JL"}, {"JNL", "JL"},
            {"JB", "JNB"}, {"JNAE", "JNB"}, {"JC", "JNC"}, {"JA", "JBE"},
            {"JNBE", "JBE"}, {"JBE", "JA"}, {"JNA", "JA"}, {"JAE", "JB"},
            {"JNB", "JB"}, {"JNC", "JC"}, {"JS", "JNS"}, {"JNS", "JS"},
            {"JO", "JNO"}, {"JNO", "JO"}, {"JP", "JNP"}, {"JPE", "JNP"},
            {"JNP", "JP"}, {"JPO", "JP"}
        };

        auto it = jccOpcodes.find(mnemonic);
        if (it != jccOpcodes.end()) {
            int targetAddr = 0;
            if (op1.type == Operand::IMMEDIATE) targetAddr = op1.val;

            bool promoted = ctx.promotedJumps.count(lineNum) > 0;

            if (promoted) {
                // Emit promoted form: inverted-Jcc over a near JMP
                // Layout: [inverted-Jcc +5] [JMP near target] = 2 + 3 = 5 bytes
                auto inv = jccInversions.find(mnemonic);
                string invMnemonic = (inv != jccInversions.end()) ? inv->second : "JNO"; // fallback
                auto invIt = jccOpcodes.find(invMnemonic);
                uint8_t invOpcode = (invIt != jccOpcodes.end()) ? invIt->second : 0x71;

                // inverted Jcc: skip over the 3-byte JMP (displacement = +3)
                emitByte(ctx, invOpcode);
                emitByte(ctx, 0x03);
                // JMP near rel16: E9 + 16-bit offset
                int jmpOffset = targetAddr - (ctx.currentAddress + 3);
                emitByte(ctx, 0xE9);
                emitWord(ctx, jmpOffset & 0xFFFF);
            } else {
                // Short form: 2 bytes
                int offset = targetAddr - (ctx.currentAddress + 2);

                if (ctx.detectPromotions && (offset < -128 || offset > 127)) {
                    // Mark for promotion — will trigger a re-run of pass 1
                    ctx.promotedJumps.insert(lineNum);
                }

                emitByte(ctx, it->second);
                emitByte(ctx, offset & 0xFF);
            }
        }
    }
    // 8. Loop Instructions
    else if (mnemonic == "LOOP" || mnemonic == "LOOPE" || mnemonic == "LOOPZ" || 
             mnemonic == "LOOPNE" || mnemonic == "LOOPNZ" || mnemonic == "JCXZ") {
        
        int targetAddr = 0;
        if (op1.type == Operand::IMMEDIATE) targetAddr = op1.val;
        
        // Loop instructions are 2 bytes. Offset = Target - (Current + 2)
        int offset = targetAddr - (ctx.currentAddress + 2);
        
        if (!ctx.isPass1) {
            if (offset < -128 || offset > 127) {
                logError(ctx, tokens[0].line, "Loop jump out of range (" + to_string(offset) + ")", "Displacement is " + to_string(offset) + " bytes (range: -128 to +127). Replace LOOP with an explicit decrement and near jump: DEC CX / JNZ target. For LOOPE/LOOPNE, add the additional flag check before the JNZ.");
            }
        }

        if (mnemonic == "LOOP") emitByte(ctx, 0xE2);
        else if (mnemonic == "LOOPE" || mnemonic == "LOOPZ") emitByte(ctx, 0xE1);
        else if (mnemonic == "LOOPNE" || mnemonic == "LOOPNZ") emitByte(ctx, 0xE0);
        else if (mnemonic == "JCXZ") emitByte(ctx, 0xE3);
        
        emitByte(ctx, offset & 0xFF);
    }
    // 8. Stack Operations
    else if (mnemonic == "PUSH" || mnemonic == "POP") {
        if (op1.type == Operand::REGISTER) {
            if (op1.size != 16) {
                string regName = getRegName(op1.reg, op1.size);
                string hint;
                if (op1.reg < 4) {
                    static const string upgrades[] = {"AX", "CX", "DX", "BX"};
                    hint = "'" + regName + "' is 8-bit. PUSH/POP require 16-bit registers. Use " + upgrades[op1.reg] + " instead.";
                } else {
                    hint = "'" + regName + "' is 8-bit. PUSH/POP require 16-bit registers (AX, BX, CX, DX, SI, DI, BP, SP).";
                }
                logError(ctx, tokens[0].line, "Stack ops require 16-bit register", hint); return;
            }
            if (mnemonic == "PUSH") emitByte(ctx, 0x50 + op1.reg);
            else emitByte(ctx, 0x58 + op1.reg);
        } 
        else if (op1.type == Operand::MEMORY) {
            // PUSH r/m16: FF /6
            // POP r/m16:  8F /0
            if (mnemonic == "PUSH") emitByte(ctx, 0xFF);
            else emitByte(ctx, 0x8F);

            int ext = (mnemonic == "PUSH") ? 6 : 0;
            
            emitModRM(ctx, ext, op1);
        }
        else if (op1.type == Operand::SEGREG) {
            // PUSH seg: ES=06, CS=0E, SS=16, DS=1E
            // POP  seg: ES=07, CS=0F(invalid), SS=17, DS=1F
            static const uint8_t pushSeg[] = { 0x06, 0x0E, 0x16, 0x1E };
            static const uint8_t popSeg[]  = { 0x07, 0x0F, 0x17, 0x1F };
            if (op1.reg >= 0 && op1.reg < 4) {
                if (mnemonic == "PUSH") emitByte(ctx, pushSeg[op1.reg]);
                else {
                    if (op1.reg == 1) { logError(ctx, tokens[0].line, "POP CS is not a valid instruction", "POP CS is architecturally invalid on 8086. To change CS, use a far JMP or far CALL."); return; }
                    emitByte(ctx, popSeg[op1.reg]);
                }
            }
        }
        else {
            logError(ctx, tokens[0].line, "Invalid stack operand", "PUSH/POP accept: 16-bit register (AX, BX, etc.), memory (WORD [addr]), or segment register (DS, ES, SS). Immediates and 8-bit registers are not valid.");
        }
    }
    // 9. String Instructions
    else if (mnemonic == "MOVSB") emitByte(ctx, 0xA4);
    else if (mnemonic == "MOVSW") emitByte(ctx, 0xA5);
    else if (mnemonic == "CMPSB") emitByte(ctx, 0xA6);
    else if (mnemonic == "CMPSW") emitByte(ctx, 0xA7);
    else if (mnemonic == "STOSB") emitByte(ctx, 0xAA);
    else if (mnemonic == "STOSW") emitByte(ctx, 0xAB);
    else if (mnemonic == "LODSB") emitByte(ctx, 0xAC);
    else if (mnemonic == "LODSW") emitByte(ctx, 0xAD);
    else if (mnemonic == "SCASB") emitByte(ctx, 0xAE);
    else if (mnemonic == "SCASW") emitByte(ctx, 0xAF);
    // 8. Call / Ret
    else if (mnemonic == "CALL") {
        if (op1.type == Operand::REGISTER && op1.size == 16) {
            // CALL r16: FF /2 with mod=11
            emitByte(ctx, 0xFF);
            emitByte(ctx, 0xC0 | (2 << 3) | op1.reg);
        } else if (op1.type == Operand::MEMORY) {
            // CALL [mem]: FF /2
            emitByte(ctx, 0xFF);
            emitModRM(ctx, 2, op1);
        } else {
            // Near CALL (E8) — label/immediate target
            int targetAddr = 0;
            if (op1.type == Operand::IMMEDIATE) targetAddr = op1.val;
            int offset = targetAddr - (ctx.currentAddress + 3);
            emitByte(ctx, 0xE8);
            emitWord(ctx, offset & 0xFFFF);
        }
    }
    else if (mnemonic == "RET") {
        emitByte(ctx, 0xC3);
    }
    // 10. Flag Instructions
    else if (mnemonic == "CLD") emitByte(ctx, 0xFC);
    else if (mnemonic == "STD") emitByte(ctx, 0xFD);
    else if (mnemonic == "CLI") emitByte(ctx, 0xFA);
    else if (mnemonic == "STI") emitByte(ctx, 0xFB);
    else if (mnemonic == "CMC") emitByte(ctx, 0xF5);
    else if (mnemonic == "CLC") emitByte(ctx, 0xF8);
    else if (mnemonic == "STC") emitByte(ctx, 0xF9);

    // 11. NOP, CBW, CWD, LAHF, SAHF, PUSHF, POPF
    else if (mnemonic == "NOP")   emitByte(ctx, 0x90);
    else if (mnemonic == "CBW")   emitByte(ctx, 0x98);
    else if (mnemonic == "CWD")   emitByte(ctx, 0x99);
    else if (mnemonic == "LAHF")  emitByte(ctx, 0x9F);
    else if (mnemonic == "SAHF")  emitByte(ctx, 0x9E);
    else if (mnemonic == "PUSHF") emitByte(ctx, 0x9C);
    else if (mnemonic == "POPF")  emitByte(ctx, 0x9D);
    else if (mnemonic == "XLAT" || mnemonic == "XLATB") emitByte(ctx, 0xD7);
    else if (mnemonic == "HLT")   emitByte(ctx, 0xF4);
    else if (mnemonic == "PUSHA") emitByte(ctx, 0x60);
    else if (mnemonic == "POPA")  emitByte(ctx, 0x61);

    // 12. XCHG
    else if (mnemonic == "XCHG") {
        if (op1.type == Operand::REGISTER && op2.type == Operand::REGISTER && op1.size == op2.size) {
            // Short form: XCHG AX, r16 (0x90+reg) or XCHG r16, AX
            if (op1.size == 16 && op1.reg == 0) {
                emitByte(ctx, 0x90 + op2.reg);  // XCHG AX, r16
            } else if (op1.size == 16 && op2.reg == 0) {
                emitByte(ctx, 0x90 + op1.reg);  // XCHG r16, AX
            } else {
                // General reg,reg: 86/87 r/m, r
                if (op1.size == 8) {
                    emitByte(ctx, 0x86);
                } else {
                    emitByte(ctx, 0x87);
                }
                emitByte(ctx, (3 << 6) | (op2.reg << 3) | op1.reg);
            }
        } else if (op1.type == Operand::REGISTER && op2.type == Operand::MEMORY) {
            if (op1.size == 8) emitByte(ctx, 0x86); else emitByte(ctx, 0x87);
            emitModRM(ctx, op1.reg, op2);
        } else if (op1.type == Operand::MEMORY && op2.type == Operand::REGISTER) {
            if (op2.size == 8) emitByte(ctx, 0x86); else emitByte(ctx, 0x87);
            emitModRM(ctx, op2.reg, op1);
        }
    }

    // Drift detection: if ISA said this mnemonic is valid but no code path handled it,
    // the instruction silently produced nothing. Flag it.
    if (!ctx.isPass1 && ctx.currentLineBytes.empty() && !ctx.globalError) {
        logError(ctx, tokens[0].line, "Internal: mnemonic '" + mnemonic + "' passed ISA validation but has no code path in assembleLine", "This is an assembler bug. The instruction is listed in the ISA database but no encoder handles it. Please report this.");
    }
    if (!ctx.isPass1) {
        BinaryMap bm;
        bm.address = startAddr;
        bm.sourceLine = lineNum;
        bm.bytes = ctx.currentLineBytes;
        bm.sourceCode = sourceLine;
        
        // --- NEW FIELDS ---
        bm.size = (int)ctx.currentLineBytes.size();
        bm.decoded = decodedStr;
        
        ctx.agentState.listing.push_back(bm);
    }
}

void emitAgentJSON(const AssemblerContext& ctx, const vector<SourceLocation>& sourceMap = {}) {
    cout << "{" << endl;

    // 1. Success Status
    cout << "  \"success\": " << (ctx.globalError ? "false" : "true") << "," << endl;

    // 2. Diagnostics
    cout << "  \"diagnostics\": [" << endl;
    for (size_t i = 0; i < ctx.agentState.diagnostics.size(); ++i) {
        const auto& d = ctx.agentState.diagnostics[i];
        cout << "    { \"level\": \"" << d.level << "\", \"line\": " << d.line;
        // Add source file info if available
        if (!sourceMap.empty() && d.line > 0 && d.line <= (int)sourceMap.size()) {
            const auto& loc = sourceMap[d.line - 1];
            cout << ", \"file\": \"" << jsonEscape(loc.file) << "\", \"sourceLine\": " << loc.line;
        }
        cout << ", \"msg\": \"" << jsonEscape(d.message) << "\", \"hint\": \"" << jsonEscape(d.hint) << "\" }";
        if (i < ctx.agentState.diagnostics.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]," << endl;

    // 3. Symbol Table
    cout << "  \"symbols\": {" << endl;
    int count = 0;
    for (const auto& kv : ctx.symbolTable) {
        cout << "    \"" << jsonEscape(kv.first) << "\": { "
             << "\"val\": " << kv.second.value << ", "
             << "\"type\": \"" << (kv.second.isConstant ? "EQU" : "LABEL") << "\", "
             << "\"line\": " << kv.second.definedLine;
        if (!sourceMap.empty() && kv.second.definedLine > 0 && kv.second.definedLine <= (int)sourceMap.size()) {
            const auto& loc = sourceMap[kv.second.definedLine - 1];
            cout << ", \"file\": \"" << jsonEscape(loc.file) << "\", \"sourceLine\": " << loc.line;
        }
        cout << " }";
        if (++count < (int)ctx.symbolTable.size()) cout << ",";
        cout << endl;
    }
    cout << "  }," << endl;

    // 4. Source Mapping (The "View")
    cout << "  \"listing\": [" << endl;
    for (size_t i = 0; i < ctx.agentState.listing.size(); ++i) {
        const auto& item = ctx.agentState.listing[i];
         cout << "    { \"addr\": " << item.address
             << ", \"line\": " << item.sourceLine
             << ", \"size\": " << item.size
             << ", \"decoded\": \"" << jsonEscape(item.decoded) << "\"";
        if (!sourceMap.empty() && item.sourceLine > 0 && item.sourceLine <= (int)sourceMap.size()) {
            const auto& loc = sourceMap[item.sourceLine - 1];
            cout << ", \"file\": \"" << jsonEscape(loc.file) << "\", \"sourceLine\": " << loc.line;
        }
        cout << ", \"src\": \"" << jsonEscape(item.sourceCode) << "\""
             << ", \"bytes\": [";
        for(size_t b=0; b<item.bytes.size(); ++b) {
            cout << (int)item.bytes[b];
            if(b < item.bytes.size()-1) cout << ",";
        }
        cout << "] }";
        if (i < ctx.agentState.listing.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]," << endl;

    // 5. Include file list
    cout << "  \"includes\": [";
    if (!sourceMap.empty()) {
        set<string> seen;
        vector<string> uniqueFiles;
        for (const auto& loc : sourceMap) {
            if (seen.insert(loc.file).second) uniqueFiles.push_back(loc.file);
        }
        for (size_t i = 0; i < uniqueFiles.size(); ++i) {
            cout << "\"" << jsonEscape(uniqueFiles[i]) << "\"";
            if (i < uniqueFiles.size() - 1) cout << ", ";
        }
    }
    cout << "]" << endl;

    cout << "}" << endl;
}


// ============================================================
// SHARED INSTRUCTION DECODER
// ============================================================
// This decoder produces structured output consumed by:
//   1. The disassembler (uses .text fields for display)
//   2. A future emulator (uses structured fields for execution)
//
// Design: every decoded instruction carries BOTH human-readable
// text AND machine-readable operand descriptors. The disassembler
// uses formatInstruction() for text; the emulator will use the
// OpKind/reg/memRM/disp fields to read/write registers and memory.
// ============================================================

// --- Operand Kinds (for emulator consumption) ---
enum class OpKind {
    NONE,       // No operand in this position
    REG8,       // 8-bit general register (reg index 0-7: AL,CL,DL,BL,AH,CH,DH,BH)
    REG16,      // 16-bit general register (reg index 0-7: AX,CX,DX,BX,SP,BP,SI,DI)
    SREG,       // Segment register (reg index 0-3: ES,CS,SS,DS)
    MEM,        // Memory operand (memRM + disp define address)
    IMM8,       // 8-bit immediate (value in disp)
    IMM16       // 16-bit immediate (value in disp)
};

struct DecodedOperand {
    OpKind kind = OpKind::NONE;
    int reg = 0;        // Register index (0-7 for GP, 0-3 for SREG)
    int memRM = -1;     // Memory R/M field: -1=direct, 0=BX+SI, 1=BX+DI, 2=BP+SI,
                        //   3=BP+DI, 4=SI, 5=DI, 6=BP, 7=BX
    int disp = 0;       // Displacement (MEM) or value (IMM8/IMM16)
    int size = 0;       // Operand size: 8 or 16
    string text;        // Formatted text for disassembly output
};

struct DecodedInst {
    bool valid = false;
    int size = 0;           // Total bytes consumed
    uint8_t opcode = 0;     // Primary opcode byte (after prefixes)
    string mnemonic;        // e.g., "MOV", "ADD", "JZ"

    DecodedOperand op1;
    DecodedOperand op2;

    bool wide = false;      // true = 16-bit operation, false = 8-bit
    int segOverride = -1;   // Segment override prefix (-1 = none, 0x26=ES, 0x2E=CS, 0x36=SS, 0x3E=DS)
    bool hasRep = false;    // REP/REPE/REPZ prefix present
    bool hasRepne = false;  // REPNE/REPNZ prefix present
    int prefixBytes = 0;    // Number of prefix bytes consumed
    string prefixText;      // Formatted prefix string: "REP ", "ES: " etc.

    int jumpTarget = -1;    // Absolute target for JMP/CALL/Jcc/LOOP (-1 if N/A)
    int modrmExt = -1;      // ModR/M reg field when used as opcode extension (e.g., ALU group)
};

// --- Enhanced ModR/M Result ---

struct ModRMResult {
    string operand;     // Formatted string: "AX", "[BX+SI+0x04]", "[0x0100]", etc.
    int reg;            // The reg field (bits 5-3), for register operand or opcode extension
    int bytesConsumed;  // 1 (ModR/M only) + 0/1/2 (displacement)
    // Structured fields for emulator use:
    int mod;            // Mod field (0-3): 3=register, 0-2=memory with displacement
    int rm;             // R/M field (0-7): register index or addressing mode
    bool isReg;         // true if mod==3 (operand is a register, not memory)
    int disp;           // Displacement value (0 if none, or disp8/disp16)
};

// --- Safe Byte Reading Helpers ---

bool hasBytesAt(const vector<uint8_t>& code, int offset, int count) {
    return (offset + count) <= (int)code.size();
}

uint8_t readByte(const vector<uint8_t>& code, int offset) {
    if (offset < (int)code.size()) return code[offset];
    return 0;
}

uint16_t readWord(const vector<uint8_t>& code, int offset) {
    if (offset + 1 < (int)code.size()) return code[offset] | (code[offset+1] << 8);
    return 0;
}

int8_t readSignedByte(const vector<uint8_t>& code, int offset) {
    return (int8_t)readByte(code, offset);
}

// --- Hex Formatting Helpers ---

string hexByte(uint8_t b) {
    stringstream ss;
    ss << uppercase << hex << setfill('0') << setw(2) << (int)b;
    return ss.str();
}

string hexBytes(const vector<uint8_t>& bytes) {
    stringstream ss;
    for (size_t i = 0; i < bytes.size(); ++i) {
        ss << hexByte(bytes[i]);
        if (i < bytes.size() - 1) ss << " ";
    }
    return ss.str();
}

string hexImm8(uint8_t val) {
    stringstream ss;
    ss << "0x" << uppercase << hex << setfill('0') << setw(2) << (int)val;
    return ss.str();
}

string hexImm16(uint16_t val) {
    stringstream ss;
    ss << "0x" << uppercase << hex << setfill('0') << setw(4) << val;
    return ss.str();
}

string dispStr(int val) {
    if (val == 0) return "";
    stringstream ss;
    ss << (val > 0 ? "+" : "-") << "0x" << uppercase << hex << setfill('0') << setw(2) << abs(val);
    return ss.str();
}

string dispStr16(int val) {
    if (val == 0) return "";
    stringstream ss;
    ss << (val > 0 ? "+" : "-") << "0x" << uppercase << hex << setfill('0') << setw(4) << abs(val);
    return ss.str();
}

// --- Enhanced ModR/M Decoder ---

ModRMResult decodeModRM(const vector<uint8_t>& code, int offset, int operandSize) {
    ModRMResult res;
    res.bytesConsumed = 0;
    res.mod = 0;
    res.rm = 0;
    res.isReg = false;
    res.disp = 0;
    res.reg = 0;

    if (!hasBytesAt(code, offset, 1)) return res;

    uint8_t modrm = readByte(code, offset);
    res.mod = (modrm >> 6) & 3;
    res.reg = (modrm >> 3) & 7;
    res.rm = modrm & 7;
    res.bytesConsumed = 1;

    // Register-to-register (mod == 3)
    if (res.mod == 3) {
        res.isReg = true;
        res.disp = 0;
        if (operandSize == 8) {
            static const string regs8[] = { "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH" };
            res.operand = regs8[res.rm];
        } else {
            static const string regs16[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
            res.operand = regs16[res.rm];
        }
        return res;
    }

    // Memory access
    res.isReg = false;
    res.operand = "[";

    if (res.mod == 0 && res.rm == 6) {
        // Direct address
        if (!hasBytesAt(code, offset+1, 2)) { res.bytesConsumed = 0; return res; }
        uint16_t d = readWord(code, offset+1);
        res.bytesConsumed += 2;
        res.disp = d;
        res.operand += hexImm16(d);
    } else {
        switch(res.rm) {
            case 0: res.operand += "BX+SI"; break;
            case 1: res.operand += "BX+DI"; break;
            case 2: res.operand += "BP+SI"; break;
            case 3: res.operand += "BP+DI"; break;
            case 4: res.operand += "SI"; break;
            case 5: res.operand += "DI"; break;
            case 6: res.operand += "BP"; break;
            case 7: res.operand += "BX"; break;
        }

        if (res.mod == 1) {
            if (!hasBytesAt(code, offset + res.bytesConsumed, 1)) { res.bytesConsumed = 0; return res; }
            int8_t d = readSignedByte(code, offset + res.bytesConsumed);
            res.bytesConsumed += 1;
            res.disp = d;
            res.operand += dispStr(d);
        } else if (res.mod == 2) {
            if (!hasBytesAt(code, offset + res.bytesConsumed, 2)) { res.bytesConsumed = 0; return res; }
            int16_t d = (int16_t)readWord(code, offset + res.bytesConsumed);
            res.bytesConsumed += 2;
            res.disp = d;
            res.operand += dispStr16(d);
        } else {
            res.disp = 0;
        }
    }

    res.operand += "]";
    return res;
}

// --- Operand Construction Helpers ---
// These build DecodedOperand structs with both structured data and display text.

DecodedOperand makeReg8(int reg) {
    static const string names[] = {"AL","CL","DL","BL","AH","CH","DH","BH"};
    DecodedOperand op;
    op.kind = OpKind::REG8;
    op.reg = reg & 7;
    op.size = 8;
    op.text = names[op.reg];
    return op;
}

DecodedOperand makeReg16(int reg) {
    static const string names[] = {"AX","CX","DX","BX","SP","BP","SI","DI"};
    DecodedOperand op;
    op.kind = OpKind::REG16;
    op.reg = reg & 7;
    op.size = 16;
    op.text = names[op.reg];
    return op;
}

DecodedOperand makeSreg(int reg) {
    static const string names[] = {"ES","CS","SS","DS"};
    DecodedOperand op;
    op.kind = OpKind::SREG;
    op.reg = reg;
    op.size = 16;
    op.text = (reg >= 0 && reg < 4) ? names[reg] : "???";
    return op;
}

DecodedOperand makeImm8(uint8_t val) {
    DecodedOperand op;
    op.kind = OpKind::IMM8;
    op.disp = val;
    op.size = 8;
    op.text = hexImm8(val);
    return op;
}

DecodedOperand makeImm16(uint16_t val) {
    DecodedOperand op;
    op.kind = OpKind::IMM16;
    op.disp = val;
    op.size = 16;
    op.text = hexImm16(val);
    return op;
}

// Convert a ModR/M result into a structured operand
DecodedOperand modrmToOperand(const ModRMResult& m, int operandSize) {
    DecodedOperand op;
    op.text = m.operand;
    op.size = operandSize;
    op.disp = m.disp;

    if (m.isReg) {
        op.kind = (operandSize == 8) ? OpKind::REG8 : OpKind::REG16;
        op.reg = m.rm;
        op.memRM = -1;
    } else {
        op.kind = OpKind::MEM;
        op.memRM = (m.mod == 0 && m.rm == 6) ? -1 : m.rm;  // -1 = direct address
    }

    return op;
}

// Build a register operand from the ModR/M reg field
DecodedOperand regFromField(int reg, int size) {
    return (size == 8) ? makeReg8(reg) : makeReg16(reg);
}

// Add "BYTE " or "WORD " prefix to a memory operand's display text
void addSizePrefix(DecodedOperand& op) {
    if (op.kind == OpKind::MEM) {
        op.text = (op.size == 8 ? "BYTE " : "WORD ") + op.text;
    }
}

// --- Instruction Formatting ---
// Produces the same text output as the old disassembler for backward compatibility.

string formatInstruction(const DecodedInst& inst) {
    string result = inst.prefixText + inst.mnemonic;
    if (inst.op1.kind != OpKind::NONE) {
        result += " " + inst.op1.text;
        if (inst.op2.kind != OpKind::NONE) {
            result += ", " + inst.op2.text;
        }
    }
    return result;
}

// --- Main Decoder Function ---
// Decodes one instruction from the byte stream at the given offset.
// Returns a fully populated DecodedInst with both structured data and display text.
// On failure, returns DecodedInst with valid=false and size=0.

DecodedInst decodeInstruction(const vector<uint8_t>& code, int offset) {
    DecodedInst inst;
    if (offset >= (int)code.size()) return inst;

    int current = offset;

    // --- Decode prefixes ---
    while (hasBytesAt(code, current, 1)) {
        uint8_t b = readByte(code, current);
        if (b == 0xF2)      { inst.hasRepne = true; inst.prefixText += "REPNE "; }
        else if (b == 0xF3) { inst.hasRep = true;   inst.prefixText += "REP "; }
        else if (b == 0x26) { inst.segOverride = 0x26; inst.prefixText += "ES: "; }
        else if (b == 0x2E) { inst.segOverride = 0x2E; inst.prefixText += "CS: "; }
        else if (b == 0x36) { inst.segOverride = 0x36; inst.prefixText += "SS: "; }
        else if (b == 0x3E) { inst.segOverride = 0x3E; inst.prefixText += "DS: "; }
        else break;

        inst.prefixBytes++;
        current++;
    }

    if (!hasBytesAt(code, current, 1)) return inst;

    inst.opcode = readByte(code, current);
    current++;  // Consume opcode byte

    uint8_t opcode = inst.opcode;

    // Shorthand: mark instruction as successfully decoded
    auto finish = [&](int totalSize) {
        inst.valid = true;
        inst.size = totalSize;
    };

    // ================================================================
    // MOV instructions
    // ================================================================

    if (opcode == 0x88) {  // MOV r/m8, r8
        ModRMResult m = decodeModRM(code, current, 8);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = false;
        inst.op1 = modrmToOperand(m, 8);
        inst.op2 = makeReg8(m.reg);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode == 0x89) {  // MOV r/m16, r16
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = modrmToOperand(m, 16);
        inst.op2 = makeReg16(m.reg);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode == 0x8A) {  // MOV r8, r/m8
        ModRMResult m = decodeModRM(code, current, 8);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = false;
        inst.op1 = makeReg8(m.reg);
        inst.op2 = modrmToOperand(m, 8);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode == 0x8B) {  // MOV r16, r/m16
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = makeReg16(m.reg);
        inst.op2 = modrmToOperand(m, 16);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode == 0x8C) {  // MOV r/m16, Sreg
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = modrmToOperand(m, 16);
        inst.op2 = makeSreg(m.reg);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode == 0x8E) {  // MOV Sreg, r/m16
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = makeSreg(m.reg);
        inst.op2 = modrmToOperand(m, 16);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }
    else if (opcode >= 0xB0 && opcode <= 0xB7) {  // MOV r8, imm8
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "MOV"; inst.wide = false;
        inst.op1 = makeReg8(opcode & 7);
        inst.op2 = makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 2);
    }
    else if (opcode >= 0xB8 && opcode <= 0xBF) {  // MOV r16, imm16
        if (!hasBytesAt(code, current, 2)) return inst;
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = makeReg16(opcode & 7);
        inst.op2 = makeImm16(readWord(code, current));
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xC6) {  // MOV r/m8, imm8
        ModRMResult m = decodeModRM(code, current, 8);
        if (!m.bytesConsumed || m.reg != 0) return inst;
        if (!hasBytesAt(code, current + m.bytesConsumed, 1)) return inst;
        inst.mnemonic = "MOV"; inst.wide = false; inst.modrmExt = 0;
        inst.op1 = modrmToOperand(m, 8);
        addSizePrefix(inst.op1);
        inst.op2 = makeImm8(readByte(code, current + m.bytesConsumed));
        finish(inst.prefixBytes + 1 + m.bytesConsumed + 1);
    }
    else if (opcode == 0xC7) {  // MOV r/m16, imm16
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed || m.reg != 0) return inst;
        if (!hasBytesAt(code, current + m.bytesConsumed, 2)) return inst;
        inst.mnemonic = "MOV"; inst.wide = true; inst.modrmExt = 0;
        inst.op1 = modrmToOperand(m, 16);
        addSizePrefix(inst.op1);
        inst.op2 = makeImm16(readWord(code, current + m.bytesConsumed));
        finish(inst.prefixBytes + 1 + m.bytesConsumed + 2);
    }

    // ================================================================
    // ALU reg/mem forms: ADD, OR, ADC, SBB, AND, SUB, XOR, CMP
    // Opcodes 0x00-0x3B where bit 2 = 0 (reg/mem operands)
    // ================================================================

    else if (opcode < 0x40 && (opcode & 4) == 0) {
        static const string mnemonics[] = { "ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP" };
        int opType = (opcode >> 3) & 7;
        inst.mnemonic = mnemonics[opType];
        inst.wide = (opcode & 1) != 0;
        bool dirToReg = (opcode & 2) != 0;
        int opSize = inst.wide ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;

        if (dirToReg) {
            inst.op1 = regFromField(m.reg, opSize);
            inst.op2 = modrmToOperand(m, opSize);
        } else {
            inst.op1 = modrmToOperand(m, opSize);
            inst.op2 = regFromField(m.reg, opSize);
        }
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // ALU accumulator immediate forms: ADD AL/AX, imm
    // Opcodes 0x04/0x05, 0x0C/0x0D, ... 0x3C/0x3D (bit pattern: bits 2:1 = 10)
    // ================================================================

    else if (opcode < 0x40 && (opcode & 6) == 4) {
        static const string mnemonics[] = { "ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP" };
        int opType = (opcode >> 3) & 7;
        inst.mnemonic = mnemonics[opType];
        inst.wide = (opcode & 1) != 0;
        int immSize = inst.wide ? 2 : 1;

        if (!hasBytesAt(code, current, immSize)) return inst;

        inst.op1 = inst.wide ? makeReg16(0) : makeReg8(0);  // AL or AX
        inst.op2 = inst.wide ? makeImm16(readWord(code, current)) : makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 1 + immSize);
    }

    // ================================================================
    // ALU immediate group: 80/81/82/83
    // ================================================================

    else if (opcode == 0x80 || opcode == 0x81 || opcode == 0x82 || opcode == 0x83) {
        static const string mnemonics[] = { "ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP" };
        bool isWord = (opcode == 0x81 || opcode == 0x83);
        bool isSignExt = (opcode == 0x83);
        int opSize = isWord ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;

        inst.mnemonic = mnemonics[m.reg];
        inst.wide = isWord;
        inst.modrmExt = m.reg;
        inst.op1 = modrmToOperand(m, opSize);
        addSizePrefix(inst.op1);

        int immSize = (isWord && !isSignExt) ? 2 : 1;
        if (!hasBytesAt(code, current + m.bytesConsumed, immSize)) return inst;

        int immVal = 0;
        if (immSize == 1) immVal = (uint8_t)readByte(code, current + m.bytesConsumed);
        else immVal = readWord(code, current + m.bytesConsumed);

        if (isSignExt) immVal = (int16_t)(int8_t)immVal;

        inst.op2 = isWord ? makeImm16(immVal & 0xFFFF) : makeImm8(immVal & 0xFF);
        finish(inst.prefixBytes + 1 + m.bytesConsumed + immSize);
    }

    // ================================================================
    // TEST r/m, r (84/85)
    // ================================================================

    else if (opcode == 0x84 || opcode == 0x85) {
        inst.wide = (opcode == 0x85);
        int opSize = inst.wide ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;

        inst.mnemonic = "TEST";
        inst.op1 = modrmToOperand(m, opSize);
        inst.op2 = regFromField(m.reg, opSize);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // Group 3: F6/F7 — TEST, NOT, NEG, MUL, IMUL, DIV, IDIV
    // ================================================================

    else if (opcode == 0xF6 || opcode == 0xF7) {
        inst.wide = (opcode == 0xF7);
        int opSize = inst.wide ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;
        inst.modrmExt = m.reg;

        if (m.reg == 0) {  // TEST r/m, imm
            int immSize = inst.wide ? 2 : 1;
            if (!hasBytesAt(code, current + m.bytesConsumed, immSize)) return inst;

            inst.mnemonic = "TEST";
            inst.op1 = modrmToOperand(m, opSize);
            addSizePrefix(inst.op1);
            inst.op2 = inst.wide ? makeImm16(readWord(code, current + m.bytesConsumed))
                                 : makeImm8(readByte(code, current + m.bytesConsumed));
            finish(inst.prefixBytes + 1 + m.bytesConsumed + immSize);
        }
        else if (m.reg == 1) {
            return inst;  // Undefined extension
        }
        else {
            static const string names[] = { "", "", "NOT", "NEG", "MUL", "IMUL", "DIV", "IDIV" };
            inst.mnemonic = names[m.reg];
            inst.op1 = modrmToOperand(m, opSize);
            addSizePrefix(inst.op1);
            finish(inst.prefixBytes + 1 + m.bytesConsumed);
        }
    }

    // ================================================================
    // Group 4: FE — INC/DEC r/m8
    // ================================================================

    else if (opcode == 0xFE) {
        ModRMResult m = decodeModRM(code, current, 8);
        if (!m.bytesConsumed) return inst;
        if (m.reg != 0 && m.reg != 1) return inst;

        inst.mnemonic = (m.reg == 0) ? "INC" : "DEC";
        inst.wide = false;
        inst.modrmExt = m.reg;
        inst.op1 = modrmToOperand(m, 8);
        addSizePrefix(inst.op1);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // Group 5: FF — INC/DEC/CALL/CALL FAR/JMP/JMP FAR/PUSH r/m16
    // ================================================================

    else if (opcode == 0xFF) {
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        inst.modrmExt = m.reg;

        switch(m.reg) {
            case 0: inst.mnemonic = "INC"; break;
            case 1: inst.mnemonic = "DEC"; break;
            case 2: inst.mnemonic = "CALL"; break;
            case 3: inst.mnemonic = "CALL FAR"; break;
            case 4: inst.mnemonic = "JMP"; break;
            case 5: inst.mnemonic = "JMP FAR"; break;
            case 6: inst.mnemonic = "PUSH"; break;
            default: return inst;
        }
        inst.wide = true;
        inst.op1 = modrmToOperand(m, 16);
        if (inst.op1.kind == OpKind::MEM && (m.reg == 0 || m.reg == 1)) {
            addSizePrefix(inst.op1);
        }
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // Short-form INC/DEC/PUSH/POP (register encoded in opcode)
    // ================================================================

    else if (opcode >= 0x40 && opcode <= 0x47) {  // INC r16
        inst.mnemonic = "INC"; inst.wide = true;
        inst.op1 = makeReg16(opcode & 7);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode >= 0x48 && opcode <= 0x4F) {  // DEC r16
        inst.mnemonic = "DEC"; inst.wide = true;
        inst.op1 = makeReg16(opcode & 7);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode >= 0x50 && opcode <= 0x57) {  // PUSH r16
        inst.mnemonic = "PUSH"; inst.wide = true;
        inst.op1 = makeReg16(opcode & 7);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode >= 0x58 && opcode <= 0x5F) {  // POP r16
        inst.mnemonic = "POP"; inst.wide = true;
        inst.op1 = makeReg16(opcode & 7);
        finish(inst.prefixBytes + 1);
    }

    // ================================================================
    // Segment register PUSH/POP
    // ================================================================

    else if (opcode == 0x06) { inst.mnemonic = "PUSH"; inst.op1 = makeSreg(0); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x0E) { inst.mnemonic = "PUSH"; inst.op1 = makeSreg(1); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x16) { inst.mnemonic = "PUSH"; inst.op1 = makeSreg(2); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x1E) { inst.mnemonic = "PUSH"; inst.op1 = makeSreg(3); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x07) { inst.mnemonic = "POP";  inst.op1 = makeSreg(0); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x17) { inst.mnemonic = "POP";  inst.op1 = makeSreg(2); finish(inst.prefixBytes + 1); }
    else if (opcode == 0x1F) { inst.mnemonic = "POP";  inst.op1 = makeSreg(3); finish(inst.prefixBytes + 1); }

    else if (opcode == 0x8F) {  // POP r/m16
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed || m.reg != 0) return inst;
        inst.mnemonic = "POP"; inst.wide = true; inst.modrmExt = 0;
        inst.op1 = modrmToOperand(m, 16);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // Shifts and Rotates
    // ================================================================

    // D0/D1: shift r/m, 1   D2/D3: shift r/m, CL
    else if (opcode == 0xD0 || opcode == 0xD1 || opcode == 0xD2 || opcode == 0xD3) {
        inst.wide = (opcode & 1) != 0;
        bool isCL = (opcode & 2) != 0;
        int opSize = inst.wide ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;
        inst.modrmExt = m.reg;

        static const string names[] = { "ROL", "ROR", "RCL", "RCR", "SHL", "SHR", "", "SAR" };
        if (m.reg == 6) return inst;  // Undefined
        inst.mnemonic = names[m.reg];
        inst.op1 = modrmToOperand(m, opSize);
        if (isCL) {
            inst.op2 = makeReg8(1);  // CL
        } else {
            inst.op2 = makeImm8(1);
            inst.op2.text = "1";     // Display as "1" not "0x01" for shift-by-one
        }
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // C0/C1: shift r/m, imm8 (80186+)
    else if (opcode == 0xC0 || opcode == 0xC1) {
        inst.wide = (opcode & 1) != 0;
        int opSize = inst.wide ? 16 : 8;

        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;
        if (!hasBytesAt(code, current + m.bytesConsumed, 1)) return inst;
        inst.modrmExt = m.reg;

        static const string names[] = { "ROL", "ROR", "", "", "SHL", "SHR", "", "" };
        if (names[m.reg].empty()) return inst;
        inst.mnemonic = names[m.reg];
        inst.op1 = modrmToOperand(m, opSize);
        inst.op2 = makeImm8(readByte(code, current + m.bytesConsumed));
        finish(inst.prefixBytes + 1 + m.bytesConsumed + 1);
    }

    // ================================================================
    // JMP / CALL / RET (direct near)
    // ================================================================

    else if (opcode == 0xE9) {  // JMP rel16
        if (!hasBytesAt(code, current, 2)) return inst;
        int16_t rel = (int16_t)readWord(code, current);
        inst.jumpTarget = (offset + 3 + inst.prefixBytes + rel) & 0xFFFF;
        inst.mnemonic = "JMP";
        inst.op1 = makeImm16(inst.jumpTarget);
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xE8) {  // CALL rel16
        if (!hasBytesAt(code, current, 2)) return inst;
        int16_t rel = (int16_t)readWord(code, current);
        inst.jumpTarget = (offset + 3 + inst.prefixBytes + rel) & 0xFFFF;
        inst.mnemonic = "CALL";
        inst.op1 = makeImm16(inst.jumpTarget);
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xC3) {  // RET
        inst.mnemonic = "RET";
        finish(inst.prefixBytes + 1);
    }

    // ================================================================
    // Conditional Jumps (all short, rel8)
    // ================================================================

    else if (opcode >= 0x70 && opcode <= 0x7F) {
        if (!hasBytesAt(code, current, 1)) return inst;
        int8_t rel = readSignedByte(code, current);
        inst.jumpTarget = (offset + 2 + inst.prefixBytes + rel) & 0xFFFF;

        static const string names[] = {
            "JO", "JNO", "JB", "JNB", "JZ", "JNZ", "JBE", "JA",
            "JS", "JNS", "JP", "JNP", "JL", "JGE", "JLE", "JG"
        };
        inst.mnemonic = names[opcode - 0x70];
        inst.op1 = makeImm16(inst.jumpTarget);
        finish(inst.prefixBytes + 2);
    }

    // ================================================================
    // LOOP / LOOPE / LOOPNE / JCXZ (short, rel8)
    // ================================================================

    else if (opcode == 0xE2 || opcode == 0xE1 || opcode == 0xE0 || opcode == 0xE3) {
        if (!hasBytesAt(code, current, 1)) return inst;
        int8_t rel = readSignedByte(code, current);
        inst.jumpTarget = (offset + 2 + inst.prefixBytes + rel) & 0xFFFF;

        if (opcode == 0xE2)      inst.mnemonic = "LOOP";
        else if (opcode == 0xE1) inst.mnemonic = "LOOPE";
        else if (opcode == 0xE0) inst.mnemonic = "LOOPNE";
        else                     inst.mnemonic = "JCXZ";

        inst.op1 = makeImm16(inst.jumpTarget);
        finish(inst.prefixBytes + 2);
    }

    // ================================================================
    // I/O Instructions
    // ================================================================

    else if (opcode == 0xE4) {  // IN AL, imm8
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "IN"; inst.wide = false;
        inst.op1 = makeReg8(0);
        inst.op2 = makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 2);
    }
    else if (opcode == 0xE5) {  // IN AX, imm8
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "IN"; inst.wide = true;
        inst.op1 = makeReg16(0);
        inst.op2 = makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 2);
    }
    else if (opcode == 0xE6) {  // OUT imm8, AL
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "OUT"; inst.wide = false;
        inst.op1 = makeImm8(readByte(code, current));
        inst.op2 = makeReg8(0);
        finish(inst.prefixBytes + 2);
    }
    else if (opcode == 0xE7) {  // OUT imm8, AX
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "OUT"; inst.wide = true;
        inst.op1 = makeImm8(readByte(code, current));
        inst.op2 = makeReg16(0);
        finish(inst.prefixBytes + 2);
    }
    else if (opcode == 0xEC) {  // IN AL, DX
        inst.mnemonic = "IN"; inst.wide = false;
        inst.op1 = makeReg8(0);
        inst.op2 = makeReg16(2);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode == 0xED) {  // IN AX, DX
        inst.mnemonic = "IN"; inst.wide = true;
        inst.op1 = makeReg16(0);
        inst.op2 = makeReg16(2);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode == 0xEE) {  // OUT DX, AL
        inst.mnemonic = "OUT"; inst.wide = false;
        inst.op1 = makeReg16(2);
        inst.op2 = makeReg8(0);
        finish(inst.prefixBytes + 1);
    }
    else if (opcode == 0xEF) {  // OUT DX, AX
        inst.mnemonic = "OUT"; inst.wide = true;
        inst.op1 = makeReg16(2);
        inst.op2 = makeReg16(0);
        finish(inst.prefixBytes + 1);
    }

    // ================================================================
    // LEA
    // ================================================================

    else if (opcode == 0x8D) {
        ModRMResult m = decodeModRM(code, current, 16);
        if (!m.bytesConsumed) return inst;
        if (m.isReg) return inst;  // LEA requires memory operand
        inst.mnemonic = "LEA"; inst.wide = true;
        inst.op1 = makeReg16(m.reg);
        inst.op2 = modrmToOperand(m, 16);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // INT
    // ================================================================

    else if (opcode == 0xCD) {
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "INT";
        inst.op1 = makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 2);
    }

    // ================================================================
    // String Instructions
    // ================================================================

    else if (opcode == 0xA4) { inst.mnemonic = "MOVSB"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xA5) { inst.mnemonic = "MOVSW"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xA6) { inst.mnemonic = "CMPSB"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xA7) { inst.mnemonic = "CMPSW"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAA) { inst.mnemonic = "STOSB"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAB) { inst.mnemonic = "STOSW"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAC) { inst.mnemonic = "LODSB"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAD) { inst.mnemonic = "LODSW"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAE) { inst.mnemonic = "SCASB"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xAF) { inst.mnemonic = "SCASW"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // Flag Instructions (previously missing from disassembler)
    // ================================================================

    else if (opcode == 0xFC) { inst.mnemonic = "CLD"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xFD) { inst.mnemonic = "STD"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xFA) { inst.mnemonic = "CLI"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xFB) { inst.mnemonic = "STI"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xF5) { inst.mnemonic = "CMC"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xF8) { inst.mnemonic = "CLC"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xF9) { inst.mnemonic = "STC"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // NOP (0x90 = XCHG AX, AX)
    // ================================================================

    else if (opcode == 0x90) { inst.mnemonic = "NOP"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // XCHG r16, AX (91-97) — not produced by assembler but useful for emulator
    // ================================================================

    else if (opcode >= 0x91 && opcode <= 0x97) {
        inst.mnemonic = "XCHG"; inst.wide = true;
        inst.op1 = makeReg16(0);          // AX
        inst.op2 = makeReg16(opcode & 7);
        finish(inst.prefixBytes + 1);
    }

    // ================================================================
    // CBW / CWD — useful for emulator
    // ================================================================

    else if (opcode == 0x98) { inst.mnemonic = "CBW"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0x99) { inst.mnemonic = "CWD"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // LAHF / SAHF
    // ================================================================

    else if (opcode == 0x9F) { inst.mnemonic = "LAHF"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0x9E) { inst.mnemonic = "SAHF"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // XCHG r/m, r (86/87)
    // ================================================================

    else if (opcode == 0x86 || opcode == 0x87) {
        inst.wide = (opcode & 1) != 0;
        int opSize = inst.wide ? 16 : 8;
        ModRMResult m = decodeModRM(code, current, opSize);
        if (!m.bytesConsumed) return inst;
        inst.mnemonic = "XCHG";
        inst.op1 = modrmToOperand(m, opSize);
        inst.op2 = regFromField(m.reg, opSize);
        finish(inst.prefixBytes + 1 + m.bytesConsumed);
    }

    // ================================================================
    // JMP short (0xEB) — rel8
    // ================================================================

    else if (opcode == 0xEB) {
        if (!hasBytesAt(code, current, 1)) return inst;
        int8_t rel = readSignedByte(code, current);
        inst.jumpTarget = (offset + 2 + inst.prefixBytes + rel) & 0xFFFF;
        inst.mnemonic = "JMP";
        inst.op1 = makeImm16(inst.jumpTarget);
        finish(inst.prefixBytes + 2);
    }

    // ================================================================
    // PUSHF / POPF
    // ================================================================

    else if (opcode == 0x9C) { inst.mnemonic = "PUSHF"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0x9D) { inst.mnemonic = "POPF"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xD7) { inst.mnemonic = "XLAT"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0xF4) { inst.mnemonic = "HLT"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0x60) { inst.mnemonic = "PUSHA"; finish(inst.prefixBytes + 1); }
    else if (opcode == 0x61) { inst.mnemonic = "POPA"; finish(inst.prefixBytes + 1); }

    // ================================================================
    // MOV with memory offset (A0-A3) — AL/AX ↔ direct address
    // ================================================================

    else if (opcode == 0xA0) {  // MOV AL, [moffs16]
        if (!hasBytesAt(code, current, 2)) return inst;
        uint16_t addr = readWord(code, current);
        inst.mnemonic = "MOV"; inst.wide = false;
        inst.op1 = makeReg8(0);  // AL
        DecodedOperand memOp;
        memOp.kind = OpKind::MEM; memOp.memRM = -1; memOp.disp = addr; memOp.size = 8;
        memOp.text = "[" + hexImm16(addr) + "]";
        inst.op2 = memOp;
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xA1) {  // MOV AX, [moffs16]
        if (!hasBytesAt(code, current, 2)) return inst;
        uint16_t addr = readWord(code, current);
        inst.mnemonic = "MOV"; inst.wide = true;
        inst.op1 = makeReg16(0);  // AX
        DecodedOperand memOp;
        memOp.kind = OpKind::MEM; memOp.memRM = -1; memOp.disp = addr; memOp.size = 16;
        memOp.text = "[" + hexImm16(addr) + "]";
        inst.op2 = memOp;
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xA2) {  // MOV [moffs16], AL
        if (!hasBytesAt(code, current, 2)) return inst;
        uint16_t addr = readWord(code, current);
        inst.mnemonic = "MOV"; inst.wide = false;
        DecodedOperand memOp;
        memOp.kind = OpKind::MEM; memOp.memRM = -1; memOp.disp = addr; memOp.size = 8;
        memOp.text = "[" + hexImm16(addr) + "]";
        inst.op1 = memOp;
        inst.op2 = makeReg8(0);  // AL
        finish(inst.prefixBytes + 3);
    }
    else if (opcode == 0xA3) {  // MOV [moffs16], AX
        if (!hasBytesAt(code, current, 2)) return inst;
        uint16_t addr = readWord(code, current);
        inst.mnemonic = "MOV"; inst.wide = true;
        DecodedOperand memOp;
        memOp.kind = OpKind::MEM; memOp.memRM = -1; memOp.disp = addr; memOp.size = 16;
        memOp.text = "[" + hexImm16(addr) + "]";
        inst.op1 = memOp;
        inst.op2 = makeReg16(0);  // AX
        finish(inst.prefixBytes + 3);
    }

    // ================================================================
    // TEST accumulator, immediate (A8/A9)
    // ================================================================

    else if (opcode == 0xA8) {  // TEST AL, imm8
        if (!hasBytesAt(code, current, 1)) return inst;
        inst.mnemonic = "TEST"; inst.wide = false;
        inst.op1 = makeReg8(0);
        inst.op2 = makeImm8(readByte(code, current));
        finish(inst.prefixBytes + 2);
    }
    else if (opcode == 0xA9) {  // TEST AX, imm16
        if (!hasBytesAt(code, current, 2)) return inst;
        inst.mnemonic = "TEST"; inst.wide = true;
        inst.op1 = makeReg16(0);
        inst.op2 = makeImm16(readWord(code, current));
        finish(inst.prefixBytes + 3);
    }

    // ================================================================
    // Fallback: unrecognized opcode
    // ================================================================

    // inst.valid remains false, inst.size remains 0

    return inst;
}

// --- Backward-Compatible Wrapper ---
// Thin adapter so disassembleFile doesn't need restructuring.

struct DisasmResult {
    bool valid;
    int size;
    string asmText;
};

DisasmResult disasmInstruction(const vector<uint8_t>& code, int offset) {
    DecodedInst inst = decodeInstruction(code, offset);
    return { inst.valid, inst.size, inst.valid ? formatInstruction(inst) : "" };
}

// ============================================================
// DISASSEMBLER (JSON output)
// ============================================================

void disassembleFile(const string& filename) {
    ifstream in(filename, ios::binary);
    if (!in) {
        cout << "{ \"error\": \"Cannot open file: " << jsonEscape(filename) << "\" }" << endl;
        return;
    }
    vector<uint8_t> code((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    in.close();

    cout << "{" << endl;
    cout << "  \"file\": \"" << jsonEscape(filename) << "\"," << endl;
    cout << "  \"fileSize\": " << code.size() << "," << endl;
    cout << "  \"instructions\": [" << endl;

    int offset = 0;
    int dataRunStart = -1;
    vector<uint8_t> dataRunBytes;

    struct DataRegion {
        int addr;
        vector<uint8_t> bytes;
    };
    vector<DataRegion> dataRegions;

    bool firstInstr = true;

    while (offset < (int)code.size()) {
        DecodedInst inst = decodeInstruction(code, offset);

        if (inst.valid) {
            // Flush any accumulated data run
            if (dataRunStart != -1) {
                dataRegions.push_back({dataRunStart, dataRunBytes});
                dataRunStart = -1;
                dataRunBytes.clear();
            }

            if (!firstInstr) cout << "," << endl;
            cout << "    {" << endl;
            cout << "      \"addr\": " << offset << "," << endl;

            // Extract raw bytes
            vector<uint8_t> raw;
            for (int k = 0; k < inst.size; ++k) raw.push_back(code[offset + k]);

            cout << "      \"bytes\": [";
            for (size_t k = 0; k < raw.size(); ++k) {
                cout << (int)raw[k];
                if (k < raw.size() - 1) cout << ", ";
            }
            cout << "]," << endl;

            cout << "      \"hex\": \"" << hexBytes(raw) << "\"," << endl;
            cout << "      \"asm\": \"" << jsonEscape(formatInstruction(inst)) << "\"," << endl;
            cout << "      \"size\": " << inst.size << endl;
            cout << "    }";

            offset += inst.size;
            firstInstr = false;
        } else {
            // Accumulate as data
            if (dataRunStart == -1) dataRunStart = offset;
            dataRunBytes.push_back(code[offset]);
            offset++;
        }
    }

    // Flush final data run
    if (dataRunStart != -1) {
        dataRegions.push_back({dataRunStart, dataRunBytes});
    }

    cout << endl << "  ]," << endl;

    // Emit Data Regions
    cout << "  \"dataRegions\": [" << endl;
    for (size_t i = 0; i < dataRegions.size(); ++i) {
        const auto& dr = dataRegions[i];
        cout << "    {" << endl;
        cout << "      \"addr\": " << dr.addr << "," << endl;
        cout << "      \"bytes\": [";
        for (size_t k = 0; k < dr.bytes.size(); ++k) {
            cout << (int)dr.bytes[k];
            if (k < dr.bytes.size() - 1) cout << ", ";
        }
        cout << "]," << endl;
        cout << "      \"hex\": \"" << hexBytes(dr.bytes) << "\"," << endl;
        cout << "      \"size\": " << dr.bytes.size() << "," << endl;
        cout << "      \"msg\": \"Decode failed or ambiguous\"" << endl;
        cout << "    }";
        if (i < dataRegions.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]" << endl;
    cout << "}" << endl;
}

// ============================================================
// 8086 EMULATOR
// ============================================================

// --- Section 1: Types ---

struct CPU {
    uint16_t regs[8] = {};   // AX=0, CX=1, DX=2, BX=3, SP=4, BP=5, SI=6, DI=7
    uint16_t sregs[4] = {};  // ES=0, CS=1, SS=2, DS=3
    uint16_t ip = 0;
    uint16_t flags = 0;

    // Flag bit positions
    static const int CF = 0, PF = 2, AF = 4, ZF = 6, SF = 7;
    static const int TF = 8, IF_ = 9, DF = 10, OF = 11;

    bool getFlag(int bit) const { return (flags >> bit) & 1; }
    void setFlag(int bit, bool v) {
        if (v) flags |= (1 << bit);
        else   flags &= ~(1 << bit);
    }

    uint8_t getReg8(int idx) const {
        // 0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH
        if (idx < 4) return (uint8_t)(regs[idx] & 0xFF);
        return (uint8_t)((regs[idx - 4] >> 8) & 0xFF);
    }
    void setReg8(int idx, uint8_t val) {
        if (idx < 4) regs[idx] = (regs[idx] & 0xFF00) | val;
        else         regs[idx - 4] = (regs[idx - 4] & 0x00FF) | ((uint16_t)val << 8);
    }
};

struct Memory {
    uint8_t data[1048576] = {};
    uint8_t vram[16000] = {};      // 2 pages of 80x50x2 bytes (char + attr interleaved)
    bool vramDirty = false;         // Track if VRAM was touched this cycle

    // === Legacy flat access (keep for backward compat) ===
    uint8_t read8(uint16_t addr) const { return data[addr]; }
    uint16_t read16(uint16_t addr) const {
        return data[addr] | ((uint16_t)data[(uint16_t)(addr + 1)] << 8);
    }
    void write8(uint16_t addr, uint8_t val) { data[addr] = val; }
    void write16(uint16_t addr, uint16_t val) {
        data[addr] = (uint8_t)(val & 0xFF);
        data[(uint16_t)(addr + 1)] = (uint8_t)(val >> 8);
    }

    // === New: Segment-aware access ===
    uint8_t sread8(uint16_t seg, uint16_t off) const {
        uint32_t linear = (uint32_t)seg * 16 + off;
        if (linear >= 0xB8000 && linear < 0xBBE80)
            return vram[linear - 0xB8000];
        return (linear < 1048576) ? data[linear] : 0;
    }

    uint16_t sread16(uint16_t seg, uint16_t off) const {
        uint32_t linear = (uint32_t)seg * 16 + off;
        if (linear >= 0xB8000 && linear < 0xBBE80) {
            uint32_t idx = linear - 0xB8000;
            uint8_t lo = vram[idx];
            uint8_t hi = (idx + 1 < 16000) ? vram[idx + 1] : 0;
            return lo | ((uint16_t)hi << 8);
        }
        if (linear + 1 < 1048576)
            return data[linear] | ((uint16_t)data[linear + 1] << 8);
        if (linear < 1048576)
            return data[linear];
        return 0;
    }

    void swrite8(uint16_t seg, uint16_t off, uint8_t val) {
        uint32_t linear = (uint32_t)seg * 16 + off;
        if (linear >= 0xB8000 && linear < 0xBBE80) {
            vram[linear - 0xB8000] = val;
            vramDirty = true;
            return;
        }
        if (linear < 1048576) data[linear] = val;
    }

    void swrite16(uint16_t seg, uint16_t off, uint16_t val) {
        uint32_t linear = (uint32_t)seg * 16 + off;
        if (linear >= 0xB8000 && linear < 0xBBE80) {
            uint32_t idx = linear - 0xB8000;
            vram[idx] = (uint8_t)(val & 0xFF);
            if (idx + 1 < 16000) vram[idx + 1] = (uint8_t)(val >> 8);
            vramDirty = true;
            return;
        }
        if (linear < 1048576) data[linear] = (uint8_t)(val & 0xFF);
        if (linear + 1 < 1048576) data[linear + 1] = (uint8_t)(val >> 8);
    }

    void loadCOM(const vector<uint8_t>& binary) {
        size_t len = binary.size();
        if (len > 65536 - 0x100) len = 65536 - 0x100;
        for (size_t i = 0; i < len; i++) data[0x100 + i] = binary[i];
    }
};

struct VRAMState {
    uint8_t cursorRow = 0;
    uint8_t cursorCol = 0;
    uint8_t defaultAttr = 0x07;  // Light grey on black
    int cols = 80;
    int rows = 50;
    // Note: actual VRAM storage is in Memory::vram[16000]

    uint16_t cursorOffset() const {
        return (uint16_t)(cursorRow * cols + cursorCol) * 2;
    }

    void advance(Memory& mem) {
        cursorCol++;
        if (cursorCol >= cols) {
            cursorCol = 0;
            cursorRow++;
            if (cursorRow >= rows) {
                scrollUp(mem, 1);
                cursorRow = rows - 1;
            }
        }
    }

    void scrollUp(Memory& mem, int lines) {
        int bytesPerRow = cols * 2;
        int shiftBytes = lines * bytesPerRow;
        int totalBytes = rows * bytesPerRow;
        // Shift VRAM up
        // Using manual move as memmove might not be available or risky with overlap in pure C++ without headers
        // But headers are included.
        // memmove(mem.vram, mem.vram + shiftBytes, totalBytes - shiftBytes);
        for(int i=0; i < totalBytes - shiftBytes; ++i) {
             mem.vram[i] = mem.vram[i + shiftBytes];
        }
        
        // Clear bottom lines
        for (int i = totalBytes - shiftBytes; i < totalBytes; i += 2) {
            mem.vram[i] = ' ';
            mem.vram[i + 1] = defaultAttr;
        }
        mem.vramDirty = true;
    }

    void writeCharAtCursor(Memory& mem, uint8_t ch, uint8_t attr) {
        uint16_t off = cursorOffset();
        if (off + 1 < 16000) {
            mem.vram[off] = ch;
            mem.vram[off + 1] = attr;
            mem.vramDirty = true;
        }
    }

    void clearScreen(Memory& mem) {
        for (int i = 0; i < rows * cols * 2; i += 2) {
            mem.vram[i] = ' ';
            mem.vram[i + 1] = defaultAttr;
        }
        cursorRow = 0;
        cursorCol = 0;
        mem.vramDirty = true;
    }
};

struct MouseState {
    bool installed = false;   // true when --mouse is used
    bool visible = false;     // INT 33h/01 show, /02 hide
    int16_t x = 0;           // pixel X (0-639 in text mode)
    int16_t y = 0;           // pixel Y (0-199 in text mode)
    uint16_t buttons = 0;    // bit 0=left, bit 1=right, bit 2=middle
    int16_t minX = 0, maxX = 639;
    int16_t minY = 0, maxY = 199;
    uint16_t pressCount[3] = {};   // per-button press counters
    uint16_t releaseCount[3] = {}; // per-button release counters
};

struct EventAction {
    string triggerType;    // "read", "poll", "tick", "int21", "int33"
    int triggerCount = 0;  // fire when counter reaches this value
    bool hasMouse = false;
    int16_t mouseX = 0, mouseY = 0;
    uint16_t mouseButtons = 0;
    bool hasKeys = false;
    string keys;
    bool snapshot = false;
    bool fired = false;
    int firedAtCycle = -1;
};

struct EventCounters {
    int readCount = 0;
    int pollCount = 0;
    int int21Count = 0;
    int int33Count = 0;
    int tickCount = 0;
};

struct EventState {
    vector<EventAction> events;
    EventCounters counters;
};

// --- DOS File I/O State ---

struct DOSFileEntry {
    FILE* fp = nullptr;
    string hostPath;       // resolved host path
    string dosPath;        // original DOS path
    uint8_t mode = 0;      // 0=read, 1=write, 2=r/w
    bool isDevice = false;  // true for handles 0-4
};

struct DOSSearchState {
    vector<pair<string,string>> entries; // (hostPath, dosName8_3) pre-enumerated
    size_t nextIndex = 0;
    uint8_t searchAttr = 0;
    string pattern;        // DOS wildcard pattern e.g. "*.*"
};

struct DOSFileState {
    string rootPath;                       // host dir from --dos-root (canonical)
    string currentDir;                     // current DOS directory, e.g. "" (root) or "SUBDIR"
    uint8_t currentDrive = 2;              // C: = 2
    uint16_t dtaSegment = 0, dtaOffset = 0x0080; // default DTA in PSP
    map<int, DOSFileEntry> handles;        // file handle table
    int nextHandle = 5;                    // next available handle (0-4 are devices)
    vector<DOSSearchState> searches;       // active FindFirst/FindNext contexts
    bool enabled = false;                  // true when --dos-root is set
    // Tracking for JSON output
    int filesOpened = 0;
    int filesClosed = 0;
    int bytesRead = 0;
    int bytesWritten = 0;
    int dirSearches = 0;
    int filesDeleted = 0;
    int filesRenamed = 0;
    int errors = 0;
};

struct DOSMemoryState {
    // Simple paragraph-based allocator for conventional memory above the COM segment.
    // COM program occupies segment 0x0000 (64KB). Allocatable memory starts at 0x1000.
    struct Block {
        uint16_t segment;    // start paragraph
        uint16_t size;       // size in paragraphs
    };
    vector<Block> allocated;
    uint16_t nextFreeSeg = 0x1000;   // first allocatable paragraph (after COM's 64KB)
    uint16_t maxSeg = 0xA000;        // 640KB conventional memory ceiling

    // Allocate `paragraphs` paragraphs. Returns segment on success, 0 on failure.
    uint16_t allocate(uint16_t paragraphs) {
        if (paragraphs == 0) paragraphs = 1;
        if ((uint32_t)nextFreeSeg + paragraphs > maxSeg) return 0;
        uint16_t seg = nextFreeSeg;
        nextFreeSeg += paragraphs;
        allocated.push_back({seg, paragraphs});
        return seg;
    }

    // Free a previously allocated block. Returns true on success.
    bool free(uint16_t segment) {
        for (size_t i = 0; i < allocated.size(); i++) {
            if (allocated[i].segment == segment) {
                // If this is the topmost block, reclaim space
                if (allocated[i].segment + allocated[i].size == nextFreeSeg) {
                    nextFreeSeg = allocated[i].segment;
                }
                allocated.erase(allocated.begin() + i);
                return true;
            }
        }
        return false;
    }

    // Resize a previously allocated block. Returns true on success.
    bool resize(uint16_t segment, uint16_t newSize) {
        if (newSize == 0) newSize = 1;
        for (size_t i = 0; i < allocated.size(); i++) {
            if (allocated[i].segment == segment) {
                // Only the topmost block can be resized
                if (allocated[i].segment + allocated[i].size == nextFreeSeg) {
                    uint32_t newEnd = (uint32_t)allocated[i].segment + newSize;
                    if (newEnd > maxSeg) return false;
                    nextFreeSeg = (uint16_t)newEnd;
                    allocated[i].size = newSize;
                    return true;
                }
                // Non-topmost: can shrink but not grow
                if (newSize <= allocated[i].size) {
                    allocated[i].size = newSize;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    // Returns largest free block in paragraphs
    uint16_t largestFree() const {
        if (nextFreeSeg >= maxSeg) return 0;
        return maxSeg - nextFreeSeg;
    }
};

// Convert DOS path to sandboxed host path. Returns empty string and sets error on failure.
static string resolveDosPath(const DOSFileState& dosFs, const string& dosPath, bool& pathError) {
    namespace fs = std::filesystem;
    pathError = false;
    string p = dosPath;
    // Replace backslashes with forward slashes
    for (char& c : p) { if (c == '\\') c = '/'; }
    // Strip drive letter if present (e.g. "C:")
    if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':') {
        p = p.substr(2);
    }
    // Strip leading slash for joining
    while (!p.empty() && p[0] == '/') p = p.substr(1);
    // If relative, prepend current directory
    if (!p.empty() && p[0] != '/') {
        if (!dosFs.currentDir.empty()) {
            p = dosFs.currentDir + "/" + p;
        }
    }
    // Join with root path
    fs::path hostPath = fs::path(dosFs.rootPath) / p;
    // Resolve to canonical form (weakly_canonical handles non-existent trailing components)
    fs::path resolved = fs::weakly_canonical(hostPath);
    string resolvedStr = resolved.string();
    // Normalize separators for comparison
    string rootNorm = dosFs.rootPath;
    for (char& c : resolvedStr) { if (c == '\\') c = '/'; }
    for (char& c : rootNorm) { if (c == '\\') c = '/'; }
    // Sandbox check: resolved path must start with root path
    if (resolvedStr.size() < rootNorm.size() ||
        resolvedStr.substr(0, rootNorm.size()) != rootNorm) {
        pathError = true;
        return "";
    }
    return resolved.string();
}

// Convert host filename to 8.3 DOS name (uppercase)
static string toShortName(const string& hostName) {
    // Find the last dot for extension split
    string name = hostName;
    // Uppercase
    for (char& c : name) c = (char)toupper((unsigned char)c);
    size_t dot = name.rfind('.');
    string base, ext;
    if (dot != string::npos && dot > 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot + 1);
    } else {
        base = name;
    }
    if (base.size() > 8) base = base.substr(0, 8);
    if (ext.size() > 3) ext = ext.substr(0, 3);
    if (ext.empty()) return base;
    return base + "." + ext;
}

// Match a DOS filename against a wildcard pattern (e.g. "*.TXT", "FILE?.DAT")
static bool matchesDosWildcard(const string& name, const string& pattern) {
    // Both should be uppercase already
    size_t ni = 0, pi = 0;
    while (ni < name.size() && pi < pattern.size()) {
        if (pattern[pi] == '*') {
            // Star matches everything to next segment
            pi++;
            if (pi >= pattern.size()) return true; // trailing * matches all
            // Find next non-wildcard char to anchor
            while (ni < name.size()) {
                if (matchesDosWildcard(name.substr(ni), pattern.substr(pi)))
                    return true;
                ni++;
            }
            return matchesDosWildcard(name.substr(ni), pattern.substr(pi));
        } else if (pattern[pi] == '?' || pattern[pi] == name[ni]) {
            ni++;
            pi++;
        } else {
            return false;
        }
    }
    // Consume trailing wildcards
    while (pi < pattern.size() && (pattern[pi] == '*' || pattern[pi] == '?')) pi++;
    // Handle trailing .<wildcards> matching names with no extension (e.g. *.* matches SUBDIR)
    if (ni == name.size() && pi < pattern.size() && pattern[pi] == '.') {
        size_t check = pi + 1;
        while (check < pattern.size() && (pattern[check] == '*' || pattern[check] == '?')) check++;
        if (check == pattern.size()) pi = check;
    }
    return ni == name.size() && pi == pattern.size();
}

// Pack host file time into DOS date/time words
static void toDosDateTime(const filesystem::file_time_type& ft, uint16_t& dosDate, uint16_t& dosTime) {
    // Convert to system_clock time_point
    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
        ft - filesystem::file_time_type::clock::now() + chrono::system_clock::now());
    time_t tt = chrono::system_clock::to_time_t(sctp);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &tt);
#else
    localtime_r(&tt, &tmBuf);
#endif
    // DOS date: bits 15-9=year-1980, 8-5=month, 4-0=day
    int year = tmBuf.tm_year + 1900;
    if (year < 1980) year = 1980;
    dosDate = (uint16_t)(((year - 1980) << 9) | ((tmBuf.tm_mon + 1) << 5) | tmBuf.tm_mday);
    // DOS time: bits 15-11=hour, 10-5=minute, 4-0=seconds/2
    dosTime = (uint16_t)((tmBuf.tm_hour << 11) | (tmBuf.tm_min << 5) | (tmBuf.tm_sec / 2));
}

// Read a null-terminated string from emulator memory at seg:off
static string readDosString(const Memory& mem, uint16_t seg, uint16_t off) {
    string s;
    for (int i = 0; i < 256; i++) {
        uint8_t ch = mem.sread8(seg, (uint16_t)(off + i));
        if (ch == 0) break;
        s += (char)ch;
    }
    return s;
}

// Sentinel bytes in stdinSource: modifier flags for the next character.
// Used by \S \C \A escapes in --input and consumed transparently by IOCapture::readChar().
// Multiple prefixes can be stacked (e.g. \C\S before a key = Ctrl+Shift).
static const uint8_t SHIFT_PREFIX = 0xFE; // bit 1: Left Shift
static const uint8_t CTRL_PREFIX  = 0xFD; // bit 2: Ctrl
static const uint8_t ALT_PREFIX   = 0xFC; // bit 3: Alt

// Decode C-style escape sequences in --input strings so that binary bytes
// (including 0x00 for extended-key prefixes) can be specified on the command line.
// Supported: \xHH  \0  \n  \r  \t  \S (shift prefix)  and backslash-backslash
// \S marks the next character as having Shift held (inserts 0xFE sentinel).
// For extended keys (two-byte sequences), use \S before each byte: \S\x00\S\x4B
static string unescapeInput(const char* raw) {
    string out;
    for (const char* p = raw; *p; ) {
        if (*p == '\\' && *(p+1)) {
            char next = *(p+1);
            if (next == 'S') {
                out += (char)SHIFT_PREFIX;
                p += 2;
            } else if (next == 'C') {
                out += (char)CTRL_PREFIX;
                p += 2;
            } else if (next == 'A') {
                out += (char)ALT_PREFIX;
                p += 2;
            } else if (next == 'x' && isxdigit((unsigned char)*(p+2)) && isxdigit((unsigned char)*(p+3))) {
                // \xHH
                char hex[3] = { *(p+2), *(p+3), 0 };
                out += (char)strtoul(hex, nullptr, 16);
                p += 4;
            } else if (next == '0') {
                out += '\0';
                p += 2;
            } else if (next == 'n') {
                out += '\n';
                p += 2;
            } else if (next == 'r') {
                out += '\r';
                p += 2;
            } else if (next == 't') {
                out += '\t';
                p += 2;
            } else if (next == '\\') {
                out += '\\';
                p += 2;
            } else {
                out += *p++;  // unrecognized escape, keep literal
            }
        } else {
            out += *p++;
        }
    }
    return out;
}

// --- Events JSON parser ---
static void skipJsonWs(const char*& p) { while (*p && isspace((unsigned char)*p)) p++; }

static string readJsonString(const char*& p, string& error) {
    skipJsonWs(p);
    if (*p != '"') { error = "Expected '\"'"; return ""; }
    p++;
    string out;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'x':
                    if (isxdigit((unsigned char)*(p+1)) && isxdigit((unsigned char)*(p+2))) {
                        char hex[3] = { *(p+1), *(p+2), 0 };
                        out += (char)strtoul(hex, nullptr, 16);
                        p += 2;
                    } else { out += 'x'; }
                    break;
                default: out += *p; break;
            }
        } else {
            out += *p;
        }
        p++;
    }
    if (*p == '"') p++;
    else if (error.empty()) error = "Unterminated string";
    return out;
}

static int readJsonInt(const char*& p, string& error) {
    skipJsonWs(p);
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    if (!isdigit((unsigned char)*p)) { error = "Expected integer"; return 0; }
    int val = 0;
    while (isdigit((unsigned char)*p)) { val = val * 10 + (*p - '0'); p++; }
    return neg ? -val : val;
}

static bool readJsonBool(const char*& p, string& error) {
    skipJsonWs(p);
    if (strncmp(p, "true", 4) == 0) { p += 4; return true; }
    if (strncmp(p, "false", 5) == 0) { p += 5; return false; }
    error = "Expected true or false";
    return false;
}

static vector<EventAction> parseEvents(const string& json, string& error) {
    vector<EventAction> events;
    const char* p = json.c_str();
    skipJsonWs(p);
    if (*p != '[') { error = "Expected '[' at start of events array"; return events; }
    p++;
    skipJsonWs(p);
    if (*p == ']') { p++; return events; }
    while (true) {
        skipJsonWs(p);
        if (*p != '{') { error = "Expected '{' at start of event object"; return events; }
        p++;
        EventAction ev;
        while (true) {
            skipJsonWs(p);
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            string key = readJsonString(p, error);
            if (!error.empty()) return events;
            skipJsonWs(p);
            if (*p != ':') { error = "Expected ':' after key \"" + key + "\""; return events; }
            p++;
            skipJsonWs(p);
            if (key == "on") {
                string val = readJsonString(p, error);
                if (!error.empty()) return events;
                size_t colon = val.find(':');
                if (colon == string::npos) { error = "Invalid trigger: \"" + val + "\" (expected type:count)"; return events; }
                ev.triggerType = val.substr(0, colon);
                ev.triggerCount = atoi(val.substr(colon + 1).c_str());
                if (ev.triggerType != "read" && ev.triggerType != "poll" && ev.triggerType != "tick" &&
                    ev.triggerType != "int21" && ev.triggerType != "int33") {
                    error = "Unknown trigger type: \"" + ev.triggerType + "\"";
                    return events;
                }
                if (ev.triggerCount <= 0) { error = "Trigger count must be positive: " + val; return events; }
            } else if (key == "mouse") {
                if (*p != '[') { error = "Expected '[' for mouse value"; return events; }
                p++;
                ev.mouseX = (int16_t)readJsonInt(p, error); if (!error.empty()) return events;
                skipJsonWs(p); if (*p == ',') p++;
                ev.mouseY = (int16_t)readJsonInt(p, error); if (!error.empty()) return events;
                skipJsonWs(p); if (*p == ',') p++;
                ev.mouseButtons = (uint16_t)readJsonInt(p, error); if (!error.empty()) return events;
                skipJsonWs(p);
                if (*p != ']') { error = "Expected ']' after mouse array"; return events; }
                p++;
                ev.hasMouse = true;
            } else if (key == "keys") {
                string raw = readJsonString(p, error);
                if (!error.empty()) return events;
                ev.keys = unescapeInput(raw.c_str());
                ev.hasKeys = true;
            } else if (key == "snapshot") {
                ev.snapshot = readJsonBool(p, error);
                if (!error.empty()) return events;
            } else {
                error = "Unknown event key: \"" + key + "\"";
                return events;
            }
        }
        events.push_back(ev);
        skipJsonWs(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; break; }
        error = "Expected ',' or ']' after event object";
        return events;
    }
    return events;
}

struct IOCapture {
    string stdoutBuf;
    string stdinSource;
    size_t stdinPos = 0;
    uint8_t shiftFlags = 0; // INT 16h/02h shift-flags byte (bit0=RShift, bit1=LShift, bit2=Ctrl, bit3=Alt)
    int exitCode = 0;
    int readChar() {
        if (stdinPos >= stdinSource.size()) return -1;
        // Accumulate modifier prefixes (stackable: \C\S = Ctrl+Shift)
        shiftFlags = 0x00;
        while (stdinPos < stdinSource.size()) {
            uint8_t b = (unsigned char)stdinSource[stdinPos];
            if      (b == SHIFT_PREFIX) { shiftFlags |= 0x02; stdinPos++; }
            else if (b == CTRL_PREFIX)  { shiftFlags |= 0x04; stdinPos++; }
            else if (b == ALT_PREFIX)   { shiftFlags |= 0x08; stdinPos++; }
            else break;
        }
        if (stdinPos >= stdinSource.size()) return -1;
        return (unsigned char)stdinSource[stdinPos++];
    }
};

struct EmulatorConfig {
    int maxCycles = 1000000;
    set<uint16_t> breakpoints;
    set<int> watchRegs;  // register indices 0-7
    uint16_t memDumpAddr = 0;
    int memDumpLen = 0;
    string stdinInput;
    // --- Output ---
    string outputFile;              // --output-file path (empty = stdout)
    // --- VRAM viewport ---
    bool hasViewport = false;       // Only emit screen data if true
    int vpCol = 0, vpRow = 0;      // Top-left corner of viewport
    int vpWidth = 80, vpHeight = 50; // Viewport dimensions
    bool vpAttrs = false;           // Include attribute data in output
    string screenshotFile;          // --screenshot <path.png|jpg|bmp>
    bool screenshotFont8x8 = false; // --font 8x8 (default: 8x16 VGA)
    // --- Mouse ---
    int mouseX = 0, mouseY = 0;
    uint16_t mouseButtons = 0;      // bit 0=left, 1=right, 2=middle
    bool mouseEnabled = false;
    // --- Events ---
    vector<EventAction> events;
    // --- DOS File I/O ---
    string dosRoot;          // --dos-root host directory path
    // --- VRAM fill ---
    bool vramFill = false;
    string vramFillText;     // empty = random fill
    // --- Command-line args ---
    string commandArgs;      // --args value → PSP command tail
};

struct Snapshot {
    uint16_t addr;
    int cycle;
    uint16_t regs[8];
    uint16_t sregs[4];
    uint16_t ip;
    uint16_t flags;
    string nextInst;
    vector<uint16_t> stack;
    vector<uint8_t> memDump;
    int hitCount = 1;
    string reason;
    // --- VRAM viewport at this snapshot ---
    vector<string> screenLines;
    vector<string> screenAttrs;
    // --- Cursor at snapshot ---
    int snapCursorRow = 0;
    int snapCursorCol = 0;
};

struct SkippedRecord {
    uint16_t addr;
    string instruction;
    string reason;
    int count = 1;
};

struct EmulatorResult {
    bool success = false;
    bool halted = false;
    string haltReason;
    int exitCode = 0;
    int cyclesExecuted = 0;
    double fidelity = 1.0;
    string output;
    vector<Snapshot> snapshots;
    vector<SkippedRecord> skipped;
    vector<string> diagnostics;
    vector<string> screen;       // Viewport text rows (populated only with --screen/--viewport)
    vector<string> screenAttrs;  // Viewport hex attribute rows (populated only with --attrs)
    // --- Cursor ---
    int cursorRow = 0;
    int cursorCol = 0;
    string screenshotPath;  // populated on successful BMP write
    // Mouse (only populated when --mouse is used)
    bool mouseEnabled = false;
    int mouseX = 0, mouseY = 0;
    uint16_t mouseButtons = 0;
    bool mouseVisible = false;
    // --- Events ---
    struct EventLogEntry {
        string on;
        int cycle;
        string action;
    };
    int eventsTotal = 0;
    int eventsFired = 0;
    vector<EventLogEntry> eventLog;
    // --- File I/O ---
    struct FileIOStats {
        int filesOpened = 0;
        int filesClosed = 0;
        int bytesRead = 0;
        int bytesWritten = 0;
        int dirSearches = 0;
        int filesDeleted = 0;
        int filesRenamed = 0;
        int errors = 0;
    };
    FileIOStats fileIO;
};

// --- Section 2: Core Engine ---

// Returns the segment register value to use for a memory operand.
// Respects: (1) explicit segment override prefix, (2) BP->SS default, (3) DS default.
uint16_t resolveSegment(const CPU& cpu, const DecodedOperand& op, int segOverride) {
    // Explicit override takes priority (except ES:DI for string destinations)
    if (segOverride != -1) {
        switch (segOverride) {
            case 0x26: return cpu.sregs[0]; // ES
            case 0x2E: return cpu.sregs[1]; // CS
            case 0x36: return cpu.sregs[2]; // SS
            case 0x3E: return cpu.sregs[3]; // DS
        }
    }
    // BP-based addressing defaults to SS
    if (op.memRM == 2 || op.memRM == 3 || op.memRM == 6) {
        return cpu.sregs[2]; // SS
    }
    // Everything else defaults to DS
    return cpu.sregs[3]; // DS
}

uint16_t calcEffectiveAddress(const CPU& cpu, const DecodedOperand& op) {
    int addr = 0;
    if (op.memRM == -1) {
        addr = op.disp;  // direct address
    } else {
        switch (op.memRM) {
            case 0: addr = cpu.regs[3] + cpu.regs[6]; break; // BX+SI
            case 1: addr = cpu.regs[3] + cpu.regs[7]; break; // BX+DI
            case 2: addr = cpu.regs[5] + cpu.regs[6]; break; // BP+SI
            case 3: addr = cpu.regs[5] + cpu.regs[7]; break; // BP+DI
            case 4: addr = cpu.regs[6]; break;                // SI
            case 5: addr = cpu.regs[7]; break;                // DI
            case 6: addr = cpu.regs[5]; break;                // BP
            case 7: addr = cpu.regs[3]; break;                // BX
        }
        addr += op.disp;
    }
    return (uint16_t)(addr & 0xFFFF);
}

uint16_t readOperand(const CPU& cpu, const Memory& mem, const DecodedOperand& op, int segOverride = -1) {
    switch (op.kind) {
        case OpKind::REG8:  return cpu.getReg8(op.reg);
        case OpKind::REG16: return cpu.regs[op.reg];
        case OpKind::SREG:  return cpu.sregs[op.reg];
        case OpKind::IMM8:  return (uint16_t)(op.disp & 0xFF);
        case OpKind::IMM16: return (uint16_t)(op.disp & 0xFFFF);
        case OpKind::MEM: {
            uint16_t addr = calcEffectiveAddress(cpu, op);
            uint16_t seg = resolveSegment(cpu, op, segOverride);
            return (op.size == 8) ? mem.sread8(seg, addr) : mem.sread16(seg, addr);
        }
        default: return 0;
    }
}

bool writeOperand(CPU& cpu, Memory& mem, const DecodedOperand& op, uint16_t val, bool& memDirty, int segOverride = -1) {
    switch (op.kind) {
        case OpKind::REG8:  cpu.setReg8(op.reg, (uint8_t)val); return true;
        case OpKind::REG16: cpu.regs[op.reg] = val; return true;
        case OpKind::SREG:  cpu.sregs[op.reg] = val; return true;
        case OpKind::MEM: {
            uint16_t addr = calcEffectiveAddress(cpu, op);
            uint16_t seg = resolveSegment(cpu, op, segOverride);
            if (op.size == 8) mem.swrite8(seg, addr, (uint8_t)val);
            else mem.swrite16(seg, addr, val);
            memDirty = true;
            return true;
        }
        default: return false;
    }
}

bool parity8(uint8_t val) {
    int bits = 0;
    for (int i = 0; i < 8; i++) bits += (val >> i) & 1;
    return (bits % 2) == 0;
}

void updateFlagsAdd(CPU& cpu, uint32_t result, uint16_t dst, uint16_t src, bool wide) {
    uint32_t mask = wide ? 0xFFFF : 0xFF;
    uint32_t signBit = wide ? 0x8000 : 0x80;
    uint16_t res = (uint16_t)(result & mask);
    cpu.setFlag(CPU::CF, result > mask);
    cpu.setFlag(CPU::ZF, res == 0);
    cpu.setFlag(CPU::SF, (res & signBit) != 0);
    cpu.setFlag(CPU::OF, ((dst ^ res) & (src ^ res) & signBit) != 0);
    cpu.setFlag(CPU::PF, parity8((uint8_t)(res & 0xFF)));
    cpu.setFlag(CPU::AF, ((dst ^ src ^ res) & 0x10) != 0);
}

void updateFlagsSub(CPU& cpu, uint32_t result, uint16_t dst, uint16_t src, bool wide) {
    uint32_t mask = wide ? 0xFFFF : 0xFF;
    uint32_t signBit = wide ? 0x8000 : 0x80;
    uint16_t res = (uint16_t)(result & mask);
    cpu.setFlag(CPU::CF, dst < src);
    cpu.setFlag(CPU::ZF, res == 0);
    cpu.setFlag(CPU::SF, (res & signBit) != 0);
    cpu.setFlag(CPU::OF, ((dst ^ src) & (dst ^ res) & signBit) != 0);
    cpu.setFlag(CPU::PF, parity8((uint8_t)(res & 0xFF)));
    cpu.setFlag(CPU::AF, ((dst ^ src ^ res) & 0x10) != 0);
}

void updateFlagsLogic(CPU& cpu, uint16_t result, bool wide) {
    uint32_t signBit = wide ? 0x8000 : 0x80;
    cpu.setFlag(CPU::CF, false);
    cpu.setFlag(CPU::OF, false);
    cpu.setFlag(CPU::ZF, result == 0);
    cpu.setFlag(CPU::SF, (result & signBit) != 0);
    cpu.setFlag(CPU::PF, parity8((uint8_t)(result & 0xFF)));
    cpu.setFlag(CPU::AF, false);
}

// --- Section 3: Condition Evaluation ---

bool evalCondition(const CPU& cpu, const string& mnemonic) {
    if (mnemonic == "JO")  return cpu.getFlag(CPU::OF);
    if (mnemonic == "JNO") return !cpu.getFlag(CPU::OF);
    if (mnemonic == "JB")  return cpu.getFlag(CPU::CF);
    if (mnemonic == "JNB") return !cpu.getFlag(CPU::CF);
    if (mnemonic == "JZ")  return cpu.getFlag(CPU::ZF);
    if (mnemonic == "JNZ") return !cpu.getFlag(CPU::ZF);
    if (mnemonic == "JBE") return cpu.getFlag(CPU::CF) || cpu.getFlag(CPU::ZF);
    if (mnemonic == "JA")  return !cpu.getFlag(CPU::CF) && !cpu.getFlag(CPU::ZF);
    if (mnemonic == "JS")  return cpu.getFlag(CPU::SF);
    if (mnemonic == "JNS") return !cpu.getFlag(CPU::SF);
    if (mnemonic == "JP")  return cpu.getFlag(CPU::PF);
    if (mnemonic == "JNP") return !cpu.getFlag(CPU::PF);
    if (mnemonic == "JL")  return cpu.getFlag(CPU::SF) != cpu.getFlag(CPU::OF);
    if (mnemonic == "JGE") return cpu.getFlag(CPU::SF) == cpu.getFlag(CPU::OF);
    if (mnemonic == "JLE") return cpu.getFlag(CPU::ZF) || (cpu.getFlag(CPU::SF) != cpu.getFlag(CPU::OF));
    if (mnemonic == "JG")  return !cpu.getFlag(CPU::ZF) && (cpu.getFlag(CPU::SF) == cpu.getFlag(CPU::OF));
    return false;
}

// --- Section 5: Interrupt Handling ---

void ttyCharToVRAM(Memory& mem, VRAMState& vram, uint8_t ch) {
    if (ch == 0x0D) { // CR
        vram.cursorCol = 0;
    } else if (ch == 0x0A) { // LF
        vram.cursorRow++;
        if (vram.cursorRow >= vram.rows) {
            vram.scrollUp(mem, 1);
            vram.cursorRow = vram.rows - 1;
        }
    } else if (ch == 0x08) { // Backspace
        if (vram.cursorCol > 0) vram.cursorCol--;
    } else if (ch == 0x07) { // Bell - ignore
    } else {
        vram.writeCharAtCursor(mem, ch, vram.defaultAttr);
        vram.advance(mem);
    }
}

void handleInt10(CPU& cpu, Memory& mem, VRAMState& vram, EmulatorResult& result) {
    uint8_t ah = cpu.getReg8(4);
    switch (ah) {
        case 0x00: { // Set video mode
            // AL = mode. We only support text modes really, but let's just clear screen.
            vram.clearScreen(mem);
            break;
        }
        case 0x02: { // Set cursor position
            uint8_t row = cpu.getReg8(6); // DH
            uint8_t col = cpu.getReg8(2); // DL
            // BH = page number (ignored)
            if (row < vram.rows && col < vram.cols) {
                vram.cursorRow = row;
                vram.cursorCol = col;
            }
            break;
        }
        case 0x03: { // Get cursor position
            // BH = page number (ignored)
            cpu.setReg8(6, vram.cursorRow); // DH
            cpu.setReg8(2, vram.cursorCol); // DL
            cpu.regs[1] = 0x0607; // CX = cursor size (standard)
            break;
        }
        case 0x06:   // Scroll up
        case 0x07: { // Scroll down
            uint8_t lines = cpu.getReg8(0); // AL (0 = clear window)
            uint8_t attr  = cpu.getReg8(7); // BH = fill attribute (FIXED: was index 5)
            uint8_t r1 = cpu.getReg8(5);    // CH = top row
            uint8_t c1 = cpu.getReg8(1);    // CL = left col
            uint8_t r2 = cpu.getReg8(6);    // DH = bottom row
            uint8_t c2 = cpu.getReg8(2);    // DL = right col

            // Clamp to screen bounds
            if (r2 >= vram.rows) r2 = vram.rows - 1;
            if (c2 >= vram.cols) c2 = vram.cols - 1;
            if (r1 > r2 || c1 > c2) break; // Invalid window

            if (lines == 0) {
                // Clear the entire window
                for (int r = r1; r <= r2; r++) {
                    for (int c = c1; c <= c2; c++) {
                        int off = (r * vram.cols + c) * 2;
                        mem.vram[off] = ' ';
                        mem.vram[off + 1] = attr;
                    }
                }
            } else if (ah == 0x06) {
                // Scroll UP by 'lines' rows within the window
                for (int r = r1; r <= r2 - lines; r++) {
                    for (int c = c1; c <= c2; c++) {
                        int dst = (r * vram.cols + c) * 2;
                        int src = ((r + lines) * vram.cols + c) * 2;
                        mem.vram[dst] = mem.vram[src];
                        mem.vram[dst + 1] = mem.vram[src + 1];
                    }
                }
                // Clear vacated bottom rows
                for (int r = max((int)r2 - lines + 1, (int)r1); r <= r2; r++) {
                    for (int c = c1; c <= c2; c++) {
                        int off = (r * vram.cols + c) * 2;
                        mem.vram[off] = ' ';
                        mem.vram[off + 1] = attr;
                    }
                }
            } else {
                // Scroll DOWN by 'lines' rows within the window
                for (int r = r2; r >= r1 + lines; r--) {
                    for (int c = c1; c <= c2; c++) {
                        int dst = (r * vram.cols + c) * 2;
                        int src = ((r - lines) * vram.cols + c) * 2;
                        mem.vram[dst] = mem.vram[src];
                        mem.vram[dst + 1] = mem.vram[src + 1];
                    }
                }
                // Clear vacated top rows
                for (int r = r1; r < min((int)r1 + lines, (int)r2 + 1); r++) {
                    for (int c = c1; c <= c2; c++) {
                        int off = (r * vram.cols + c) * 2;
                        mem.vram[off] = ' ';
                        mem.vram[off + 1] = attr;
                    }
                }
            }
            mem.vramDirty = true;
            break;
        }
        case 0x08: { // Read char/attr at cursor
            // BH = page (ignored)
            uint16_t off = vram.cursorOffset();
            if (off + 1 < 16000) {
                cpu.setReg8(0, mem.vram[off]);     // AL = char
                cpu.setReg8(4, mem.vram[off+1]);   // AH = attr
            }
            break;
        }
        case 0x09: { // Write char+attr at cursor, CX times, no cursor advance
            uint8_t ch = cpu.getReg8(0);    // AL
            uint8_t attr = cpu.getReg8(3);  // BL
            uint16_t count = cpu.regs[1];   // CX = repeat count
            // Writes character to consecutive cells starting at cursor position.
            // Cursor position itself is NOT updated.
            uint16_t off = vram.cursorOffset();
            for (uint16_t i = 0; i < count; i++) {
                uint16_t currentOff = off + i * 2;
                if (currentOff + 1 < 8000) {
                    mem.vram[currentOff] = ch;
                    mem.vram[currentOff + 1] = attr;
                }
            }
            mem.vramDirty = true;
            break;
        }
        case 0x0A: { // Write char at cursor, keep existing attribute, CX times
            uint8_t ch  = cpu.getReg8(0);   // AL
            uint16_t cx = cpu.regs[1];      // CX = repeat count
            // Does NOT advance cursor, does NOT change attribute
            int col = vram.cursorCol;
            int row = vram.cursorRow;
            for (uint16_t i = 0; i < cx && row < vram.rows; i++) {
                int off = (row * vram.cols + col) * 2;
                if (off + 1 < 16000) {
                    mem.vram[off] = ch;
                    // attribute byte at off+1 left unchanged
                }
                col++;
                if (col >= vram.cols) { col = 0; row++; }
            }
            mem.vramDirty = true;
            break;
        }
        case 0x0E: { // Teletype output
            uint8_t ch = cpu.getReg8(0);
            ttyCharToVRAM(mem, vram, ch);
            break;
        }
        case 0x0F: { // Get video mode
            cpu.setReg8(0, 3);    // AL = mode 3 (80x25 color text)
            cpu.setReg8(4, 80);   // AH = number of columns
            cpu.setReg8(7, 0);    // BH = active page
            break;
        }
        default:
            result.skipped.push_back({ cpu.ip, "INT 10h AH=" + hexByte(ah), "Unimplemented Video function", 1 });
            break;
    }
}

// Forward declaration (defined in Section 6)
void captureSnapshot(const CPU& cpu, const Memory& mem,
                     const vector<uint8_t>& code,
                     int cycle, const string& reason,
                     const EmulatorConfig& config,
                     const VRAMState& vram,
                     vector<Snapshot>& snapshots);

static void processEvents(EventState& evState, int cycle, MouseState& mouse, IOCapture& io,
                          EmulatorResult& result, const CPU& cpu, const Memory& mem,
                          const vector<uint8_t>& code, const EmulatorConfig& config,
                          const VRAMState& vram) {
    for (auto& ev : evState.events) {
        if (ev.fired) continue;
        int current = 0;
        if (ev.triggerType == "read") current = evState.counters.readCount;
        else if (ev.triggerType == "poll") current = evState.counters.pollCount;
        else if (ev.triggerType == "tick") current = evState.counters.tickCount;
        else if (ev.triggerType == "int21") current = evState.counters.int21Count;
        else if (ev.triggerType == "int33") current = evState.counters.int33Count;
        if (current >= ev.triggerCount) {
            ev.fired = true;
            ev.firedAtCycle = cycle;
            if (ev.hasMouse) {
                mouse.x = ev.mouseX; mouse.y = ev.mouseY; mouse.buttons = ev.mouseButtons;
            }
            if (ev.hasKeys) {
                io.stdinSource.insert(io.stdinSource.begin() + io.stdinPos,
                                      ev.keys.begin(), ev.keys.end());
            }
            if (ev.snapshot) {
                captureSnapshot(cpu, mem, code, cycle,
                               "Event: " + ev.triggerType + ":" + to_string(ev.triggerCount),
                               config, vram, result.snapshots);
            }
        }
    }
}

void handleInt21(CPU& cpu, Memory& mem, IOCapture& io, EmulatorResult& result, VRAMState& vram,
                 EventState& evState, MouseState& mouse, const vector<uint8_t>& code,
                 const EmulatorConfig& config, int cycle, DOSFileState& dosFs,
                 DOSMemoryState& dosMem) {
    static const int MAX_OUTPUT = 4096;
    evState.counters.int21Count++;
    uint8_t ah = cpu.getReg8(4); // AH
    switch (ah) {
        case 0x01: { // Read char with echo
            evState.counters.readCount++;
            processEvents(evState, cycle, mouse, io, result, cpu, mem, code, config, vram);
            int ch = io.readChar();
            if (ch < 0) ch = 0x0D;
            cpu.setReg8(0, (uint8_t)ch); // AL
            if ((int)io.stdoutBuf.size() < MAX_OUTPUT)
                io.stdoutBuf += (char)ch;
            ttyCharToVRAM(mem, vram, (uint8_t)ch);
            break;
        }
        case 0x02: { // Write DL to stdout
            uint8_t dl = cpu.getReg8(2);
            if ((int)io.stdoutBuf.size() < MAX_OUTPUT) {
                io.stdoutBuf += (char)dl;
            }
            ttyCharToVRAM(mem, vram, dl);
            break;
        }
        case 0x06: { // Direct console I/O
            uint8_t dl = cpu.getReg8(2);
            if (dl == 0xFF) {
                evState.counters.readCount++;
                processEvents(evState, cycle, mouse, io, result, cpu, mem, code, config, vram);
                int ch = io.readChar();
                if (ch < 0) { cpu.setFlag(CPU::ZF, true); cpu.setReg8(0, 0); }
                else { cpu.setFlag(CPU::ZF, false); cpu.setReg8(0, (uint8_t)ch); }
            } else {
                if ((int)io.stdoutBuf.size() < MAX_OUTPUT)
                    io.stdoutBuf += (char)dl;
                ttyCharToVRAM(mem, vram, dl);
            }
            break;
        }
        case 0x09: { // Write $-terminated string from DS:DX
            uint16_t seg = cpu.sregs[3];  // DS
            uint16_t off = cpu.regs[2];   // DX
            bool truncated = false;
            for (int i = 0; i < 65536; i++) {
                uint8_t ch = mem.sread8(seg, (uint16_t)(off + i));
                if (ch == '$') break;
                if ((int)io.stdoutBuf.size() < MAX_OUTPUT) {
                    io.stdoutBuf += (char)ch;
                } else if (!truncated) {
                    truncated = true;
                    result.diagnostics.push_back(
                        "Output truncated at " + to_string(MAX_OUTPUT) +
                        " bytes (no '$' terminator found - possible bad pointer in DX=" +
                        hexImm16(off) + ")");
                }
                ttyCharToVRAM(mem, vram, ch);
            }
            break;
        }
        case 0x0E: { // Select disk - set current drive
            uint8_t dl = cpu.getReg8(2); // DL = drive number
            dosFs.currentDrive = dl;
            cpu.setReg8(0, 26); // AL = total drives (report 26: A-Z)
            break;
        }
        case 0x19: { // Get current disk
            cpu.setReg8(0, dosFs.currentDrive); // AL = current drive (0=A, 2=C)
            break;
        }
        case 0x1A: { // Set DTA address
            dosFs.dtaSegment = cpu.sregs[3]; // DS
            dosFs.dtaOffset = cpu.regs[2];   // DX
            break;
        }
        case 0x25: { // Set interrupt vector - stub (programs set INT 24h error handler)
            // Silently ignore
            break;
        }
        case 0x2A: { // Get date - stub
            cpu.regs[1] = 2026;  // CX = year
            cpu.setReg8(6, 2);   // DH = month
            cpu.setReg8(2, 13);  // DL = day
            cpu.setReg8(0, 5);   // AL = day of week (Friday)
            break;
        }
        case 0x2C: { // Get time - stub
            cpu.setReg8(4, 12);  // CH = hour
            cpu.setReg8(1, 0);   // CL = minute
            cpu.setReg8(6, 0);   // DH = second
            cpu.setReg8(2, 0);   // DL = centisecond
            break;
        }
        case 0x2F: { // Get DTA address
            cpu.sregs[0] = dosFs.dtaSegment; // ES
            cpu.regs[3] = dosFs.dtaOffset;   // BX
            break;
        }
        case 0x30: { // Get DOS version - stub
            cpu.setReg8(0, 5);   // AL = major
            cpu.setReg8(4, 0);   // AH = minor
            break;
        }
        case 0x35: { // Get interrupt vector - stub
            cpu.sregs[0] = 0;  // ES = 0
            cpu.regs[3] = 0;   // BX = 0
            break;
        }
        case 0x3B: { // Change directory
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 3; // path not found
                dosFs.errors++;
                break;
            }
            {
                string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]);
                bool pathError = false;
                string hostPath = resolveDosPath(dosFs, dosPath, pathError);
                namespace fs = std::filesystem;
                if (pathError || !fs::is_directory(hostPath)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                } else {
                    // Convert host path back to relative DOS dir within root
                    string rootNorm = dosFs.rootPath;
                    string hostNorm = hostPath;
                    for (char& c : rootNorm) { if (c == '\\') c = '/'; }
                    for (char& c : hostNorm) { if (c == '\\') c = '/'; }
                    string rel;
                    if (hostNorm.size() > rootNorm.size() + 1) {
                        rel = hostNorm.substr(rootNorm.size() + 1);
                    }
                    // Uppercase
                    for (char& c : rel) c = (char)toupper((unsigned char)c);
                    dosFs.currentDir = rel;
                    cpu.setFlag(CPU::CF, false);
                }
            }
            break;
        }
        case 0x3C: { // Create file
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 2; // file not found (no filesystem)
                dosFs.errors++;
                break;
            }
            {
                string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]);
                bool pathError = false;
                string hostPath = resolveDosPath(dosFs, dosPath, pathError);
                if (pathError) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }
                FILE* fp = fopen(hostPath.c_str(), "w+b");
                if (!fp) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 5; // access denied
                    dosFs.errors++;
                    break;
                }
                int handle = dosFs.nextHandle++;
                DOSFileEntry entry;
                entry.fp = fp;
                entry.hostPath = hostPath;
                entry.dosPath = dosPath;
                entry.mode = 2; // r/w
                dosFs.handles[handle] = entry;
                dosFs.filesOpened++;
                cpu.regs[0] = (uint16_t)handle; // AX = handle
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x3D: { // Open file
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 2; // file not found
                dosFs.errors++;
                break;
            }
            {
                string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]);
                uint8_t mode = cpu.getReg8(0); // AL = access mode
                bool pathError = false;
                string hostPath = resolveDosPath(dosFs, dosPath, pathError);
                if (pathError) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }
                const char* fmode;
                uint8_t dosMode = mode & 0x03;
                if (dosMode == 0) fmode = "rb";       // read-only
                else if (dosMode == 1) fmode = "r+b";  // write-only (open existing)
                else fmode = "r+b";                     // read/write
                FILE* fp = fopen(hostPath.c_str(), fmode);
                if (!fp) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 2; // file not found
                    dosFs.errors++;
                    break;
                }
                int handle = dosFs.nextHandle++;
                DOSFileEntry entry;
                entry.fp = fp;
                entry.hostPath = hostPath;
                entry.dosPath = dosPath;
                entry.mode = dosMode;
                dosFs.handles[handle] = entry;
                dosFs.filesOpened++;
                cpu.regs[0] = (uint16_t)handle; // AX = handle
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x3E: { // Close file
            {
                int handle = (int)cpu.regs[3]; // BX
                auto it = dosFs.handles.find(handle);
                if (it == dosFs.handles.end()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 6; // invalid handle
                    dosFs.errors++;
                    break;
                }
                if (!it->second.isDevice && it->second.fp) {
                    fclose(it->second.fp);
                }
                dosFs.handles.erase(it);
                dosFs.filesClosed++;
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x3F: { // Read file/device
            {
                int handle = (int)cpu.regs[3]; // BX
                uint16_t count = cpu.regs[1];  // CX = bytes to read
                uint16_t seg = cpu.sregs[3];   // DS
                uint16_t off = cpu.regs[2];    // DX = buffer offset
                auto it = dosFs.handles.find(handle);
                if (it == dosFs.handles.end()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 6; // invalid handle
                    dosFs.errors++;
                    break;
                }
                if (it->second.isDevice) {
                    // Handle 0 = STDIN: read from io.readChar()
                    if (handle == 0) {
                        uint16_t bytesRead = 0;
                        for (uint16_t i = 0; i < count; i++) {
                            int ch = io.readChar();
                            if (ch < 0) break;
                            mem.swrite8(seg, (uint16_t)(off + i), (uint8_t)ch);
                            bytesRead++;
                            if (ch == 0x0D || ch == 0x0A) { bytesRead++; break; } // line-oriented
                        }
                        cpu.regs[0] = bytesRead;
                        dosFs.bytesRead += bytesRead;
                    } else {
                        // Other device handles: return 0 bytes
                        cpu.regs[0] = 0;
                    }
                    cpu.setFlag(CPU::CF, false);
                } else {
                    if (!it->second.fp) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 6;
                        dosFs.errors++;
                        break;
                    }
                    // Read into a temp buffer, then write to emulator memory
                    vector<uint8_t> buf(count);
                    size_t got = fread(buf.data(), 1, count, it->second.fp);
                    for (size_t i = 0; i < got; i++) {
                        mem.swrite8(seg, (uint16_t)(off + i), buf[i]);
                    }
                    cpu.regs[0] = (uint16_t)got; // AX = bytes actually read
                    dosFs.bytesRead += (int)got;
                    cpu.setFlag(CPU::CF, false);
                }
            }
            break;
        }
        case 0x40: { // Write file/device
            {
                int handle = (int)cpu.regs[3]; // BX
                uint16_t count = cpu.regs[1];  // CX = bytes to write
                uint16_t seg = cpu.sregs[3];   // DS
                uint16_t off = cpu.regs[2];    // DX = buffer offset
                auto it = dosFs.handles.find(handle);
                if (it == dosFs.handles.end()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 6; // invalid handle
                    dosFs.errors++;
                    break;
                }
                if (it->second.isDevice) {
                    // Handles 1,2 = STDOUT/STDERR: append to stdout capture
                    if (handle == 1 || handle == 2) {
                        for (uint16_t i = 0; i < count; i++) {
                            uint8_t ch = mem.sread8(seg, (uint16_t)(off + i));
                            if ((int)io.stdoutBuf.size() < MAX_OUTPUT) {
                                io.stdoutBuf += (char)ch;
                            }
                            ttyCharToVRAM(mem, vram, ch);
                        }
                    }
                    // Handle 0 (stdin write), 3 (AUX), 4 (PRN): silently discard
                    cpu.regs[0] = count; // AX = bytes written
                    dosFs.bytesWritten += count;
                    cpu.setFlag(CPU::CF, false);
                } else {
                    if (!it->second.fp) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 6;
                        dosFs.errors++;
                        break;
                    }
                    // Read from emulator memory into temp buffer, then write to file
                    vector<uint8_t> buf(count);
                    for (uint16_t i = 0; i < count; i++) {
                        buf[i] = mem.sread8(seg, (uint16_t)(off + i));
                    }
                    size_t wrote = fwrite(buf.data(), 1, count, it->second.fp);
                    fflush(it->second.fp);
                    cpu.regs[0] = (uint16_t)wrote; // AX = bytes written
                    dosFs.bytesWritten += (int)wrote;
                    cpu.setFlag(CPU::CF, false);
                }
            }
            break;
        }
        case 0x41: { // Delete file
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 2; // file not found (no filesystem)
                dosFs.errors++;
                break;
            }
            {
                string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]); // DS:DX = path
                bool pathError = false;
                string hostPath = resolveDosPath(dosFs, dosPath, pathError);
                if (pathError) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }
                namespace fs = std::filesystem;
                std::error_code ec;
                if (!fs::exists(hostPath, ec)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 2; // file not found
                    dosFs.errors++;
                    break;
                }
                if (fs::is_directory(hostPath, ec)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 5; // access denied (can't delete directories with 0x41)
                    dosFs.errors++;
                    break;
                }
                if (!fs::remove(hostPath, ec)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 5; // access denied
                    dosFs.errors++;
                    break;
                }
                dosFs.filesDeleted++;
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x42: { // Seek (lseek)
            {
                int handle = (int)cpu.regs[3]; // BX
                uint8_t origin = cpu.getReg8(0); // AL = origin (0=start, 1=cur, 2=end)
                int32_t offset = (int32_t)(((uint32_t)cpu.regs[1] << 16) | cpu.regs[2]); // CX:DX
                auto it = dosFs.handles.find(handle);
                if (it == dosFs.handles.end() || it->second.isDevice || !it->second.fp) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 6; // invalid handle
                    dosFs.errors++;
                    break;
                }
                int whence;
                if (origin == 0) whence = SEEK_SET;
                else if (origin == 1) whence = SEEK_CUR;
                else whence = SEEK_END;
                if (fseek(it->second.fp, offset, whence) != 0) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 1; // invalid function
                    dosFs.errors++;
                    break;
                }
                long pos = ftell(it->second.fp);
                cpu.regs[2] = (uint16_t)(pos & 0xFFFF);         // DX:AX = new position
                cpu.regs[0] = (uint16_t)((pos >> 16) & 0xFFFF);
                // Note: DOS returns new pos in DX:AX but convention is DX=high, AX=low
                // Correction: AX=low word, DX=high word
                cpu.regs[0] = (uint16_t)(pos & 0xFFFF);  // AX = low
                cpu.regs[2] = (uint16_t)((pos >> 16) & 0xFFFF); // DX = high
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x43: { // Get/set file attributes
            {
                uint8_t al = cpu.getReg8(0);
                if (al == 0) { // Get attributes
                    if (!dosFs.enabled) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 2;
                        dosFs.errors++;
                        break;
                    }
                    string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]);
                    bool pathError = false;
                    string hostPath = resolveDosPath(dosFs, dosPath, pathError);
                    namespace fs = std::filesystem;
                    if (pathError || !fs::exists(hostPath)) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 2; // file not found
                        dosFs.errors++;
                        break;
                    }
                    uint16_t attrs = 0x20; // Archive bit
                    if (fs::is_directory(hostPath)) attrs = 0x10; // Directory
                    cpu.regs[1] = attrs; // CX = attributes
                    cpu.setFlag(CPU::CF, false);
                } else { // Set attributes - stub (succeed silently)
                    cpu.setFlag(CPU::CF, false);
                }
            }
            break;
        }
        case 0x44: { // IOCTL
            {
                uint8_t al = cpu.getReg8(0);
                if (al == 0x00) { // Get device info
                    int handle = (int)cpu.regs[3]; // BX
                    auto it = dosFs.handles.find(handle);
                    if (it == dosFs.handles.end()) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 6; // invalid handle
                        dosFs.errors++;
                        break;
                    }
                    uint16_t info = 0;
                    if (it->second.isDevice) {
                        info = 0x80; // bit 7 = is device
                        if (handle == 0) info |= 0x01; // STDIN
                        if (handle == 1 || handle == 2) info |= 0x02; // STDOUT
                    }
                    // For files: info = 0 (block device, not EOF, etc.)
                    cpu.regs[2] = info; // DX = device info word
                    cpu.setFlag(CPU::CF, false);
                } else if (al == 0x09) { // Check if block device is remote
                    // BL = drive number (0=default, 1=A:, 2=B:, 3=C:, ...)
                    uint8_t drive = cpu.getReg8(2); // BL
                    if (drive == 0) drive = 3; // default = C:
                    if (drive == 3) { // C: is our emulated drive
                        // DX bit 12 = 0 (local), bit 9 = 0 (not shared)
                        cpu.regs[2] = 0x0000; // local, not remote
                        cpu.setFlag(CPU::CF, false);
                    } else {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 0x0F; // invalid drive
                        dosFs.errors++;
                    }
                } else {
                    // Other IOCTL subfunctions: stub
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 1; // invalid function
                }
            }
            break;
        }
        case 0x47: { // Get current directory
            {
                // DS:SI = buffer to receive path (64 bytes max, no drive letter, no leading backslash)
                uint16_t seg = cpu.sregs[3]; // DS
                uint16_t off = cpu.regs[6];  // SI
                string dir = dosFs.currentDir;
                // Convert forward slashes to backslashes for DOS
                for (char& c : dir) { if (c == '/') c = '\\'; }
                // Write to emulator memory (no leading backslash, null-terminated)
                for (size_t i = 0; i < dir.size() && i < 63; i++) {
                    mem.swrite8(seg, (uint16_t)(off + i), (uint8_t)dir[i]);
                }
                mem.swrite8(seg, (uint16_t)(off + dir.size()), 0); // null terminator
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x4C: { // Exit with AL as exit code
            io.exitCode = cpu.getReg8(0); // AL
            result.halted = true;
            result.haltReason = "INT 21h/4Ch exit (code=" + to_string(io.exitCode) + ")";
            result.exitCode = io.exitCode;
            break;
        }
        case 0x4E: { // FindFirst - find first matching file
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 2; // file not found
                dosFs.errors++;
                break;
            }
            {
                namespace fs = std::filesystem;
                string dosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]);
                uint16_t searchAttr = cpu.regs[1]; // CX = attribute mask
                dosFs.dirSearches++;

                // Split path into directory + wildcard pattern
                string pathCopy = dosPath;
                for (char& c : pathCopy) { if (c == '\\') c = '/'; }
                string dirPart, patternPart;
                size_t lastSlash = pathCopy.rfind('/');
                if (lastSlash != string::npos) {
                    dirPart = pathCopy.substr(0, lastSlash);
                    patternPart = pathCopy.substr(lastSlash + 1);
                } else {
                    patternPart = pathCopy;
                }
                // Uppercase the pattern
                for (char& c : patternPart) c = (char)toupper((unsigned char)c);

                // Resolve the directory part
                string searchDir;
                if (dirPart.empty()) {
                    // Search in current directory
                    if (dosFs.currentDir.empty()) {
                        searchDir = dosFs.rootPath;
                    } else {
                        searchDir = dosFs.rootPath + "/" + dosFs.currentDir;
                    }
                } else {
                    bool pathError = false;
                    searchDir = resolveDosPath(dosFs, dirPart, pathError);
                    if (pathError || !fs::is_directory(searchDir)) {
                        cpu.setFlag(CPU::CF, true);
                        cpu.regs[0] = 3; // path not found
                        dosFs.errors++;
                        break;
                    }
                }

                // Enumerate directory and filter
                DOSSearchState search;
                search.searchAttr = (uint8_t)(searchAttr & 0xFF);
                search.pattern = patternPart;
                try {
                    for (auto& dirEntry : fs::directory_iterator(searchDir)) {
                        string fname = dirEntry.path().filename().string();
                        string shortName = toShortName(fname);
                        bool isDir = dirEntry.is_directory();
                        // Filter by attribute: directories only included if attr bit 0x10 set
                        if (isDir && !(searchAttr & 0x10)) continue;
                        // Skip hidden/system files (names starting with '.')
                        if (!fname.empty() && fname[0] == '.') continue;
                        // Match against wildcard pattern
                        if (matchesDosWildcard(shortName, patternPart)) {
                            search.entries.push_back({dirEntry.path().string(), shortName});
                        }
                    }
                } catch (...) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }

                if (search.entries.empty()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 18; // no more files
                    break;
                }

                // Store search state and get index
                search.nextIndex = 1; // first entry consumed now
                size_t searchId = dosFs.searches.size();
                dosFs.searches.push_back(search);

                // Write first match to DTA
                auto& firstEntry = search.entries[0];
                string& hostEntryPath = firstEntry.first;
                string& dosName = firstEntry.second;

                // Get file info
                uint32_t fileSize = 0;
                uint16_t dosDate = 0, dosTime = 0;
                uint8_t fileAttr = 0x20; // Archive
                try {
                    auto stat = fs::status(hostEntryPath);
                    if (fs::is_directory(stat)) fileAttr = 0x10;
                    if (fs::is_regular_file(stat)) fileSize = (uint32_t)fs::file_size(hostEntryPath);
                    auto ftime = fs::last_write_time(hostEntryPath);
                    toDosDateTime(ftime, dosDate, dosTime);
                } catch (...) {}

                // Write DTA (43 bytes)
                uint16_t dtaSeg = dosFs.dtaSegment;
                uint16_t dtaOff = dosFs.dtaOffset;
                // Bytes 0-20: reserved (store search ID as uint16_t at offset 0)
                for (int i = 0; i < 21; i++) mem.swrite8(dtaSeg, (uint16_t)(dtaOff + i), 0);
                mem.swrite8(dtaSeg, dtaOff, (uint8_t)(searchId & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 1), (uint8_t)((searchId >> 8) & 0xFF));
                // Byte 0x15: file attribute
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x15), fileAttr);
                // Bytes 0x16-0x17: file time
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x16), (uint8_t)(dosTime & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x17), (uint8_t)(dosTime >> 8));
                // Bytes 0x18-0x19: file date
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x18), (uint8_t)(dosDate & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x19), (uint8_t)(dosDate >> 8));
                // Bytes 0x1A-0x1D: file size (DWORD little-endian)
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1A), (uint8_t)(fileSize & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1B), (uint8_t)((fileSize >> 8) & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1C), (uint8_t)((fileSize >> 16) & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1D), (uint8_t)((fileSize >> 24) & 0xFF));
                // Bytes 0x1E-0x2A: filename (ASCIIZ, 13 bytes max)
                for (size_t i = 0; i < dosName.size() && i < 12; i++) {
                    mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1E + i), (uint8_t)dosName[i]);
                }
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1E + min(dosName.size(), (size_t)12)), 0);

                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x4F: { // FindNext - find next matching file
            {
                // Read search ID from DTA reserved bytes
                uint16_t dtaSeg = dosFs.dtaSegment;
                uint16_t dtaOff = dosFs.dtaOffset;
                uint16_t searchId = mem.sread8(dtaSeg, dtaOff) |
                                    ((uint16_t)mem.sread8(dtaSeg, (uint16_t)(dtaOff + 1)) << 8);

                if (searchId >= dosFs.searches.size()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 18; // no more files
                    break;
                }

                DOSSearchState& search = dosFs.searches[searchId];
                if (search.nextIndex >= search.entries.size()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 18; // no more files
                    break;
                }

                namespace fs = std::filesystem;
                auto& entry = search.entries[search.nextIndex];
                search.nextIndex++;

                string& hostEntryPath = entry.first;
                string& dosName = entry.second;

                uint32_t fileSize = 0;
                uint16_t dosDate = 0, dosTime = 0;
                uint8_t fileAttr = 0x20;
                try {
                    auto stat = fs::status(hostEntryPath);
                    if (fs::is_directory(stat)) fileAttr = 0x10;
                    if (fs::is_regular_file(stat)) fileSize = (uint32_t)fs::file_size(hostEntryPath);
                    auto ftime = fs::last_write_time(hostEntryPath);
                    toDosDateTime(ftime, dosDate, dosTime);
                } catch (...) {}

                // Write DTA
                // Preserve search ID in reserved bytes, update the rest
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x15), fileAttr);
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x16), (uint8_t)(dosTime & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x17), (uint8_t)(dosTime >> 8));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x18), (uint8_t)(dosDate & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x19), (uint8_t)(dosDate >> 8));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1A), (uint8_t)(fileSize & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1B), (uint8_t)((fileSize >> 8) & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1C), (uint8_t)((fileSize >> 16) & 0xFF));
                mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1D), (uint8_t)((fileSize >> 24) & 0xFF));
                // Clear and write filename
                for (int i = 0; i < 13; i++) mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1E + i), 0);
                for (size_t i = 0; i < dosName.size() && i < 12; i++) {
                    mem.swrite8(dtaSeg, (uint16_t)(dtaOff + 0x1E + i), (uint8_t)dosName[i]);
                }

                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x56: { // Rename/move file
            if (!dosFs.enabled) {
                cpu.setFlag(CPU::CF, true);
                cpu.regs[0] = 2; // file not found (no filesystem)
                dosFs.errors++;
                break;
            }
            {
                string oldDosPath = readDosString(mem, cpu.sregs[3], cpu.regs[2]); // DS:DX = old name
                string newDosPath = readDosString(mem, cpu.sregs[0], cpu.regs[7]); // ES:DI = new name
                bool pathError = false;
                string oldHostPath = resolveDosPath(dosFs, oldDosPath, pathError);
                if (pathError) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }
                string newHostPath = resolveDosPath(dosFs, newDosPath, pathError);
                if (pathError) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 3; // path not found
                    dosFs.errors++;
                    break;
                }
                namespace fs = std::filesystem;
                std::error_code ec;
                if (!fs::exists(oldHostPath, ec)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 2; // file not found
                    dosFs.errors++;
                    break;
                }
                if (fs::exists(newHostPath, ec)) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 5; // access denied (target already exists)
                    dosFs.errors++;
                    break;
                }
                fs::rename(oldHostPath, newHostPath, ec);
                if (ec) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 17; // not same device (or general rename failure)
                    dosFs.errors++;
                    break;
                }
                dosFs.filesRenamed++;
                cpu.setFlag(CPU::CF, false);
            }
            break;
        }
        case 0x57: { // Get/set file date and time
            {
                uint8_t al = cpu.getReg8(0);
                int handle = (int)cpu.regs[3]; // BX
                auto it = dosFs.handles.find(handle);
                if (it == dosFs.handles.end()) {
                    cpu.setFlag(CPU::CF, true);
                    cpu.regs[0] = 6; // invalid handle
                    dosFs.errors++;
                    break;
                }
                if (al == 0x00) { // Get file date/time
                    namespace fs = std::filesystem;
                    if (it->second.isDevice || it->second.hostPath.empty()) {
                        cpu.regs[1] = 0; // CX = time
                        cpu.regs[2] = 0; // DX = date
                    } else {
                        try {
                            auto ftime = fs::last_write_time(it->second.hostPath);
                            uint16_t dosDate, dosTime;
                            toDosDateTime(ftime, dosDate, dosTime);
                            cpu.regs[1] = dosTime; // CX = time
                            cpu.regs[2] = dosDate; // DX = date
                        } catch (...) {
                            cpu.regs[1] = 0;
                            cpu.regs[2] = 0;
                        }
                    }
                    cpu.setFlag(CPU::CF, false);
                } else { // Set file date/time - stub (succeed silently)
                    cpu.setFlag(CPU::CF, false);
                }
            }
            break;
        }
        case 0x62: { // Get PSP segment
            cpu.regs[3] = 0; // BX = PSP segment (we use segment 0)
            cpu.setFlag(CPU::CF, false);
            break;
        }
        case 0x48: { // Allocate memory - BX = paragraphs requested
            uint16_t requested = cpu.regs[3]; // BX
            uint16_t seg = dosMem.allocate(requested);
            if (seg != 0) {
                cpu.regs[0] = seg;  // AX = segment of allocated block
                cpu.setFlag(CPU::CF, false);
            } else {
                cpu.regs[0] = 8;    // AX = error 8 (insufficient memory)
                cpu.regs[3] = dosMem.largestFree(); // BX = largest available
                cpu.setFlag(CPU::CF, true);
            }
            break;
        }
        case 0x49: { // Free memory - ES = segment to free
            uint16_t seg = cpu.sregs[0]; // ES
            if (dosMem.free(seg)) {
                cpu.setFlag(CPU::CF, false);
            } else {
                cpu.regs[0] = 9;    // AX = error 9 (invalid memory block)
                cpu.setFlag(CPU::CF, true);
            }
            break;
        }
        case 0x4A: { // Resize memory block - ES = segment, BX = new size in paragraphs
            uint16_t seg = cpu.sregs[0]; // ES
            uint16_t newSize = cpu.regs[3]; // BX
            if (dosMem.resize(seg, newSize)) {
                cpu.setFlag(CPU::CF, false);
            } else {
                cpu.regs[0] = 8;    // AX = error 8 (insufficient memory)
                cpu.regs[3] = dosMem.largestFree(); // BX = largest available
                cpu.setFlag(CPU::CF, true);
            }
            break;
        }
        default: {
            result.skipped.push_back({ cpu.ip, "INT 21h AH=" + hexByte(ah), "Unimplemented DOS function", 1 });
            break;
        }
    }
}

void handleInt33(CPU& cpu, MouseState& mouse, EmulatorResult& result,
                 EventState& evState, Memory& mem, IOCapture& io,
                 const vector<uint8_t>& code, const EmulatorConfig& config,
                 const VRAMState& vram, int cycle) {
    uint16_t func = cpu.regs[0]; // INT 33h dispatches on full AX
    if (!mouse.installed) {
        // Driver not installed — return AX=0 for reset/detect, skip everything else
        if (func == 0x0000) {
            cpu.regs[0] = 0x0000; // not installed
            cpu.regs[3] = 0;     // BX = 0 buttons
        }
        return;
    }
    evState.counters.int33Count++;
    switch (func) {
        case 0x0000: // Reset/detect
            cpu.regs[0] = 0xFFFF; // installed
            cpu.regs[3] = 3;     // BX = 3 buttons
            mouse.x = (mouse.minX + mouse.maxX) / 2;
            mouse.y = (mouse.minY + mouse.maxY) / 2;
            mouse.visible = false;
            mouse.buttons = 0;
            for (int i = 0; i < 3; i++) { mouse.pressCount[i] = 0; mouse.releaseCount[i] = 0; }
            break;
        case 0x0001: // Show cursor
            mouse.visible = true;
            break;
        case 0x0002: // Hide cursor
            mouse.visible = false;
            break;
        case 0x0003: // Get position and button status
            evState.counters.pollCount++;
            processEvents(evState, cycle, mouse, io, result, cpu, mem, code, config, vram);
            cpu.regs[3] = mouse.buttons;   // BX
            cpu.regs[1] = (uint16_t)mouse.x; // CX
            cpu.regs[2] = (uint16_t)mouse.y; // DX
            break;
        case 0x0004: { // Set cursor position
            int16_t nx = (int16_t)cpu.regs[1]; // CX
            int16_t ny = (int16_t)cpu.regs[2]; // DX
            if (nx < mouse.minX) nx = mouse.minX;
            if (nx > mouse.maxX) nx = mouse.maxX;
            if (ny < mouse.minY) ny = mouse.minY;
            if (ny > mouse.maxY) ny = mouse.maxY;
            mouse.x = nx;
            mouse.y = ny;
            break;
        }
        case 0x0005: { // Get button press info
            int btn = cpu.regs[3] & 3; // BX = button index
            if (btn < 3) {
                cpu.regs[3] = mouse.pressCount[btn]; // BX = count
                mouse.pressCount[btn] = 0;
            } else {
                cpu.regs[3] = 0;
            }
            cpu.regs[0] = mouse.buttons; // AX = current button state
            cpu.regs[1] = (uint16_t)mouse.x; // CX = last press x
            cpu.regs[2] = (uint16_t)mouse.y; // DX = last press y
            break;
        }
        case 0x0006: { // Get button release info
            int btn = cpu.regs[3] & 3; // BX = button index
            if (btn < 3) {
                cpu.regs[3] = mouse.releaseCount[btn]; // BX = count
                mouse.releaseCount[btn] = 0;
            } else {
                cpu.regs[3] = 0;
            }
            cpu.regs[0] = mouse.buttons; // AX = current button state
            cpu.regs[1] = (uint16_t)mouse.x; // CX = last release x
            cpu.regs[2] = (uint16_t)mouse.y; // DX = last release y
            break;
        }
        case 0x0007: // Set horizontal range
            mouse.minX = (int16_t)cpu.regs[1]; // CX
            mouse.maxX = (int16_t)cpu.regs[2]; // DX
            if (mouse.x < mouse.minX) mouse.x = mouse.minX;
            if (mouse.x > mouse.maxX) mouse.x = mouse.maxX;
            break;
        case 0x0008: // Set vertical range
            mouse.minY = (int16_t)cpu.regs[1]; // CX
            mouse.maxY = (int16_t)cpu.regs[2]; // DX
            if (mouse.y < mouse.minY) mouse.y = mouse.minY;
            if (mouse.y > mouse.maxY) mouse.y = mouse.maxY;
            break;
        case 0x000B: // Get motion counters
            cpu.regs[1] = 0; // CX = horizontal mickeys
            cpu.regs[2] = 0; // DX = vertical mickeys
            break;
        case 0x000C: // Set event handler — silently ignored
            break;
        default:
            result.skipped.push_back({ cpu.ip, "INT 33h AX=" + hexImm16(func), "Unimplemented mouse function", 1 });
            break;
    }
}

// --- INT 16h: BIOS Keyboard Services ---
void handleInt16(CPU& cpu, IOCapture& io, EmulatorResult& result) {
    uint8_t ah = cpu.getReg8(4); // AH
    switch (ah) {
        case 0x02: // Get shift flags
            cpu.setReg8(0, io.shiftFlags); // AL = shift flags byte
            break;
        default:
            result.skipped.push_back({ cpu.ip, "INT 16h AH=" + hexByte(ah), "Unimplemented keyboard function", 1 });
            break;
    }
}

void handleInterrupt(CPU& cpu, Memory& mem, IOCapture& io, EmulatorResult& result,
                     uint8_t intNum, VRAMState& vram, MouseState& mouse,
                     EventState& evState, const vector<uint8_t>& code,
                     const EmulatorConfig& config, int cycle, DOSFileState& dosFs,
                     DOSMemoryState& dosMem) {
    if (intNum == 0x20) {
        result.halted = true;
        result.haltReason = "INT 20h program terminate";
        result.exitCode = 0;
    } else if (intNum == 0x21) {
        handleInt21(cpu, mem, io, result, vram, evState, mouse, code, config, cycle, dosFs, dosMem);
    } else if (intNum == 0x10) {
        handleInt10(cpu, mem, vram, result);
    } else if (intNum == 0x16) {
        handleInt16(cpu, io, result);
    } else if (intNum == 0x33) {
        handleInt33(cpu, mouse, result, evState, mem, io, code, config, vram, cycle);
    } else {
        result.skipped.push_back({ cpu.ip, "INT " + hexByte(intNum), "Unimplemented interrupt", 1 });
    }
}

// --- Section 4: Instruction Execution ---

void executeInstruction(CPU& cpu, Memory& mem, IOCapture& io, const DecodedInst& inst,
                        EmulatorResult& result, vector<uint8_t>& code, bool& memDirty, VRAMState& vram, MouseState& mouse,
                        EventState& evState, const EmulatorConfig& config, int cycle, DOSFileState& dosFs,
                        DOSMemoryState& dosMem) {
    const string& mn = inst.mnemonic;

    // --- ALU: ADD, ADC, SUB, SBB, CMP, AND, OR, XOR, TEST ---
    if (mn == "ADD" || mn == "ADC" || mn == "SUB" || mn == "SBB" ||
        mn == "CMP" || mn == "AND" || mn == "OR"  || mn == "XOR" || mn == "TEST") {
        uint16_t a = readOperand(cpu, mem, inst.op1, inst.segOverride);
        uint16_t b = readOperand(cpu, mem, inst.op2, inst.segOverride);
        bool wide = inst.wide;
        uint32_t mask = wide ? 0xFFFF : 0xFF;
        uint32_t res;

        if (mn == "ADD") {
            res = (uint32_t)a + b;
            updateFlagsAdd(cpu, res, a, b, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "ADC") {
            uint16_t cf = cpu.getFlag(CPU::CF) ? 1 : 0;
            res = (uint32_t)a + b + cf;
            updateFlagsAdd(cpu, res, a, (uint16_t)(b + cf), wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "SUB") {
            res = (uint32_t)a - b;
            updateFlagsSub(cpu, res, a, b, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "SBB") {
            uint16_t cf = cpu.getFlag(CPU::CF) ? 1 : 0;
            res = (uint32_t)a - b - cf;
            updateFlagsSub(cpu, res, a, (uint16_t)(b + cf), wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "CMP") {
            res = (uint32_t)a - b;
            updateFlagsSub(cpu, res, a, b, wide);
        } else if (mn == "AND") {
            res = a & b;
            updateFlagsLogic(cpu, (uint16_t)res, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "OR") {
            res = a | b;
            updateFlagsLogic(cpu, (uint16_t)res, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else if (mn == "XOR") {
            res = a ^ b;
            updateFlagsLogic(cpu, (uint16_t)res, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else { // TEST
            res = a & b;
            updateFlagsLogic(cpu, (uint16_t)res, wide);
        }
    }

    // --- INC / DEC (preserve CF) ---
    else if (mn == "INC" || mn == "DEC") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        bool wide = inst.wide;
        uint32_t mask = wide ? 0xFFFF : 0xFF;
        bool savedCF = cpu.getFlag(CPU::CF);
        if (mn == "INC") {
            uint32_t res = (uint32_t)val + 1;
            updateFlagsAdd(cpu, res, val, 1, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        } else {
            uint32_t res = (uint32_t)val - 1;
            updateFlagsSub(cpu, res, val, 1, wide);
            writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
        }
        cpu.setFlag(CPU::CF, savedCF); // preserve CF
    }

    // --- NOT ---
    else if (mn == "NOT") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        uint32_t mask = inst.wide ? 0xFFFF : 0xFF;
        writeOperand(cpu, mem, inst.op1, (uint16_t)(~val & mask), memDirty, inst.segOverride);
    }

    // --- NEG ---
    else if (mn == "NEG") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        bool wide = inst.wide;
        uint32_t mask = wide ? 0xFFFF : 0xFF;
        uint32_t res = (uint32_t)(0 - val);
        updateFlagsSub(cpu, res, 0, val, wide);
        cpu.setFlag(CPU::CF, val != 0);
        writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
    }

    // --- MUL ---
    else if (mn == "MUL") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        if (inst.wide) {
            uint32_t res = (uint32_t)cpu.regs[0] * val;
            cpu.regs[0] = (uint16_t)(res & 0xFFFF);
            cpu.regs[2] = (uint16_t)(res >> 16);
            bool hi = cpu.regs[2] != 0;
            cpu.setFlag(CPU::CF, hi);
            cpu.setFlag(CPU::OF, hi);
        } else {
            uint16_t res = (uint16_t)cpu.getReg8(0) * (uint16_t)(val & 0xFF);
            cpu.regs[0] = res;
            bool hi = (res >> 8) != 0;
            cpu.setFlag(CPU::CF, hi);
            cpu.setFlag(CPU::OF, hi);
        }
    }

    // --- IMUL ---
    else if (mn == "IMUL") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        if (inst.wide) {
            int32_t res = (int32_t)(int16_t)cpu.regs[0] * (int32_t)(int16_t)val;
            cpu.regs[0] = (uint16_t)(res & 0xFFFF);
            cpu.regs[2] = (uint16_t)((uint32_t)res >> 16);
            int16_t lo = (int16_t)cpu.regs[0];
            bool ext = (int32_t)lo != res;
            cpu.setFlag(CPU::CF, ext);
            cpu.setFlag(CPU::OF, ext);
        } else {
            int16_t res = (int16_t)(int8_t)cpu.getReg8(0) * (int16_t)(int8_t)(val & 0xFF);
            cpu.regs[0] = (uint16_t)res;
            int8_t lo = (int8_t)(res & 0xFF);
            bool ext = (int16_t)lo != res;
            cpu.setFlag(CPU::CF, ext);
            cpu.setFlag(CPU::OF, ext);
        }
    }

    // --- DIV ---
    else if (mn == "DIV") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        if (val == 0) {
            result.halted = true;
            result.haltReason = "Division by zero";
            return;
        }
        if (inst.wide) {
            uint32_t dividend = ((uint32_t)cpu.regs[2] << 16) | cpu.regs[0];
            uint32_t quot = dividend / val;
            uint16_t rem = (uint16_t)(dividend % val);
            if (quot > 0xFFFF) {
                result.halted = true;
                result.haltReason = "Division overflow";
                return;
            }
            cpu.regs[0] = (uint16_t)quot;
            cpu.regs[2] = rem;
        } else {
            uint16_t dividend = cpu.regs[0];
            uint16_t divisor = val & 0xFF;
            uint16_t quot = dividend / divisor;
            uint8_t rem = (uint8_t)(dividend % divisor);
            if (quot > 0xFF) {
                result.halted = true;
                result.haltReason = "Division overflow";
                return;
            }
            cpu.setReg8(0, (uint8_t)quot);   // AL = quotient
            cpu.setReg8(4, rem);              // AH = remainder
        }
    }

    // --- IDIV ---
    else if (mn == "IDIV") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        if (val == 0) {
            result.halted = true;
            result.haltReason = "Division by zero";
            return;
        }
        if (inst.wide) {
            int32_t dividend = (int32_t)(((uint32_t)cpu.regs[2] << 16) | cpu.regs[0]);
            int16_t divisor = (int16_t)val;
            int32_t quot = dividend / divisor;
            int16_t rem = (int16_t)(dividend % divisor);
            if (quot > 32767 || quot < -32768) {
                result.halted = true;
                result.haltReason = "Division overflow";
                return;
            }
            cpu.regs[0] = (uint16_t)(int16_t)quot;
            cpu.regs[2] = (uint16_t)rem;
        } else {
            int16_t dividend = (int16_t)cpu.regs[0];
            int8_t divisor = (int8_t)(val & 0xFF);
            int16_t quot = dividend / divisor;
            int8_t rem = (int8_t)(dividend % divisor);
            if (quot > 127 || quot < -128) {
                result.halted = true;
                result.haltReason = "Division overflow";
                return;
            }
            cpu.setReg8(0, (uint8_t)(int8_t)quot);
            cpu.setReg8(4, (uint8_t)rem);
        }
    }

    // --- Shifts and Rotates ---
    else if (mn == "SHL" || mn == "SHR" || mn == "SAR" ||
             mn == "ROL" || mn == "ROR" || mn == "RCL" || mn == "RCR") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        uint16_t cnt = readOperand(cpu, mem, inst.op2, inst.segOverride) & 0x1F; // mask to 0-31
        if (cnt == 0) return; // no operation, no flag changes
        bool wide = inst.wide;
        uint32_t mask = wide ? 0xFFFF : 0xFF;
        int bits = wide ? 16 : 8;
        uint32_t signBit = wide ? 0x8000 : 0x80;
        uint16_t res = val;

        if (mn == "SHL") {
            for (uint16_t i = 0; i < cnt; i++) {
                cpu.setFlag(CPU::CF, (res & signBit) != 0);
                res = (uint16_t)((res << 1) & mask);
            }
            if (cnt == 1) cpu.setFlag(CPU::OF, (bool)(res & signBit) != cpu.getFlag(CPU::CF));
            cpu.setFlag(CPU::ZF, (res & mask) == 0);
            cpu.setFlag(CPU::SF, (res & signBit) != 0);
            cpu.setFlag(CPU::PF, parity8((uint8_t)(res & 0xFF)));
        } else if (mn == "SHR") {
            if (cnt == 1) cpu.setFlag(CPU::OF, (val & signBit) != 0);
            for (uint16_t i = 0; i < cnt; i++) {
                cpu.setFlag(CPU::CF, (res & 1) != 0);
                res = (uint16_t)((res >> 1) & mask);
            }
            cpu.setFlag(CPU::ZF, (res & mask) == 0);
            cpu.setFlag(CPU::SF, (res & signBit) != 0);
            cpu.setFlag(CPU::PF, parity8((uint8_t)(res & 0xFF)));
        } else if (mn == "SAR") {
            if (cnt == 1) cpu.setFlag(CPU::OF, false);
            for (uint16_t i = 0; i < cnt; i++) {
                cpu.setFlag(CPU::CF, (res & 1) != 0);
                if (wide) res = (uint16_t)((int16_t)res >> 1);
                else res = (uint16_t)((uint8_t)((int8_t)(uint8_t)res >> 1));
            }
            res &= mask;
            cpu.setFlag(CPU::ZF, res == 0);
            cpu.setFlag(CPU::SF, (res & signBit) != 0);
            cpu.setFlag(CPU::PF, parity8((uint8_t)(res & 0xFF)));
        } else if (mn == "ROL") {
            for (uint16_t i = 0; i < cnt; i++) {
                bool msb = (res & signBit) != 0;
                res = (uint16_t)(((res << 1) | (msb ? 1 : 0)) & mask);
            }
            cpu.setFlag(CPU::CF, (res & 1) != 0);
            if (cnt == 1) cpu.setFlag(CPU::OF, (bool)(res & signBit) != cpu.getFlag(CPU::CF));
        } else if (mn == "ROR") {
            for (uint16_t i = 0; i < cnt; i++) {
                bool lsb = (res & 1) != 0;
                res = (uint16_t)((res >> 1) & mask);
                if (lsb) res |= signBit;
            }
            cpu.setFlag(CPU::CF, (res & signBit) != 0);
            if (cnt == 1) cpu.setFlag(CPU::OF, (bool)(res & signBit) != (bool)(res & (signBit >> 1)));
        } else if (mn == "RCL") {
            for (uint16_t i = 0; i < cnt; i++) {
                bool oldCF = cpu.getFlag(CPU::CF);
                cpu.setFlag(CPU::CF, (res & signBit) != 0);
                res = (uint16_t)(((res << 1) | (oldCF ? 1 : 0)) & mask);
            }
            if (cnt == 1) cpu.setFlag(CPU::OF, (bool)(res & signBit) != cpu.getFlag(CPU::CF));
        } else { // RCR
            for (uint16_t i = 0; i < cnt; i++) {
                bool oldCF = cpu.getFlag(CPU::CF);
                cpu.setFlag(CPU::CF, (res & 1) != 0);
                res = (uint16_t)((res >> 1) & mask);
                if (oldCF) res |= signBit;
            }
            if (cnt == 1) cpu.setFlag(CPU::OF, (bool)(res & signBit) != (bool)(res & (signBit >> 1)));
        }
        writeOperand(cpu, mem, inst.op1, (uint16_t)(res & mask), memDirty, inst.segOverride);
    }

    // --- MOV ---
    else if (mn == "MOV") {
        uint16_t val = readOperand(cpu, mem, inst.op2, inst.segOverride);
        writeOperand(cpu, mem, inst.op1, val, memDirty, inst.segOverride);
    }

    // --- XCHG ---
    else if (mn == "XCHG") {
        uint16_t a = readOperand(cpu, mem, inst.op1, inst.segOverride);
        uint16_t b = readOperand(cpu, mem, inst.op2, inst.segOverride);
        writeOperand(cpu, mem, inst.op1, b, memDirty, inst.segOverride);
        writeOperand(cpu, mem, inst.op2, a, memDirty, inst.segOverride);
    }

    // --- LEA ---
    else if (mn == "LEA") {
        uint16_t addr = calcEffectiveAddress(cpu, inst.op2);
        writeOperand(cpu, mem, inst.op1, addr, memDirty);
    }

    // --- PUSH ---
    else if (mn == "PUSH") {
        uint16_t val = readOperand(cpu, mem, inst.op1, inst.segOverride);
        cpu.regs[4] -= 2;  // SP -= 2
        mem.swrite16(cpu.sregs[2], cpu.regs[4], val);
        memDirty = true;
    }

    // --- POP ---
    else if (mn == "POP") {
        uint16_t val = mem.sread16(cpu.sregs[2], cpu.regs[4]);
        cpu.regs[4] += 2;  // SP += 2
        writeOperand(cpu, mem, inst.op1, val, memDirty, inst.segOverride);
    }

    // --- JMP ---
    else if (mn == "JMP") {
        if (inst.jumpTarget >= 0) {
            cpu.ip = (uint16_t)inst.jumpTarget;
        } else {
            // Indirect JMP through register/memory (FF /4)
            uint16_t target = readOperand(cpu, mem, inst.op1, inst.segOverride);
            cpu.ip = target;
        }
    }

    // --- CALL ---
    else if (mn == "CALL") {
        uint16_t nextIP = cpu.ip; // already advanced
        cpu.regs[4] -= 2;
        mem.swrite16(cpu.sregs[2], cpu.regs[4], nextIP);
        memDirty = true;
        if (inst.jumpTarget >= 0) {
            cpu.ip = (uint16_t)inst.jumpTarget;
        } else {
            uint16_t target = readOperand(cpu, mem, inst.op1, inst.segOverride);
            cpu.ip = target;
        }
    }

    // --- RET ---
    else if (mn == "RET") {
        cpu.ip = mem.sread16(cpu.sregs[2], cpu.regs[4]);
        cpu.regs[4] += 2;
    }

    // --- Conditional Jumps ---
    else if (mn.size() >= 2 && mn[0] == 'J' && mn != "JMP" && inst.jumpTarget >= 0) {
        if (evalCondition(cpu, mn)) {
            cpu.ip = (uint16_t)inst.jumpTarget;
        }
    }

    // --- LOOP / LOOPE / LOOPNE / JCXZ ---
    else if (mn == "LOOP" || mn == "LOOPE" || mn == "LOOPNE" || mn == "JCXZ") {
        if (mn == "JCXZ") {
            if (cpu.regs[1] == 0) cpu.ip = (uint16_t)inst.jumpTarget;
        } else {
            cpu.regs[1]--;  // CX--
            bool branch = false;
            if (mn == "LOOP") branch = (cpu.regs[1] != 0);
            else if (mn == "LOOPE") branch = (cpu.regs[1] != 0) && cpu.getFlag(CPU::ZF);
            else if (mn == "LOOPNE") branch = (cpu.regs[1] != 0) && !cpu.getFlag(CPU::ZF);
            if (branch) cpu.ip = (uint16_t)inst.jumpTarget;
        }
    }

    // --- String Operations ---
    else if (mn == "MOVSB" || mn == "MOVSW" || mn == "CMPSB" || mn == "CMPSW" ||
             mn == "STOSB" || mn == "STOSW" || mn == "LODSB" || mn == "LODSW" ||
             mn == "SCASB" || mn == "SCASW") {
        bool isWord = (mn.back() == 'W');
        int step = isWord ? 2 : 1;
        int dir = cpu.getFlag(CPU::DF) ? -step : step;
        bool hasRepPrefix = inst.hasRep || inst.hasRepne;
        bool isCompare = (mn.substr(0, 4) == "CMPS" || mn.substr(0, 4) == "SCAS");

        auto doOne = [&]() {
            // Source segment: DS by default, overridable by prefix
            uint16_t srcSeg = resolveSegment(cpu, inst.op1, inst.segOverride);
            // Destination segment: always ES, never overridable
            uint16_t dstSeg = cpu.sregs[0];  // ES

            if (mn.substr(0, 4) == "MOVS") {
                if (isWord) mem.swrite16(dstSeg, cpu.regs[7], mem.sread16(srcSeg, cpu.regs[6]));
                else        mem.swrite8(dstSeg, cpu.regs[7], mem.sread8(srcSeg, cpu.regs[6]));
                cpu.regs[6] += dir; cpu.regs[7] += dir;
                memDirty = true;
            } else if (mn.substr(0, 4) == "CMPS") {
                uint16_t a, b;
                if (isWord) { a = mem.sread16(srcSeg, cpu.regs[6]); b = mem.sread16(dstSeg, cpu.regs[7]); }
                else        { a = mem.sread8(srcSeg, cpu.regs[6]);   b = mem.sread8(dstSeg, cpu.regs[7]); }
                updateFlagsSub(cpu, (uint32_t)a - b, a, b, isWord);
                cpu.regs[6] += dir; cpu.regs[7] += dir;
            } else if (mn.substr(0, 4) == "STOS") {
                if (isWord) mem.swrite16(dstSeg, cpu.regs[7], cpu.regs[0]);
                else        mem.swrite8(dstSeg, cpu.regs[7], cpu.getReg8(0));
                cpu.regs[7] += dir;
                memDirty = true;
            } else if (mn.substr(0, 4) == "LODS") {
                if (isWord) cpu.regs[0] = mem.sread16(srcSeg, cpu.regs[6]);
                else        cpu.setReg8(0, mem.sread8(srcSeg, cpu.regs[6]));
                cpu.regs[6] += dir;
            } else if (mn.substr(0, 4) == "SCAS") {
                uint16_t a, b;
                if (isWord) { a = cpu.regs[0]; b = mem.sread16(dstSeg, cpu.regs[7]); }
                else        { a = cpu.getReg8(0); b = mem.sread8(dstSeg, cpu.regs[7]); }
                updateFlagsSub(cpu, (uint32_t)a - b, a, b, isWord);
                cpu.regs[7] += dir;
            }
        };

        if (hasRepPrefix) {
            while (cpu.regs[1] != 0) { // CX
                doOne();
                cpu.regs[1]--;
                if (isCompare) {
                    if (inst.hasRep && !cpu.getFlag(CPU::ZF)) break;   // REPE: stop if not equal
                    if (inst.hasRepne && cpu.getFlag(CPU::ZF)) break;  // REPNE: stop if equal
                }
            }
        } else {
            doOne();
        }
    }

    // --- Flag Operations ---
    else if (mn == "CLC") { cpu.setFlag(CPU::CF, false); }
    else if (mn == "STC") { cpu.setFlag(CPU::CF, true); }
    else if (mn == "CMC") { cpu.setFlag(CPU::CF, !cpu.getFlag(CPU::CF)); }
    else if (mn == "CLD") { cpu.setFlag(CPU::DF, false); }
    else if (mn == "STD") { cpu.setFlag(CPU::DF, true); }
    else if (mn == "CLI") { cpu.setFlag(CPU::IF_, false); }
    else if (mn == "STI") { cpu.setFlag(CPU::IF_, true); }

    // --- PUSHF / POPF ---
    else if (mn == "PUSHF") {
        cpu.regs[4] -= 2;
        mem.swrite16(cpu.sregs[2], cpu.regs[4], cpu.flags);
        memDirty = true;
    }
    else if (mn == "POPF") {
        cpu.flags = mem.sread16(cpu.sregs[2], cpu.regs[4]);
        cpu.regs[4] += 2;
    }

    // --- Misc ---
    else if (mn == "NOP") { /* nothing */ }
    else if (mn == "CBW") {
        int8_t al = (int8_t)cpu.getReg8(0);
        cpu.regs[0] = (uint16_t)(int16_t)al;
    }
    else if (mn == "CWD") {
        cpu.regs[2] = ((int16_t)cpu.regs[0] < 0) ? 0xFFFF : 0x0000;
    }
    else if (mn == "LAHF") {
        cpu.setReg8(4, (uint8_t)(cpu.flags & 0xFF)); // AH = low byte of flags
    }
    else if (mn == "SAHF") {
        cpu.flags = (cpu.flags & 0xFF00) | cpu.getReg8(4); // low byte of flags = AH
    }

    // --- XLAT ---
    else if (mn == "XLAT") {
        uint16_t addr = (uint16_t)(cpu.regs[3] + cpu.getReg8(0)); // BX + AL
        // XLAT uses DS:BX+AL
        cpu.setReg8(0, mem.sread8(cpu.sregs[3], addr));
    }

    // --- HLT ---
    else if (mn == "HLT") {
        result.halted = true;
        result.haltReason = "HLT instruction at " + hexImm16((uint16_t)(cpu.ip - inst.size));
    }

    // --- PUSHA (80186+) ---
    else if (mn == "PUSHA") {
        uint16_t origSP = cpu.regs[4];
        // Push order: AX, CX, DX, BX, SP(original), BP, SI, DI
        int order[] = {0, 1, 2, 3, 4, 5, 6, 7};
        for (int r : order) {
            cpu.regs[4] -= 2;
            if (r == 4) mem.swrite16(cpu.sregs[2], cpu.regs[4], origSP);
            else        mem.swrite16(cpu.sregs[2], cpu.regs[4], cpu.regs[r]);
        }
        memDirty = true;
    }

    // --- POPA (80186+) ---
    else if (mn == "POPA") {
        // Pop order: DI, SI, BP, (skip SP), BX, DX, CX, AX
        cpu.regs[7] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // DI
        cpu.regs[6] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // SI
        cpu.regs[5] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // BP
        cpu.regs[4] += 2;                                                          // skip SP
        cpu.regs[3] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // BX
        cpu.regs[2] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // DX
        cpu.regs[1] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // CX
        cpu.regs[0] = mem.sread16(cpu.sregs[2], cpu.regs[4]); cpu.regs[4] += 2; // AX
    }

    // --- INT ---
    else if (mn == "INT") {
        uint8_t intNum = (uint8_t)(inst.op1.disp & 0xFF);
        handleInterrupt(cpu, mem, io, result, intNum, vram, mouse, evState, code, config, cycle, dosFs, dosMem);
    }

    // --- IN / OUT ---
    else if (mn == "IN" || mn == "OUT") {
        result.skipped.push_back({ cpu.ip, formatInstruction(inst), "I/O not emulated", 1 });
    }

    // --- Unknown ---
    else {
        result.skipped.push_back({ cpu.ip, mn, "Unimplemented instruction", 1 });
    }
}

// --- Section 6: Breakpoints & Watchpoints ---

// Replaces dumpVRAMViewport
void captureViewport(const Memory& mem, const EmulatorConfig& config,
                     vector<string>& textOut, vector<string>& attrOut) {
    if (!config.hasViewport) return;

    // Viewport dimensions (clamped to 80x50 screen)
    int startRow = config.vpRow;
    int startCol = config.vpCol;
    int rows = config.vpHeight;
    int cols = config.vpWidth;

    for (int r = 0; r < rows; r++) {
        int screenRow = startRow + r;
        if (screenRow >= 50) break;

        string textLine;
        string attrLine;
        textLine.reserve(cols);

        for (int c = 0; c < cols; c++) {
            int screenCol = startCol + c;
            if (screenCol >= 80) break;

            int off = (screenRow * 80 + screenCol) * 2;
            uint8_t ch = mem.vram[off];
            uint8_t at = mem.vram[off + 1];

            // Replace non-printable with '.' for clean JSON
            textLine += (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';

            if (config.vpAttrs) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02X", at);
                attrLine += hex;
            }
        }

        textOut.push_back(textLine);
        if (config.vpAttrs) attrOut.push_back(attrLine);
    }
}

void captureSnapshot(const CPU& cpu, const Memory& mem,
                     const vector<uint8_t>& code,
                     int cycle, const string& reason,
                     const EmulatorConfig& config,
                     const VRAMState& vram,
                     vector<Snapshot>& snapshots) {
    Snapshot snap;
    // Limit snapshots to prevent massive output loops
    if (snapshots.size() >= 100) return;

    snap.addr = cpu.ip;
    snap.cycle = cycle;
    for(int i=0; i<8; i++) snap.regs[i] = cpu.regs[i];
    for(int i=0; i<4; i++) snap.sregs[i] = cpu.sregs[i];
    snap.ip = cpu.ip;
    snap.flags = cpu.flags;
    snap.reason = reason;
    snap.snapCursorRow = vram.cursorRow;
    snap.snapCursorCol = vram.cursorCol;

    // Decode next instruction for context
    DecodedInst inst = decodeInstruction(code, cpu.ip);
    if (inst.valid) snap.nextInst = formatInstruction(inst);
    else snap.nextInst = "???";

    // Capture stack (top 8 words, SS-relative)
    uint16_t sp = cpu.regs[4];
    uint16_t ss = cpu.sregs[2];
    for (int i=0; i<8; i++) {
        snap.stack.push_back(mem.sread16(ss, (uint16_t)(sp + i*2)));
    }

    // Optional memory dump
    if (config.memDumpLen > 0) {
        for(int i=0; i<config.memDumpLen; i++) {
             snap.memDump.push_back(mem.read8((uint16_t)(config.memDumpAddr + i)));
        }
    }

    // Optional viewport capture
    if (config.hasViewport) {
        captureViewport(mem, config, snap.screenLines, snap.screenAttrs);
    }

    snapshots.push_back(snap);
}

void checkBreakpoints(const CPU& cpu, const Memory& mem, const vector<uint8_t>& code,
                      EmulatorResult& result, const EmulatorConfig& config, int cycle,
                      const VRAMState& vram) {
    if (config.breakpoints.count(cpu.ip)) {
        // Hit limiting: full snapshot for first 10 hits per address, then just count
        int hits = 0;
        for (auto& s : result.snapshots) {
            if (s.addr == cpu.ip && s.reason.find("Breakpoint") != string::npos) hits++;
        }
        if (hits < 10) {
            captureSnapshot(cpu, mem, code, cycle, "Breakpoint at " + hexImm16(cpu.ip), config, vram, result.snapshots);
        } else {
            // Just increment the last matching snapshot's hitCount
            for (int i = (int)result.snapshots.size() - 1; i >= 0; i--) {
                if (result.snapshots[i].addr == cpu.ip) {
                    result.snapshots[i].hitCount++;
                    break;
                }
            }
        }
    }
}

// Check watchpoints: any register change
void checkWatchpoints(const CPU& cpu, uint16_t prevRegs[8], const EmulatorConfig& config,
                      const Memory& mem, const vector<uint8_t>& code,
                      EmulatorResult& result, int cycle, const VRAMState& vram) {
    for (int regIdx : config.watchRegs) {
        if (cpu.regs[regIdx] != prevRegs[regIdx]) {
             string regName = getRegName(regIdx, 16);
             string msg = "Watchpoint: " + regName + " changed from " +
                          hexImm16(prevRegs[regIdx]) + " to " + hexImm16(cpu.regs[regIdx]);
             captureSnapshot(cpu, mem, code, cycle, msg, config, vram, result.snapshots);
        }
    }
}

// --- Section 7: Main Loop ---

double computeFidelity(const EmulatorResult& result) {
    if (result.skipped.empty()) return 1.0;
    int totalSkips = 0;
    for (auto& s : result.skipped) totalSkips += s.count;
    double ratio = 1.0 - ((double)totalSkips / (double)(result.cyclesExecuted + 1));
    return ratio < 0 ? 0 : ratio;
}

void captureViewport(const Memory& mem, const EmulatorConfig& config,
                     vector<string>& textOut, vector<string>& attrOut); // Moved up
static bool writeScreenshot(const uint8_t vram[16000],
                            const string& filename, bool use8x8); // Forward decl

EmulatorResult runEmulator(const vector<uint8_t>& binary, const EmulatorConfig& config, CPU& cpuOut) {
    EmulatorResult result;
    CPU cpu;
    static Memory mem;  // static: 1MB+ too large for stack
    memset(&mem, 0, sizeof(mem)); // zero-initialize each call
    VRAMState vram;
    MouseState mouse;
    IOCapture io;
    io.stdinSource = config.stdinInput;

    // Init mouse
    if (config.mouseEnabled) {
        mouse.installed = true;
        mouse.x = (int16_t)config.mouseX;
        mouse.y = (int16_t)config.mouseY;
        mouse.buttons = config.mouseButtons;
        // Clamp to default range
        if (mouse.x < mouse.minX) mouse.x = mouse.minX;
        if (mouse.x > mouse.maxX) mouse.x = mouse.maxX;
        if (mouse.y < mouse.minY) mouse.y = mouse.minY;
        if (mouse.y > mouse.maxY) mouse.y = mouse.maxY;
        // Pre-populate press counts for buttons that are down
        for (int i = 0; i < 3; i++) {
            if (mouse.buttons & (1 << i)) mouse.pressCount[i] = 1;
        }
    }

    // Init events
    EventState evState;
    evState.events = config.events;

    // Init DOS memory state
    DOSMemoryState dosMem;

    // Init DOS file state
    DOSFileState dosFs;
    // Always pre-populate device handles (0-4) so AH=40h stdout works without --dos-root
    for (int h = 0; h < 5; h++) {
        DOSFileEntry dev;
        dev.isDevice = true;
        dev.mode = 2; // r/w
        dosFs.handles[h] = dev;
    }
    if (!config.dosRoot.empty()) {
        namespace fs = std::filesystem;
        try {
            dosFs.rootPath = fs::canonical(config.dosRoot).string();
            dosFs.enabled = true;
        } catch (const fs::filesystem_error&) {
            // rootPath not valid - dosFs.enabled stays false
        }
    }

    // Init CPU
    cpu.ip = 0x100;
    cpu.regs[4] = 0xFFFE;  // SP
    cpu.flags = 0x0202;     // IF set
    cpu.sregs[3] = 0;       // DS = 0

    // Init VRAM
    vram.clearScreen(mem);

    // Apply --vram-fill if requested
    if (config.vramFill) {
        const int totalCells = vram.cols * vram.rows;  // 80*50 = 4000
        if (config.vramFillText.empty()) {
            // Random fill with printable characters
            static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%&*+-=.:~ ";
            srand((unsigned)time(nullptr));
            for (int i = 0; i < totalCells; i++) {
                mem.vram[i * 2] = charset[rand() % (sizeof(charset) - 1)];
            }
        } else {
            // Tile the provided text across all character cells
            const string& txt = config.vramFillText;
            for (int i = 0; i < totalCells; i++) {
                mem.vram[i * 2] = (uint8_t)txt[i % txt.size()];
            }
        }
    }

    // Load binary and PSP INT 20h
    mem.loadCOM(binary);
    mem.write8(0x0000, 0xCD); // INT 20h at PSP:0000
    mem.write8(0x0001, 0x20);

    // Write command-line tail to PSP (0x80 = length, 0x81+ = tail + CR)
    if (!config.commandArgs.empty()) {
        string tail = " " + config.commandArgs;  // leading space per DOS convention
        if (tail.size() > 126) tail.resize(126); // max 126 chars + CR
        mem.write8(0x0080, (uint8_t)tail.size());
        for (size_t i = 0; i < tail.size(); i++)
            mem.write8(0x0081 + (uint16_t)i, (uint8_t)tail[i]);
        mem.write8(0x0081 + (uint16_t)tail.size(), 0x0D); // CR terminator
    } else {
        mem.write8(0x0080, 0);    // zero-length tail
        mem.write8(0x0081, 0x0D); // CR terminator
    }

    // Code vector for decoder
    vector<uint8_t> code(mem.data, mem.data + 65536);
    bool memDirty = false;

    int cycle = 0;
    while (cycle < config.maxCycles) {
        // Resync code on self-modifying writes
        if (memDirty && config.memDumpLen > 0) {
            // Re-capture if memory dump region changed?
            // This is expensive to check every cycle.
            // Simplified: we only capture on breakpoints/watchpoints instructions.
        }

        // Save previous register state for watchpoints
        uint16_t prevRegs[8];
        for (int i = 0; i < 8; i++) prevRegs[i] = cpu.regs[i];

        // Check breakpoints
        if (!config.breakpoints.empty()) {
            checkBreakpoints(cpu, mem, code, result, config, cycle, vram);
        }

        // Decode
        DecodedInst inst = decodeInstruction(code, cpu.ip);
        if (!inst.valid) {
            result.halted = true;
            result.haltReason = "Invalid opcode at " + hexImm16(cpu.ip);
            break;
        }

        // Advance IP before execution (branches will overwrite)
        cpu.ip = (uint16_t)(cpu.ip + inst.size);

        // Execute
        executeInstruction(cpu, mem, io, inst, result, code, memDirty, vram, mouse, evState, config, cycle, dosFs, dosMem);
        cycle++;

        // Process tick-based events
        evState.counters.tickCount = cycle;
        processEvents(evState, cycle, mouse, io, result, cpu, mem, code, config, vram);

        if (result.halted) break;

        // Check watchpoints
        if (!config.watchRegs.empty()) {
            checkWatchpoints(cpu, prevRegs, config, mem, code, result, cycle, vram);
        }
    }

    if (!result.halted && cycle >= config.maxCycles) {
        result.halted = true;
        result.haltReason = "Cycle limit reached (" + to_string(config.maxCycles) + ")";
    }

    // Populate event log
    result.eventsTotal = (int)evState.events.size();
    for (auto& ev : evState.events) {
        if (ev.fired) {
            result.eventsFired++;
            EmulatorResult::EventLogEntry entry;
            entry.on = ev.triggerType + ":" + to_string(ev.triggerCount);
            entry.cycle = ev.firedAtCycle;
            string acts;
            if (ev.hasMouse) acts += "mouse [" + to_string(ev.mouseX) + "," + to_string(ev.mouseY) + "," + to_string(ev.mouseButtons) + "]";
            if (ev.hasKeys) { if (!acts.empty()) acts += ", "; acts += "keys \"" + ev.keys + "\""; }
            if (ev.snapshot) { if (!acts.empty()) acts += ", "; acts += "snapshot"; }
            entry.action = acts;
            result.eventLog.push_back(entry);
        }
    }

    result.success = true;
    result.cyclesExecuted = cycle;
    result.output = io.stdoutBuf;
    result.fidelity = computeFidelity(result);
    // Capture viewport if requested
    if (config.hasViewport) {
        captureViewport(mem, config, result.screen, result.screenAttrs);
    }
    result.cursorRow = vram.cursorRow;
    result.cursorCol = vram.cursorCol;
    // Capture mouse state
    if (mouse.installed) {
        result.mouseEnabled = true;
        result.mouseX = mouse.x;
        result.mouseY = mouse.y;
        result.mouseButtons = mouse.buttons;
        result.mouseVisible = mouse.visible;
    }
    // Write screenshot if requested
    if (!config.screenshotFile.empty()) {
        if (writeScreenshot(mem.vram, config.screenshotFile, config.screenshotFont8x8)) {
            result.screenshotPath = config.screenshotFile;
        } else {
            result.diagnostics.push_back("Failed to write screenshot: " + config.screenshotFile);
        }
    }
    // Close any open DOS file handles and populate file I/O stats
    for (auto& kv : dosFs.handles) {
        if (!kv.second.isDevice && kv.second.fp) {
            fclose(kv.second.fp);
            kv.second.fp = nullptr;
        }
    }
    result.fileIO.filesOpened = dosFs.filesOpened;
    result.fileIO.filesClosed = dosFs.filesClosed;
    result.fileIO.bytesRead = dosFs.bytesRead;
    result.fileIO.bytesWritten = dosFs.bytesWritten;
    result.fileIO.dirSearches = dosFs.dirSearches;
    result.fileIO.filesDeleted = dosFs.filesDeleted;
    result.fileIO.filesRenamed = dosFs.filesRenamed;
    result.fileIO.errors = dosFs.errors;

    cpuOut = cpu;
    return result;
}

// --- Section 8: JSON Emitter ---

void emitEmulatorJSON(const EmulatorResult& result, const CPU& cpu) {
    cout << "{" << endl;
    cout << "  \"success\": " << (result.success ? "true" : "false") << "," << endl;
    cout << "  \"halted\": " << (result.halted ? "true" : "false") << "," << endl;
    cout << "  \"haltReason\": \"" << jsonEscape(result.haltReason) << "\"," << endl;
    cout << "  \"exitCode\": " << result.exitCode << "," << endl;
    cout << "  \"cyclesExecuted\": " << result.cyclesExecuted << "," << endl;
    cout << "  \"fidelity\": " << result.fidelity << "," << endl;
    cout << "  \"output\": \"" << jsonEscape(result.output) << "\"," << endl;

    // Hex-encoded output for binary-safe inspection
    cout << "  \"outputHex\": \"";
    for (unsigned char ch : result.output) {
        cout << hexByte(ch);
    }
    cout << "\"," << endl;

    // Final state
    cout << "  \"finalState\": {" << endl;
    static const string regNames[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    cout << "    \"registers\": {";
    for (int i = 0; i < 8; i++) {
        cout << "\"" << regNames[i] << "\": \"" << hexImm16(cpu.regs[i]) << "\"";
        if (i < 7) cout << ", ";
    }
    cout << "}," << endl;
    static const string sregNames[] = { "ES", "CS", "SS", "DS" };
    cout << "    \"sregs\": {";
    for (int i = 0; i < 4; i++) {
        cout << "\"" << sregNames[i] << "\": \"" << hexImm16(cpu.sregs[i]) << "\"";
        if (i < 3) cout << ", ";
    }
    cout << "}," << endl;
    cout << "    \"IP\": \"" << hexImm16(cpu.ip) << "\"," << endl;
    cout << "    \"flags\": \"" << hexImm16(cpu.flags) << "\"," << endl;
    cout << "    \"flagBits\": {";
    cout << "\"CF\": " << cpu.getFlag(CPU::CF) << ", ";
    cout << "\"PF\": " << cpu.getFlag(CPU::PF) << ", ";
    cout << "\"AF\": " << cpu.getFlag(CPU::AF) << ", ";
    cout << "\"ZF\": " << cpu.getFlag(CPU::ZF) << ", ";
    cout << "\"SF\": " << cpu.getFlag(CPU::SF) << ", ";
    cout << "\"OF\": " << cpu.getFlag(CPU::OF) << ", ";
    cout << "\"DF\": " << cpu.getFlag(CPU::DF) << ", ";
    cout << "\"IF\": " << cpu.getFlag(CPU::IF_);
    cout << "}" << "," << endl;
    cout << "    \"cursor\": {\"row\": " << result.cursorRow
         << ", \"col\": " << result.cursorCol << "}";
    if (result.mouseEnabled) {
        cout << "," << endl;
        cout << "    \"mouse\": {\"x\": " << result.mouseX
             << ", \"y\": " << result.mouseY
             << ", \"buttons\": " << result.mouseButtons
             << ", \"visible\": " << (result.mouseVisible ? "true" : "false") << "}";
    }
    cout << endl;
    cout << "  }," << endl;

    // Snapshots
    cout << "  \"snapshots\": [" << endl;
    for (size_t i = 0; i < result.snapshots.size(); i++) {
        const auto& s = result.snapshots[i];
        cout << "    {" << endl;
        cout << "      \"addr\": \"" << hexImm16(s.addr) << "\"," << endl;
        cout << "      \"cycle\": " << s.cycle << "," << endl;
        cout << "      \"reason\": \"" << jsonEscape(s.reason) << "\"," << endl;
        cout << "      \"nextInst\": \"" << jsonEscape(s.nextInst) << "\"," << endl;
        cout << "      \"hitCount\": " << s.hitCount << "," << endl;
        cout << "      \"registers\": {";
        for (int r = 0; r < 8; r++) {
            cout << "\"" << regNames[r] << "\": \"" << hexImm16(s.regs[r]) << "\"";
            if (r < 7) cout << ", ";
        }
        cout << "}," << endl;
        cout << "      \"flags\": \"" << hexImm16(s.flags) << "\"," << endl;
        cout << "      \"cursor\": {\"row\": " << s.snapCursorRow
             << ", \"col\": " << s.snapCursorCol << "}," << endl;
        cout << "      \"stack\": [";
        for (size_t k = 0; k < s.stack.size(); k++) {
            cout << "\"" << hexImm16(s.stack[k]) << "\"";
            if (k < s.stack.size() - 1) cout << ", ";
        }
        cout << "]";
        if (!s.memDump.empty()) {
            cout << "," << endl << "      \"memDump\": \"";
            for (auto b : s.memDump) cout << hexByte(b);
            cout << "\"";
        }
        if (!s.screenLines.empty()) {
            cout << "," << endl << "      \"screen\": [";
            for (size_t k = 0; k < s.screenLines.size(); k++) {
                cout << "\"" << jsonEscape(s.screenLines[k]) << "\"";
                if (k < s.screenLines.size() - 1) cout << ", ";
            }
            cout << "]";
            if (!s.screenAttrs.empty()) {
                cout << "," << endl << "      \"screenAttrs\": [";
                for (size_t k = 0; k < s.screenAttrs.size(); k++) {
                    cout << "\"" << s.screenAttrs[k] << "\"";
                    if (k < s.screenAttrs.size() - 1) cout << ", ";
                }
                cout << "]";
            }
        }
        cout << endl << "    }";
        if (i < result.snapshots.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]," << endl;

    // Skipped
    cout << "  \"skipped\": [" << endl;
    for (size_t i = 0; i < result.skipped.size(); i++) {
        const auto& s = result.skipped[i];
        cout << "    {\"addr\": \"" << hexImm16(s.addr) << "\", \"instruction\": \"" << jsonEscape(s.instruction)
             << "\", \"reason\": \"" << jsonEscape(s.reason) << "\", \"count\": " << s.count << "}";
        if (i < result.skipped.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]," << endl;

    // Diagnostics
    cout << "  \"diagnostics\": [" << endl;
    for (size_t i = 0; i < result.diagnostics.size(); i++) {
        cout << "    \"" << jsonEscape(result.diagnostics[i]) << "\"";
        if (i < result.diagnostics.size() - 1) cout << ",";
        cout << endl;
    }
    cout << "  ]";

    // Events (conditional)
    if (result.eventsTotal > 0) {
        cout << "," << endl;
        cout << "  \"events\": {" << endl;
        cout << "    \"total\": " << result.eventsTotal << "," << endl;
        cout << "    \"fired\": " << result.eventsFired << "," << endl;
        cout << "    \"pending\": " << (result.eventsTotal - result.eventsFired) << "," << endl;
        cout << "    \"log\": [" << endl;
        for (size_t i = 0; i < result.eventLog.size(); i++) {
            const auto& e = result.eventLog[i];
            cout << "      {\"on\": \"" << jsonEscape(e.on) << "\", \"cycle\": " << e.cycle
                 << ", \"action\": \"" << jsonEscape(e.action) << "\"}";
            if (i < result.eventLog.size() - 1) cout << ",";
            cout << endl;
        }
        cout << "    ]" << endl;
        cout << "  }";
    }

    // File I/O stats (conditional)
    if (result.fileIO.filesOpened > 0 || result.fileIO.dirSearches > 0 || result.fileIO.filesDeleted > 0 || result.fileIO.filesRenamed > 0) {
        cout << "," << endl;
        cout << "  \"fileIO\": {" << endl;
        cout << "    \"filesOpened\": " << result.fileIO.filesOpened << "," << endl;
        cout << "    \"filesClosed\": " << result.fileIO.filesClosed << "," << endl;
        cout << "    \"bytesRead\": " << result.fileIO.bytesRead << "," << endl;
        cout << "    \"bytesWritten\": " << result.fileIO.bytesWritten << "," << endl;
        cout << "    \"dirSearches\": " << result.fileIO.dirSearches << "," << endl;
        cout << "    \"filesDeleted\": " << result.fileIO.filesDeleted << "," << endl;
        cout << "    \"filesRenamed\": " << result.fileIO.filesRenamed << "," << endl;
        cout << "    \"errors\": " << result.fileIO.errors << endl;
        cout << "  }";
    }

    // Screen (conditional)
    if (!result.screen.empty()) {
        cout << "," << endl;
        cout << "  \"screen\": [" << endl;
        for (size_t i = 0; i < result.screen.size(); i++) {
            cout << "    \"" << jsonEscape(result.screen[i]) << "\"";
            if (i < result.screen.size() - 1) cout << ",";
            cout << endl;
        }
        cout << "  ]";

        if (!result.screenAttrs.empty()) {
            cout << "," << endl;
            cout << "  \"screenAttrs\": [" << endl;
            for (size_t i = 0; i < result.screenAttrs.size(); i++) {
                cout << "    \"" << jsonEscape(result.screenAttrs[i]) << "\"";
                if (i < result.screenAttrs.size() - 1) cout << ",";
                cout << endl;
            }
            cout << "  ]" << endl;
        }
    }
    if (!result.screenshotPath.empty()) {
        cout << "," << endl;
        cout << "  \"screenshot\": \"" << jsonEscape(result.screenshotPath) << "\"";
    }
    cout << endl << "}" << endl;
}

void emitCombinedJSON(AssemblerContext& asmCtx, const EmulatorResult& emuResult, const CPU& cpu, const vector<SourceLocation>& sourceMap = {}) {
    cout << "{" << endl;

    // Assembly section
    cout << "  \"assembly\": ";
    // Redirect assembly output inline
    cout << "{" << endl;
    cout << "    \"success\": " << (!asmCtx.globalError ? "true" : "false") << "," << endl;
    cout << "    \"size\": " << asmCtx.machineCode.size() << "," << endl;
    cout << "    \"diagnostics\": [";
    for (size_t i = 0; i < asmCtx.agentState.diagnostics.size(); i++) {
        const auto& d = asmCtx.agentState.diagnostics[i];
        cout << "{\"level\": \"" << d.level << "\", \"line\": " << d.line;
        if (!sourceMap.empty() && d.line > 0 && d.line <= (int)sourceMap.size()) {
            const auto& loc = sourceMap[d.line - 1];
            cout << ", \"file\": \"" << jsonEscape(loc.file) << "\", \"sourceLine\": " << loc.line;
        }
        cout << ", \"message\": \"" << jsonEscape(d.message) << "\"";
        if (!d.hint.empty()) cout << ", \"hint\": \"" << jsonEscape(d.hint) << "\"";
        cout << "}";
        if (i < asmCtx.agentState.diagnostics.size() - 1) cout << ",";
    }
    cout << "]" << endl;
    cout << "  }," << endl;

    // Emulation section
    cout << "  \"emulation\": {" << endl;
    cout << "    \"success\": " << (emuResult.success ? "true" : "false") << "," << endl;
    cout << "    \"halted\": " << (emuResult.halted ? "true" : "false") << "," << endl;
    cout << "    \"haltReason\": \"" << jsonEscape(emuResult.haltReason) << "\"," << endl;
    cout << "    \"exitCode\": " << emuResult.exitCode << "," << endl;
    cout << "    \"cyclesExecuted\": " << emuResult.cyclesExecuted << "," << endl;
    cout << "    \"fidelity\": " << emuResult.fidelity << "," << endl;
    cout << "    \"output\": \"" << jsonEscape(emuResult.output) << "\"," << endl;

    // Hex-encoded output for binary-safe inspection
    cout << "    \"outputHex\": \"";
    for (unsigned char ch : emuResult.output) {
        cout << hexByte(ch);
    }
    cout << "\"," << endl;

    // Final state
    static const string regNames[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    cout << "    \"finalState\": {" << endl;
    cout << "      \"registers\": {";
    for (int i = 0; i < 8; i++) {
        cout << "\"" << regNames[i] << "\": \"" << hexImm16(cpu.regs[i]) << "\"";
        if (i < 7) cout << ", ";
    }
    cout << "}," << endl;
    cout << "      \"IP\": \"" << hexImm16(cpu.ip) << "\"," << endl;
    cout << "      \"flags\": \"" << hexImm16(cpu.flags) << "\"," << endl;
    cout << "      \"cursor\": {\"row\": " << emuResult.cursorRow
         << ", \"col\": " << emuResult.cursorCol << "}";
    if (emuResult.mouseEnabled) {
        cout << "," << endl;
        cout << "      \"mouse\": {\"x\": " << emuResult.mouseX
             << ", \"y\": " << emuResult.mouseY
             << ", \"buttons\": " << emuResult.mouseButtons
             << ", \"visible\": " << (emuResult.mouseVisible ? "true" : "false") << "}";
    }
    cout << endl;
    cout << "    }," << endl;

    // Skipped
    cout << "    \"skipped\": [";
    for (size_t i = 0; i < emuResult.skipped.size(); i++) {
        const auto& s = emuResult.skipped[i];
        cout << "{\"instruction\": \"" << jsonEscape(s.instruction) << "\", \"reason\": \"" << jsonEscape(s.reason) << "\"}";
        if (i < emuResult.skipped.size() - 1) cout << ",";
    }
    cout << "]" << endl;

    // Events (conditional)
    if (emuResult.eventsTotal > 0) {
        cout << "," << endl;
        cout << "    \"events\": {" << endl;
        cout << "      \"total\": " << emuResult.eventsTotal << "," << endl;
        cout << "      \"fired\": " << emuResult.eventsFired << "," << endl;
        cout << "      \"pending\": " << (emuResult.eventsTotal - emuResult.eventsFired) << "," << endl;
        cout << "      \"log\": [" << endl;
        for (size_t i = 0; i < emuResult.eventLog.size(); i++) {
            const auto& e = emuResult.eventLog[i];
            cout << "        {\"on\": \"" << jsonEscape(e.on) << "\", \"cycle\": " << e.cycle
                 << ", \"action\": \"" << jsonEscape(e.action) << "\"}";
            if (i < emuResult.eventLog.size() - 1) cout << ",";
            cout << endl;
        }
        cout << "      ]" << endl;
        cout << "    }";
    }

    // File I/O stats (conditional)
    if (emuResult.fileIO.filesOpened > 0 || emuResult.fileIO.dirSearches > 0 || emuResult.fileIO.filesDeleted > 0 || emuResult.fileIO.filesRenamed > 0) {
        cout << "," << endl;
        cout << "    \"fileIO\": {" << endl;
        cout << "      \"filesOpened\": " << emuResult.fileIO.filesOpened << "," << endl;
        cout << "      \"filesClosed\": " << emuResult.fileIO.filesClosed << "," << endl;
        cout << "      \"bytesRead\": " << emuResult.fileIO.bytesRead << "," << endl;
        cout << "      \"bytesWritten\": " << emuResult.fileIO.bytesWritten << "," << endl;
        cout << "      \"dirSearches\": " << emuResult.fileIO.dirSearches << "," << endl;
        cout << "      \"filesDeleted\": " << emuResult.fileIO.filesDeleted << "," << endl;
        cout << "      \"filesRenamed\": " << emuResult.fileIO.filesRenamed << "," << endl;
        cout << "      \"errors\": " << emuResult.fileIO.errors << endl;
        cout << "    }";
    }

    // Screen (for Combined JSON)
    if (!emuResult.screen.empty()) {
        cout << "," << endl;
        cout << "    \"screen\": [" << endl;
        for (size_t i = 0; i < emuResult.screen.size(); i++) {
            cout << "      \"" << jsonEscape(emuResult.screen[i]) << "\"";
            if (i < emuResult.screen.size() - 1) cout << ",";
            cout << endl;
        }
        cout << "    ]";

        if (!emuResult.screenAttrs.empty()) {
            cout << "," << endl;
            cout << "    \"screenAttrs\": [" << endl;
            for (size_t i = 0; i < emuResult.screenAttrs.size(); i++) {
                cout << "      \"" << jsonEscape(emuResult.screenAttrs[i]) << "\"";
                if (i < emuResult.screenAttrs.size() - 1) cout << ",";
                cout << endl;
            }
            cout << "    ]" << endl;
        }
    }
    if (!emuResult.screenshotPath.empty()) {
        cout << "," << endl;
        cout << "    \"screenshot\": \"" << jsonEscape(emuResult.screenshotPath) << "\"";
    }
    cout << "  }" << endl;
    cout << "}" << endl;
}

// ============================================================
// SCREENSHOT RENDERING — PNG / JPG / BMP OUTPUT
// ============================================================

static const uint8_t cgaPalette[16][3] = {
    {0x00,0x00,0x00}, {0x00,0x00,0xAA}, {0x00,0xAA,0x00}, {0x00,0xAA,0xAA},
    {0xAA,0x00,0x00}, {0xAA,0x00,0xAA}, {0xAA,0x55,0x00}, {0xAA,0xAA,0xAA},
    {0x55,0x55,0x55}, {0x55,0x55,0xFF}, {0x55,0xFF,0x55}, {0x55,0xFF,0xFF},
    {0xFF,0x55,0x55}, {0xFF,0x55,0xFF}, {0xFF,0xFF,0x55}, {0xFF,0xFF,0xFF},
};

#include "cp437font.h"

static string getExtLower(const string& filename) {
    auto dot = filename.rfind('.');
    if (dot == string::npos) return "";
    string ext = filename.substr(dot);
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static bool writeScreenshot(const uint8_t vram[16000],
                            const string& filename, bool use8x8) {
    const uint8_t* font = use8x8 ? cp437_8x8 : cp437_8x16;
    const int GLYPH_H = use8x8 ? 8 : 16;
    const int IMG_W = 640;
    const int IMG_H = (use8x8 ? 400 : 800);

    // Render VRAM to top-down RGB pixel buffer
    vector<uint8_t> rgb(IMG_W * IMG_H * 3);
    for (int row = 0; row < 50; row++) {
        if (row * GLYPH_H >= IMG_H) break;
        for (int col = 0; col < 80; col++) {
            int idx = (row * 80 + col) * 2;
            uint8_t ch   = vram[idx];
            uint8_t attr = vram[idx + 1];
            const uint8_t* fg = cgaPalette[attr & 0x0F];
            const uint8_t* bg = cgaPalette[(attr >> 4) & 0x0F];
            const uint8_t* glyph = font + ch * GLYPH_H;

            for (int gy = 0; gy < GLYPH_H; gy++) {
                uint8_t bits = glyph[gy];
                int pixY = row * GLYPH_H + gy;
                int baseX = col * 8;
                for (int gx = 0; gx < 8; gx++) {
                    const uint8_t* color = (bits >> (7 - gx)) & 1 ? fg : bg;
                    int off = (pixY * IMG_W + baseX + gx) * 3;
                    rgb[off]     = color[0]; // R
                    rgb[off + 1] = color[1]; // G
                    rgb[off + 2] = color[2]; // B
                }
            }
        }
    }

    string ext = getExtLower(filename);

    if (ext == ".png") {
        return stbi_write_png(filename.c_str(), IMG_W, IMG_H, 3,
                              rgb.data(), IMG_W * 3) != 0;
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return stbi_write_jpg(filename.c_str(), IMG_W, IMG_H, 3,
                              rgb.data(), 90) != 0;
    }

    // Default: BMP (bottom-up BGR format)
    const int rowStride = IMG_W * 3;
    const int pixelDataSize = rowStride * IMG_H;
    const int fileSize = 54 + pixelDataSize;
    vector<uint8_t> bmp(fileSize);

    // BMP file header (14 bytes)
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = fileSize & 0xFF; bmp[3] = (fileSize >> 8) & 0xFF;
    bmp[4] = (fileSize >> 16) & 0xFF; bmp[5] = (fileSize >> 24) & 0xFF;
    bmp[10] = 54;

    // DIB header (40 bytes)
    bmp[14] = 40;
    bmp[18] = IMG_W & 0xFF; bmp[19] = (IMG_W >> 8) & 0xFF;
    bmp[22] = IMG_H & 0xFF; bmp[23] = (IMG_H >> 8) & 0xFF;
    bmp[26] = 1;   // color planes
    bmp[28] = 24;  // bits per pixel

    // Convert top-down RGB to bottom-up BGR
    for (int y = 0; y < IMG_H; y++) {
        int srcRow = y;
        int dstRow = IMG_H - 1 - y;
        for (int x = 0; x < IMG_W; x++) {
            int si = (srcRow * IMG_W + x) * 3;
            int di = 54 + dstRow * rowStride + x * 3;
            bmp[di]     = rgb[si + 2]; // B
            bmp[di + 1] = rgb[si + 1]; // G
            bmp[di + 2] = rgb[si];     // R
        }
    }

    ofstream out(filename, ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    return out.good();
}

// ============================================================
// INCLUDE DIRECTIVE — PRE-EXPANSION
// ============================================================

static string getDirectory(const string& filepath) {
    namespace fs = std::filesystem;
    fs::path p(filepath);
    fs::path dir = p.parent_path();
    return dir.empty() ? "." : dir.string();
}

static string resolvePath(const string& baseDir, const string& includePath) {
    namespace fs = std::filesystem;
    fs::path inc(includePath);
    if (inc.is_absolute()) return inc.string();
    return (fs::path(baseDir) / inc).string();
}

static bool expandIncludesRecursive(
    const string& filepath,
    const string& baseDir,
    vector<string>& outLines,
    vector<SourceLocation>& outSourceMap,
    vector<Diagnostic>& outErrors,
    set<string>& includeStack,
    int depth)
{
    namespace fs = std::filesystem;

    if (depth > MAX_INCLUDE_DEPTH) {
        outErrors.push_back({"ERROR", 0, "Include nesting depth exceeded (" + to_string(MAX_INCLUDE_DEPTH) + ")",
            "Check for deeply nested or recursive INCLUDE chains"});
        return false;
    }

    string resolvedPath = resolvePath(baseDir, filepath);

    // Canonicalize for circular detection
    string canonical;
    try {
        canonical = fs::canonical(resolvedPath).string();
    } catch (...) {
        // File doesn't exist — report error
        outErrors.push_back({"ERROR", 0,
            "Cannot open include file: " + resolvedPath,
            "Resolved from: " + filepath + " relative to " + baseDir});
        return false;
    }

    if (includeStack.count(canonical)) {
        outErrors.push_back({"ERROR", 0,
            "Circular include detected: " + filepath,
            "File already in include chain: " + canonical});
        return false;
    }

    ifstream in(resolvedPath);
    if (!in) {
        outErrors.push_back({"ERROR", 0,
            "Cannot open include file: " + resolvedPath,
            "Resolved from: " + filepath + " relative to " + baseDir});
        return false;
    }

    vector<string> fileLines;
    string line;
    while (getline(in, line)) fileLines.push_back(line);
    in.close();

    includeStack.insert(canonical);
    string fileDir = getDirectory(resolvedPath);

    bool ok = true;
    for (int i = 0; i < (int)fileLines.size(); ++i) {
        const string& raw = fileLines[i];

        // Check if this line is an INCLUDE directive
        // Find first non-whitespace
        size_t pos = 0;
        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) pos++;

        // Check for INCLUDE keyword (case-insensitive)
        bool isInclude = false;
        if (pos + 7 <= raw.size()) {
            string keyword = raw.substr(pos, 7);
            for (auto& c : keyword) c = toupper((unsigned char)c);
            if (keyword == "INCLUDE" && (pos + 7 == raw.size() || raw[pos+7] == ' ' || raw[pos+7] == '\t' || raw[pos+7] == '\'' || raw[pos+7] == '"')) {
                isInclude = true;
            }
        }

        if (!isInclude) {
            outLines.push_back(raw);
            outSourceMap.push_back({resolvedPath, i + 1});
            continue;
        }

        // Parse the include filename
        size_t fnStart = pos + 7;
        while (fnStart < raw.size() && (raw[fnStart] == ' ' || raw[fnStart] == '\t')) fnStart++;

        if (fnStart >= raw.size()) {
            outErrors.push_back({"ERROR", (int)outLines.size() + 1,
                "INCLUDE directive missing filename",
                "Usage: INCLUDE 'file.asm' or INCLUDE \"file.asm\" or INCLUDE file.asm"});
            outLines.push_back("; ERROR: INCLUDE missing filename");
            outSourceMap.push_back({resolvedPath, i + 1});
            ok = false;
            continue;
        }

        string incFile;
        if (raw[fnStart] == '\'' || raw[fnStart] == '"') {
            char quote = raw[fnStart];
            size_t fnEnd = raw.find(quote, fnStart + 1);
            if (fnEnd == string::npos) {
                outErrors.push_back({"ERROR", (int)outLines.size() + 1,
                    "Unterminated string in INCLUDE directive",
                    "Expected closing " + string(1, quote) + " in: " + raw});
                outLines.push_back("; ERROR: Unterminated INCLUDE string");
                outSourceMap.push_back({resolvedPath, i + 1});
                ok = false;
                continue;
            }
            incFile = raw.substr(fnStart + 1, fnEnd - fnStart - 1);
        } else {
            // Bare filename — up to first whitespace or semicolon
            size_t fnEnd = fnStart;
            while (fnEnd < raw.size() && raw[fnEnd] != ' ' && raw[fnEnd] != '\t' && raw[fnEnd] != ';') fnEnd++;
            incFile = raw.substr(fnStart, fnEnd - fnStart);
        }

        if (incFile.empty()) {
            outErrors.push_back({"ERROR", (int)outLines.size() + 1,
                "INCLUDE directive missing filename",
                "Usage: INCLUDE 'file.asm' or INCLUDE \"file.asm\" or INCLUDE file.asm"});
            outLines.push_back("; ERROR: INCLUDE missing filename");
            outSourceMap.push_back({resolvedPath, i + 1});
            ok = false;
            continue;
        }

        // Replace INCLUDE line with marker comment
        outLines.push_back("; >>> INCLUDE " + incFile);
        outSourceMap.push_back({resolvedPath, i + 1});

        // Recurse
        if (!expandIncludesRecursive(incFile, fileDir, outLines, outSourceMap, outErrors, includeStack, depth + 1)) {
            ok = false;
        }

        outLines.push_back("; <<< END INCLUDE " + incFile);
        outSourceMap.push_back({resolvedPath, i + 1});
    }

    includeStack.erase(canonical);
    return ok;
}

static bool expandIncludes(
    const string& filename,
    vector<string>& outLines,
    vector<SourceLocation>& outSourceMap,
    vector<Diagnostic>& outErrors)
{
    set<string> includeStack;
    string baseDir = getDirectory(filename);
    namespace fs = std::filesystem;
    // Use just the filename part to avoid double-joining with baseDir
    string fname = fs::path(filename).filename().string();
    return expandIncludesRecursive(fname, baseDir, outLines, outSourceMap, outErrors, includeStack, 0);
}

// ============================================================
// MACRO PREPROCESSOR
// ============================================================

struct MacroDefinition {
    string name;                // upper-cased macro name
    vector<string> params;      // parameter names, upper-cased
    vector<string> locals;      // LOCAL label names, upper-cased
    vector<string> body;        // raw body lines (excluding LOCAL lines)
    int definedAtLine;          // line index (0-based) for diagnostics
};

using MacroTable = map<string, MacroDefinition>;

static const int MAX_MACRO_EXPANSION_ITERATIONS = 10000;

// --- Macro helper: trim whitespace ---
static string macroTrim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// --- Macro helper: split a line into up to 3 parts ---
// Returns {token1, token2, rest} where tokens are whitespace-delimited.
// Respects ; comments and '...' strings in the rest portion.
struct LineParts {
    string tok1, tok2, rest;
};

static LineParts splitMacroLine(const string& line) {
    LineParts lp;
    size_t i = 0;
    size_t len = line.size();

    // Skip leading whitespace
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    // If comment or empty, return empty
    if (i >= len || line[i] == ';') return lp;

    // Token 1
    size_t start = i;
    // Handle label with colon
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != ';') {
        if (line[i] == '\'') {
            i++;
            while (i < len && line[i] != '\'') i++;
            if (i < len) i++;
        } else {
            i++;
        }
    }
    lp.tok1 = line.substr(start, i - start);

    // Skip whitespace
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] == ';') return lp;

    // Token 2
    start = i;
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != ';') {
        if (line[i] == '\'') {
            i++;
            while (i < len && line[i] != '\'') i++;
            if (i < len) i++;
        } else {
            i++;
        }
    }
    lp.tok2 = line.substr(start, i - start);

    // Skip whitespace
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    // Rest (up to comment)
    if (i < len && line[i] != ';') {
        lp.rest = line.substr(i);
        // Strip trailing comment (respecting strings)
        string cleaned;
        bool inStr = false;
        for (size_t j = 0; j < lp.rest.size(); j++) {
            if (lp.rest[j] == '\'' && !inStr) { inStr = true; cleaned += lp.rest[j]; }
            else if (lp.rest[j] == '\'' && inStr) { inStr = false; cleaned += lp.rest[j]; }
            else if (lp.rest[j] == ';' && !inStr) { break; }
            else { cleaned += lp.rest[j]; }
        }
        // Trim trailing whitespace from cleaned rest
        size_t e = cleaned.find_last_not_of(" \t\r\n");
        lp.rest = (e == string::npos) ? "" : cleaned.substr(0, e + 1);
    }

    return lp;
}

// --- Macro helper: parse comma-separated identifiers ---
static vector<string> parseCommaSeparatedIdents(const string& s) {
    vector<string> result;
    string current;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ';') break;
        if (s[i] == ',') {
            string t = macroTrim(current);
            if (!t.empty()) result.push_back(t);
            current.clear();
        } else {
            current += s[i];
        }
    }
    string t = macroTrim(current);
    if (!t.empty()) result.push_back(t);
    return result;
}

// --- Macro helper: parse a simple numeric literal ---
// Supports decimal, hex (h suffix or 0x prefix), binary (b suffix or 0b prefix), octal (o/q suffix)
static int parseSimpleNumber(const string& s, bool& ok) {
    ok = false;
    if (s.empty()) return 0;
    string u = toUpper(s);

    int base = 10;
    string digits = u;

    char suffix = digits.back();
    if (suffix == 'H') {
        base = 16; digits.pop_back();
    } else if (suffix == 'B') {
        base = 2; digits.pop_back();
    } else if (suffix == 'O' || suffix == 'Q') {
        base = 8; digits.pop_back();
    } else if (suffix == 'D') {
        base = 10; digits.pop_back();
    } else if (digits.size() > 2 && digits.substr(0, 2) == "0X") {
        base = 16; digits = digits.substr(2);
    } else if (digits.size() > 2 && digits.substr(0, 2) == "0B") {
        base = 2; digits = digits.substr(2);
    }

    if (digits.empty()) return 0;

    for (char c : digits) {
        if (base == 2 && c != '0' && c != '1') return 0;
        if (base == 8 && (c < '0' || c > '7')) return 0;
        if (base == 10 && !isdigit(c)) return 0;
        if (base == 16 && !isxdigit(c)) return 0;
    }

    try {
        size_t pos = 0;
        long long val = stoll(digits, &pos, base);
        if (pos != digits.size()) return 0;
        if (val < 0 || val > 1000000) return 0;  // reasonable limit for REPT
        ok = true;
        return (int)val;
    } catch (...) {
        return 0;
    }
}

// --- Macro helper: check if a word is a reserved instruction/register/directive ---
static bool isMacroReservedWord(const string& upper) {
    static const set<string> reserved = {
        // Registers
        "AX", "BX", "CX", "DX", "SP", "BP", "SI", "DI",
        "AL", "AH", "BL", "BH", "CL", "CH", "DL", "DH",
        "CS", "DS", "ES", "SS", "IP",
        // Instructions (common subset)
        "MOV", "ADD", "SUB", "MUL", "DIV", "IMUL", "IDIV",
        "INC", "DEC", "NEG", "NOT",
        "AND", "OR", "XOR", "TEST", "CMP",
        "PUSH", "POP", "PUSHF", "POPF",
        "JMP", "JE", "JNE", "JZ", "JNZ", "JG", "JGE", "JL", "JLE",
        "JA", "JAE", "JB", "JBE", "JC", "JNC", "JO", "JNO", "JS", "JNS",
        "JCXZ", "LOOP", "LOOPE", "LOOPNE", "LOOPZ", "LOOPNZ",
        "CALL", "RET", "RETF", "INT", "IRET", "INTO",
        "NOP", "HLT", "CLC", "STC", "CMC", "CLD", "STD", "CLI", "STI",
        "SHL", "SHR", "SAL", "SAR", "ROL", "ROR", "RCL", "RCR",
        "LEA", "LDS", "LES", "XCHG", "XLAT", "XLATB",
        "CBW", "CWD", "AAA", "AAD", "AAM", "AAS", "DAA", "DAS",
        "IN", "OUT", "INS", "OUTS", "INSB", "INSW", "OUTSB", "OUTSW",
        "MOVSB", "MOVSW", "CMPSB", "CMPSW", "SCASB", "SCASW",
        "LODSB", "LODSW", "STOSB", "STOSW",
        "REP", "REPE", "REPNE", "REPZ", "REPNZ",
        "LOCK", "WAIT", "ESC",
        "LAHF", "SAHF",
        // Directives
        "ORG", "DB", "DW", "EQU", "PROC", "ENDP", "SEGMENT", "ENDS",
        "ASSUME", "END", "INCLUDE",
        "MACRO", "ENDM", "LOCAL", "REPT", "IRP",
        "STRUC", "STRUCT",
        // Size specifiers
        "BYTE", "WORD", "PTR", "OFFSET", "SHORT", "NEAR", "FAR",
        "DUP",
    };
    return reserved.count(upper) > 0;
}

// --- Macro helper: substitute parameters in a body line ---
static string substituteParams(
    const string& line,
    const vector<string>& paramNames,  // upper-cased
    const vector<string>& argValues,
    const vector<string>& localNames,  // upper-cased
    const vector<string>& localReplacements)
{
    string result;
    size_t i = 0;
    size_t len = line.size();
    bool inString = false;
    bool inComment = false;

    while (i < len) {
        if (inComment) {
            result += line[i++];
            continue;
        }

        if (line[i] == ';' && !inString) {
            inComment = true;
            result += line[i++];
            continue;
        }

        if (line[i] == '\'') {
            inString = !inString;
            result += line[i++];
            continue;
        }

        if (inString) {
            result += line[i++];
            continue;
        }

        // & concatenation operator - consume it
        if (line[i] == '&') {
            i++;
            continue;
        }

        // Identifier characters
        if (isalnum(line[i]) || line[i] == '_' || line[i] == '?' || line[i] == '.') {
            size_t start = i;
            while (i < len && (isalnum(line[i]) || line[i] == '_' || line[i] == '?' || line[i] == '.')) i++;
            string word = line.substr(start, i - start);
            string upper = toUpper(word);

            // Check parameters
            bool replaced = false;
            for (size_t p = 0; p < paramNames.size(); p++) {
                if (upper == paramNames[p]) {
                    result += (p < argValues.size()) ? argValues[p] : "";
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                // Check locals
                for (size_t l = 0; l < localNames.size(); l++) {
                    if (upper == localNames[l]) {
                        result += localReplacements[l];
                        replaced = true;
                        break;
                    }
                }
            }
            if (!replaced) {
                result += word;
            }
        } else {
            result += line[i++];
        }
    }

    return result;
}

// --- Macro helper: parse invocation arguments ---
// Comma-separated, but commas inside <...> or '...' don't count as separators.
static vector<string> parseMacroArguments(const string& argStr) {
    vector<string> args;
    if (macroTrim(argStr).empty()) return args;

    string current;
    int angleBracketDepth = 0;
    bool inString = false;

    for (size_t i = 0; i < argStr.size(); i++) {
        char c = argStr[i];

        if (c == ';' && !inString && angleBracketDepth == 0) break;

        if (c == '\'' && angleBracketDepth == 0) {
            inString = !inString;
            current += c;
        } else if (c == '<' && !inString) {
            angleBracketDepth++;
            current += c;
        } else if (c == '>' && !inString && angleBracketDepth > 0) {
            angleBracketDepth--;
            current += c;
        } else if (c == ',' && !inString && angleBracketDepth == 0) {
            args.push_back(macroTrim(current));
            current.clear();
        } else {
            current += c;
        }
    }

    string t = macroTrim(current);
    if (!t.empty()) args.push_back(t);

    return args;
}

// --- Macro helper: find matching ENDM, tracking nested blocks ---
// Returns index of ENDM line, or -1 if not found.
static int findMatchingEndm(const vector<string>& lines, int startAfter) {
    int depth = 1;
    for (int i = startAfter; i < (int)lines.size(); i++) {
        LineParts lp = splitMacroLine(lines[i]);
        string u1 = toUpper(lp.tok1);
        string u2 = toUpper(lp.tok2);

        // Check for nested MACRO/REPT/IRP
        if (u2 == "MACRO" || u1 == "REPT" || u1 == "IRP") {
            depth++;
        } else if (u1 == "ENDM") {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

// --- expandRept: expand a REPT/ENDM block in place ---
static bool expandRept(
    vector<string>& lines,
    vector<SourceLocation>& sourceMap,
    vector<Diagnostic>& outErrors,
    int reptLine)
{
    LineParts lp = splitMacroLine(lines[reptLine]);
    // lp.tok1 = "REPT", lp.tok2 = count

    if (lp.tok2.empty()) {
        outErrors.push_back({
            "ERROR", sourceMap[reptLine].line,
            "REPT directive missing repeat count",
            "Usage: REPT <count>"
        });
        return false;
    }

    bool numOk = false;
    int count = parseSimpleNumber(lp.tok2, numOk);
    if (!numOk || count < 0) {
        outErrors.push_back({
            "ERROR", sourceMap[reptLine].line,
            "REPT count must be a non-negative numeric literal",
            "Got: '" + lp.tok2 + "'"
        });
        return false;
    }

    int endmLine = findMatchingEndm(lines, reptLine + 1);
    if (endmLine < 0) {
        outErrors.push_back({
            "ERROR", sourceMap[reptLine].line,
            "REPT without matching ENDM",
            ""
        });
        return false;
    }

    // Collect body lines
    vector<string> body;
    for (int i = reptLine + 1; i < endmLine; i++) {
        body.push_back(lines[i]);
    }

    SourceLocation invocLoc = sourceMap[reptLine];

    // Build expansion
    vector<string> expansion;
    expansion.push_back("; >>> REPT " + lp.tok2);
    for (int r = 0; r < count; r++) {
        for (const auto& bline : body) {
            expansion.push_back(bline);
        }
    }
    expansion.push_back("; <<< END REPT");

    // Splice: replace [reptLine .. endmLine] with expansion
    vector<string> newLines;
    vector<SourceLocation> newMap;
    for (int i = 0; i < reptLine; i++) {
        newLines.push_back(lines[i]);
        newMap.push_back(sourceMap[i]);
    }
    for (const auto& el : expansion) {
        newLines.push_back(el);
        newMap.push_back(invocLoc);
    }
    for (int i = endmLine + 1; i < (int)lines.size(); i++) {
        newLines.push_back(lines[i]);
        newMap.push_back(sourceMap[i]);
    }
    lines = move(newLines);
    sourceMap = move(newMap);
    return true;
}

// --- expandIrp: expand an IRP/ENDM block in place ---
static bool expandIrp(
    vector<string>& lines,
    vector<SourceLocation>& sourceMap,
    vector<Diagnostic>& outErrors,
    int irpLine)
{
    // IRP param, <item1, item2, ...>
    // We need to parse: tok1=IRP, tok2=param, rest=, <items>
    LineParts lp = splitMacroLine(lines[irpLine]);

    if (lp.tok2.empty()) {
        outErrors.push_back({
            "ERROR", sourceMap[irpLine].line,
            "IRP directive missing parameter name",
            "Usage: IRP param, <item1, item2, ...>"
        });
        return false;
    }

    string paramName = toUpper(lp.tok2);
    // Remove trailing comma from param if present
    if (!paramName.empty() && paramName.back() == ',') {
        paramName.pop_back();
    }

    // The rest should be: , <items> or <items>
    string rest = lp.rest;
    // If tok2 didn't have trailing comma, rest should start with comma
    string upperTok2 = toUpper(lp.tok2);
    if (upperTok2.back() != ',') {
        // rest should start with comma
        rest = macroTrim(rest);
        if (rest.empty() || rest[0] != ',') {
            outErrors.push_back({
                "ERROR", sourceMap[irpLine].line,
                "IRP directive missing comma after parameter name",
                "Usage: IRP param, <item1, item2, ...>"
            });
            return false;
        }
        rest = macroTrim(rest.substr(1));
    } else {
        rest = macroTrim(rest);
    }

    // rest should now be <items>
    if (rest.empty() || rest[0] != '<') {
        outErrors.push_back({
            "ERROR", sourceMap[irpLine].line,
            "IRP directive missing angle-bracket list",
            "Usage: IRP param, <item1, item2, ...>"
        });
        return false;
    }

    // Find matching >
    size_t closePos = string::npos;
    int depth = 0;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '<') depth++;
        else if (rest[i] == '>') {
            depth--;
            if (depth == 0) { closePos = i; break; }
        }
    }
    if (closePos == string::npos) {
        outErrors.push_back({
            "ERROR", sourceMap[irpLine].line,
            "IRP directive has unmatched '<'",
            "Usage: IRP param, <item1, item2, ...>"
        });
        return false;
    }

    string itemsStr = rest.substr(1, closePos - 1);  // strip < >

    // Parse items (comma-separated)
    vector<string> items = parseCommaSeparatedIdents(itemsStr);

    int endmLine = findMatchingEndm(lines, irpLine + 1);
    if (endmLine < 0) {
        outErrors.push_back({
            "ERROR", sourceMap[irpLine].line,
            "IRP without matching ENDM",
            ""
        });
        return false;
    }

    // Collect body lines
    vector<string> body;
    for (int i = irpLine + 1; i < endmLine; i++) {
        body.push_back(lines[i]);
    }

    SourceLocation invocLoc = sourceMap[irpLine];

    // Build expansion
    vector<string> expansion;
    expansion.push_back("; >>> IRP " + lp.tok2);
    vector<string> paramNames = { paramName };
    vector<string> emptyLocals, emptyLocalRepls;

    for (const auto& item : items) {
        vector<string> argVals = { item };
        for (const auto& bline : body) {
            expansion.push_back(substituteParams(bline, paramNames, argVals, emptyLocals, emptyLocalRepls));
        }
    }
    expansion.push_back("; <<< END IRP");

    // Splice: replace [irpLine .. endmLine] with expansion
    vector<string> newLines;
    vector<SourceLocation> newMap;
    for (int i = 0; i < irpLine; i++) {
        newLines.push_back(lines[i]);
        newMap.push_back(sourceMap[i]);
    }
    for (const auto& el : expansion) {
        newLines.push_back(el);
        newMap.push_back(invocLoc);
    }
    for (int i = endmLine + 1; i < (int)lines.size(); i++) {
        newLines.push_back(lines[i]);
        newMap.push_back(sourceMap[i]);
    }
    lines = move(newLines);
    sourceMap = move(newMap);
    return true;
}

// --- Main macro expansion function ---
static bool expandMacros(
    vector<string>& lines,
    vector<SourceLocation>& sourceMap,
    vector<Diagnostic>& outErrors)
{
    MacroTable macros;
    static int localCounter = 0;

    // ==========================================
    // Phase 1: Collect macro definitions
    // ==========================================
    for (int i = 0; i < (int)lines.size(); /* no increment */) {
        LineParts lp = splitMacroLine(lines[i]);
        string u1 = toUpper(lp.tok1);
        string u2 = toUpper(lp.tok2);

        // Detect "name MACRO [params]"
        if (u2 == "MACRO") {
            string macroName = toUpper(lp.tok1);

            // Validate name
            if (isMacroReservedWord(macroName)) {
                outErrors.push_back({
                    "ERROR", sourceMap[i].line,
                    "Cannot define macro with reserved name '" + macroName + "'",
                    ""
                });
                return false;
            }

            // Check redefinition
            if (macros.count(macroName)) {
                outErrors.push_back({
                    "WARNING", sourceMap[i].line,
                    "Macro '" + macroName + "' redefined (previous at line " +
                    to_string(macros[macroName].definedAtLine + 1) + ")",
                    ""
                });
            }

            // Parse parameters
            vector<string> params;
            if (!lp.rest.empty()) {
                vector<string> rawParams = parseCommaSeparatedIdents(lp.rest);
                for (const auto& p : rawParams) {
                    params.push_back(toUpper(p));
                }
            }

            // Find matching ENDM
            int endmLine = findMatchingEndm(lines, i + 1);
            if (endmLine < 0) {
                outErrors.push_back({
                    "ERROR", sourceMap[i].line,
                    "MACRO '" + macroName + "' without matching ENDM",
                    ""
                });
                return false;
            }

            // Collect body, extracting LOCAL declarations
            MacroDefinition def;
            def.name = macroName;
            def.params = params;
            def.definedAtLine = i;

            for (int j = i + 1; j < endmLine; j++) {
                LineParts bodyLp = splitMacroLine(lines[j]);
                if (toUpper(bodyLp.tok1) == "LOCAL") {
                    // Parse LOCAL names
                    string localArgs = bodyLp.tok2;
                    if (!bodyLp.rest.empty()) localArgs += " " + bodyLp.rest;
                    vector<string> localNames = parseCommaSeparatedIdents(localArgs);
                    for (const auto& ln : localNames) {
                        def.locals.push_back(toUpper(ln));
                    }
                } else {
                    def.body.push_back(lines[j]);
                }
            }

            macros[macroName] = def;

            // Replace definition lines with comments
            for (int j = i; j <= endmLine; j++) {
                lines[j] = "; [MACRO DEF] " + lines[j];
            }

            i = endmLine + 1;
            continue;
        }

        // Skip REPT/IRP blocks in phase 1 (they'll be expanded in phase 2)
        if (u1 == "REPT" || u1 == "IRP") {
            int endmLine = findMatchingEndm(lines, i + 1);
            if (endmLine < 0) {
                outErrors.push_back({
                    "ERROR", sourceMap[i].line,
                    u1 + " without matching ENDM",
                    ""
                });
                return false;
            }
            i = endmLine + 1;
            continue;
        }

        // Detect orphan ENDM
        if (u1 == "ENDM") {
            outErrors.push_back({
                "ERROR", sourceMap[i].line,
                "ENDM without matching MACRO, REPT, or IRP",
                ""
            });
            return false;
        }

        i++;
    }

    // If no macros defined and no REPT/IRP, quick check
    if (macros.empty()) {
        // Still need to check for REPT/IRP
        bool hasReptIrp = false;
        for (const auto& line : lines) {
            LineParts lp = splitMacroLine(line);
            string u1 = toUpper(lp.tok1);
            if (u1 == "REPT" || u1 == "IRP") { hasReptIrp = true; break; }
        }
        if (!hasReptIrp) return true;
    }

    // ==========================================
    // Phase 2: Iterative expansion
    // ==========================================
    for (int iteration = 0; iteration < MAX_MACRO_EXPANSION_ITERATIONS; iteration++) {
        bool expanded = false;

        for (int i = 0; i < (int)lines.size(); i++) {
            // Skip comments and blank lines
            string trimmed = macroTrim(lines[i]);
            if (trimmed.empty() || trimmed[0] == ';') continue;

            LineParts lp = splitMacroLine(lines[i]);
            string u1 = toUpper(lp.tok1);
            string u2 = toUpper(lp.tok2);

            // Check for REPT
            if (u1 == "REPT") {
                if (!expandRept(lines, sourceMap, outErrors, i)) return false;
                expanded = true;
                break;  // restart scan
            }

            // Check for IRP
            if (u1 == "IRP") {
                if (!expandIrp(lines, sourceMap, outErrors, i)) return false;
                expanded = true;
                break;  // restart scan
            }

            // Check for macro invocation
            // Case 1: first token is macro name: "MacroName arg1, arg2"
            // Case 2: first token is a label, second is macro name: "label: MacroName arg1"
            string macroName;
            string argStr;
            string labelPrefix;

            if (macros.count(u1)) {
                macroName = u1;
                // Arguments are tok2 + rest
                argStr = lp.tok2;
                if (!lp.rest.empty()) {
                    if (!argStr.empty()) argStr += " ";
                    argStr += lp.rest;
                }
            } else if (lp.tok1.size() > 0 && lp.tok1.back() == ':' && macros.count(u2)) {
                // Label before macro invocation
                macroName = u2;
                labelPrefix = lp.tok1;
                argStr = lp.rest;
            }

            if (!macroName.empty()) {
                const MacroDefinition& def = macros[macroName];

                // Parse arguments
                vector<string> args = parseMacroArguments(argStr);

                // Warn on argument count mismatch
                if (args.size() < def.params.size()) {
                    outErrors.push_back({
                        "WARNING", sourceMap[i].line,
                        "Macro '" + macroName + "' invoked with " + to_string(args.size()) +
                        " args, expected " + to_string(def.params.size()),
                        "Missing arguments will be empty strings"
                    });
                } else if (args.size() > def.params.size()) {
                    outErrors.push_back({
                        "WARNING", sourceMap[i].line,
                        "Macro '" + macroName + "' invoked with " + to_string(args.size()) +
                        " args, expected " + to_string(def.params.size()),
                        "Extra arguments will be ignored"
                    });
                }

                // Generate LOCAL label replacements
                vector<string> localReplacements;
                for (size_t l = 0; l < def.locals.size(); l++) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "??%04X", localCounter++);
                    localReplacements.push_back(string(buf));
                }

                SourceLocation invocLoc = sourceMap[i];

                // Build expansion
                vector<string> expansion;
                if (!labelPrefix.empty()) {
                    expansion.push_back(labelPrefix);
                }
                expansion.push_back("; >>> MACRO " + macroName);
                for (const auto& bodyLine : def.body) {
                    expansion.push_back(substituteParams(bodyLine, def.params, args, def.locals, localReplacements));
                }
                expansion.push_back("; <<< END MACRO " + macroName);

                // Splice: replace line i with expansion
                vector<string> newLines;
                vector<SourceLocation> newMap;
                for (int j = 0; j < i; j++) {
                    newLines.push_back(lines[j]);
                    newMap.push_back(sourceMap[j]);
                }
                for (const auto& el : expansion) {
                    newLines.push_back(el);
                    newMap.push_back(invocLoc);
                }
                for (int j = i + 1; j < (int)lines.size(); j++) {
                    newLines.push_back(lines[j]);
                    newMap.push_back(sourceMap[j]);
                }
                lines = move(newLines);
                sourceMap = move(newMap);
                expanded = true;
                break;  // restart scan
            }
        }

        if (!expanded) return true;  // stable — done
    }

    outErrors.push_back({
        "ERROR", 0,
        "Macro expansion iteration limit exceeded (" + to_string(MAX_MACRO_EXPANSION_ITERATIONS) + ")",
        "Check for recursive or mutually-recursive macro invocations"
    });
    return false;
}

// ============================================================
// HELP SYSTEM
// ============================================================

static vector<HelpEntry> loadHelp(const string& path) {
    vector<HelpEntry> entries;
    ifstream f(path);
    if (!f) return entries;
    HelpEntry cur;
    bool inTopic = false;
    string line;
    while (getline(f, line)) {
        // Strip trailing \r for Windows line endings
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("@@TOPIC ", 0) == 0) {
            if (inTopic) entries.push_back(cur);
            cur = HelpEntry();
            cur.topic = line.substr(8);
            inTopic = true;
        } else if (line.rfind("@@GROUP ", 0) == 0 && inTopic) {
            cur.group = line.substr(8);
        } else if (line.rfind("@@BRIEF ", 0) == 0 && inTopic) {
            cur.brief = line.substr(8);
        } else if (line.rfind("@@RELATED ", 0) == 0 && inTopic) {
            stringstream ss(line.substr(10));
            string tok;
            while (getline(ss, tok, ',')) {
                // Trim whitespace
                size_t s = tok.find_first_not_of(' ');
                size_t e = tok.find_last_not_of(' ');
                if (s != string::npos) cur.related.push_back(tok.substr(s, e - s + 1));
            }
        } else if (line == "@@EXAMPLES" && inTopic) {
            // Trim content collected so far
            while (!cur.content.empty() && cur.content.front() == '\n') cur.content.erase(0, 1);
            while (!cur.content.empty() && cur.content.back() == '\n') cur.content.pop_back();
            // Read example lines until @@END
            while (getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "@@END") {
                    entries.push_back(cur);
                    cur = HelpEntry();
                    inTopic = false;
                    break;
                }
                // Skip blank lines; non-blank lines are examples
                size_t first = line.find_first_not_of(' ');
                if (first != string::npos) {
                    cur.examples.push_back(line.substr(first));
                }
            }
        } else if (line == "@@END" && inTopic) {
            // Trim leading/trailing newlines from content
            while (!cur.content.empty() && cur.content.front() == '\n') cur.content.erase(0, 1);
            while (!cur.content.empty() && cur.content.back() == '\n') cur.content.pop_back();
            entries.push_back(cur);
            cur = HelpEntry();
            inTopic = false;
        } else if (inTopic) {
            cur.content += line + "\n";
        }
    }
    if (inTopic) entries.push_back(cur);
    return entries;
}

static void emitHelpIndex(const vector<HelpEntry>& entries) {
    cout << "{\"help\":true,\"topics\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) cout << ",";
        cout << "{\"name\":\"" << jsonEscape(entries[i].topic) << "\""
             << ",\"group\":\"" << jsonEscape(entries[i].group) << "\""
             << ",\"brief\":\"" << jsonEscape(entries[i].brief) << "\"}";
    }
    cout << "]}" << endl;
}

static void emitHelpTopic(const HelpEntry& e) {
    cout << "{\"help\":true"
         << ",\"topic\":\"" << jsonEscape(e.topic) << "\""
         << ",\"group\":\"" << jsonEscape(e.group) << "\""
         << ",\"brief\":\"" << jsonEscape(e.brief) << "\""
         << ",\"content\":\"" << jsonEscape(e.content) << "\"";
    if (!e.examples.empty()) {
        cout << ",\"examples\":[";
        for (size_t i = 0; i < e.examples.size(); ++i) {
            if (i) cout << ",";
            cout << "\"" << jsonEscape(e.examples[i]) << "\"";
        }
        cout << "]";
    }
    cout << ",\"related\":[";
    for (size_t i = 0; i < e.related.size(); ++i) {
        if (i) cout << ",";
        cout << "\"" << jsonEscape(e.related[i]) << "\"";
    }
    cout << "]}" << endl;
}

static void emitHelpError(const string& topic, const vector<HelpEntry>& entries) {
    cout << "{\"help\":true,\"error\":\"Unknown help topic: " << jsonEscape(topic) << "\""
         << ",\"available\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) cout << ",";
        cout << "\"" << jsonEscape(entries[i].topic) << "\"";
    }
    cout << "]}" << endl;
}

// ============================================================
// MAIN
// ============================================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bool disasmMode = false;
    bool agentMode = false;
    bool runMode = false;
    bool runSourceMode = false;
    string filename;
    EmulatorConfig emuConfig;

    // Arg parsing
    for(int i=1; i<argc; ++i) {
        string arg = argv[i];
        if(arg == "--version" || arg == "-v") {
            cout << "{\"tool\":\"agent86\",\"version\":\"" << AGENT86_VERSION << "\"}" << endl;
            return 0;
        } else if(arg == "--help" || arg == "-h") {
            namespace fs = std::filesystem;
            fs::path helpPath = fs::path(argv[0]).parent_path() / "agent86.hlp";
            vector<HelpEntry> helpEntries = loadHelp(helpPath.string());
            if (helpEntries.empty()) {
                cout << "{\"help\":true,\"error\":\"Cannot load help file: "
                     << jsonEscape(helpPath.string()) << "\"}" << endl;
                return 1;
            }
            // Check if next arg is a topic name
            if (i + 1 < argc) {
                string topic = argv[i + 1];
                // Find matching topic (case-insensitive, also try with -- prefix)
                string topicLower = topic;
                transform(topicLower.begin(), topicLower.end(), topicLower.begin(), ::tolower);
                const HelpEntry* found = nullptr;
                for (const auto& e : helpEntries) {
                    string eName = e.topic;
                    transform(eName.begin(), eName.end(), eName.begin(), ::tolower);
                    if (eName == topicLower) { found = &e; break; }
                }
                if (found) {
                    emitHelpTopic(*found);
                } else {
                    emitHelpError(topic, helpEntries);
                }
            } else {
                emitHelpIndex(helpEntries);
            }
            return 0;
        } else if(arg == "--agent") {
            agentMode = true;
        } else if (arg == "--explain") {
            if (i + 1 < argc) {
                printInstructionHelp(argv[i+1]);
                return 0;
            }
        } else if (arg == "--dump-isa") {
            cout << "[";
            for (size_t k = 0; k < isaDB.size(); ++k) {
                const auto& entry = isaDB[k];
                cout << "{ \"mnemonic\": \"" << entry.mnemonic << "\", \"desc\": \"" << entry.description << "\"}";
                if (k < isaDB.size() - 1) cout << ",";
            }
            cout << "]" << endl;
            return 0;
        } else if (arg == "--disassemble") {
            disasmMode = true;
        } else if (arg == "--run") {
            runMode = true;
        } else if (arg == "--run-source") {
            runSourceMode = true;
        } else if (arg == "--breakpoints" && i + 1 < argc) {
            ++i;
            stringstream bpss(argv[i]);
            string tok;
            while (getline(bpss, tok, ',')) {
                uint16_t addr = (uint16_t)stoul(tok, nullptr, 16);
                emuConfig.breakpoints.insert(addr);
            }
        } else if (arg == "--watch-regs" && i + 1 < argc) {
            ++i;
            stringstream wrss(argv[i]);
            string tok;
            static const string rnames[] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
            while (getline(wrss, tok, ',')) {
                string upper = toUpper(tok);
                for (int r = 0; r < 8; r++) {
                    if (upper == rnames[r]) { emuConfig.watchRegs.insert(r); break; }
                }
            }
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            ++i;
            emuConfig.maxCycles = stoi(argv[i]);
        } else if (arg == "--input" && i + 1 < argc) {
            ++i;
            emuConfig.stdinInput = unescapeInput(argv[i]);
        } else if (arg == "--args" && i + 1 < argc) {
            ++i;
            emuConfig.commandArgs = argv[i];
        } else if (arg == "--mem-dump" && i + 1 < argc) {
            ++i;
            string mdarg = argv[i];
            size_t comma = mdarg.find(',');
            if (comma != string::npos) {
                emuConfig.memDumpAddr = (uint16_t)stoul(mdarg.substr(0, comma), nullptr, 16);
                emuConfig.memDumpLen = stoi(mdarg.substr(comma + 1));
            }
        } else if (arg == "--screen") {
            // Full 80x50 screen capture
            emuConfig.hasViewport = true;
            emuConfig.vpCol = 0;
            emuConfig.vpRow = 0;
            emuConfig.vpWidth = 80;
            emuConfig.vpHeight = 50;
        } else if (arg == "--viewport" && i + 1 < argc) {
            ++i;
            // Format: col,row,width,height
            int c = 0, r = 0, w = 80, h = 50;
            if (sscanf(argv[i], "%d,%d,%d,%d", &c, &r, &w, &h) == 4) {
                emuConfig.hasViewport = true;
                emuConfig.vpCol = c;
                emuConfig.vpRow = r;
                emuConfig.vpWidth = w;
                emuConfig.vpHeight = h;
            } else {
                cerr << "Invalid --viewport format. Use: col,row,width,height" << endl;
                return 1;
            }
        } else if (arg == "--vram-fill") {
            emuConfig.vramFill = true;
            if (i + 1 < argc && string(argv[i + 1]).substr(0, 2) != "--") {
                ++i;
                emuConfig.vramFillText = argv[i];
            }
        } else if (arg == "--attrs") {
            emuConfig.vpAttrs = true;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            ++i;
            emuConfig.screenshotFile = argv[i];
        } else if (arg == "--font" && i + 1 < argc) {
            ++i;
            string fontArg = argv[i];
            if (fontArg == "8x8") {
                emuConfig.screenshotFont8x8 = true;
            } else if (fontArg != "8x16") {
                cerr << "Unknown font: " << fontArg << ". Use 8x8 or 8x16." << endl;
                return 1;
            }
        } else if (arg == "--output-file" && i + 1 < argc) {
            ++i;
            emuConfig.outputFile = argv[i];
        } else if (arg == "--mouse" && i + 1 < argc) {
            ++i;
            int mx = 0, my = 0, mb = 0;
            int parsed = sscanf(argv[i], "%d,%d,%d", &mx, &my, &mb);
            if (parsed >= 2) {
                emuConfig.mouseEnabled = true;
                emuConfig.mouseX = mx;
                emuConfig.mouseY = my;
                emuConfig.mouseButtons = (uint16_t)mb;
            } else {
                cerr << "Invalid --mouse format. Use: x,y[,buttons]" << endl;
                return 1;
            }
        } else if (arg == "--events" && i + 1 < argc) {
            ++i;
            string evJson;
            if (argv[i][0] == '@') {
                string path = string(argv[i] + 1);
                ifstream ef(path);
                if (!ef) { cerr << "Cannot open events file: " << path << endl; return 1; }
                evJson.assign(istreambuf_iterator<char>(ef), istreambuf_iterator<char>());
            } else {
                evJson = argv[i];
            }
            string err;
            emuConfig.events = parseEvents(evJson, err);
            if (!err.empty()) { cerr << "Events parse error: " << err << endl; return 1; }
            for (const auto& ev : emuConfig.events) {
                if (ev.hasMouse) { emuConfig.mouseEnabled = true; break; }
            }
        } else if (arg == "--dos-root" && i + 1 < argc) {
            ++i;
            emuConfig.dosRoot = argv[i];
            namespace fs = std::filesystem;
            if (!fs::is_directory(emuConfig.dosRoot)) {
                cerr << "Error: --dos-root path is not a directory: " << emuConfig.dosRoot << endl;
                return 1;
            }
        } else {
            filename = arg;
        }
    }

    // Redirect output to file if requested
    ofstream outputFileStream;
    if (!emuConfig.outputFile.empty()) {
        outputFileStream.open(emuConfig.outputFile, ios::binary);
        if (!outputFileStream) {
            cerr << "Cannot open output file: " << emuConfig.outputFile << endl;
            return 1;
        }
        cout.rdbuf(outputFileStream.rdbuf());
    }

    // --- Disassemble mode ---
    if (disasmMode) {
        if (filename.empty()) {
            cout << "{ \"error\": \"No input file for disassembly\" }" << endl;
            return 0;
        }
        disassembleFile(filename);
        return 0;
    }

    // --- Run .COM binary mode ---
    if (runMode) {
        if (filename.empty()) {
            cout << "{ \"error\": \"No input file for emulation\" }" << endl;
            return 0;
        }
        ifstream binIn(filename, ios::binary);
        if (!binIn) {
            cout << "{ \"error\": \"Cannot open file: " << jsonEscape(filename) << "\" }" << endl;
            return 1;
        }
        vector<uint8_t> binary((istreambuf_iterator<char>(binIn)), istreambuf_iterator<char>());
        binIn.close();

        CPU finalCpu;
        EmulatorResult emuResult = runEmulator(binary, emuConfig, finalCpu);
        emitEmulatorJSON(emuResult, finalCpu);
        return 0;
    }

    // --- Run source (assemble + emulate) mode ---
    if (runSourceMode) {
        if (filename.empty()) {
            cout << "{ \"error\": \"No input file\" }" << endl;
            return 0;
        }
        vector<string> lines;
        vector<SourceLocation> sourceMap;
        vector<Diagnostic> expandErrors;
        if (!expandIncludes(filename, lines, sourceMap, expandErrors)) {
            // Report expand errors through a minimal context and bail
            AssemblerContext ctx;
            for (const auto& e : expandErrors) ctx.agentState.diagnostics.push_back(e);
            ctx.globalError = true;
            emitCombinedJSON(ctx, EmulatorResult(), CPU(), sourceMap);
            return 0;
        }

        // Expand macros (MACRO/ENDM, REPT, IRP)
        vector<Diagnostic> macroErrors;
        if (!expandMacros(lines, sourceMap, macroErrors)) {
            AssemblerContext ctx;
            for (const auto& e : macroErrors) ctx.agentState.diagnostics.push_back(e);
            ctx.globalError = true;
            emitCombinedJSON(ctx, EmulatorResult(), CPU(), sourceMap);
            return 0;
        }
        // Forward any macro warnings
        // (they'll be merged into ctx diagnostics after ctx is created)
        vector<Diagnostic> savedMacroWarnings = macroErrors;

        AssemblerContext ctx;
        for (const auto& w : savedMacroWarnings) ctx.agentState.diagnostics.push_back(w);
        // Pass 1 (iterative: re-run if Jcc promotions change label addresses)
        // Iteration 0: populate symbol table (no promotion detection)
        // Iteration 1+: detect out-of-range Jcc and mark for promotion; repeat until stable
        for (int pass1Iter = 0; pass1Iter < 20; ++pass1Iter) {
            size_t prevPromotions = ctx.promotedJumps.size();
            int prevEndAddress = ctx.currentAddress; // track total code size
            ctx.isPass1 = true; ctx.currentAddress = 0;
            // Don't clear symbolTable — forward refs need previous iteration's values
            ctx.currentProcedureName = "";
            ctx.currentStructName = "";
            ctx.agentState.diagnostics.clear();
            for (const auto& w : savedMacroWarnings) ctx.agentState.diagnostics.push_back(w);
            ctx.globalError = false;
            for (int i = 0; i < (int)lines.size(); ++i) {
                vector<Token> tokens = tokenize(lines[i], i+1);
                assembleLine(ctx, tokens, i+1, lines[i]);
            }
            if (!ctx.detectPromotions) {
                // First iteration done — enable detection and always run at least one more
                ctx.detectPromotions = true;
            } else if (ctx.promotedJumps.size() == prevPromotions
                       && ctx.currentAddress == prevEndAddress) {
                break; // stable — no new promotions and no address changes
            }
        }
        // Pass 2
        ctx.agentState.diagnostics.clear();
        for (const auto& w : savedMacroWarnings) ctx.agentState.diagnostics.push_back(w);
        ctx.globalError = false;
        ctx.isPass1 = false; ctx.currentAddress = 0; ctx.machineCode.clear();
        ctx.currentStructName = "";
        for (int i = 0; i < (int)lines.size(); ++i) {
            vector<Token> tokens = tokenize(lines[i], i+1);
            assembleLine(ctx, tokens, i+1, lines[i]);
        }
        if (!ctx.currentStructName.empty()) {
            logError(ctx, 0, "Unclosed STRUC '" + ctx.currentStructName + "'",
                "Every STRUC must have a matching ENDS.");
        }

        if (ctx.globalError) {
            emitCombinedJSON(ctx, EmulatorResult(), CPU(), sourceMap);
            return 0;
        }

        CPU finalCpu;
        EmulatorResult emuResult = runEmulator(ctx.machineCode, emuConfig, finalCpu);
        emitCombinedJSON(ctx, emuResult, finalCpu, sourceMap);
        return 0;
    }

    // --- Default: Assemble mode ---
    if (filename.empty()) {
        if(agentMode) {
             cout << "{ \"error\": \"No input file\" }" << endl;
             return 0;
        }
        cerr << "Usage: agent86 [--agent] source.asm" << endl;
        return 1;
    }
    string outfile = "output.com";
    if (filename.size() > 4 && filename.substr(filename.size()-4) == ".asm") {
        outfile = filename.substr(0, filename.size()-4) + ".com";
    }

    vector<string> lines;
    vector<SourceLocation> sourceMap;
    vector<Diagnostic> expandErrors;
    if (!expandIncludes(filename, lines, sourceMap, expandErrors)) {
        if (agentMode) {
            AssemblerContext ctx;
            for (const auto& e : expandErrors) ctx.agentState.diagnostics.push_back(e);
            ctx.globalError = true;
            emitAgentJSON(ctx, sourceMap);
            return 0;
        }
        for (const auto& e : expandErrors) cerr << e.message << endl;
        return 1;
    }

    // Expand macros (MACRO/ENDM, REPT, IRP)
    {
        vector<Diagnostic> macroErrors;
        if (!expandMacros(lines, sourceMap, macroErrors)) {
            if (agentMode) {
                AssemblerContext ctx;
                for (const auto& e : macroErrors) ctx.agentState.diagnostics.push_back(e);
                ctx.globalError = true;
                emitAgentJSON(ctx, sourceMap);
                return 0;
            }
            for (const auto& e : macroErrors) cerr << e.message << endl;
            return 1;
        }
        // Macro warnings will be merged into ctx below
        expandErrors.insert(expandErrors.end(), macroErrors.begin(), macroErrors.end());
    }

    // Context instantiation
    AssemblerContext ctx;
    // Forward macro warnings into assembler diagnostics
    for (const auto& w : expandErrors) ctx.agentState.diagnostics.push_back(w);

    // Pass 1 (iterative: re-run if Jcc promotions change label addresses)
    // Iteration 0: populate symbol table (no promotion detection)
    // Iteration 1+: detect out-of-range Jcc and mark for promotion; repeat until stable
    for (int pass1Iter = 0; pass1Iter < 20; ++pass1Iter) {
        size_t prevPromotions = ctx.promotedJumps.size();
        int prevEndAddress = ctx.currentAddress; // track total code size
        ctx.isPass1 = true;
        ctx.currentAddress = 0;
        // Don't clear symbolTable — forward refs need previous iteration's values
        ctx.currentProcedureName = "";
        ctx.currentStructName = "";
        ctx.agentState.diagnostics.clear();
        for (const auto& w : expandErrors) ctx.agentState.diagnostics.push_back(w);
        ctx.globalError = false;
        for (int i = 0; i < (int)lines.size(); ++i) {
            vector<Token> tokens = tokenize(lines[i], i+1);
            assembleLine(ctx, tokens, i+1, lines[i]);
        }
        if (!ctx.detectPromotions) {
            // First iteration done — enable detection and always run at least one more
            ctx.detectPromotions = true;
        } else if (ctx.promotedJumps.size() == prevPromotions
                   && ctx.currentAddress == prevEndAddress) {
            break; // stable — no new promotions and no address changes
        }
    }

    // Pass 2
    ctx.agentState.diagnostics.clear();
    for (const auto& w : expandErrors) ctx.agentState.diagnostics.push_back(w);
    ctx.globalError = false;

    ctx.isPass1 = false;
    ctx.currentAddress = 0;
    ctx.machineCode.clear();
    ctx.currentStructName = "";
    for (int i = 0; i < (int)lines.size(); ++i) {
        vector<Token> tokens = tokenize(lines[i], i+1);
        assembleLine(ctx, tokens, i+1, lines[i]);
    }
    if (!ctx.currentStructName.empty()) {
        logError(ctx, 0, "Unclosed STRUC '" + ctx.currentStructName + "'",
            "Every STRUC must have a matching ENDS.");
    }

    if (ctx.globalError) {
        if (agentMode) {
            emitAgentJSON(ctx, sourceMap);
            return 0;
        }
        cerr << "Assembly failed with errors." << endl;
        remove(outfile.c_str());
        return 1;
    }

    ofstream out(outfile, ios::binary);
    out.write((char*)ctx.machineCode.data(), ctx.machineCode.size());
    out.close();

    if (agentMode) {
        emitAgentJSON(ctx, sourceMap);
        return 0;
    }

    cout << "Successfully assembled " << filename << " -> " << outfile << endl;
    cout << "Output size: " << ctx.machineCode.size() << " bytes" << endl;

    return 0;
}
