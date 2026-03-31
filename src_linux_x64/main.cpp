#include "asm.h"
#include "jit/jit.h"
#include "jit/kbd.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static void printFailedJson(const std::vector<AsmError>& errors) {
    std::cout << "{\"compiled\":\"FAILED\",\"errors\":[";
    for (size_t i = 0; i < errors.size(); i++) {
        if (i > 0) std::cout << ",";
        std::cout << "{\"line\":" << errors[i].line;
        if (!errors[i].file.empty()) {
            std::cout << ",\"file\":\"" << jsonEscape(errors[i].file) << "\"";
        }
        std::cout << ",\"source\":\"" << jsonEscape(errors[i].source) << "\""
                  << ",\"message\":\"" << jsonEscape(errors[i].message) << "\"}";
    }
    std::cout << "]}" << std::endl;
}

// ---- Event JSON parser ----

static void skipWhitespace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

// Parse a JSON string value (after opening quote). Handles standard JSON escapes.
// Produces literal \S \C \A from \\S \\C \\A for downstream injectKeys processing.
static bool parseJsonString(const std::string& s, size_t& i, std::string& out) {
    out.clear();
    // i should be right after the opening "
    while (i < s.size()) {
        char ch = s[i++];
        if (ch == '"') return true;
        if (ch == '\\') {
            if (i >= s.size()) return false;
            char esc = s[i++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // \uXXXX unicode escape (RFC 8259)
                    if (i + 4 > s.size()) return false;
                    unsigned val = 0;
                    for (int j = 0; j < 4; j++) {
                        char hc = s[i++];
                        val <<= 4;
                        if (hc >= '0' && hc <= '9') val |= (hc - '0');
                        else if (hc >= 'a' && hc <= 'f') val |= (hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') val |= (hc - 'A' + 10);
                        else return false;
                    }
                    out += (char)(uint8_t)(val & 0xFF);
                    break;
                }
                case 'S':  out += "\\S"; break;  // modifier escape passthrough
                case 'C':  out += "\\C"; break;
                case 'A':  out += "\\A"; break;
                default:   out += esc;   break;
            }
        } else {
            out += ch;
        }
    }
    return false; // unterminated string
}

// Parse a JSON integer (signed or unsigned). Returns false if no digits found.
static bool parseJsonNumber(const std::string& s, size_t& i, int64_t& out) {
    if (i >= s.size()) return false;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    int64_t val = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (s[i] - '0');
        i++;
    }
    out = neg ? -val : val;
    return true;
}

// Parse a mouse object: {"buttons":N,"x":N,"y":N} (buttons optional, default 0)
// i should be right after the opening '{'.
static bool parseMouseObject(const std::string& json, size_t& i,
                             MouseEvent& out, std::string& error) {
    out = {};
    bool has_x = false, has_y = false;
    int field_count = 0;
    while (true) {
        skipWhitespace(json, i);
        if (i < json.size() && json[i] == '}') { i++; break; }
        if (field_count > 0) {
            if (i >= json.size() || json[i] != ',') { error = "expected ',' or '}' in mouse object"; return false; }
            i++;
            skipWhitespace(json, i);
        }
        // Parse key
        if (i >= json.size() || json[i] != '"') { error = "expected '\"' for mouse field key"; return false; }
        i++;
        std::string key;
        if (!parseJsonString(json, i, key)) { error = "unterminated mouse field key"; return false; }
        skipWhitespace(json, i);
        if (i >= json.size() || json[i] != ':') { error = "expected ':' after mouse field '" + key + "'"; return false; }
        i++;
        skipWhitespace(json, i);
        int64_t val;
        if (!parseJsonNumber(json, i, val)) { error = "expected number for mouse field '" + key + "'"; return false; }
        if (key == "x") { out.x = (uint16_t)val; has_x = true; }
        else if (key == "y") { out.y = (uint16_t)val; has_y = true; }
        else if (key == "buttons") { out.buttons = (uint16_t)val; }
        else { error = "unknown mouse field: " + key; return false; }
        field_count++;
    }
    if (!has_x) { error = "mouse object missing 'x' field"; return false; }
    if (!has_y) { error = "mouse object missing 'y' field"; return false; }
    return true;
}

static bool parseEventsJson(const std::string& json,
                            std::vector<KeyEvent>& triggered,
                            std::vector<InputEvent>& sequential,
                            std::string& error) {
    triggered.clear();
    sequential.clear();
    size_t i = 0;
    skipWhitespace(json, i);
    if (i >= json.size() || json[i] != '[') { error = "expected '[' at start of events JSON"; return false; }
    i++;
    skipWhitespace(json, i);
    if (i < json.size() && json[i] == ']') { i++; return true; } // empty array

    while (true) {
        skipWhitespace(json, i);
        if (i >= json.size() || json[i] != '{') { error = "expected '{' for event object"; return false; }
        i++;

        std::string on_val, keys_val;
        bool has_on = false, has_keys = false, has_mouse = false;
        MouseEvent mouse_val;
        int field_count = 0;

        // Parse object key-value pairs
        while (true) {
            skipWhitespace(json, i);
            if (i < json.size() && json[i] == '}') { i++; break; }
            if (field_count > 0) {
                if (i >= json.size() || json[i] != ',') { error = "expected ',' or '}' in event object"; return false; }
                i++;
                skipWhitespace(json, i);
            }
            // Parse key
            if (i >= json.size() || json[i] != '"') { error = "expected '\"' for key"; return false; }
            i++;
            std::string key;
            if (!parseJsonString(json, i, key)) { error = "unterminated key string"; return false; }
            skipWhitespace(json, i);
            if (i >= json.size() || json[i] != ':') { error = "expected ':' after key"; return false; }
            i++;
            skipWhitespace(json, i);

            if (key == "mouse") {
                // Parse nested mouse object
                if (i >= json.size() || json[i] != '{') { error = "expected '{' for mouse value"; return false; }
                i++;
                if (!parseMouseObject(json, i, mouse_val, error)) return false;
                has_mouse = true;
            } else {
                // Parse string value
                if (i >= json.size() || json[i] != '"') { error = "expected '\"' for value of '" + key + "'"; return false; }
                i++;
                std::string val;
                if (!parseJsonString(json, i, val)) { error = "unterminated value string for '" + key + "'"; return false; }
                if (key == "on") { on_val = val; has_on = true; }
                else if (key == "keys") { keys_val = val; has_keys = true; }
            }
            field_count++;
        }

        // Validate field combinations
        if (has_mouse && has_keys) { error = "event object cannot have both 'mouse' and 'keys'"; return false; }
        if (has_mouse && has_on) { error = "mouse event cannot have 'on' trigger"; return false; }

        if (has_on) {
            // Legacy triggered event: must have "on" + "keys"
            if (!has_keys) { error = "triggered event missing 'keys' field"; return false; }
            KeyEvent ev;
            auto colon = on_val.find(':');
            if (colon == std::string::npos) { error = "invalid 'on' format: " + on_val + " (expected trigger:N)"; return false; }
            std::string trigger_str = on_val.substr(0, colon);
            std::string count_str = on_val.substr(colon + 1);
            if (trigger_str == "read") ev.trigger = KeyEvent::TRIGGER_READ;
            else if (trigger_str == "poll") ev.trigger = KeyEvent::TRIGGER_POLL;
            else { error = "unknown trigger type: " + trigger_str; return false; }
            ev.count = (uint32_t)std::stoul(count_str);
            if (ev.count == 0) { error = "trigger count must be >= 1"; return false; }
            ev.keys = keys_val;
            triggered.push_back(std::move(ev));
        } else if (has_mouse) {
            // Sequential mouse event
            InputEvent ie;
            ie.kind = InputEvent::KIND_MOUSE;
            ie.mouse = mouse_val;
            sequential.push_back(std::move(ie));
        } else if (has_keys) {
            // Sequential keys event (no "on" trigger)
            InputEvent ie;
            ie.kind = InputEvent::KIND_KEYS;
            ie.keys = keys_val;
            sequential.push_back(std::move(ie));
        } else {
            error = "event object must have 'keys', 'mouse', or 'on'+'keys'";
            return false;
        }

        skipWhitespace(json, i);
        if (i < json.size() && json[i] == ',') { i++; continue; }
        if (i < json.size() && json[i] == ']') { i++; break; }
        error = "expected ',' or ']' after event object";
        return false;
    }
    return true;
}

static std::string loadEventsArg(const std::string& arg,
                                 std::vector<KeyEvent>& triggered,
                                 std::vector<InputEvent>& sequential,
                                 std::string& error) {
    std::string json;
    if (!arg.empty() && arg[0] == '[') {
        json = arg;
    } else {
        // Read from file
        std::ifstream ifs(arg);
        if (!ifs) { error = "cannot open events file: " + arg; return ""; }
        std::stringstream ss;
        ss << ifs.rdbuf();
        json = ss.str();
    }
    if (!parseEventsJson(json, triggered, sequential, error)) return "";
    return json;
}

// ---- Help system: --help [flag] ----

static void helpOverview() {
    std::cout << R"HELP(agent86 v0.20.0 -- 8086 assembler + JIT emulator for .COM binaries

USAGE
  agent86 <file.asm>                   Assemble to <file.com>
  agent86 <file.asm> -o <out.com>      Assemble with explicit output path
  agent86 <file.asm> --build_run [N]   Assemble + execute in one step
  agent86 <file.asm> --build_trace [N] Assemble + trace in one step
  agent86 <file.com> --run [N]         Execute pre-compiled .COM binary
  agent86 <file.com> --trace [N]       Trace pre-compiled .COM binary
  agent86 --help [flag]                Show help (overview or per-flag detail)

  stdout is always JSON. DOS output and diagnostics go to stderr.

FLAGS
  --build_run [N]   Assemble .asm, then execute (recommended workflow)
  --build_trace [N] Assemble .asm, then trace with debug directives
  --run [N]         Execute a pre-compiled .COM binary
  --trace [N]       Trace a pre-compiled .COM binary with debug directives
  --args <string>       Set PSP command tail (program arguments at 0x80)
  --events <json|file>  Inject keyboard/mouse input (inline JSON or file path)
  --screen <mode>   Enable video framebuffer (MDA, CGA40, CGA80, VGA50)
  -o <path>         Output path override (assemble/build modes)
  --help             This overview, or --help <flag> for detail

  Use --help <topic> for detailed usage and examples:
    agent86 --help asm
    agent86 --help o
    agent86 --help run
    agent86 --help trace
    agent86 --help directives
    agent86 --help args
    agent86 --help events
    agent86 --help screen

JSON SHAPES
  Assemble OK:    {"compiled":"OK","size":N,"symbols":{...}}
                  Optional: "prints":[...], "hex_dumps":[...]
  Assemble fail:  {"compiled":"FAILED","errors":[{"line":N,"source":"...","message":"..."},...]}
  Execute OK:     {"executed":"OK","instructions":N}
                  With --screen: includes "screen":{...} object
                  With VRAMOUT: includes "vram_dumps":[...] array
  Idle:           {"executed":"IDLE","instructions":N,"idle_polls":N}
                  Auto-terminates when 1000 consecutive keyboard polls return no key
                  Exit code 0 -- program reached stable idle state (screen included)
  Execute fail:   {"executed":"FAILED","error":"..."}
                  With --screen: includes "screen":{...} object
  Breakpoint:     {"executed":"BREAKPOINT","addr":N,"name":"...","instructions":N}
                  With VRAMOUT modifier: includes "screen":{...}
  Assert fail:    {"executed":"ASSERT_FAILED","addr":N,"assert":"...","actual":N,"expected":N,"instructions":N}
                  With VRAMOUT modifier: includes "screen":{...}
  Mem assert:     {"executed":"ASSERT_FAILED","addr":N,"assert":"MEM_ASSERT ...","snap_name":"...","mismatch_offset":N,"expected":N,"actual":N,"instructions":N}
)HELP" << std::flush;
}

static void helpFlagO() {
    std::cout << R"HELP(-o <path> -- override output file path

DEFAULT BEHAVIOR
  Without -o, the output path is the input filename with .com extension:
    agent86 program.asm        -> writes program.com
    agent86 test_hello.asm     -> writes test_hello.com
    agent86 src\main.asm       -> writes src\main.com

WITH -o
  agent86 program.asm -o build\out.com
  agent86 program.asm -o test.com

  The output directory must already exist. agent86 will not create directories.

EXAMPLES
  Assemble and run in one line:
    agent86 hello.asm -o hello.com && agent86 hello.com --run

  Assemble to a build directory:
    agent86 src\boot.asm -o build\boot.com

  Overwrite a previous build:
    agent86 program.asm -o program.com

NOTE
  -o only applies in assemble mode. It is ignored with --run/--trace.
)HELP" << std::flush;
}

static void helpFlagRun() {
    std::cout << R"HELP(--run [max_cycles] -- execute a .COM binary in the JIT emulator

USAGE
  agent86 <file.com> --run           Execute with default 100M cycle limit
  agent86 <file.com> --run 500       Execute with 500 cycle limit

  Loads a pre-compiled .COM binary at offset 100h into a 1MB segmented
  memory space and executes via per-instruction JIT (decode 8086 ->
  emit x64 -> call). Silent execution -- no .dbg file is loaded.

  To assemble and run in one step, use --build_run instead.

STDOUT (JSON)
  {"executed":"OK","instructions":3557}
  {"executed":"IDLE","instructions":121869,"idle_polls":1000,"screen":{...}}
  {"executed":"FAILED","error":"instruction limit exceeded"}

  IDLE: auto-terminates when 1000 consecutive keyboard polls return "no key".
  This is normal for interactive programs (TUI editors, menus) that reach their
  event loop with no input pending. Exit code 0 -- includes screen data.

STDERR
  All DOS output (INT 21h AH=01/02/06/09/40h) prints to stderr.
  With --screen, output also writes to VRAM at the cursor position.

EXAMPLES
  agent86 hello.com --run            Run a pre-compiled binary
  agent86 program.com --run 1000     Run with cycle limit
)HELP" << std::flush;
}

static void helpFlagTrace() {
    std::cout << R"HELP(--trace [max_cycles] -- directive-driven debug execution

USAGE
  agent86 <file.com> --trace         Trace with directives from .dbg file
  agent86 <file.com> --trace 500     Same, with 500 cycle limit

  Loads the .dbg file alongside a pre-compiled .COM and honors runtime
  directives placed in the assembly source. No directives = silent
  (same as --run). If .dbg is missing, degrades gracefully to silent
  execution.

  To assemble and trace in one step, use --build_trace instead.

  See --help directives for the full list of assembly directives.

TRACE OUTPUT (stderr)
  When tracing is active (between TRACE_START/TRACE_STOP), prints
  source line + hex dump before each instruction and register dump
  after execution.

JSON OUTPUT (stdout)
  {"executed":"OK","instructions":N}
  {"executed":"BREAKPOINT","addr":N,"instructions":N}
  {"executed":"ASSERT_FAILED","addr":N,"assert":"...","actual":N,"expected":N,"instructions":N}
  {"executed":"ASSERT_FAILED","addr":N,"assert":"MEM_ASSERT ...","snap_name":"...","mismatch_offset":N,...}

EXAMPLES
  agent86 prog.asm && agent86 prog.com --trace
  agent86 prog.com --trace 500       Stop after 500 instructions
)HELP" << std::flush;
}

static void helpDirectives() {
    std::cout << R"HELP(Assembly directives -- all emit zero bytes of machine code

COMPILE-TIME DIRECTIVES
  Evaluated during assembly (pass 2). Results appear in the compile JSON.

  ASSERT expr
    Fail assembly if expr evaluates to zero.
      ASSERT NUM_ENTRIES           ; fail if NUM_ENTRIES is 0
      ASSERT table_end - table     ; fail if table is empty

  ASSERT expr1, expr2
    Fail assembly if expr1 != expr2.
      ASSERT 2 + 3, 5             ; passes (5 == 5)
      ASSERT data_end - data, 10  ; fail if data section isn't exactly 10 bytes

    JSON on failure (standard compile error):
      {"compiled":"FAILED","errors":[{"line":7,"message":"ASSERT failed: 6 != 5"}]}

  PRINT "message"
  PRINT expr
  PRINT "message", expr
    Emit a message to the compile JSON "prints" array. Also echoes to stderr.
      PRINT "hello"               ; {"line":5,"text":"hello"}
      PRINT 2 + 3                 ; {"line":6,"text":"5"}
      PRINT "size = ", end - start; {"line":7,"text":"size = 42"}

    JSON output:
      {"compiled":"OK","size":N,"symbols":{...},
       "prints":[{"line":5,"text":"hello"},{"line":7,"text":"size = 42"}]}

  HEX_START / HEX_END
    Capture the bytes emitted between the two markers. The hex dump
    appears in the compile JSON "hex_dumps" array.
      HEX_START
        MOV AX, 1
        MOV BX, 2
      HEX_END

    JSON output:
      {"compiled":"OK","size":N,"symbols":{...},
       "hex_dumps":[{"addr":259,"size":6,"bytes":"B8 01 00 BB 02 00"}]}

    Errors: nesting HEX_START or omitting HEX_END fails assembly.

  SCREEN <mode>
    Declare the video mode in the assembly source. Equivalent to --screen
    on the command line. Modes: MDA, CGA40, CGA80, VGA50.
      SCREEN CGA80               ; enable 80x25 color text mode

RUNTIME DIRECTIVES
  Stored in the .dbg file at assembly time. Honored by --trace at runtime.
  Ignored by --run (which does not load .dbg).

  TRACE_START
    Begin printing trace output (source lines, hex, register dumps).

  TRACE_STOP
    Stop printing trace output.

      TRACE_START
        MOV AX, 1       ; this section is traced
        ADD AX, BX
      TRACE_STOP
        MOV CX, 3       ; this runs silently

  BREAKPOINT
  BREAKPOINT name
  BREAKPOINT name, N
    Halt execution and return BREAKPOINT JSON. Inline-only: halts at
    the address where the directive appears.
      BREAKPOINT               ; halt here on first hit
      BREAKPOINT init           ; named "init", halt on first hit
      BREAKPOINT loop_top, 5   ; halt after 5 passes (6th hit)

    JSON: {"executed":"BREAKPOINT","addr":N,"name":"init","instructions":N}

  BREAKPOINT : VRAMOUT [options]
  BREAKPOINT : REGS
  BREAKPOINT : VRAMOUT [options] : REGS
    Halt and include screen data and/or register state in BREAKPOINT JSON.
    Modifiers are colon-separated and can appear in any order.
      BREAKPOINT : VRAMOUT                ; full screen, text only
      BREAKPOINT init : VRAMOUT FULL, ATTRS  ; named, with attributes
      BREAKPOINT : VRAMOUT PARTIAL 0,0,40,12 ; with region
      BREAKPOINT : REGS                   ; register state only
      BREAKPOINT init : VRAMOUT : REGS    ; both screen and registers

    JSON: {"executed":"BREAKPOINT","addr":N,"instructions":N,
           "screen":{"mode":"CGA80",...},
           "regs":{"AX":42,"BX":0,...,"IP":300,"FL":2,"flags":"--------"}}

  ASSERT_EQ register, value
  ASSERT_EQ BYTE [address], value
  ASSERT_EQ WORD [address], value
    Halt execution if the actual value doesn't match expected.
    Checked BEFORE the instruction at that address, so place ASSERT_EQ
    after the instruction you want to verify.

      MOV AX, 42
      ASSERT_EQ AX, 42           ; passes: AX is 42

      MOV BYTE [200h], 41h
      ASSERT_EQ BYTE [200h], 41h ; passes: memory byte is 41h

      MOV WORD [300h], 1234h
      ASSERT_EQ WORD [300h], 1234h

    Registers: AX, BX, CX, DX, SP, BP, SI, DI (16-bit only).
    Multiple ASSERT_EQ at the same address are all checked.

    On failure, stderr shows:
      ASSERT_EQ FAILED at 0103: AX == 42 (actual=99)
      AX=0063 BX=0000 ...          (full register dump)
      0100: BB 63 00 B8 ...        (4 rows of memory hex)

    JSON: {"executed":"ASSERT_FAILED","addr":N,"assert":"AX == 42",
           "actual":99,"expected":42,"instructions":N}

  ASSERT_EQ reg, val : VRAMOUT [options]
  ASSERT_EQ reg, val : REGS
  ASSERT_EQ reg, val : VRAMOUT [options] : REGS
    Include screen data and/or register state in ASSERT_FAILED JSON on failure.
      ASSERT_EQ AX, 5 : VRAMOUT FULL, ATTRS
      ASSERT_EQ AX, 5 : REGS              ; registers on failure
      ASSERT_EQ AX, 5 : VRAMOUT : REGS    ; both screen and registers

  VRAMOUT
  VRAMOUT FULL
  VRAMOUT FULL, ATTRS
  VRAMOUT PARTIAL x, y, w, h [, ATTRS]
    Non-halting VRAM snapshot. Accumulates in "vram_dumps" JSON array
    (max 32 snapshots per run). Silently no-ops without --screen.

      VRAMOUT                         ; full screen, text only
      VRAMOUT FULL, ATTRS             ; full screen with attribute bytes
      VRAMOUT PARTIAL 0, 0, 40, 12   ; 40x12 region at top-left

    Defaults: FULL mode, no attributes.

    JSON: {"executed":"OK","instructions":N,
           "vram_dumps":[{"addr":280,"instr":50,
             "screen":{"mode":"CGA80",...,"lines":[...]}}]}

  REGS
    Non-halting register snapshot. Accumulates in "reg_dumps" JSON array
    (max 32 snapshots per run). Records all 16-bit registers, segment
    registers, IP, flags register, and decoded flag string.

      MOV AX, 42
      REGS                               ; snapshot all registers here

    JSON: {"executed":"OK","instructions":N,
           "reg_dumps":[{"addr":259,"instr":3,
             "regs":{"AX":42,"BX":0,"CX":0,"DX":0,"SP":65534,"BP":0,
                     "SI":0,"DI":0,"DS":0,"ES":0,"SS":0,"CS":0,
                     "IP":259,"FL":2,"flags":"--------"}}]}

  LOG "message"
  LOG "message", register
  LOG "message", BYTE [address]
  LOG "message", WORD [address]
    Non-halting runtime debug print. Accumulates in "log" JSON array
    (max 256 entries per run). Optionally logs a register or memory value.

      LOG "checkpoint alpha"             ; message only
      LOG "AX value", AX                 ; message + register value
      LOG "byte at 0x200", BYTE [0x200]  ; message + memory byte
      LOG "word at 0x202", WORD [0x202]  ; message + memory word

    Registers: AX, BX, CX, DX, SP, BP, SI, DI (16-bit only).

    JSON: {"executed":"OK","instructions":N,
           "log":[
             {"addr":259,"instr":3,"message":"checkpoint alpha"},
             {"addr":262,"instr":4,"message":"AX value","value":42,"reg":"AX"},
             {"addr":270,"instr":6,"message":"byte at 0x200","value":72,"mem_addr":512}]}

  LOG_ONCE label, "message" [, register_or_memory]
    Like LOG, but fires only once per label. Useful inside loops to log
    the first iteration without flooding the output.

      MOV CX, 100
    .loop:
      LOG_ONCE loop_entry, "entered loop", CX  ; fires once
      LOG "iteration", CX                       ; fires every time
      DEC CX
      JNZ .loop

    The label is for deduplication only -- it does not define an assembly
    symbol. Multiple LOG_ONCE with the same label only fire the first one.

  DOS_FAIL <int_num>, <ah_func> [, <error_code>]
    Arms a one-shot DOS failure. The next INT <int_num> with AH=<ah_func>
    will skip the real DOS call, set CF=1, and return AX=<error_code>.
    Default error code is 5 (access denied). One-shot: subsequent calls
    succeed normally.

      DOS_FAIL 21h, 3Ch            ; next file create fails (AX=5)
      DOS_FAIL 21h, 3Ch, 3         ; next file create fails (AX=3, path not found)
      DOS_FAIL 21h, 48h, 8         ; next memory alloc fails (AX=8, insufficient memory)

  DOS_PARTIAL <int_num>, <ah_func>, <count>
    Arms a one-shot partial result. The next INT <int_num> with AH=<ah_func>
    will skip the real DOS call, set CF=0, and return AX=<count>.
    Useful for testing partial-write handling.

      DOS_PARTIAL 21h, 40h, 10     ; next write returns only 10 bytes written
      DOS_PARTIAL 21h, 40h, 0      ; next write returns 0 bytes written

  MEM_SNAPSHOT <name>, <seg_reg>, <offset>, <length>
    Capture a named copy of <length> bytes at <seg_reg>:<offset>.
    The segment register (ES/CS/SS/DS) is resolved at runtime.
    Offset and length may be expressions (labels, EQU, arithmetic).
    Max 32 concurrent snapshots, max 65536 bytes each.

      MEM_SNAPSHOT pt_before, ES, 0, 100      ; save ES:0000..0063h
      MEM_SNAPSHOT buf_snap, DS, my_buf, 64   ; save DS:my_buf, 64 bytes
      MEM_SNAPSHOT hdr, DS, buf+4, BUF_LEN    ; label offset + EQU length

  MEM_ASSERT <name>, <seg_reg>, <offset>, <length>
    Compare current memory at <seg_reg>:<offset> against a previously
    captured snapshot with the same name. Halts with ASSERT_FAILED on
    the first byte that differs, reporting the offset, expected, and
    actual values. Also halts if no snapshot with <name> was captured.

      MEM_ASSERT pt_before, ES, 0, 100       ; verify region unchanged
      MEM_ASSERT buf_snap, DS, my_buf, 64    ; compare against saved copy

    Typical pattern -- verify a call doesn't corrupt a region:
      MEM_SNAPSHOT s, DS, table, 100
      CALL risky_function
      MEM_ASSERT s, DS, table, 100            ; passes if table unchanged

    JSON on mismatch:
      {"executed":"ASSERT_FAILED","addr":N,"assert":"MEM_ASSERT s",
       "snap_name":"s","mismatch_offset":42,"expected":255,"actual":0,
       "instructions":N}

    JSON when snapshot not found:
      {"executed":"ASSERT_FAILED","addr":N,"assert":"MEM_ASSERT s (no snapshot)",
       "snap_name":"s","instructions":N}
)HELP" << std::flush;
}

static void helpBuildRun() {
    std::cout << R"HELP(--build_run [N] / --build_trace [N] -- assemble and execute in one step

USAGE
  agent86 <file.asm> --build_run         Assemble + run (default 100M cycle limit)
  agent86 <file.asm> --build_run 500     Assemble + run with 500 cycle limit
  agent86 <file.asm> --build_trace       Assemble + trace with debug directives
  agent86 <file.asm> --build_trace 500   Same, with 500 cycle limit

  Assembles the .asm file, writes .com and .dbg files, emits the
  compile JSON, then immediately loads and executes the .com.
  If assembly fails, only the compile error JSON is emitted.

  --build_trace loads the .dbg and honors runtime directives
  (TRACE_START, BREAKPOINT, ASSERT_EQ). --build_run ignores .dbg.

STDOUT (JSON)
  Two JSON objects, one per line:
    {"compiled":"OK","size":42,"symbols":{...}}
    {"executed":"OK","instructions":150}

  On compile failure, only the first line is emitted:
    {"compiled":"FAILED","errors":[...]}

STDERR
  All DOS output (INT 21h AH=01/02/06/09/40h) and PRINT directives go to stderr.
  With --screen, output also writes to VRAM at the cursor position.

EXAMPLES
  agent86 hello.asm --build_run          Assemble and run
  agent86 prog.asm --build_trace         Assemble and trace
  agent86 prog.asm --build_run 1000      Assemble and run with cycle limit
  agent86 prog.asm --build_trace 500     Assemble and trace, stop after 500
)HELP" << std::flush;
}

static void helpAsm() {
    std::cout << R"HELP(Assembly language reference -- writing .COM programs for agent86

CODE STRUCTURE
  Every .COM program MUST start with ORG 100h. This is required because
  .COM binaries are loaded at offset 100h in memory. Without it, all
  label addresses will be wrong and the program will crash.

    ORG 100h              ; REQUIRED -- .COM loads at 100h
        JMP main          ; skip over data

    msg: DB 'Hello$'      ; data section

    main:
        MOV DX, msg       ; address is correct because of ORG 100h
        MOV AH, 09h
        INT 21h
        MOV AX, 4C00h     ; exit
        INT 21h

  Programs without ORG 100h will assemble but produce wrong addresses.

INCLUDE
  Textual file inclusion, expanded recursively before assembly.
    INCLUDE "constants.inc"    ; quoted path
    INCLUDE TUI\helpers.inc    ; unquoted path (also supported)

MACROS
  MASM-compatible MACRO/ENDM with parameters:
    PushAll MACRO
        PUSH AX
        PUSH BX
    ENDM
    PushAll                    ; expands to PUSH AX / PUSH BX

  Parameterized macros:
    WriteChar MACRO ch
        MOV DL, ch
        MOV AH, 02h
        INT 21h
    ENDM
    WriteChar 'A'              ; DL = 'A'

  IRP (Indefinite Repeat) -- standalone or inside macros:
    IRP reg, <AX, BX, CX, DX>
        PUSH reg
    ENDM                       ; expands to 4 PUSH instructions

DOS SERVICES
  INT 21h -- 33 subfunctions: console I/O, file system, directory,
  memory management, FindFirst/FindNext. Key services:
    AH=02h  Print character    DL = character
    AH=09h  Print string       DS:DX = addr of '$'-terminated string
    AH=3Ch  Create file        AH=3Dh open, 3Eh close, 3Fh read, 40h write
    AH=48h  Alloc memory       BX = paragraphs → AX = segment (>= 0x1000)
    AH=4Ch  Exit program       AL = return code
    INT 20h also exits (no return code)

  All INT 21h functions respect segment registers: DS:DX for paths and
  buffers, ES:DI for rename new path, DS:SI for get CWD. Programs can
  set DS to an allocated segment and do file I/O directly into it.
  AH=48h allocates above the COM segment (>= 0x1000, physical 0x10000).

  INT 10h -- Video BIOS (12 subfunctions, requires --screen)
  INT 16h -- Keyboard BIOS (requires --events for input)
  INT 33h -- Mouse driver (6 subfunctions, --events for mouse injection)

  With --screen, INT 21h text output also writes to the video framebuffer.

DATA DIRECTIVES
  DB value, value, ...    Define bytes (numbers, strings, expressions)
  DW value, value, ...    Define words (16-bit, little-endian)
  RESB count              Reserve count zero bytes
  RESW count              Reserve count zero words
  EQU                     Named constant:  MAX EQU 100

  SECTION .bss            Switch to BSS (uninitialized data) section.
    After this directive, RESB/RESW advance the location counter but
    emit no bytes to the .COM file. Labels are assigned addresses as
    usual. DB/DW and instructions are errors in BSS. Must be the final
    section -- once entered, persists until end of file.

      ORG 100h
          JMP main
      msg: DB 'Hello$'
      main:
          MOV DX, msg
          HLT
      SECTION .bss
      buffer:  RESB 4000     ; 0 bytes in file, valid at runtime
      counter: RESW 1

    JSON: {"compiled":"OK","size":N,"bss_start":N,"bss_end":N,...}

LABELS
  Global:   main:          Defines address, sets scope for local labels
  Local:    .loop:         Dot-prefixed, scoped to the preceding global label
  PROC/ENDP:              my_func PROC ... ENDP (syntactic, no code emitted)

REGISTERS
  General:  AX BX CX DX SP BP SI DI  (16-bit)
            AL AH BL BH CL CH DL DH  (8-bit halves)
  Segment:  CS DS ES SS
  Flags:    CF ZF SF OF DF PF AF (set by ALU operations)

MEMORY OPERANDS
  [BX]  [SI]  [DI]  [BP]           Base/index registers
  [BX+SI]  [BX+DI]  [BP+SI]  [BP+DI]  Combined
  [BX+10h]  [label]  [BX+label]   With displacement
  BYTE [addr]  WORD [addr]         Size override when ambiguous
  ES:[DI]  CS:[BX+5]  SS:[BP-2]   Segment override prefix
  BYTE ES:[DI+2]                   Combined size + segment override

EXPRESSIONS
  Arithmetic:   + - * / %
  Bitwise:      & | ^ ~ << >>
  Parentheses:  (expr)
  Symbols:      Labels, EQU names, $ (current address)
  Example:      DB (data_end - data_start) * 2 + 1

INSTRUCTION SET
  MOV, ADD, ADC, SUB, SBB, AND, OR, XOR, NOT, NEG, INC, DEC,
  CMP, TEST, SHL, SHR, SAR, ROL, ROR, RCL, RCR,
  MUL, IMUL, DIV, IDIV, CBW, CWD,
  PUSH, POP, PUSHA, POPA, PUSHF, POPF,
  JMP, CALL, RET, RETF, IRET, INT, INTO,
  Jcc: JZ/JE, JNZ/JNE, JB/JC, JAE/JNC, JBE/JNA, JA/JNBE,
       JL/JNGE, JGE/JNL, JLE/JNG, JG/JNLE, JO, JNO, JS, JNS,
       JP/JPE, JNP/JPO, JCXZ,
  LOOP, LOOPE/LOOPZ, LOOPNE/LOOPNZ,
  LEA, LDS, LES, XCHG, XLAT, LAHF, SAHF,
  REP MOVSB/MOVSW, REP STOSB/STOSW, REP LODSB/LODSW,
  REPE CMPSB/CMPSW, REPNE SCASB/SCASW,
  CLC, STC, CLD, STD, CLI, STI, CMC,
  DAA, DAS, AAA, AAS, AAM, AAD,
  IN, OUT, NOP, HLT, WAIT, LOCK

WORKFLOW
  agent86 prog.asm --build_run       Assemble + run in one step (recommended)
  agent86 prog.asm --build_trace     Assemble + trace with debug directives
  agent86 prog.asm                   Assemble only
  agent86 prog.com --run             Execute pre-compiled .COM
  agent86 prog.com --trace           Trace pre-compiled .COM

  Parse the JSON stdout to check results. Fix errors and repeat.
  See --help directives for ASSERT, PRINT, HEX_START, ASSERT_EQ, VRAMOUT,
    MEM_SNAPSHOT, MEM_ASSERT, DOS_FAIL, DOS_PARTIAL, LOG, REGS.
)HELP" << std::flush;
}

static void helpEvents() {
    std::cout << R"HELP(--events <json|file> -- inject keyboard and mouse input

USAGE
  Inline JSON (argument starts with '['):
    agent86 prog.com --run --events '[{"keys":"Hello\r"}]'
    agent86 prog.asm --build_run --events '[{"keys":"A"},{"mouse":{"x":40,"y":12}},{"keys":"B"}]'

  From a JSON file (argument is a file path):
    agent86 prog.com --run --events events.json
    agent86 prog.com --trace --events my_test/input.json

  Works with --run, --trace, --build_run, and --build_trace.
  Detection: if the argument starts with '[' it is parsed as inline JSON,
  otherwise it is treated as a file path and its contents are read.

EVENT FORMAT
  A JSON array of event objects. Two modes:

  SEQUENTIAL (no "on" trigger) -- consumed in order:
    {"keys":"..."}                   Keyboard input
    {"mouse":{"x":N,"y":N}}         Mouse position/button update
    {"mouse":{"buttons":1,"x":N,"y":N}}  Mouse with button press

  TRIGGERED (legacy, with "on") -- fired at specific counts:
    {"on":"read:1","keys":"..."}     Fire on Nth INT 16h read
    {"on":"poll:5","keys":"..."}     Fire on Nth INT 16h poll

  Both modes can coexist in the same array.

SEQUENTIAL EVENTS
  Events are consumed in order. Mouse events act as lazy barriers:
    1. Keys events inject when the keyboard buffer is empty
    2. Mouse events BLOCK until INT 33h AX=0003h is called
    3. On INT 33h query: mouse state is applied, cursor advances,
       and the next keys batch is injected (if buffer is empty)

  This ensures correct ordering in TUI main loops:
    Stream: [keys:"A", mouse:{click}, keys:"B"]
    -> "A" injected at init
    -> program reads "A" via INT 16h
    -> program queries mouse via INT 33h -> click applied, "B" injected
    -> program reads "B" via INT 16h

  Example -- type "A", click at (40,12), then type "B":
    [
      {"keys":"A"},
      {"mouse":{"buttons":1,"x":40,"y":12}},
      {"keys":"B"}
    ]

MOUSE OBJECT
  "x"       -- horizontal pixel position (required)
  "y"       -- vertical pixel position (required)
  "buttons" -- button bitmask (optional, default 0)
               1=left, 2=right, 4=middle

  Mouse state is applied lazily on INT 33h AX=0003h (get position).
  In text mode, character position = pixel / 8.

TRIGGERED EVENTS
  read:N   Fires on the Nth INT 16h AH=00h call (1-based)
  poll:N   Fires on the Nth INT 16h AH=01h call (1-based)

  When an event fires, its keys are injected into the keyboard buffer.
  Multiple events can share the same trigger -- all matching events fire.

KEYS STRING
  Standard JSON escapes: \n \r \t \\ \"
  Modifier toggles (use \\S in JSON, becomes \S after JSON parsing):
    \S   Toggle Shift (bits 0+1 of modifier byte)
    \C   Toggle Ctrl (bit 2)
    \A   Toggle Alt (bit 3)

  Modifiers toggle on first occurrence and off on second. Each keystroke
  is stamped with the active modifier state at injection time.

INT 16h FUNCTIONS
  AH=00h  Blocking read -- dequeue one key, AH=scancode AL=ASCII.
          Empty buffer after event check = FAILED JSON error.
  AH=01h  Poll -- peek without consuming. ZF=0 if key available, ZF=1 if empty.
  AH=02h  Shift status -- return current modifier byte in AL.

MODIFIER BYTE (standard BIOS 0040:0017)
  Bit 0 (1)   Right Shift     Bit 4 (16)  Scroll Lock
  Bit 1 (2)   Left Shift      Bit 5 (32)  Num Lock
  Bit 2 (4)   Ctrl            Bit 6 (64)  Caps Lock
  Bit 3 (8)   Alt             Bit 7 (128) Insert mode

EXAMPLES
  Sequential keys (simplest form):
    --events '[{"keys":"Hello\r"}]'

  Mouse click then keystroke:
    --events '[{"mouse":{"buttons":1,"x":80,"y":40}},{"keys":"X"}]'

  Interleaved keys and mouse:
    --events '[{"keys":"A"},{"mouse":{"x":40,"y":12}},{"keys":"B"}]'

  Legacy triggered format (still works):
    --events '[{"on":"read:1","keys":"Y"}]'

  Mixed triggered + sequential:
    --events '[{"on":"read:1","keys":"init"},{"keys":"hello"},{"mouse":{"x":10,"y":5}}]'

  From a JSON file (file contains the same array format):
    --events events.json

ERROR JSON
  {"executed":"FAILED","error":"INT 16h AH=00: blocking read with no keys available (read #1)"}
)HELP" << std::flush;
}

static void helpScreen() {
    std::cout << R"HELP(--screen <mode> -- enable video framebuffer for VRAM rendering

USAGE
  agent86 prog.com --run --screen CGA80
  agent86 prog.asm --build_run --screen CGA80
  agent86 prog.asm --build_trace --screen VGA50

  Enables a text-mode video framebuffer. Programs can write directly to
  VRAM (segment B800h for CGA/VGA, B000h for MDA), use INT 10h BIOS
  services, or use INT 21h text output (AH=01h/02h/06h/09h/40h) -- all
  three methods write to the framebuffer at the cursor position. At
  program exit, the framebuffer is rendered as a JSON "screen" object.

  Can also be enabled via the SCREEN assembly directive:
    SCREEN CGA80

MODES
  MDA     80x25  mono   VRAM at B000:0000
  CGA40   40x25  color  VRAM at B800:0000
  CGA80   80x25  color  VRAM at B800:0000
  VGA50   80x50  color  VRAM at B800:0000

VRAM LAYOUT
  Each cell is 2 bytes: [character][attribute], row-major order.
  To write 'A' with white-on-black at row 0, col 0:
    MOV AX, 0B800h
    MOV ES, AX
    MOV WORD [ES:0], 0741h  ; char='A' (41h), attr=07h (white on black)

INT 10h SERVICES (available with --screen)
  AH=00h  Set video mode     AL = mode number
  AH=01h  Set cursor shape   CH = start scan line (bit 5 = hide), CL = end
  AH=02h  Set cursor pos     DH = row, DL = col, BH = page (ignored)
  AH=03h  Get cursor pos     Returns DH=row, DL=col, CH/CL=cursor shape
  AH=06h  Scroll up          AL = lines, BH = fill attr, CX/DX = window
  AH=07h  Scroll down        AL = lines, BH = fill attr, CX/DX = window
  AH=08h  Read char+attr     Returns AH = attr, AL = char at cursor
  AH=09h  Write char+attr    AL = char, BL = attr, CX = count
  AH=0Ah  Write char only    AL = char, CX = count (keeps existing attr)
  AH=0Eh  Teletype output    AL = char (handles CR/LF/BS/TAB, auto-scroll)
  AH=0Fh  Get video mode     Returns AL = mode, AH = cols, BH = page

INT 33h SERVICES (mouse driver)
  AX=0000h  Reset/detect      Returns AX=FFFFh if mouse present
  AX=0001h  Show cursor
  AX=0002h  Hide cursor
  AX=0003h  Get position      BX=buttons, CX=x, DX=y
  AX=0007h  Set H range       CX=min, DX=max
  AX=0008h  Set V range       CX=min, DX=max

JSON OUTPUT
  {"executed":"OK","instructions":N,
   "screen":{"mode":"CGA80","cols":80,"rows":25,
             "cursor":[0,5],"cursor_hidden":false,
             "lines":["Hello","","",...]}}

  Lines are right-trimmed. The "lines" array always has exactly `rows` entries.
  Screen data is also included in FAILED JSON when --screen is active.

EXAMPLES
  Direct VRAM write:
    agent86 vram_test.asm --build_run --screen CGA80

  TUI application with keyboard + mouse:
    agent86 tui_app.asm --build_run --screen CGA80 --events '[...]'
)HELP" << std::flush;
}

static void helpArgs() {
    std::cout << R"HELP(--args <string> -- set PSP command tail (program arguments)

USAGE
  agent86 prog.com --run --args "hello world"
  agent86 prog.asm --build_run --args "/v /f output.txt"
  agent86 prog.asm --build_trace --args "test"

DESCRIPTION
  Writes the argument string into the PSP command tail at offset 0x80,
  exactly as DOS does when launching a .COM program with arguments.

  Memory layout at runtime:
    [0x80]       = length byte (space + args, max 126)
    [0x81]       = 0x20 (leading space, per DOS convention)
    [0x82..0x80+len] = argument characters
    [0x81+len]   = 0x0D (CR terminator)

  The program can read SI=0x80, LODSB to get the length, then read
  the argument bytes starting at 0x82 (skipping the leading space).
  Max argument length: 125 characters (126 with the leading space).

EXAMPLE (reading args in assembly)
    MOV SI, 0x80
    LODSB               ; AL = command tail length
    OR  AL, AL
    JZ  no_args
    MOV CL, AL
    XOR CH, CH
    ; SI now points to 0x81 (the space + args)
    INC SI               ; skip leading space, SI -> first arg char
    DEC CX
    ; CX = number of arg chars, SI = pointer to first char
)HELP" << std::flush;
}

static bool printHelp(const std::string& topic) {
    if (topic.empty())   { helpOverview();   return true; }
    if (topic == "o")    { helpFlagO();      return true; }
    if (topic == "run")  { helpFlagRun();    return true; }
    if (topic == "trace"){ helpFlagTrace();  return true; }
    if (topic == "build_run" || topic == "build_trace" || topic == "build") {
        helpBuildRun(); return true;
    }
    if (topic == "asm" || topic == "assembly" || topic == "syntax") {
        helpAsm(); return true;
    }
    if (topic == "directives" || topic == "directive" || topic == "dir") {
        helpDirectives(); return true;
    }
    if (topic == "args" || topic == "arguments" || topic == "psp") {
        helpArgs(); return true;
    }
    if (topic == "events" || topic == "keyboard" || topic == "keys" || topic == "input") {
        helpEvents(); return true;
    }
    if (topic == "screen" || topic == "video" || topic == "vram" || topic == "framebuffer") {
        helpScreen(); return true;
    }
    if (topic == "help") { helpOverview();   return true; }
    std::cout << "Unknown topic: " << topic << "\n\n"
              << "Available topics: asm, args, o, build_run, run, trace, directives, events, screen\n"
              << "Usage: agent86 --help <topic>\n";
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::vector<AsmError> errs = {{0, "", "no input file specified"}};
        printFailedJson(errs);
        return 1;
    }

    // Parse arguments
    std::string input_file;
    std::string output_file;
    std::string help_topic;
    std::string events_arg;
    std::string screen_mode;
    std::string program_args;
    bool run_mode = false;
    bool build_run_mode = false;
    bool help_mode = false;
    RunMode mode = RunMode::RUN;
    uint64_t max_cycles = 100000000;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "-?") {
            help_mode = true;
            // Check for optional topic argument
            if (i + 1 < argc) {
                help_topic = argv[++i];
                // Strip leading dashes: --run -> run, -o -> o
                while (!help_topic.empty() && help_topic[0] == '-')
                    help_topic.erase(0, 1);
                // Lowercase for matching
                for (auto& c : help_topic) c = (char)tolower((unsigned char)c);
            }
        } else if (arg == "--run") {
            run_mode = true;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
                max_cycles = std::stoull(argv[++i]);
            }
        } else if (arg == "--trace") {
            run_mode = true;
            mode = RunMode::TRACE;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
                max_cycles = std::stoull(argv[++i]);
            }
        } else if (arg == "--build_run") {
            build_run_mode = true;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
                max_cycles = std::stoull(argv[++i]);
            }
        } else if (arg == "--build_trace") {
            build_run_mode = true;
            mode = RunMode::TRACE;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
                max_cycles = std::stoull(argv[++i]);
            }
        } else if (arg == "--args" && i + 1 < argc) {
            program_args = argv[++i];
        } else if (arg == "--events" && i + 1 < argc) {
            events_arg = argv[++i];
        } else if (arg == "--screen" && i + 1 < argc) {
            screen_mode = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (input_file.empty()) {
            input_file = arg;
        }
    }

    if (help_mode) {
        return printHelp(help_topic) ? 0 : 1;
    }

    if (input_file.empty()) {
        std::vector<AsmError> errs = {{0, "", "no input file specified"}};
        printFailedJson(errs);
        return 1;
    }

    // --run/--trace mode: execute a pre-compiled .COM file
    if (run_mode) {
        std::ifstream ifs(input_file, std::ios::binary);
        if (!ifs) {
            std::cout << "{\"executed\":\"FAILED\",\"error\":\"cannot open file\"}" << std::endl;
            return 1;
        }
        std::vector<uint8_t> comData(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        ifs.close();

        JitEngine jit;
        if (!program_args.empty()) {
            jit.setArgs(program_args);
        }
        if (!events_arg.empty()) {
            std::vector<KeyEvent> triggered;
            std::vector<InputEvent> sequential;
            std::string err;
            loadEventsArg(events_arg, triggered, sequential, err);
            if (!err.empty()) {
                std::cout << "{\"executed\":\"FAILED\",\"error\":\"" << jsonEscape(err) << "\"}" << std::endl;
                return 1;
            }
            jit.setEvents(std::move(triggered), std::move(sequential));
        }
        if (!screen_mode.empty()) {
            if (screen_mode != "MDA" && screen_mode != "CGA40" &&
                screen_mode != "CGA80" && screen_mode != "VGA50") {
                std::cout << "{\"executed\":\"FAILED\",\"error\":\"unknown screen mode: "
                          << jsonEscape(screen_mode) << " (valid: MDA, CGA40, CGA80, VGA50)\"}" << std::endl;
                return 1;
            }
            jit.setScreen(screen_mode);
        }
        std::string dbg_path;
        if (mode == RunMode::TRACE) {
            dbg_path = input_file;
            auto dot = dbg_path.rfind('.');
            if (dot != std::string::npos) {
                dbg_path = dbg_path.substr(0, dot);
            }
            dbg_path += ".dbg";
        }
        return jit.run(comData.data(), comData.size(), mode, dbg_path, max_cycles);
    }

    // --build_run/--build_trace mode: assemble .asm then execute
    if (build_run_mode) {
        // Read source
        std::ifstream ifs(input_file, std::ios::binary);
        if (!ifs) {
            std::vector<AsmError> errs = {{0, "", "cannot open input file: " + input_file}};
            printFailedJson(errs);
            return 1;
        }
        std::stringstream bss;
        bss << ifs.rdbuf();
        std::string source = bss.str();
        ifs.close();

        // Assemble
        Assembler assembler;
        std::vector<uint8_t> asmOutput;
        if (!assembler.assemble(source, asmOutput, input_file)) {
            printFailedJson(assembler.errors());
            return 1;
        }

        // Derive .com path
        std::string com_file = output_file.empty() ? input_file : output_file;
        auto dot = com_file.rfind('.');
        if (dot != std::string::npos) {
            com_file = com_file.substr(0, dot);
        }
        com_file += ".com";

        // Write .com
        {
            std::ofstream ofs(com_file, std::ios::binary);
            if (!ofs) {
                std::vector<AsmError> errs = {{0, "", "cannot open output file: " + com_file}};
                printFailedJson(errs);
                return 1;
            }
            ofs.write(reinterpret_cast<const char*>(asmOutput.data()), asmOutput.size());
        }

        // Write .dbg
        {
            std::string dbg_file = com_file;
            auto ddot = dbg_file.rfind('.');
            if (ddot != std::string::npos) {
                dbg_file = dbg_file.substr(0, ddot);
            }
            dbg_file += ".dbg";

            std::ofstream dbg(dbg_file);
            if (dbg) {
                dbg << "{\"source_map\":[\n";
                auto& entries = assembler.debugEntries();
                for (size_t i = 0; i < entries.size(); i++) {
                    if (i > 0) dbg << ",\n";
                    dbg << "{\"addr\":" << entries[i].addr
                        << ",\"file\":\"" << jsonEscape(entries[i].file)
                        << "\",\"line\":" << entries[i].line
                        << ",\"source\":\"" << jsonEscape(entries[i].source) << "\"}";
                }
                dbg << "\n],\"symbols\":{";
                auto syms = assembler.symbols().dump();
                bool first = true;
                for (auto& kv : syms) {
                    if (!first) dbg << ",";
                    first = false;
                    dbg << "\"" << jsonEscape(kv.first) << "\":{\"addr\":"
                        << kv.second.value << ",\"type\":\""
                        << (kv.second.is_equ ? "equ" : "label") << "\"}";
                }
                dbg << "},\"directives\":[";
                auto& directives = assembler.debugDirectives();
                for (size_t i = 0; i < directives.size(); i++) {
                    if (i > 0) dbg << ",";
                    const char* type_str = "unknown";
                    switch (directives[i].type) {
                        case DebugDirective::TRACE_START: type_str = "trace_start"; break;
                        case DebugDirective::TRACE_STOP:  type_str = "trace_stop"; break;
                        case DebugDirective::BREAKPOINT:  type_str = "breakpoint"; break;
                        case DebugDirective::ASSERT_EQ:   type_str = "assert_eq"; break;
                        case DebugDirective::VRAMOUT:     type_str = "vramout"; break;
                        case DebugDirective::REGS:        type_str = "regs"; break;
                        case DebugDirective::LOG:         type_str = "log"; break;
                        case DebugDirective::LOG_ONCE:    type_str = "log_once"; break;
                        case DebugDirective::DOS_FAIL:    type_str = "dos_fail"; break;
                        case DebugDirective::DOS_PARTIAL: type_str = "dos_partial"; break;
                        case DebugDirective::MEM_SNAPSHOT: type_str = "mem_snapshot"; break;
                        case DebugDirective::MEM_ASSERT:   type_str = "mem_assert"; break;
                    }
                    dbg << "{\"type\":\"" << type_str
                        << "\",\"addr\":" << directives[i].addr
                        << ",\"count\":" << directives[i].count;
                    if (!directives[i].label.empty()) {
                        dbg << ",\"name\":\"" << jsonEscape(directives[i].label) << "\"";
                    }
                    if (directives[i].type == DebugDirective::MEM_SNAPSHOT ||
                        directives[i].type == DebugDirective::MEM_ASSERT) {
                        dbg << ",\"snap_name\":\"" << jsonEscape(directives[i].snap_name) << "\""
                            << ",\"snap_seg\":" << directives[i].snap_seg
                            << ",\"snap_offset\":" << directives[i].snap_offset
                            << ",\"snap_length\":" << directives[i].snap_length;
                    }
                    if (directives[i].type == DebugDirective::LOG ||
                        directives[i].type == DebugDirective::LOG_ONCE) {
                        dbg << ",\"message\":\"" << jsonEscape(directives[i].message) << "\"";
                        if (!directives[i].log_once_label.empty()) {
                            dbg << ",\"once_label\":\"" << jsonEscape(directives[i].log_once_label) << "\"";
                        }
                        if (directives[i].check_kind != DebugDirective::CHECK_NONE) {
                            const char* check_str = "none";
                            switch (directives[i].check_kind) {
                                case DebugDirective::CHECK_REG:      check_str = "reg"; break;
                                case DebugDirective::CHECK_MEM_BYTE:  check_str = "mem_byte"; break;
                                case DebugDirective::CHECK_MEM_WORD:  check_str = "mem_word"; break;
                                default: break;
                            }
                            dbg << ",\"check\":\"" << check_str << "\"";
                            if (directives[i].check_kind == DebugDirective::CHECK_REG) {
                                dbg << ",\"reg\":\"" << directives[i].reg_name << "\""
                                    << ",\"reg_index\":" << directives[i].reg_index;
                            } else {
                                dbg << ",\"mem_addr\":" << directives[i].mem_addr;
                                if (directives[i].mem_seg >= 0)
                                    dbg << ",\"mem_seg\":" << directives[i].mem_seg;
                            }
                        }
                    }
                    if (directives[i].type == DebugDirective::ASSERT_EQ) {
                        const char* check_str = "none";
                        switch (directives[i].check_kind) {
                            case DebugDirective::CHECK_REG:      check_str = "reg"; break;
                            case DebugDirective::CHECK_MEM_BYTE:  check_str = "mem_byte"; break;
                            case DebugDirective::CHECK_MEM_WORD:  check_str = "mem_word"; break;
                            default: break;
                        }
                        dbg << ",\"check\":\"" << check_str << "\"";
                        if (directives[i].check_kind == DebugDirective::CHECK_REG) {
                            dbg << ",\"reg\":\"" << directives[i].reg_name << "\""
                                << ",\"reg_index\":" << directives[i].reg_index;
                        } else {
                            dbg << ",\"mem_addr\":" << directives[i].mem_addr;
                            if (directives[i].mem_seg >= 0)
                                dbg << ",\"mem_seg\":" << directives[i].mem_seg;
                        }
                        dbg << ",\"expected\":" << directives[i].expected;
                    }
                    if (directives[i].vramout.active) {
                        dbg << ",\"vramout\":{\"full\":"
                            << (directives[i].vramout.full ? "true" : "false")
                            << ",\"attrs\":"
                            << (directives[i].vramout.attrs ? "true" : "false");
                        if (!directives[i].vramout.full) {
                            dbg << ",\"x\":" << directives[i].vramout.x
                                << ",\"y\":" << directives[i].vramout.y
                                << ",\"w\":" << directives[i].vramout.w
                                << ",\"h\":" << directives[i].vramout.h;
                        }
                        dbg << "}";
                    }
                    if (directives[i].regs) {
                        dbg << ",\"regs\":true";
                    }
                    if (directives[i].type == DebugDirective::DOS_FAIL ||
                        directives[i].type == DebugDirective::DOS_PARTIAL) {
                        dbg << ",\"int_num\":" << (int)directives[i].int_num
                            << ",\"ah_func\":" << (int)directives[i].ah_func;
                        if (directives[i].type == DebugDirective::DOS_FAIL)
                            dbg << ",\"fail_code\":" << directives[i].fail_code;
                        else
                            dbg << ",\"partial_count\":" << directives[i].partial_count;
                    }
                    dbg << "}";
                }
                dbg << "]";
                if (!assembler.screenMode().empty()) {
                    dbg << ",\"screen\":\"" << assembler.screenMode() << "\"";
                }
                dbg << "}\n";
            }
        }

        // Emit compile JSON
        std::cout << "{\"compiled\":\"OK\",\"size\":" << asmOutput.size();
        if (assembler.bssStart() >= 0) {
            std::cout << ",\"bss_start\":" << assembler.bssStart()
                      << ",\"bss_end\":" << assembler.bssEnd();
        }
        std::cout << ",\"symbols\":{";
        auto syms = assembler.symbols().dump();
        bool first = true;
        for (auto& kv : syms) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "\"" << jsonEscape(kv.first) << "\":{\"addr\":" << kv.second.value
                      << ",\"type\":\"" << (kv.second.is_equ ? "equ" : "label") << "\"}";
        }
        std::cout << "}";
        auto& prints = assembler.prints();
        if (!prints.empty()) {
            std::cout << ",\"prints\":[";
            for (size_t i = 0; i < prints.size(); i++) {
                if (i > 0) std::cout << ",";
                std::cout << "{\"line\":" << prints[i].line
                          << ",\"text\":\"" << jsonEscape(prints[i].text) << "\"}";
            }
            std::cout << "]";
        }
        auto& hex_dumps = assembler.hexDumps();
        if (!hex_dumps.empty()) {
            std::cout << ",\"hex_dumps\":[";
            for (size_t i = 0; i < hex_dumps.size(); i++) {
                if (i > 0) std::cout << ",";
                uint16_t start = hex_dumps[i].start_addr;
                uint16_t end = hex_dumps[i].end_addr;
                uint16_t size = end - start;
                int buf_offset = start - (int)assembler.origin();
                std::cout << "{\"addr\":" << start
                          << ",\"size\":" << size
                          << ",\"bytes\":\"";
                for (int b = 0; b < size && (buf_offset + b) < (int)asmOutput.size(); b++) {
                    if (b > 0) std::cout << " ";
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%02X", asmOutput[buf_offset + b]);
                    std::cout << hex;
                }
                std::cout << "\"}";
            }
            std::cout << "]";
        }
        std::cout << "}" << std::endl;

        // Now run the .com file
        std::ifstream cfs(com_file, std::ios::binary);
        if (!cfs) {
            std::cout << "{\"executed\":\"FAILED\",\"error\":\"cannot open file\"}" << std::endl;
            return 1;
        }
        std::vector<uint8_t> comData(
            (std::istreambuf_iterator<char>(cfs)),
            std::istreambuf_iterator<char>());
        cfs.close();

        JitEngine jit;
        if (!program_args.empty()) {
            jit.setArgs(program_args);
        }
        if (!events_arg.empty()) {
            std::vector<KeyEvent> triggered;
            std::vector<InputEvent> sequential;
            std::string err;
            loadEventsArg(events_arg, triggered, sequential, err);
            if (!err.empty()) {
                std::cout << "{\"executed\":\"FAILED\",\"error\":\"" << jsonEscape(err) << "\"}" << std::endl;
                return 1;
            }
            jit.setEvents(std::move(triggered), std::move(sequential));
        }
        if (!screen_mode.empty()) {
            if (screen_mode != "MDA" && screen_mode != "CGA40" &&
                screen_mode != "CGA80" && screen_mode != "VGA50") {
                std::cout << "{\"executed\":\"FAILED\",\"error\":\"unknown screen mode: "
                          << jsonEscape(screen_mode) << " (valid: MDA, CGA40, CGA80, VGA50)\"}" << std::endl;
                return 1;
            }
            jit.setScreen(screen_mode);
        } else if (!assembler.screenMode().empty()) {
            jit.setScreen(assembler.screenMode());
        }
        std::string dbg_path;
        if (mode == RunMode::TRACE) {
            dbg_path = com_file;
            auto ddot = dbg_path.rfind('.');
            if (ddot != std::string::npos) {
                dbg_path = dbg_path.substr(0, ddot);
            }
            dbg_path += ".dbg";
        }
        return jit.run(comData.data(), comData.size(), mode, dbg_path, max_cycles);
    }

    // Default: assemble mode
    // Default output: input with .com extension
    if (output_file.empty()) {
        output_file = input_file;
        auto dot = output_file.rfind('.');
        if (dot != std::string::npos) {
            output_file = output_file.substr(0, dot);
        }
        output_file += ".com";
    }

    // Read input file
    std::ifstream ifs(input_file, std::ios::binary);
    if (!ifs) {
        std::vector<AsmError> errs = {{0, "", "cannot open input file: " + input_file}};
        printFailedJson(errs);
        return 1;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();
    ifs.close();

    // Assemble
    Assembler assembler;
    std::vector<uint8_t> output;
    if (!assembler.assemble(source, output, input_file)) {
        printFailedJson(assembler.errors());
        return 1;
    }

    // Write output
    std::ofstream ofs(output_file, std::ios::binary);
    if (!ofs) {
        std::vector<AsmError> errs = {{0, "", "cannot open output file: " + output_file}};
        printFailedJson(errs);
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(output.data()), output.size());
    ofs.close();

    // Write .dbg file alongside .com
    {
        std::string dbg_file = output_file;
        auto dot = dbg_file.rfind('.');
        if (dot != std::string::npos) {
            dbg_file = dbg_file.substr(0, dot);
        }
        dbg_file += ".dbg";

        std::ofstream dbg(dbg_file);
        if (dbg) {
            dbg << "{\"source_map\":[\n";
            auto& entries = assembler.debugEntries();
            for (size_t i = 0; i < entries.size(); i++) {
                if (i > 0) dbg << ",\n";
                dbg << "{\"addr\":" << entries[i].addr
                    << ",\"file\":\"" << jsonEscape(entries[i].file)
                    << "\",\"line\":" << entries[i].line
                    << ",\"source\":\"" << jsonEscape(entries[i].source) << "\"}";
            }
            dbg << "\n],\"symbols\":{";
            auto syms = assembler.symbols().dump();
            bool first = true;
            for (auto& kv : syms) {
                if (!first) dbg << ",";
                first = false;
                dbg << "\"" << jsonEscape(kv.first) << "\":{\"addr\":"
                    << kv.second.value << ",\"type\":\""
                    << (kv.second.is_equ ? "equ" : "label") << "\"}";
            }
            dbg << "},\"directives\":[";
            auto& directives = assembler.debugDirectives();
            for (size_t i = 0; i < directives.size(); i++) {
                if (i > 0) dbg << ",";
                const char* type_str = "unknown";
                switch (directives[i].type) {
                    case DebugDirective::TRACE_START: type_str = "trace_start"; break;
                    case DebugDirective::TRACE_STOP:  type_str = "trace_stop"; break;
                    case DebugDirective::BREAKPOINT:  type_str = "breakpoint"; break;
                    case DebugDirective::ASSERT_EQ:   type_str = "assert_eq"; break;
                    case DebugDirective::VRAMOUT:     type_str = "vramout"; break;
                    case DebugDirective::REGS:        type_str = "regs"; break;
                    case DebugDirective::LOG:         type_str = "log"; break;
                    case DebugDirective::LOG_ONCE:    type_str = "log_once"; break;
                    case DebugDirective::DOS_FAIL:    type_str = "dos_fail"; break;
                    case DebugDirective::DOS_PARTIAL: type_str = "dos_partial"; break;
                    case DebugDirective::MEM_SNAPSHOT: type_str = "mem_snapshot"; break;
                    case DebugDirective::MEM_ASSERT:   type_str = "mem_assert"; break;
                }
                dbg << "{\"type\":\"" << type_str
                    << "\",\"addr\":" << directives[i].addr
                    << ",\"count\":" << directives[i].count;
                if (!directives[i].label.empty()) {
                    dbg << ",\"name\":\"" << jsonEscape(directives[i].label) << "\"";
                }
                if (directives[i].type == DebugDirective::MEM_SNAPSHOT ||
                    directives[i].type == DebugDirective::MEM_ASSERT) {
                    dbg << ",\"snap_name\":\"" << jsonEscape(directives[i].snap_name) << "\""
                        << ",\"snap_seg\":" << directives[i].snap_seg
                        << ",\"snap_offset\":" << directives[i].snap_offset
                        << ",\"snap_length\":" << directives[i].snap_length;
                }
                if (directives[i].type == DebugDirective::ASSERT_EQ) {
                    const char* check_str = "none";
                    switch (directives[i].check_kind) {
                        case DebugDirective::CHECK_REG:      check_str = "reg"; break;
                        case DebugDirective::CHECK_MEM_BYTE:  check_str = "mem_byte"; break;
                        case DebugDirective::CHECK_MEM_WORD:  check_str = "mem_word"; break;
                        default: break;
                    }
                    dbg << ",\"check\":\"" << check_str << "\"";
                    if (directives[i].check_kind == DebugDirective::CHECK_REG) {
                        dbg << ",\"reg\":\"" << directives[i].reg_name << "\""
                            << ",\"reg_index\":" << directives[i].reg_index;
                    } else {
                        dbg << ",\"mem_addr\":" << directives[i].mem_addr;
                        if (directives[i].mem_seg >= 0)
                            dbg << ",\"mem_seg\":" << directives[i].mem_seg;
                    }
                    dbg << ",\"expected\":" << directives[i].expected;
                }
                if (directives[i].vramout.active) {
                    dbg << ",\"vramout\":{\"full\":"
                        << (directives[i].vramout.full ? "true" : "false")
                        << ",\"attrs\":"
                        << (directives[i].vramout.attrs ? "true" : "false");
                    if (!directives[i].vramout.full) {
                        dbg << ",\"x\":" << directives[i].vramout.x
                            << ",\"y\":" << directives[i].vramout.y
                            << ",\"w\":" << directives[i].vramout.w
                            << ",\"h\":" << directives[i].vramout.h;
                    }
                    dbg << "}";
                }
                if (directives[i].regs) {
                    dbg << ",\"regs\":true";
                }
                if (directives[i].type == DebugDirective::DOS_FAIL ||
                    directives[i].type == DebugDirective::DOS_PARTIAL) {
                    dbg << ",\"int_num\":" << (int)directives[i].int_num
                        << ",\"ah_func\":" << (int)directives[i].ah_func;
                    if (directives[i].type == DebugDirective::DOS_FAIL)
                        dbg << ",\"fail_code\":" << directives[i].fail_code;
                    else
                        dbg << ",\"partial_count\":" << directives[i].partial_count;
                }
                dbg << "}";
            }
            dbg << "]";
            if (!assembler.screenMode().empty()) {
                dbg << ",\"screen\":\"" << assembler.screenMode() << "\"";
            }
            dbg << "}\n";
            dbg.close();
        }
    }

    // Success: emit structured JSON with size and symbols
    std::cout << "{\"compiled\":\"OK\",\"size\":" << output.size();
    if (assembler.bssStart() >= 0) {
        std::cout << ",\"bss_start\":" << assembler.bssStart()
                  << ",\"bss_end\":" << assembler.bssEnd();
    }
    std::cout << ",\"symbols\":{";
    auto syms = assembler.symbols().dump();
    bool first = true;
    for (auto& kv : syms) {
        if (!first) std::cout << ",";
        first = false;
        std::cout << "\"" << jsonEscape(kv.first) << "\":{\"addr\":" << kv.second.value
                  << ",\"type\":\"" << (kv.second.is_equ ? "equ" : "label") << "\"}";
    }
    std::cout << "}";

    // Optional prints array
    auto& prints = assembler.prints();
    if (!prints.empty()) {
        std::cout << ",\"prints\":[";
        for (size_t i = 0; i < prints.size(); i++) {
            if (i > 0) std::cout << ",";
            std::cout << "{\"line\":" << prints[i].line
                      << ",\"text\":\"" << jsonEscape(prints[i].text) << "\"}";
        }
        std::cout << "]";
    }

    // Optional hex_dumps array
    auto& hex_dumps = assembler.hexDumps();
    if (!hex_dumps.empty()) {
        std::cout << ",\"hex_dumps\":[";
        for (size_t i = 0; i < hex_dumps.size(); i++) {
            if (i > 0) std::cout << ",";
            uint16_t start = hex_dumps[i].start_addr;
            uint16_t end = hex_dumps[i].end_addr;
            uint16_t size = end - start;
            // Convert addresses to buffer offsets
            int buf_offset = start - (int)assembler.origin();
            std::cout << "{\"addr\":" << start
                      << ",\"size\":" << size
                      << ",\"bytes\":\"";
            for (int b = 0; b < size && (buf_offset + b) < (int)output.size(); b++) {
                if (b > 0) std::cout << " ";
                char hex[4];
                snprintf(hex, sizeof(hex), "%02X", output[buf_offset + b]);
                std::cout << hex;
            }
            std::cout << "\"}";
        }
        std::cout << "]";
    }

    std::cout << "}" << std::endl;

    return 0;
}
