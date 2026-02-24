# agent86 — Agentic AI User Manual

## Identity

You are interfacing with **agent86**, a 16-bit x86 real-mode assembler and emulator that produces and executes flat `.COM` binaries. It is a two-pass assembler written in C++ with a built-in 8086 CPU emulator, both with structured JSON output modes designed for agentic AI consumption.

**Target architecture:** Intel 8086/8088 (with optional 80186 extensions)
**Output format:** Flat binary (`.COM`), loaded at offset `0100h` for emulation
**Maximum addressable space:** 65,535 bytes (one 64KB segment)

---

## Quick Start

```
agent86 [flags] source.asm
```

Assemble `hello.asm` and receive structured JSON output:

```bash
agent86 --agent hello.asm
```

Assemble and immediately run `hello.asm` in the emulator:

```bash
agent86 --run-source hello.asm
```

Run a pre-compiled `.COM` binary in the emulator:

```bash
agent86 --run hello.com
```

> **Windows note:** Run agent86 directly — do NOT wrap in `cmd.exe /c`. The cmd startup banner will contaminate the JSON output.
> ```
> PowerShell:  .\agent86.exe --agent source.asm
> cmd:         agent86.exe --agent source.asm
> ```
> **PowerShell 5.x encoding trap:** The `>` redirect operator in PowerShell 5.x silently re-encodes output as UTF-16LE, which breaks JSON parsers. Use `--output-file result.json` instead of shell redirection, or upgrade to PowerShell 7+ where `>` defaults to UTF-8.

---

## Command-Line Flags

### Assembler Flags

| Flag | Purpose | Output |
|---|---|---|
| `--agent` | **Primary assembly mode.** Structured JSON report to stdout. Always use this for assembly. | Full JSON (see Assembly Schema below) |
| `--disassemble` | **Disassemble a `.COM` binary** back to structured JSON. Round-trip verification tool. | Full JSON (see Disassembly Schema below) |
| `--explain <MNEMONIC>` | Query the ISA knowledge base for a single instruction. | JSON with valid operand forms |
| `--dump-isa` | Dump the entire supported instruction set. | JSON array of all mnemonics |
| *(no flags)* | Human-readable mode. Not recommended for agents. | Plain text summary |

### Emulator Flags

| Flag | Purpose | Default |
|---|---|---|
| `--run <file.com>` | **Run a `.COM` binary** in the emulator. Emits emulation JSON to stdout. | — |
| `--run-source <file.asm>` | **Assemble and run** in one step. Emits combined assembly + emulation JSON. | — |
| `--breakpoints <addr1,addr2,...>` | Set breakpoints at hex addresses (comma-separated). | None |
| `--watch-regs <reg1,reg2,...>` | Watch registers for changes. Emits snapshots when watched registers change. | None |
| `--max-cycles <N>` | Maximum CPU cycles before forced halt. | 1000000 |
| `--input <string>` | Provide stdin input for the program (consumed by INT 21h/01h and 06h). Supports escapes: `\xHH`, `\0`, `\n`, `\r`, `\t`, `\\`, and `\S` (shift-held prefix — see [INT 16h](#int-16h--bios-keyboard-services)). | Empty |
| `--mem-dump <addr,len>` | Include memory dump in breakpoint snapshots. Address in hex, length in decimal. | None |
| `--screen` | **Capture full 80×50 screen** from VRAM into JSON output. | Off |
| `--viewport <col,row,w,h>` | **Capture a rectangular region** of the screen. Implies `--screen` behavior but only for the specified window. | Off |
| `--attrs` | **Include attribute bytes** in screen output. Emits `screenAttrs[]` alongside `screen[]`. | Off |
| `--screenshot <file.bmp>` | **Render VRAM as a BMP image file.** Writes a 24-bit BMP using CP437 fonts with CGA 16-color palette. | Off |
| `--font 8x8\|8x16` | **Select font size for screenshot.** `8x16` (default) produces 640x800, `8x8` produces 640x400. | 8x16 |
| `--mouse <x>,<y>[,<buttons>]` | **Configure mouse state** for INT 33h emulation. `x`,`y` are pixel coordinates; `buttons` is a bitmask (1=left, 2=right, 4=middle, default 0). Presence enables the mouse driver. | Off |
| `--events '<JSON>'` or `--events @file.json` | **Script a timeline of user interactions** triggered by interrupt call counts. JSON array of event objects. If any event includes a `mouse` action, the mouse driver is auto-enabled. See [Event Scripting](#event-scripting). | Off |
| `--output-file <path>` | **Write JSON to file** instead of stdout. Bypasses shell encoding issues (no BOM, no re-encoding). | stdout |
| `--vram-fill [text]` | **Pre-fill VRAM with known content** before the program runs. With a text argument, tiles the string across all 4000 character cells (80×50). Without an argument, fills with random printable characters. Attribute bytes are left at default (`07h`). Useful for seeing which cells a program actually writes to. | Off |
| `--dos-root <dir>` | **Mount a host directory** as DOS filesystem root (C:\). Enables file I/O (open/read/write/close) and directory operations (FindFirst/FindNext, chdir). All paths sandboxed within this directory. | Off |
| `--args <string>` | **Command-line tail** written to PSP at offset 80h. Max 126 characters. | Empty |

### Help Flags

| Flag | Purpose | Output |
|---|---|---|
| `--help` | **List all help topics.** Returns a JSON index of every queryable topic with name, group, and brief description. | JSON topic listing |
| `--help <topic>` | **Show detailed help for a specific topic.** Returns content, examples, and related topics. Topic lookup is case-insensitive. | JSON topic detail |

The help system reads from `agent86.hlp` (located alongside the executable). Topics cover every CLI flag, assembly syntax, directives, macros, structs, the full instruction set, addressing modes, all emulated interrupts, JSON output schemas, and diagnostic references.

```bash
agent86 --help                    # list all topics
agent86 --help --agent            # help for --agent flag
agent86 --help syntax             # assembly syntax reference
agent86 --help int21h             # INT 21h function table
agent86 --help nonexistent        # error with list of available topics
```

**`--help` output (no argument):**
```json
{
  "help": true,
  "topics": [
    {"name": "overview", "group": "general", "brief": "Identity, architecture, and quick start guide."},
    {"name": "--agent", "group": "assembly", "brief": "Primary assembly mode..."},
    ...
  ]
}
```

**`--help <topic>` output:**
```json
{
  "help": true,
  "topic": "--agent",
  "group": "assembly",
  "brief": "Primary assembly mode. Structured JSON report to stdout.",
  "content": "Assemble a source file and emit...",
  "examples": ["agent86 --agent source.asm"],
  "related": ["--run-source", "--disassemble", "json-agent", "errors", "warnings"]
}
```

**`--help <unknown>` output:**
```json
{
  "help": true,
  "error": "Unknown help topic: foo",
  "available": ["overview", "--agent", "--run", ...]
}
```

### Flag Usage Patterns

**Standard assembly loop:**
```bash
agent86 --agent myprogram.asm
```

**Assemble, run, and observe output in one step:**
```bash
agent86 --run-source myprogram.asm
```

**Run with breakpoints and register watching:**
```bash
agent86 --run-source myprogram.asm --breakpoints 0120,0135 --watch-regs AX,DX
```

**Run with stdin input:**
```bash
agent86 --run-source myprogram.asm --input "Hello"
```

**Run a pre-compiled binary with memory inspection:**
```bash
agent86 --run output.com --breakpoints 0100 --mem-dump 0200,64
```

**Before writing an instruction you are unsure about:**
```bash
agent86 --explain MOV
agent86 --explain PUSH
```

**Discovering what instructions are available:**
```bash
agent86 --dump-isa
```

**Disassembling a compiled binary for verification:**
```bash
agent86 --disassemble output.com
```

**Run a VRAM program and capture the full screen:**
```bash
agent86 --run-source life.asm --screen
```

**Capture just a 40×25 window from the top-left:**
```bash
agent86 --run-source life.asm --viewport 0,0,40,25
```

**Include color/attribute data in screen output:**
```bash
agent86 --run-source colors.asm --screen --attrs
```

**Animate with breakpoints — capture screen at each generation:**
```bash
agent86 --run-source life.asm --viewport 0,0,40,40 --breakpoints 0150
```

**Render the screen as a BMP image:**
```bash
agent86 --run-source life.asm --screenshot screen.bmp
```

**Render with the smaller 8x8 font (640x400 instead of 640x800):**
```bash
agent86 --run-source life.asm --screenshot screen.bmp --font 8x8
```

**Write output to file (avoids PowerShell encoding issues):**
```bash
agent86 --run-source life.asm --screen --output-file result.json
```

**Pre-fill VRAM with a repeating pattern to see which cells a program writes to:**
```bash
agent86 --run-source game.asm --vram-fill "ABCD" --screen
```

**Pre-fill VRAM with random characters (no argument):**
```bash
agent86 --run-source game.asm --vram-fill --screen
```

**Run a mouse-driven program with initial mouse state:**
```bash
agent86 --run-source menu.asm --mouse 320,100,1
```

**Script coordinated keyboard and mouse interactions:**
```bash
agent86 --run-source app.asm --events '[{"on":"poll:3","mouse":[320,100,1]},{"on":"read:1","keys":"Y"}]'
```

**Load events from a file (useful for complex scripts):**
```bash
agent86 --run-source app.asm --events @events.json
```

**Provide shifted keys (for INT 16h/02h shift detection):**
```bash
agent86 --run-source arrows.asm --input "\S\x00\S\x4B"
```

**Mount a host directory for DOS file I/O:**
```bash
agent86 --run-source filetest.asm --dos-root ./testdir
```

**Read a file and observe I/O stats:**
```bash
agent86 --run-source reader.asm --dos-root /path/to/files
```
The JSON output will include a `fileIO` object with operation counts.

**Combine file I/O with debugging:**
```bash
agent86 --run-source fileapp.asm --dos-root ./data --breakpoints 0120,0140 --watch-regs AX,BX
```

---

## JSON Output Schema (`--agent`)

Every `--agent` invocation produces a single JSON object on stdout with this structure:

```json
{
  "success": true,

  "diagnostics": [
    {
      "level": "WARNING",
      "line": 5,
      "msg": "Size mismatch between operands",
      "hint": "Op1 is 8-bit (AL), Op2 is 16-bit (BX). Both operands must be the same width."
    }
  ],

  "symbols": {
    "START": { "val": 0, "type": "LABEL", "line": 1, "file": "main.asm", "sourceLine": 1 },
    "BUFSIZE": { "val": 256, "type": "EQU", "line": 2, "file": "main.asm", "sourceLine": 2 }
  },

  "listing": [
    {
      "addr": 0,
      "line": 3,
      "size": 3,
      "decoded": "MOV REG(AX), IMM(5)",
      "file": "main.asm",
      "sourceLine": 3,
      "src": "    MOV AX, 5",
      "bytes": [184, 5, 0]
    }
  ],

  "includes": ["main.asm"]
}
```

### Field Reference

**`success`** — `true` if a binary was written. `false` if any errors occurred. The binary file is NOT created on failure.

**`diagnostics[]`** — All errors and warnings from pass 2. Each entry contains:
- `level`: Severity. `ERROR` means assembly failed. `WARNING` means the binary was produced but something may be wrong (e.g., 80186-only encoding used). `INFO` is advisory.
- `line`: 1-indexed source line number (flat index across all included files).
- `file`: *(when INCLUDE is used)* Path of the source file where this diagnostic originated.
- `sourceLine`: *(when INCLUDE is used)* 1-indexed line number within that specific file.
- `msg`: Human-readable description of the problem.
- `hint`: Agent-actionable context. Contains a concrete fix, valid alternatives, or a code pattern you can apply directly. **Always read this field first — every diagnostic now includes an actionable hint written specifically for you.**

**`symbols{}`** — The complete symbol table after assembly. Each key is an uppercase symbol name.
- `val`: Resolved integer value (address for labels, constant for EQU).
- `type`: `"LABEL"` (address in code) or `"EQU"` (compile-time constant).
- `line`: Source line where the symbol was defined (flat index).
- `file`: *(when INCLUDE is used)* Path of the source file where the symbol was defined.
- `sourceLine`: *(when INCLUDE is used)* 1-indexed line number within that specific file.

**`listing[]`** — One entry per instruction/directive that emitted bytes. This is your primary debugging view.
- `addr`: Starting byte address of this instruction in the binary.
- `line`: Source line number (flat index).
- `size`: Number of bytes emitted.
- `decoded`: How the assembler *interpreted* your instruction (e.g., `MOV REG(AX), IMM(5)`). **Compare this against your intent to detect misparses.**
- `file`: *(when INCLUDE is used)* Path of the source file containing this line.
- `sourceLine`: *(when INCLUDE is used)* 1-indexed line number within that specific file.
- `src`: Original source text.
- `bytes`: Raw machine code bytes as decimal integers.

**`includes[]`** — List of all source files involved in the assembly, in the order they were first encountered. Always present (contains at least the top-level file).

### Error-Only Output

If no input file is provided:
```json
{ "error": "No input file" }
```

---

## JSON Output Schema (`--run-source`)

The `--run-source` flag assembles a source file and immediately runs the resulting binary in the emulator. It produces a single JSON object with both assembly and emulation results:

```json
{
  "assembly": {
    "success": true,
    "size": 42,
    "diagnostics": []
  },
  "emulation": {
    "success": true,
    "halted": true,
    "haltReason": "INT 20h program terminate",
    "exitCode": 0,
    "cyclesExecuted": 15,
    "fidelity": 1,
    "output": "Hello, World!\r\n",
    "outputHex": "48656C6C6F2C20576F726C64210D0A",
    "finalState": {
      "registers": {"AX": "0x0900", "CX": "0x0000", "DX": "0x0112", "BX": "0x0000", "SP": "0xFFFE", "BP": "0x0000", "SI": "0x0000", "DI": "0x0000"},
      "IP": "0x0110",
      "flags": "0x0246",
      "cursor": {"row": 1, "col": 0}
    },
    "skipped": [],
    "screen": ["Hello, World!                    ..."],
    "screenAttrs": ["0707070707070707..."]
  }
}
```

> **Note:** `screen[]`, `screenAttrs[]`, and `cursor` are always present in `finalState`. The `screen` and `screenAttrs` arrays only appear when `--screen` or `--viewport` is used. Without those flags, screen data is omitted to keep output compact.

If assembly fails, the `emulation` section will have `success: false` with no meaningful state.

### Emulation Field Reference

**`success`** — `true` if the emulator ran without internal errors. Note: a program that halts normally has `success: true` even if it produces wrong output.

**`halted`** — `true` if the program terminated (via INT 20h, INT 21h/4Ch, or cycle limit). `false` if still running when stopped.

**`haltReason`** — Human-readable reason for halt. Common values:
- `"INT 20h program terminate"` — Normal .COM termination
- `"INT 21h/4Ch exit (code=N)"` — DOS exit with return code
- `"HLT instruction at 0xNNNN"` — Program executed HLT instruction
- `"Cycle limit reached (N)"` — Hit `--max-cycles` limit
- `"Division by zero"` — DIV/IDIV with zero divisor
- `"Division overflow"` — DIV/IDIV quotient too large

**`exitCode`** — Program exit code (from INT 21h/4Ch AL value, or 0 for INT 20h).

**`cyclesExecuted`** — Number of instructions executed. Use this to detect infinite loops (will equal `--max-cycles`).

**`fidelity`** — Float between 0 and 1. Ratio of fully-emulated instructions to total. 1.0 means every instruction was fully handled. Below 1.0 means some instructions were skipped (check `skipped[]`).

**`output`** — Text output produced by the program (via INT 21h/02h, 06h, or 09h). All non-printable and non-ASCII bytes are escaped as `\u00XX` for JSON safety. Output is capped at 4096 bytes to prevent runaway programs from producing unbounded output.

**`outputHex`** — The exact same output as `output`, but hex-encoded byte-by-byte. Use this for binary-safe inspection when the program outputs non-text data. For example, `"48656C6C6F"` = "Hello". **When debugging garbage output, compare `outputHex` against expected bytes to spot bad pointers.**

**`finalState`** — CPU state when execution stopped:
- `registers`: All 8 general-purpose registers as hex strings (AX, CX, DX, BX, SP, BP, SI, DI).
- `IP`: Instruction pointer as hex string.
- `flags`: Flags register as hex string.
- `flagBits` (in `--run` mode): Individual flag values as booleans: CF, PF, AF, ZF, SF, OF, DF, IF.
- `cursor`: VRAM cursor position as `{"row": N, "col": N}`. Always present. Tracks where INT 10h teletype output will write next.
- `mouse` *(only when `--mouse` or `--events` with mouse actions is used)*: `{"x": N, "y": N, "buttons": N, "visible": bool}`. Final mouse driver state.

**`screen[]`** — *(Only present when `--screen` or `--viewport` is used.)* Array of strings, one per row. Each string contains the visible text characters from the VRAM viewport. Non-printable bytes are replaced with `.` for clean JSON. Without `--screen`/`--viewport`, this field is omitted entirely to keep output compact.

**`screenAttrs[]`** — *(Only present when `--attrs` is also used.)* Array of strings, one per row, parallel to `screen[]`. Each string contains two hex digits per cell representing the CGA text-mode attribute byte (e.g., `"07"` = light grey on black, `"1F"` = white on blue). Omitted unless `--attrs` is specified.

**`screenshot`** — *(Only present when `--screenshot` is used and the BMP was written successfully.)* String containing the path to the rendered BMP file. The image uses CP437 fonts with the CGA 16-color palette. Resolution depends on `--font`: 640x800 for `8x16` (default), 640x400 for `8x8`.

**`skipped[]`** — Instructions that were encountered but not fully emulated (e.g., unimplemented interrupts, I/O ports). Each entry:
- `instruction`: Disassembly text of the skipped instruction.
- `reason`: Why it was skipped.

**`diagnostics[]`** — Runtime diagnostics (e.g., output truncation warnings). Distinct from assembly diagnostics.

**`events`** — *(Only present when `--events` is used.)* Object with event scripting results:
- `total`: Number of events defined.
- `fired`: Number of events that triggered during execution.
- `pending`: Number of events that never triggered (program exited first).
- `log[]`: Array of fired events, each with `on` (trigger string), `cycle` (when it fired), and `action` (human-readable summary).

**`fileIO`** — *(Only present when `--dos-root` is used and file operations occurred.)* Object with file I/O statistics:
- `filesOpened`: Number of files opened (AH=3Ch, 3Dh).
- `filesClosed`: Number of files closed (AH=3Eh).
- `bytesRead`: Total bytes read from files (AH=3Fh on file handles, not device handles).
- `bytesWritten`: Total bytes written to files (AH=40h on file handles, not device handles).
- `dirSearches`: Number of FindFirst calls (AH=4Eh).
- `errors`: Number of failed file operations (CF=1 returns).

Example:
```json
"fileIO": {
  "filesOpened": 2,
  "filesClosed": 2,
  "bytesRead": 128,
  "bytesWritten": 0,
  "dirSearches": 1,
  "errors": 0
}
```

---

## JSON Output Schema (`--run`)

The `--run` flag runs a pre-compiled `.COM` binary. The JSON output is the same as the `emulation` section of `--run-source`, but as a top-level object. It also includes additional detail:

- `finalState.sregs`: Segment registers (ES, CS, SS, DS) as hex strings.
- `finalState.flagBits`: Individual flags as booleans.
- `finalState.cursor`: VRAM cursor position as `{"row": N, "col": N}`.
- `finalState.mouse` *(only when `--mouse` or `--events` with mouse actions is used)*: `{"x": N, "y": N, "buttons": N, "visible": bool}`.
- `snapshots[]`: Breakpoint and watchpoint snapshots (see Breakpoints section).
- `screen[]`: Screen text (when `--screen` or `--viewport` is used).
- `screenAttrs[]`: Attribute hex strings (when `--attrs` is also used).
- `screenshot`: Path to rendered BMP file (when `--screenshot` is used and the file was written successfully).
- `events` *(only when `--events` is used)*: Object with `total`, `fired`, `pending`, and `log[]`. See [Event Scripting](#event-scripting).
- `fileIO` *(only when `--dos-root` is used and file operations occurred)*: Object with `filesOpened`, `filesClosed`, `bytesRead`, `bytesWritten`, `dirSearches`, and `errors`. See [Emulation Field Reference](#emulation-field-reference).

---

## Breakpoints and Watchpoints

### Breakpoints (`--breakpoints`)

When execution reaches a breakpoint address, the emulator captures a full snapshot of CPU state and appends it to the `snapshots[]` array.

```bash
agent86 --run-source prog.asm --breakpoints 0110,0120
```

Each snapshot contains:
- `addr`: IP where the breakpoint fired (hex).
- `cycle`: Instruction count when hit.
- `reason`: e.g., `"Breakpoint at 0x0110"`.
- `nextInst`: Disassembly of the instruction about to execute.
- `registers`: All 8 registers as hex.
- `flags`: Flags register as hex.
- `cursor`: VRAM cursor position as `{"row": N, "col": N}`.
- `stack`: Top 8 words from the stack.
- `memDump`: Hex string of memory region (if `--mem-dump` is configured).
- `screen[]`: Screen text at this snapshot (if `--screen` or `--viewport` is used).
- `screenAttrs[]`: Attribute data at this snapshot (if `--attrs` is used).
- `hitCount`: Number of times this address has been hit (for the 10th and final snapshot, this accumulates all subsequent hits).

Snapshots are limited to 10 full captures per breakpoint address. The first 10 hits each produce a complete snapshot with distinct register state, screen content, and cursor position — ideal for watching animations or game-of-life grids evolve frame by frame. After 10 hits, only the `hitCount` on the last snapshot increments. Total snapshots across all addresses are capped at 100.

### Register Watchpoints (`--watch-regs`)

When a watched register changes value, a snapshot is emitted with a reason like `"AX changed: 0x0005 -> 0x0006"`.

```bash
agent86 --run-source prog.asm --watch-regs AX,DX,CX
```

### Memory Dumps (`--mem-dump`)

When combined with breakpoints, each snapshot includes a hex dump of the specified memory region.

```bash
agent86 --run-source prog.asm --breakpoints 0110 --mem-dump 0200,32
```

---

## JSON Output Schema (`--disassemble`)

The `--disassemble` flag reads a flat `.COM` binary and emits a JSON disassembly to stdout. This is the primary round-trip verification tool: assemble with `--agent`, then disassemble the output to confirm the bytes decode back to the intended instructions.

```json
{
  "file": "output.com",
  "fileSize": 93,

  "instructions": [
    {
      "addr": 0,
      "bytes": [184, 5, 0],
      "hex": "B8 05 00",
      "asm": "MOV AX, 0x0005",
      "size": 3
    }
  ],

  "dataRegions": [
    {
      "addr": 85,
      "bytes": [72, 101, 108, 108, 111],
      "hex": "48 65 6C 6C 6F",
      "size": 5,
      "msg": "Decode failed or ambiguous"
    }
  ]
}
```

### Field Reference

**`file`** — The input filename.

**`fileSize`** — Total size of the binary in bytes.

**`instructions[]`** — Decoded instructions in address order. Each entry contains:
- `addr`: Byte offset in the binary.
- `bytes`: Raw machine code as decimal integers.
- `hex`: Human-readable hex string (uppercase, space-separated).
- `asm`: Decoded assembly text (e.g., `MOV AX, 0x0005`). **Compare this against your `--agent` listing to verify the assembler produced the correct bytes.**
- `size`: Number of bytes consumed by this instruction.

**`dataRegions[]`** — Contiguous runs of bytes that could not be decoded as valid instructions. These typically correspond to `DB`/`DW`/`DD` data embedded in the binary.
- `addr`, `bytes`, `hex`, `size`: Same as instructions.
- `msg`: Reason for failure (always `"Decode failed or ambiguous"`).

### Error Output

If the file cannot be opened:
```json
{ "error": "Cannot open file: missing.com" }
```

If no file is provided:
```json
{ "error": "No input file for disassembly" }
```

### Disassembly Notes

- The disassembler uses **linear sweep** — it decodes bytes sequentially from offset 0. It does not follow control flow.
- Only opcodes the assembler itself can produce are decoded. Unrecognized bytes are accumulated into `dataRegions`.
- Immediates are displayed in uppercase hex with `0x` prefix (e.g., `0x0005`, `0xFF`).
- Jump/call targets are shown as **absolute addresses** (not relative offsets).
- String data (`DB`) will appear in `dataRegions` since ASCII bytes may partially match valid opcodes.

---

## `--explain` Output Schema

```json
{
  "mnemonic": "MOV",
  "forms": [
    { "op1": "REG", "op2": "REG", "notes": "" },
    { "op1": "REG", "op2": "IMM", "notes": "" },
    { "op1": "REG", "op2": "MEM", "notes": "" },
    { "op1": "MEM", "op2": "REG", "notes": "" },
    { "op1": "MEM", "op2": "IMM", "notes": "" },
    { "op1": "REG", "op2": "SREG", "notes": "" },
    { "op1": "SREG", "op2": "REG", "notes": "" }
  ],
  "found": true
}
```

**Operand type codes:**
- `REG` — Any general-purpose register (8-bit or 16-bit)
- `REG8` — 8-bit register only (AL, CL, DL, BL, AH, CH, DH, BH)
- `REG16` — 16-bit register only (AX, CX, DX, BX, SP, BP, SI, DI)
- `MEM` — Memory operand (`[BX]`, `[BX+SI+4]`, `[100h]`, etc.)
- `MEM16` — 16-bit memory operand
- `IMM` — Immediate (constant value, label reference, or expression)
- `LABEL` — Same as IMM; used for jump/call targets to indicate relative addressing
- `SEG` / `SREG` — Segment register (ES, CS, SS, DS)
- `AL/AX` — Accumulator register specifically
- `CL` — CL register specifically (for shift/rotate counts)
- `DX` — DX register specifically (for I/O port addressing)
- `1` — The literal constant `1` (for shift-by-one)
- `NONE` — No operand in this position

---

## Supported Instruction Set

### Data Movement
| Instruction | Forms | Notes |
|---|---|---|
| `MOV` | reg/mem/sreg in all valid combinations | Use `BYTE` or `WORD` prefix for mem←imm to set size |
| `XCHG` | reg←→reg, reg←→mem | Swaps values between two operands |
| `LEA` | reg16←mem | Load effective address, not the value at that address |
| `PUSH` | reg16, mem16, sreg | Pushes 16-bit value onto stack |
| `POP` | reg16, mem16, sreg | `POP CS` is invalid and will error |
| `PUSHA` | *(no operands)* | Push all 8 registers (AX,CX,DX,BX,SP,BP,SI,DI). 80186+ |
| `POPA` | *(no operands)* | Pop all 8 registers (reverse order, SP value discarded). 80186+ |
| `PUSHF` | *(no operands)* | Push flags register onto stack |
| `POPF` | *(no operands)* | Pop stack into flags register |
| `XLAT` | *(no operands)* | Table lookup: `AL = DS:[BX + AL]`. BX = table base, AL = index |
| `IN` | AL/AX←imm, AL/AX←DX | Read from I/O port |
| `OUT` | imm←AL/AX, DX←AL/AX | Write to I/O port |

### Arithmetic
| Instruction | Forms | Notes |
|---|---|---|
| `ADD` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | Affects CF, ZF, SF, OF |
| `ADC` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | Add with carry. Adds source + destination + CF. Used for multi-precision arithmetic. |
| `SUB` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | |
| `SBB` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | Subtract with borrow. Subtracts source + CF from destination. Used for multi-precision arithmetic. |
| `CMP` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | Like SUB but discards result, sets flags only |
| `INC` | reg, mem | 16-bit regs use short-form encoding (1 byte). Preserves CF. |
| `DEC` | reg, mem | Same as INC. Preserves CF. |
| `NEG` | reg, mem | Two's complement negate |
| `MUL` | reg, mem | Unsigned: AX = AL x src (8-bit) or DX:AX = AX x src (16-bit) |
| `IMUL` | reg, mem | Signed multiply |
| `DIV` | reg, mem | Unsigned: AL = AX / src, AH = remainder (8-bit) |
| `IDIV` | reg, mem | Signed divide |
| `CBW` | *(no operands)* | Sign-extend AL into AX (AL bit 7 → all of AH) |
| `CWD` | *(no operands)* | Sign-extend AX into DX:AX (AX bit 15 → all of DX) |

### Logic
| Instruction | Forms | Notes |
|---|---|---|
| `AND` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | |
| `OR` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | |
| `XOR` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | `XOR AX, AX` is the standard idiom for zeroing a register |
| `NOT` | reg, mem | One's complement (bitwise invert) |
| `TEST` | reg←reg, reg←mem, mem←reg, reg←imm, mem←imm | Like AND but discards result, sets flags only |

### Shift & Rotate
| Instruction | Forms | Notes |
|---|---|---|
| `SHL` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | imm>1 uses 80186+ encoding; assembler will warn |
| `SAL` | *(same as SHL)* | Alias for SHL. Produces identical machine code. Disassembles as SHL. |
| `SHR` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Logical shift right (zero-fill) |
| `SAR` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Arithmetic shift right (sign-preserving) |
| `ROL` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Rotate left |
| `ROR` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Rotate right |
| `RCL` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Rotate left through carry |
| `RCR` | reg/mem, 1 / reg/mem, imm / reg/mem, CL | Rotate right through carry |

All shift and rotate instructions accept both register and memory destinations. For memory operands, always specify `BYTE` or `WORD` to avoid the default-to-WORD warning.

> **Important:** If you need strict 8086 compatibility, always use `SHL AX, 1` or `SHL AX, CL`. The `SHL AX, 4` form will assemble correctly but produces an 80186-only opcode. The assembler emits a `WARNING` diagnostic with the concrete replacement code (e.g., `MOV CL, 4 / SHL AX, CL`).

### Flag Operations
| Instruction | Effect |
|---|---|
| `CLC` | Clear carry flag (CF=0) |
| `STC` | Set carry flag (CF=1) |
| `CMC` | Complement (toggle) carry flag |
| `CLD` | Clear direction flag (DF=0, string ops go forward) |
| `STD` | Set direction flag (DF=1, string ops go backward) |
| `CLI` | Clear interrupt flag (IF=0) |
| `STI` | Set interrupt flag (IF=1) |
| `LAHF` | Load flags (SF, ZF, AF, PF, CF) into AH |
| `SAHF` | Store AH into flags (SF, ZF, AF, PF, CF) |

### Miscellaneous
| Instruction | Effect |
|---|---|
| `NOP` | No operation (1 byte, opcode 90h) |
| `XLAT` | Table lookup: `AL = byte at [BX + AL]`. No flags affected. Also accepted as `XLATB`. |
| `HLT` | Halt the CPU. In the emulator, this cleanly terminates execution. |

### Control Flow

**Unconditional:**
| Instruction | Encoding | Range |
|---|---|---|
| `JMP label` | Near (E9, 3 bytes) | Full 16-bit range |
| `JMP reg16` | Indirect (FF /4, 2 bytes) | Any address in register (e.g., `JMP BX`) |
| `JMP mem16` | Indirect (FF /4, 2-4 bytes) | Any address in memory (e.g., `JMP [BX+2]`, `JMP WORD [table]`) |
| `CALL label` | Near (E8, 3 bytes) | Full 16-bit range |
| `CALL reg16` | Indirect (FF /2, 2 bytes) | Any address in register (e.g., `CALL BX`) |
| `CALL mem16` | Indirect (FF /2, 2-4 bytes) | Any address in memory (e.g., `CALL [BX]`, `CALL WORD [vtable+SI]`) |
| `RET` | C3 (1 byte) | Returns to caller |

**Conditional (short form 2 bytes; auto-promoted to 5 bytes when target exceeds short range):**

| Mnemonic(s) | Condition | Flag Test |
|---|---|---|
| `JZ` / `JE` | Zero / Equal | ZF=1 |
| `JNZ` / `JNE` | Not Zero / Not Equal | ZF=0 |
| `JL` / `JNGE` | Less (signed) | SF!=OF |
| `JG` / `JNLE` | Greater (signed) | ZF=0 and SF=OF |
| `JLE` / `JNG` | Less or Equal (signed) | ZF=1 or SF!=OF |
| `JGE` / `JNL` | Greater or Equal (signed) | SF=OF |
| `JB` / `JNAE` / `JC` | Below (unsigned) / Carry | CF=1 |
| `JA` / `JNBE` | Above (unsigned) | CF=0 and ZF=0 |
| `JBE` / `JNA` | Below or Equal (unsigned) | CF=1 or ZF=1 |
| `JAE` / `JNB` / `JNC` | Above or Equal (unsigned) / No Carry | CF=0 |
| `JS` | Sign (negative) | SF=1 |
| `JNS` | No Sign (positive/zero) | SF=0 |
| `JO` | Overflow | OF=1 |
| `JNO` | No Overflow | OF=0 |
| `JP` / `JPE` | Parity Even | PF=1 |
| `JNP` / `JPO` | Parity Odd | PF=0 |

**Loops:**
| Instruction | Behavior | Range |
|---|---|---|
| `LOOP` | Decrement CX; jump if CX!=0 | Short (-128 to +127) |
| `LOOPE` / `LOOPZ` | Decrement CX; jump if CX!=0 AND ZF=1 | Short |
| `LOOPNE` / `LOOPNZ` | Decrement CX; jump if CX!=0 AND ZF=0 | Short |
| `JCXZ` | Jump if CX=0 (no decrement) | Short |

> **Automatic near-jump promotion:** Conditional jumps whose targets exceed the short range (-128 to +127 bytes) are automatically promoted to a near form. The assembler emits an inverted condition that skips over a 3-byte near JMP, making this transparent. No manual restructuring is needed. LOOP/LOOPZ/LOOPNZ/JCXZ are NOT auto-promoted; use `DEC CX` / `JNZ` for far loop targets.

### String Instructions
| Instruction | Operation |
|---|---|
| `MOVSB` / `MOVSW` | Copy byte/word from DS:SI to ES:DI, advance SI and DI |
| `CMPSB` / `CMPSW` | Compare DS:SI with ES:DI, set flags, advance SI and DI |
| `STOSB` / `STOSW` | Store AL/AX at ES:DI, advance DI |
| `LODSB` / `LODSW` | Load DS:SI into AL/AX, advance SI |
| `SCASB` / `SCASW` | Compare AL/AX with ES:DI, set flags, advance DI |

**Prefix instructions** (place immediately before a string instruction):
| Prefix | Behavior |
|---|---|
| `REP` / `REPE` / `REPZ` | Repeat CX times (or until ZF=0 for CMPS/SCAS) |
| `REPNE` / `REPNZ` | Repeat CX times or until ZF=1 |

### Interrupts
| Instruction | Notes |
|---|---|
| `INT imm8` | Software interrupt. `INT 21h` is the standard DOS services call. `INT 20h` terminates a .COM program. |

---

## Emulated DOS Interrupts

The emulator handles the following interrupts. All others are logged in the `skipped[]` array.

### INT 10h — BIOS Video Services

The emulator provides an 8KB VRAM buffer (segment `B800h`, offsets `0000h–1F3Fh`) representing an 80×50 CGA text-mode screen. Each cell is 2 bytes: character byte followed by attribute byte. Programs can write to VRAM directly via `ES:DI` with segment `B800h`, or use INT 10h services below.

Text output via INT 21h (functions 02h, 06h, 09h) produces **dual output**: characters appear in both the `output` string and the VRAM buffer. This means `--screen` captures everything a program prints, whether it uses DOS calls or direct VRAM writes.

| AH | Function | Behavior |
|---|---|---|
| `00h` | Set video mode | AL=mode. Accepted but only mode 3 (80×25 color text) is meaningful. Clears screen. |
| `02h` | Set cursor position | DH=row, DL=column, BH=page (ignored). Moves the VRAM write cursor. |
| `03h` | Get cursor position | Returns DH=row, DL=column, CX=cursor shape. BH=page (ignored). |
| `06h` | Scroll window up | AL=lines (0=clear window). BH=fill attribute. CH,CL=top-left row,col. DH,DL=bottom-right row,col. |
| `07h` | Scroll window down | Same parameters as AH=06h but scrolls content downward. |
| `08h` | Read char/attr at cursor | Returns AL=character, AH=attribute at current cursor position. |
| `09h` | Write char+attr at cursor | AL=char, BL=attr, CX=repeat count. Does NOT advance cursor. |
| `0Ah` | Write char at cursor | AL=char, CX=repeat count. Keeps existing attribute. Does NOT advance cursor. |
| `0Eh` | Teletype output | AL=char. Writes character at cursor and advances. Handles CR (0Dh), LF (0Ah), BS (08h). Scrolls screen when cursor passes bottom row. |
| `0Fh` | Get video mode | Returns AL=3 (mode), AH=80 (columns), BH=0 (page). |

> **Common pattern — clear screen with color:**
> ```asm
> MOV AX, 0600h       ; AH=06 (scroll up), AL=0 (clear entire window)
> MOV BH, 1Fh         ; Fill attribute: white on blue
> XOR CX, CX          ; CH=0, CL=0 (top-left corner)
> MOV DH, 24          ; Bottom row (24 for 25-line mode)
> MOV DL, 79          ; Right column
> INT 10h
> ```

> **Common pattern — set cursor then print:**
> ```asm
> MOV AH, 02h         ; Set cursor position
> MOV DH, 5           ; Row 5
> MOV DL, 10          ; Column 10
> XOR BH, BH          ; Page 0
> INT 10h
> MOV AH, 0Eh         ; Teletype output
> MOV AL, 'A'
> INT 10h             ; Writes 'A' at row 5, col 10
> ```

### CGA Text-Mode Attribute Byte

The attribute byte at each VRAM cell controls foreground and background color:

```
  Bit 7    Bits 6-4    Bits 3-0
  Blink    Background  Foreground
```

| Value | Color | Value | Color |
|---|---|---|---|
| `0` | Black | `8` | Dark Grey |
| `1` | Blue | `9` | Light Blue |
| `2` | Green | `A` | Light Green |
| `3` | Cyan | `B` | Light Cyan |
| `4` | Red | `C` | Light Red |
| `5` | Magenta | `D` | Light Magenta |
| `6` | Brown | `E` | Yellow |
| `7` | Light Grey | `F` | White |

Common attribute values: `07h` = light grey on black (default), `1Fh` = white on blue, `4Eh` = yellow on red, `0Fh` = bright white on black.

### INT 16h — BIOS Keyboard Services

| AH | Function | Behavior |
|---|---|---|
| `02h` | Get shift flags | Returns the current shift-flags byte in AL. |

**Shift-flags byte (AL):**

| Bit | Meaning |
|-----|---------|
| 0 | Right Shift held |
| 1 | Left Shift held |
| 2 | Ctrl held (not yet emulated) |
| 3 | Alt held (not yet emulated) |
| 4–7 | Lock states (not emulated) |

**How shift state is driven:**

The emulator tracks a `shiftFlags` byte that is set by a `\S` prefix in the `--input` string (or event `keys`). When `\S` precedes a byte in the input stream, that byte is delivered with Left Shift (bit 1) set. The shift state persists after key consumption, so a program that reads a key via INT 21h/06h and then immediately calls INT 16h/02h will see the correct flags.

- **Single-byte shifted key:** `\SA` — delivers `'A'` with shift held.
- **Extended shifted key (two bytes):** `\S\x00\S\x4B` — delivers the extended-key pair `00h, 4Bh` (Left arrow) with shift held for both reads.
- **Unshifted key:** any byte not preceded by `\S` clears the shift flags.

> **Common pattern — detect Shift+arrow:**
> ```asm
> MOV AH, 06h
> MOV DL, 0FFh
> INT 21h              ; Read first byte (00h for extended key)
> JZ no_key
> CMP AL, 0
> JNE not_extended
> MOV AH, 06h
> MOV DL, 0FFh
> INT 21h              ; Read scan code
> ; Now check shift state
> MOV AH, 02h
> INT 16h              ; AL = shift flags
> AND AL, 03h          ; Isolate Shift bits
> JNZ shifted_arrow
> ```

### INT 20h — Program Terminate

Halts emulation with `exitCode: 0`. This is the standard .COM termination method. A program that returns with an empty stack (RET when SP=FFFEh) will execute the `INT 20h` at address 0000h (placed there by the emulator as a PSP stub).

### INT 21h — DOS Function Calls

The emulator implements 28 INT 21h functions organized into four categories: console I/O, system information, file handle I/O, and directory operations. Any unimplemented AH value is logged in `skipped[]` as `"Unimplemented DOS function"`.

#### Console I/O Functions

| AH | Function | Behavior |
|---|---|---|
| `01h` | Read character with echo | Reads one byte from `--input` string (or 0Dh if exhausted). Returns in AL. Echoes to output. |
| `02h` | Write character | Writes DL to output buffer. |
| `06h` | Direct console I/O | If DL=FFh: reads input (ZF=0 and AL=char if available, ZF=1 if not). Otherwise: writes DL to output. |
| `09h` | Write $-terminated string | Reads bytes from DS:DX until `$` terminator. Writes to output buffer. **Capped at 4096 bytes**. |

#### System Functions

| AH | Function | Behavior |
|---|---|---|
| `0Eh` | Select disk | Sets current drive to DL (0=A, 2=C). Returns AL=26 (total drives). |
| `19h` | Get current disk | Returns current drive in AL (default=2, i.e. C:). |
| `1Ah` | Set DTA | Sets Disk Transfer Area address to DS:DX for FindFirst/FindNext. |
| `25h` | Set interrupt vector | Stub — silently ignored. Programs often set INT 24h (critical error handler); this is safely ignored. |
| `2Ah` | Get date | Returns CX=year, DH=month, DL=day, AL=day-of-week. Uses real system date. |
| `2Ch` | Get time | Returns CH=hour, CL=minute, DH=second, DL=centisecond. Uses real system time. |
| `2Fh` | Get DTA | Returns DTA address in ES:BX. Default is PSP:0080h. |
| `30h` | Get DOS version | Returns AL=5 (major), AH=0 (minor). Reports as DOS 5.0. |
| `35h` | Get interrupt vector | Stub — returns ES:BX=0000:0000. |
| `62h` | Get PSP segment | Returns BX=0 (PSP segment base). |

#### File Handle I/O Functions

These functions operate on file handles. Handles 0–4 are pre-allocated device handles that work without `--dos-root`. File handles 5+ require `--dos-root` to be set.

| AH | Function | Behavior |
|---|---|---|
| `3Ch` | Create file | Creates/truncates file at DS:DX path. CX=attributes (ignored). Returns handle in AX. CF=1 on error. Requires `--dos-root`. |
| `3Dh` | Open file | Opens file at DS:DX path. AL=mode (0=read, 1=write, 2=read/write). Returns handle in AX. CF=1 if not found. Requires `--dos-root`. |
| `3Eh` | Close file | Closes handle in BX. Closing device handles (0–4) is a silent no-op. CF=1 if invalid handle. |
| `3Fh` | Read file/device | Reads CX bytes from handle BX into buffer at DS:DX. Returns AX=bytes actually read. Returns AX=0 at EOF. CF=1 on error. |
| `40h` | Write file/device | Writes CX bytes from buffer at DS:DX to handle BX. Returns AX=bytes written. CF=1 on error. |
| `42h` | Seek (lseek) | Repositions file pointer for handle BX. AL=origin (0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END). CX:DX=32-bit offset (CX=high, DX=low). Returns new position in DX:AX. CF=1 on error. On device handles, returns DX:AX=0 without error. |
| `43h` | Get/set file attributes | AL=0: get file attributes into CX. AL=1: set attributes (stub, returns success). CF=1 if file not found. Requires `--dos-root`. |
| `44h/00` | IOCTL get device info | Returns device information word in DX for handle BX. Bit 7 set = character device. Bit 7 clear = file (bits 5:0 = drive number). CF=1 if invalid handle. |
| `44h/09` | IOCTL check if block device is remote | BL=drive number (0=default, 1=A:, 2=B:, 3=C:, ...). Default drive maps to C:. Returns DX=0000h (local) and CF=0 for drive C:. CF=1 and AX=0Fh (invalid drive) for all other drives. Used by programs to probe available drives. |
| `57h` | Get file date/time | AL=0: get file modification time/date for handle BX. Returns CX=packed time, DX=packed date. CF=1 on error. |
| `4Ch` | Exit with return code | Halts emulation. `exitCode` = AL. |

#### Device Handle Table

Handles 0–4 are always available, even without `--dos-root`:

| Handle | Device | Read behavior | Write behavior |
|--------|--------|---------------|----------------|
| 0 | STDIN | Reads from `--input` buffer (byte-by-byte). Returns 0 bytes at end of input. | Writes to stdout capture (same as handle 1). |
| 1 | STDOUT | Returns 0 bytes (not readable). | Appends to `output` / VRAM display. |
| 2 | STDERR | Returns 0 bytes (not readable). | Appends to `output` / VRAM display. |
| 3 | AUX | Returns 0 bytes. | Silently discarded. |
| 4 | PRN | Returns 0 bytes. | Silently discarded. |

Writing to handles 1 or 2 via AH=40h produces the same output as AH=02h or AH=09h — bytes appear in both the `output` field and the VRAM display.

#### Directory Operations

| AH | Function | Behavior |
|---|---|---|
| `3Bh` | Change directory | Changes current DOS directory to path at DS:DX. CF=1 (AX=3) if directory not found. Requires `--dos-root`. |
| `47h` | Get current directory | Writes current directory path to buffer at DS:SI. No leading backslash, null-terminated. DL=drive (0=current). Returns empty string at root. |
| `4Eh` | FindFirst | Finds first file matching wildcard pattern at DS:DX (e.g., `*.*`, `*.TXT`). CX=attribute mask (set bit 4 / 10h to include subdirectories). Writes result to DTA. CF=1 (AX=18) if no match. Requires `--dos-root`. |
| `4Fh` | FindNext | Finds next file from previous FindFirst search. Writes result to DTA. CF=1 (AX=18) when no more files. Check DTA byte at offset 15h for the entry's attribute (10h = directory). |

#### DOS Error Convention

All file and directory functions use the carry flag (CF) to signal errors:
- **CF=0**: Success. Return values are in the documented registers (AX, CX, DX, etc.).
- **CF=1**: Failure. AX contains the DOS error code.

| AX Error Code | Meaning |
|---|---|
| 1 | Invalid function |
| 2 | File not found |
| 3 | Path not found |
| 5 | Access denied |
| 6 | Invalid handle |
| 18 | No more files (FindNext exhausted) |

When `--dos-root` is not set, all file/directory functions (3Bh–4Fh) return CF=1, AX=2 (file not found). Console I/O and system functions work regardless.

#### Path Sandboxing

All file paths are resolved relative to the `--dos-root` directory. The emulator enforces strict sandboxing:

1. **Drive letters are stripped**: `C:\FILE.TXT` becomes `FILE.TXT` relative to the root.
2. **Backslashes are normalized**: `SUBDIR\FILE.TXT` is converted to forward slashes internally.
3. **Relative paths use the current directory**: After `AH=3Bh` changes to `SUBDIR`, opening `FILE.TXT` resolves to `<dos-root>/SUBDIR/FILE.TXT`.
4. **Directory traversal is blocked**: Paths containing `..` that would escape the root directory are rejected with error 3 (path not found). For example, `..\..\etc\passwd` cannot escape the sandbox.
5. **Canonical path validation**: The resolved host path is checked against the canonical root path to ensure containment.

#### Disk Transfer Area (DTA)

FindFirst (AH=4Eh) and FindNext (AH=4Fh) write results to the Disk Transfer Area, a 43-byte buffer in the program's memory. The default DTA is at PSP offset 0080h. Programs can relocate it with AH=1Ah.

**DTA layout (43 bytes):**

| Offset | Size | Field |
|--------|------|-------|
| 00h | 21 | Reserved (emulator stores search state ID) |
| 15h | 1 | File attribute byte |
| 16h | 2 | File time (packed, little-endian) |
| 18h | 2 | File date (packed, little-endian) |
| 1Ah | 4 | File size in bytes (little-endian DWORD) |
| 1Eh | 13 | Filename in 8.3 format, null-terminated (ASCIIZ) |

**Packed DOS time format** (16-bit word at offset 16h):
```
Bits 15-11: Hours (0-23)
Bits 10-5:  Minutes (0-59)
Bits 4-0:   Seconds / 2 (0-29)
```

**Packed DOS date format** (16-bit word at offset 18h):
```
Bits 15-9: Year - 1980 (0-127)
Bits 8-5:  Month (1-12)
Bits 4-0:  Day (1-31)
```

#### FindFirst Attribute Mask (CX)

The CX register in FindFirst controls which entry types are returned:

| CX Value | Entries Returned |
|----------|-----------------|
| `00h` | Normal files only (default). Subdirectories are excluded. |
| `10h` | Normal files **and** subdirectories. Check attribute byte at DTA offset 15h — `10h` means directory, `20h` means file. |
| `02h` | Normal files and hidden files. |
| `16h` | All: normal + hidden + directories. |

CX is a bitmask — set bit 4 (10h) to include directories, bit 1 (02h) for hidden files. Normal files are always returned regardless of CX.

#### Wildcard Matching

FindFirst pattern matching follows DOS conventions:

| Pattern | Matches |
|---------|---------|
| `*.*` | **All entries** — files and directories, with or without extensions. This is the "match everything" pattern. |
| `*.TXT` | Only entries with a `.TXT` extension. Does **not** match names without extensions. |
| `DATA.*` | `DATA.BIN`, `DATA.TXT`, and also `DATA` (no extension). |
| `FILE?.TXT` | `FILE1.TXT`, `FILEA.TXT`, etc. `?` matches exactly one character. |
| `*` | All entries (equivalent to `*.*`). |

Filenames are matched in 8.3 uppercase format. Host filenames longer than 8+3 characters are truncated. The pattern is case-insensitive.

#### File I/O JSON Output

When `--dos-root` is used and file operations occur, the JSON output includes a `"fileIO"` object:

```json
"fileIO": {
  "filesOpened": 2,
  "filesClosed": 2,
  "bytesRead": 256,
  "bytesWritten": 64,
  "dirSearches": 1,
  "errors": 0
}
```

Use `fileIO.errors` to check if any operations failed. Use `bytesRead`/`bytesWritten` to verify data transfer amounts. `dirSearches` counts FindFirst calls.

### INT 33h — Mouse Services

Enabled by the `--mouse` CLI flag or by `--events` with any `mouse` action. When neither is specified, INT 33h function 0000h returns AX=0 (driver not installed) and all other functions are no-ops. INT 33h dispatches on the full AX register, not just AH.

| AX | Function | Behavior |
|---|---|---|
| `0000h` | Reset / detect | Returns AX=FFFFh (installed), BX=3 (buttons). Resets position to center, clears button counters. |
| `0001h` | Show cursor | Sets cursor visible flag (reported in `finalState.mouse.visible`). |
| `0002h` | Hide cursor | Clears cursor visible flag. |
| `0003h` | Get position & buttons | Returns BX=buttons, CX=x, DX=y. This is the primary polling function. |
| `0004h` | Set position | CX=x, DX=y. Clamped to current range. |
| `0005h` | Get button press info | BX=button index → BX=press count (reset after read), AX=buttons, CX=x, DX=y. |
| `0006h` | Get button release info | Same as 0005h but for releases. |
| `0007h` | Set horizontal range | CX=min, DX=max. Clamps current position. |
| `0008h` | Set vertical range | CX=min, DX=max. Clamps current position. |
| `000Bh` | Get motion counters | Returns CX=0, DX=0 (mickeys not tracked). |
| `000Ch` | Set event handler | Silently ignored (no callback support). |

**Example — read mouse position:**
```asm
ORG 100h
    MOV AX, 3       ; Function 03h: get position & buttons
    INT 33h          ; BX=buttons, CX=x, DX=y
    INT 20h
```

Run with: `agent86 --run-source mouse.asm --mouse 320,100,1`

### Event Scripting

The `--events` flag scripts a timeline of user interactions — keyboard input, mouse state changes, and snapshots — triggered by what the program does. This enables testing interactive programs that interleave keyboard and mouse interaction (menus, dialogs, drawing tools) that can't be tested with static `--input` and `--mouse` alone.

**Format:** A JSON array of event objects, passed inline or from a file:

```bash
agent86 --run-source app.asm --events '[{"on":"poll:3","mouse":[320,100,1]},{"on":"read:1","keys":"Y"}]'
agent86 --run-source app.asm --events @events.json
```

**Trigger types** — each event fires once when the counter reaches the specified value:

| Trigger | Meaning |
|---------|---------|
| `read:N` | Nth INT 21h keyboard read (AH=01h or AH=06h/DL=FFh). Fires before the read, so injected keys are available immediately. |
| `poll:N` | Nth INT 33h/0003h mouse position poll. Fires before the poll returns, so the program sees the new state. |
| `tick:N` | After the Nth CPU cycle (instruction). |
| `int21:N` | Nth INT 21h call (any function). |
| `int33:N` | Nth INT 33h call (any function, after driver is installed). |

**Action types** — each event can combine multiple actions:

| Key | Value | Effect |
|-----|-------|--------|
| `mouse` | `[x, y, buttons]` | Set mouse position and button state. Auto-enables the mouse driver. |
| `keys` | `"string"` | Inject bytes into the keyboard buffer. Supports `\xHH`, `\n`, `\r`, `\t`, `\0`, `\\`, and `\S` (shift prefix) escapes. |
| `snapshot` | `true` | Capture a CPU/memory snapshot (appears in the `snapshots` array). |

**Semantics:**
- Events fire once (never re-trigger).
- `read` and `poll` triggers fire **inside** the interrupt handler, so injected state is available to the current read/poll — a `read:1` event with `keys` makes those keys available for the 1st read itself.
- `tick`, `int21`, and `int33` triggers fire after each cycle in the main loop.
- `--events` is composable with `--input` and `--mouse` (events can override mouse state set by `--mouse`; event-injected keys are inserted at the current read position in the `--input` buffer).

**JSON output** — when events are used, the output includes an `events` object:
```json
"events": {
  "total": 2,
  "fired": 2,
  "pending": 0,
  "log": [
    {"on": "poll:3", "cycle": 5, "action": "mouse [320,100,1]"},
    {"on": "read:1", "cycle": 8, "action": "keys \"Y\""}
  ]
}
```

**Example — scripting a mouse-driven menu:**
```json
[
  {"on": "poll:3",  "mouse": [320, 100, 1], "snapshot": true},
  {"on": "poll:8",  "mouse": [320, 100, 0]},
  {"on": "read:1",  "keys": "Y"},
  {"on": "tick:5000", "snapshot": true}
]
```
This clicks at (320,100) on the 3rd mouse poll (with snapshot), releases on the 8th, types "Y" on the first keyboard read, and captures a late snapshot at cycle 5000 if execution reaches that point.

**Workflow for debugging interactive programs:**

1. Run with `--events` to script the interaction sequence.
2. Check `events.fired` vs `events.total` — if events are `pending`, the program exited before reaching those triggers.
3. Add `"snapshot": true` to key events to capture CPU state at the moment of interaction.
4. Combine with `--screen` to see the visual state when each event fires.
5. Use `read:N` triggers to answer prompts and `poll:N` triggers to simulate mouse clicks at the right moment in the program's polling loop.

---

## Directives

| Directive | Syntax | Purpose |
|---|---|---|
| `ORG` | `ORG 100h` | Set the assembler's address counter. Use `ORG 100h` for .COM files loaded by DOS (PSP occupies first 256 bytes). |
| `DB` | `DB 'Hello', 0Dh, 0Ah, '$'` | Define bytes. Accepts strings, numbers, expressions, and comma-separated lists. |
| `DW` | `DW 1234h, label_addr` | Define 16-bit words (little-endian). |
| `DD` | `DD 12345678h` | Define 32-bit doublewords (little-endian). |
| `RESB` | `RESB 64` | Reserve N zero-initialized bytes. Accepts expressions (`RESB BUFSIZE`). |
| `RESW` | `RESW 32` | Reserve N zero-initialized words (emits N x 2 zero bytes). |
| `EQU` | `BUFSIZE EQU 256` | Define a compile-time constant. Does not emit bytes. |
| `PROC` | `myproc: PROC` | Begin a named procedure (enables local labels). |
| `ENDP` | `ENDP` | End the current procedure scope. |
| `INCLUDE` | `INCLUDE 'file.asm'` | Insert the contents of another source file at this point. Supports single-quoted, double-quoted, or bare filenames. Relative paths resolve from the including file's directory. Nesting up to 16 levels; circular includes are detected. |
| `MACRO` | `name MACRO [params]` | Begin a named macro definition. Parameters are comma-separated identifiers substituted at invocation. |
| `ENDM` | `ENDM` | End a `MACRO`, `REPT`, or `IRP` block. |
| `LOCAL` | `LOCAL lab1, lab2` | Declare labels inside a MACRO body that get unique names (`??XXXX`) per expansion. Must appear before any instructions in the macro body. |
| `REPT` | `REPT 5` | Repeat the enclosed block N times. Count must be a non-negative numeric literal. |
| `IRP` | `IRP reg, <AX,BX,CX>` | Iterate: expand the body once for each item in the angle-bracket list, substituting the parameter. |
| `STRUC` | `name STRUC` | Begin a named structure definition. Fields are declared with `DB`, `DW`, `DD`, `RESB`, or `RESW`. No code is emitted; field offsets are calculated automatically and added to the symbol table as `name.field`. |
| `ENDS` | `name ENDS` | End the current structure definition. The structure's total size is defined as a constant with the structure's name. |

### STRUC/ENDS — Structure Definitions

Define named data structures with automatic field offset calculation (MASM-compatible).

```asm
; Definition — no code emitted, creates offset constants
POINT STRUC
    X  DW  0          ; POINT.X = 0, default = 0
    Y  DW  0          ; POINT.Y = 2, default = 0
POINT ENDS            ; POINT = 4 (total size)
```

**Field types:** `DB` (1 byte), `DW` (2 bytes), `DD` (4 bytes), `RESB n` (n bytes), `RESW n` (n*2 bytes).

**Using offsets in expressions:**
```asm
MOV AX, [BX + POINT.X]    ; access field at offset 0
MOV CX, [BX + POINT.Y]    ; access field at offset 2
ADD BX, POINT              ; advance pointer by struct size
```

**Instance allocation with angle-bracket overrides:**
```asm
pt1 POINT <100, 200>      ; emits DW 100, DW 200
pt2 POINT <>               ; emits defaults (DW 0, DW 0)
pt3 POINT <10>             ; partial: X=10, Y=default
```

Skipped fields in angle brackets use defaults: `<10, , 30>` overrides the first and third fields, keeping the second at its default.

### INCLUDE Directive

Split source across multiple files for organization — data definitions, constants, subroutines, etc. can live in separate files.

```asm
; main.asm
ORG 100h
    MOV AH, 9
    MOV DX, msg
    INT 21h
    INT 20h
INCLUDE 'data.asm'          ; single quotes
INCLUDE "lib/utils.asm"     ; double quotes, relative path
INCLUDE constants.inc       ; bare filename (no quotes)
```

**Syntax:** `INCLUDE` must be the first non-whitespace token on the line. Trailing comments (after `;`) are allowed. The keyword is case-insensitive (`include`, `Include`, `INCLUDE` all work).

**Path resolution:** Relative paths resolve from the directory of the file containing the INCLUDE, not from the working directory. This means `INCLUDE 'lib/defs.asm'` inside `src/main.asm` looks for `src/lib/defs.asm`.

**Nesting:** Files can include other files up to 16 levels deep. Circular includes (A includes B, B includes A) are detected and reported as errors.

**JSON output:** When using `--agent` or `--run-source`, diagnostics, symbols, and listing entries include `"file"` and `"sourceLine"` fields so errors can be traced to their original source file. An `"includes"` array lists all files involved in the assembly.

### Macros

Define reusable code patterns with `MACRO`/`ENDM`. Macros are expanded as a text-level preprocessor step after `INCLUDE` expansion but before the two-pass assembler. The assembler pipeline is:

```
expandIncludes() → expandMacros() → two-pass assembly
```

The two-pass assembler sees only expanded code — macros are purely a text substitution layer.

#### Named Macros with Parameters

```asm
PrintChar MACRO ch
    MOV DL, ch
    MOV AH, 02h
    INT 21h
ENDM

PrintChar 'A'       ; expands to MOV DL,'A' / MOV AH,02h / INT 21h
PrintChar 'B'
```

**Syntax:** `name MACRO [param1, param2, ...]` ... `ENDM`. The macro name appears *before* the `MACRO` keyword. Parameters are comma-separated identifiers after `MACRO`. Invoke by using the macro name as if it were an instruction.

**Substitution rules:**
- Case-insensitive: parameter `ch` matches `ch`, `CH`, `Ch` in the body
- Word-boundary only: a parameter named `x` will **not** replace the `x` inside `AX` or `BX` — only standalone `x` tokens
- Strings and comments are protected: `'x'` and `; x` are never substituted
- Macro names themselves are case-insensitive: defining `Foo MACRO` means `foo`, `FOO`, `Foo` all invoke it

**`&` concatenation:** The `&` operator joins parameter text with surrounding characters. The `&` is consumed during substitution:

```asm
MakeLabel MACRO name, num
    name&num:
        DB 0
ENDM
MakeLabel msg, 1    ; defines label msg1
MakeLabel msg, 2    ; defines label msg2
```

#### LOCAL Labels

Use `LOCAL` to declare labels that get unique names (`??0000`, `??0001`, ...) per expansion, avoiding duplicate-label errors:

```asm
Skip MACRO
    LOCAL done
    JMP done
    NOP
done:
ENDM

Skip    ; done → ??0000
Skip    ; done → ??0001
```

`LOCAL` must appear before any instructions in the macro body. Multiple locals can be on one line: `LOCAL a, b, c`. The generated `??XXXX` labels are global and work in expressions like any other label.

#### REPT — Repeat N Times

```asm
REPT 5
    NOP
ENDM
```

Count must be a **numeric literal** — decimal, hex (`0Ah`), binary (`100b`), or octal. EQU constants and expressions are not supported as the count; use a literal value. `REPT 0` is valid and emits nothing.

REPT blocks can be nested:

```asm
REPT 3
    REPT 2
        NOP
    ENDM
ENDM
; emits 6 NOPs
```

#### IRP — Iterate Over a List

```asm
IRP reg, <AX, BX, CX, DX>
    PUSH reg
ENDM
; expands to: PUSH AX / PUSH BX / PUSH CX / PUSH DX
```

The list must be enclosed in angle brackets `<>`. Commas inside the list separate items. An empty list `<>` is valid and emits nothing.

#### Nesting and Composition

Macros can invoke other macros, and macro bodies can contain REPT and IRP blocks. All combinations work:

```asm
; Macro invoking another macro
Inner MACRO val
    MOV AL, val
ENDM
Outer MACRO x
    Inner x
ENDM
Outer 42h           ; expands to MOV AL, 42h

; IRP inside a macro body
SaveRegs MACRO
    IRP r, <AX, BX, CX, DX>
        PUSH r
    ENDM
ENDM

; REPT inside a macro body
FillZero MACRO count
    REPT count
        DB 0
    ENDM
ENDM

; Macro invocation inside REPT/IRP
Emit MACRO val
    DB val
ENDM
IRP v, <10h, 20h, 30h>
    Emit v
ENDM
```

Expansion iterates until stable, up to 10,000 iterations. Self-recursive or mutually-recursive macros will hit this limit and produce an error.

A label before an invocation is preserved: `start: PrintChar 'A'` places the label on its own line, then the expansion.

#### Restrictions

- Macro names cannot shadow reserved words (instructions, registers, directives like `DUP`, `BYTE`, etc.)
- Redefining a macro emits a warning; the new definition replaces the old one
- Argument count mismatches emit warnings: missing args become empty strings, extras are ignored
- REPT count must be a numeric literal, not a symbol or expression

#### Source Map and Diagnostics

All expanded lines inherit the source location of the invocation site, so assembler errors within a macro expansion point back to the line that invoked the macro. Comment markers (`; >>> MACRO name` / `; <<< END MACRO name`) delimit expansions visually.

**Note:** Data directives (`DB`, `DW`) emitted by macro expansions are assembled normally but do not appear as individual entries in the JSON `listing` array. Verify data emission by checking the address gap between surrounding listing entries, or by running with `--run-source` and inspecting memory.

---

## Syntax Reference

### Registers

**8-bit:** `AL`, `CL`, `DL`, `BL`, `AH`, `CH`, `DH`, `BH`
**16-bit:** `AX`, `CX`, `DX`, `BX`, `SP`, `BP`, `SI`, `DI`
**Segment:** `ES`, `CS`, `SS`, `DS`

Register names are case-insensitive.

### Numeric Literals

| Format | Example | Notes |
|---|---|---|
| Decimal | `100`, `100d` | Default base |
| Hexadecimal (suffix) | `0FFh`, `100h` | **Must start with a digit** — write `0FFh`, not `FFh` |
| Hexadecimal (prefix) | `0xFF` | C-style alternative |
| Binary (suffix) | `10110b` | |
| Binary (prefix) | `0b10110` | C-style alternative |
| Octal | `177o`, `177q` | |

> **Critical hex literal rule:** Hex numbers starting with a letter (A-F) MUST be prefixed with `0`. Otherwise the assembler will interpret them as a label name. For example: `0ABh` is the number 171 decimal. `ABh` is an undefined label (the assembler will emit a helpful hint: "Did you mean 0ABH?").

### Labels

```asm
start:          ; Global label — visible everywhere
    MOV AX, 5

myproc: PROC
.loop:          ; Local label — scoped to myproc, stored as MYPROC.LOOP
    DEC CX
    JNZ .loop
ENDP
```

Labels are case-insensitive (internally stored as uppercase). Local labels (prefixed with `.`) are prepended with the enclosing `PROC` name.

### Memory Operands

The assembler supports the 8086 addressing modes:

```asm
MOV AX, [100h]          ; Direct address
MOV AX, [BX]            ; Base register
MOV AX, [BX+4]          ; Base + displacement
MOV AX, [BX+SI]         ; Base + index
MOV AX, [BX+SI+10h]     ; Base + index + displacement
MOV AX, [BP+DI]         ; BP-based (defaults to SS segment)
```

**Valid base/index combinations:**

| R/M | Registers | Default Segment |
|---|---|---|
| 0 | `[BX+SI]` | DS |
| 1 | `[BX+DI]` | DS |
| 2 | `[BP+SI]` | SS |
| 3 | `[BP+DI]` | SS |
| 4 | `[SI]` | DS |
| 5 | `[DI]` | DS |
| 6 | `[BP]` | SS |
| 7 | `[BX]` | DS |

**Segment overrides:**
```asm
MOV AX, ES:[BX]         ; Force ES segment
MOV AX, [ES:BX]         ; Also valid (inside brackets)
```

**Size prefixes** for ambiguous memory-immediate operations:
```asm
MOV BYTE [BX], 5        ; Store 8-bit value
MOV WORD [BX], 5        ; Store 16-bit value
ADD BYTE [SI], 1         ; 8-bit add to memory
SHL WORD [BX], 1         ; 16-bit shift left on memory
SAR BYTE [SI], CL        ; 8-bit arithmetic shift right on memory
```

> When writing any instruction with a memory destination and immediate source (MOV, ADD, SHL, etc.), always specify `BYTE` or `WORD`. Without it, the assembler defaults to WORD and emits a `WARNING` diagnostic.

### Expressions

The assembler supports constant expressions with standard arithmetic:

```asm
BUFSIZE EQU 256
MOV CX, BUFSIZE * 2          ; 512
MOV AX, (BUFSIZE + 4) / 2    ; 130
msg DB 'A' + 1                ; 66 (ASCII 'B')
JMP $ + 5                     ; Jump 5 bytes forward from current address
```

**Operators:** `+`, `-`, `*`, `/` (integer division), unary `-`, parentheses
**Special symbol:** `$` = current address counter

### Comments

Semicolons begin a comment that extends to end of line:
```asm
MOV AX, 5    ; This is a comment
```

---

## Agentic Workflow

### Recommended Assembly + Run Loop

```
1.  Write .asm source file
2.  Run:  agent86 --run-source source.asm
3.  Parse the JSON output
4.  If assembly.success == false:
      a. Read assembly.diagnostics[].message and .hint
      b. Apply fixes to source
      c. Go to step 2
5.  If assembly.success == true:
      a. Check emulation.halted and emulation.haltReason
      b. Check emulation.output against expected output
      c. If output is wrong, inspect emulation.finalState.registers
      d. Use emulation.outputHex for byte-level comparison
      e. Check emulation.skipped[] for unimplemented instructions
      f. If fidelity < 1.0, some instructions were not fully emulated
6.  For VRAM programs, add --screen or --viewport:
      a. Re-run:  agent86 --run-source source.asm --screen
      b. Check emulation.screen[] for expected display content
      c. Check emulation.finalState.cursor for cursor position
      d. Add --attrs to inspect color attributes if display looks wrong
7.  If debugging is needed:
      a. Re-run with --breakpoints at suspect addresses
      b. Add --watch-regs to track register changes
      c. Inspect snapshots[] for state at each breakpoint
      d. For VRAM programs, snapshots include screen[] at each hit
```

### Assembly-Only Loop

```
1.  Write .asm source file
2.  Run:  agent86 --agent source.asm
3.  Parse the JSON output
4.  If success == false:
      a. Read diagnostics[].msg and diagnostics[].hint
      b. Apply fixes to source
      c. Go to step 2
5.  If success == true:
      a. Verify listing[].decoded matches your intent
      b. Check for WARNING diagnostics (may indicate subtle issues)
      c. Run:  agent86 --disassemble output.com
      d. Cross-reference instructions[].asm with your source
      e. Binary is ready
```

### Round-Trip Verification

After a successful assembly, use `--disassemble` to confirm the binary contains the expected instructions:

```
1.  Assemble:     agent86 --agent source.asm
2.  Disassemble:  agent86 --disassemble source.com
3.  For each instruction in the disassembly:
      a. Verify instructions[].asm matches your original intent
      b. Check that data regions correspond to DB/DW/DD directives
4.  If a mismatch is found:
      a. Compare the hex bytes in the listing with the disassembly
      b. The assembler's listing[].decoded shows how it parsed your source
      c. The disassembler's instructions[].asm shows what the CPU would execute
      d. If these differ, there is an encoding bug
```

### Debugging with Breakpoints

When a program produces wrong output, use breakpoints to inspect state at key points:

```bash
# Step 1: Find the address of the suspect instruction from the listing
agent86 --agent source.asm
# (read listing[].addr for the instruction you want to inspect)

# Step 2: Run with breakpoints
agent86 --run-source source.asm --breakpoints 0110,0120 --watch-regs AX,DX
```

The `snapshots[]` array will contain a full state dump at each breakpoint, including the stack and the next instruction to execute. Register watchpoints capture every change, making it easy to trace data flow.

### Debugging VRAM Programs

For programs that write to the screen (games, TUI apps, Conway's Life, etc.), add `--screen` or `--viewport` together with `--breakpoints` to capture the screen at each breakpoint hit. Each of the first 10 snapshots at a given address includes a full `screen[]` array, letting you watch the display evolve frame by frame:

```bash
# Capture a 40x25 viewport at each iteration of a game loop
agent86 --run-source life.asm --viewport 0,0,40,25 --breakpoints 0150

# Write to file to avoid shell encoding issues
agent86 --run-source life.asm --screen --breakpoints 0150 --output-file frames.json
```

Each snapshot in the JSON contains independent `screen[]` and `cursor` data, so you can diff consecutive snapshots to see exactly what changed between frames.

**Identifying untouched cells with `--vram-fill`:** If you want to see exactly which screen cells a program writes to (vs. which remain at their initial state), pre-fill VRAM with a known pattern:

```bash
agent86 --run-source game.asm --vram-fill "." --screen
```

Any cell still showing `.` in the `screen[]` output was never written by the program. Using a repeating multi-character string like `"ABCD"` makes it even easier to spot partial overwrites. Without an argument, `--vram-fill` uses random characters, which is useful for detecting programs that assume VRAM starts blank.

### Debugging Interactive Programs with Events

For programs that require coordinated keyboard and mouse input (menus, dialogs, drawing tools), use `--events` to script a timeline of interactions:

```bash
# Click a button, then type a response
agent86 --run-source menu.asm --events '[
  {"on":"poll:5","mouse":[320,100,1],"snapshot":true},
  {"on":"poll:10","mouse":[320,100,0]},
  {"on":"read:1","keys":"Y"}
]' --screen
```

**Strategy for building event scripts:**

1. **First run** — Run with no events and check `events.log` output or `haltReason`. If the program is waiting for keyboard input, it will hit the cycle limit or read a default `0Dh`.
2. **Add keyboard events** — Use `read:N` triggers to inject keys at each read point. Start with `read:1`, `read:2`, etc.
3. **Add mouse events** — Use `poll:N` triggers to change mouse position/buttons at the right polling points. The program sees the new state on that exact poll.
4. **Add snapshots** — Attach `"snapshot": true` to events at moments you want to inspect CPU/memory/screen state.
5. **Check `events.pending`** — If events remain pending, the program exited before reaching those triggers. Adjust trigger counts or add more input.
6. **Iterate** — Refine the timeline based on what the program does at each step.

### Debugging File I/O Programs

For programs that use INT 21h file operations (open, read, write, seek, directory listing), use `--dos-root` and inspect the `fileIO` stats:

```
1.  Create a test directory with the files your program expects
2.  Run:  agent86 --run-source fileapp.asm --dos-root ./testdir
3.  Parse the JSON output
4.  Check emulation.exitCode — non-zero usually means a file operation failed
5.  Check emulation.fileIO:
      a. filesOpened matches expected count
      b. bytesRead/bytesWritten match expected transfer sizes
      c. errors == 0 (no failed operations)
6.  If exitCode is non-zero:
      a. Add --breakpoints before and after INT 21h calls
      b. Check CF (carry flag) in the flags register after each INT 21h
      c. If CF=1, check AX for the DOS error code (2=file not found, 3=path not found, 6=bad handle)
7.  For FindFirst/FindNext issues:
      a. Check that the DTA is set up (AH=1Ah) before AH=4Eh
      b. Verify the wildcard pattern (e.g., *.*, *.TXT) in DS:DX
      c. Check fileIO.dirSearches to confirm FindFirst was called
8.  For read/write issues:
      a. Verify the handle in BX is valid (returned by AH=3Ch or 3Dh)
      b. Check AX after read/write — it contains actual bytes transferred
      c. AX=0 on read means EOF was reached
```

### Diagnosing Garbage Output

If the program output looks wrong (garbled, too short, or empty):

1. Check `outputHex` — it shows exact bytes. Compare against expected values.
2. Check `finalState.registers.DX` — for INT 21h/09h, DX points to the string. A wrong DX value means a bad pointer.
3. Check `diagnostics[]` — if output was truncated (no `$` terminator found), a diagnostic will report the bad DX address.
4. Re-run with `--breakpoints` just before the INT 21h call to inspect register state.
5. For VRAM programs, add `--screen` to see what's actually on the virtual display. If `output` is empty but `screen[]` shows text, the program is using direct VRAM writes instead of DOS output calls.
6. Add `--attrs` to check color attributes — a common bug is writing text with attribute `00h` (black on black), making it invisible.

### Diagnostic-Driven Fixing

Every diagnostic includes an actionable `hint` field. Read the hint first — it tells you exactly what to do. The table below provides additional context:

| Error Pattern | Likely Cause | Hint Tells You |
|---|---|---|
| `"Undefined label X"` with hint `"Did you mean 0XH?"` | Hex literal missing leading zero | The correct hex literal with `0` prefix |
| `"Undefined label X"` with hint `"Did you mean 'Y'?"` | Typo in label name | The closest matching symbol and its definition line |
| `"Undefined label X"` with hint about registers | Register used in expression | That registers can't appear in arithmetic expressions |
| `"Undefined label X"` with hint about local labels | `.label` used outside PROC | To wrap code in PROC/ENDP or use a global label |
| `"Conditional jump auto-promoted"` | Jcc target exceeded short range; assembler auto-emitted inverted-Jcc + near JMP (5 bytes) | The expanded encoding details (INFO-level, not an error) |
| `"Loop jump out of range (N)"` | Loop body exceeds 127 bytes | The `DEC CX / JNZ target` replacement pattern |
| `"Size mismatch between operands"` | Mixing 8-bit and 16-bit registers | Both operand names and sizes (e.g., "Op1 is 8-bit (AL), Op2 is 16-bit (BX)") |
| `"Stack ops require 16-bit register"` | `PUSH AL` or similar | The specific 16-bit counterpart to use (e.g., "Use AX instead") |
| `"POP CS is not a valid instruction"` | Attempting POP CS | To use far JMP or far CALL to change CS |
| `"Invalid operands for X"` | Wrong operand combination | All valid forms from the ISA database AND what you actually provided |
| `"Internal: mnemonic 'X' passed ISA validation..."` | Assembler bug (code path missing) | To report the bug |
| `"Invalid numeric literal: X"` | Malformed number | The specific invalid character/digit and what's valid for the base |
| `"Unexpected token in expression: X"` | Register, directive, or invalid token in expression | Whether it's a register (use `[]`), a directive, or unrecognized |
| `"Invalid register in memory operand: X"` | Used AX, CX, DX, SP inside `[]` | Lists the four valid base/index registers |
| `"Invalid addressing mode combination"` | Illegal register combination in `[]` | All valid 8086 addressing mode combinations |
| `"Expected ')'"` | Unclosed parenthesis | To check for unmatched parentheses |
| `"Expected comma in DB/DW/DD"` | Missing comma in data list | Example syntax for the specific directive |
| `"PROC without label"` | PROC directive with no label | Example syntax: `myproc: PROC` |
| `"Extra tokens at end of line"` | Stray content after instruction | Common causes: missing commas, stray characters, comment without `;` |
| `"LEA requires 16-bit register"` | 8-bit register with LEA | Lists all valid 16-bit registers |
| `"Invalid operands for LEA"` | Wrong operand types for LEA | Example: `LEA DI, [BX+SI+10h]` |
| `"Invalid IN/OUT operands"` | Wrong I/O operand combination | All valid IN or OUT forms |
| `"Division by zero"` | Expression divides by zero | To check the divisor value or EQU constant |
| `"Cannot define macro with reserved name 'X'"` | MACRO name shadows MOV, AX, DUP, etc. | Rename the macro to a non-reserved identifier |
| `"MACRO 'X' without matching ENDM"` | Unclosed macro definition | Add `ENDM` after the macro body |
| `"ENDM without matching MACRO, REPT, or IRP"` | Orphan ENDM | Remove the stray ENDM or add the missing opening directive |
| `"REPT count must be a non-negative numeric literal"` | REPT with EQU constant, expression, or invalid token | Use a literal number (e.g., `5`, `0Ah`, `100b`), not a symbol |
| `"REPT/IRP without matching ENDM"` | Unclosed repeat block | Add `ENDM` to close the block |
| `"IRP directive missing angle-bracket list"` | IRP without `<...>` items | Use `IRP param, <item1, item2, ...>` syntax |
| `"Macro expansion iteration limit exceeded"` | Recursive or mutually-recursive macros | The macro expands to itself endlessly; break the cycle |

**Warnings** (assembly succeeds but something may be wrong):

| Warning Pattern | Meaning | Hint Tells You |
|---|---|---|
| `"uses 80186+ encoding"` | Shift/rotate with imm > 1 | The concrete `MOV CL, N / SHL reg, CL` replacement code |
| `"No size prefix on memory-immediate"` | `MOV [BX], 5` without BYTE/WORD | To add BYTE or WORD before the memory operand, with example |
| `"No size prefix on memory shift/rotate"` | `SHL [BX], 1` without BYTE/WORD | To add BYTE or WORD before the memory operand |
| `"truncated to 8-bit"` | Immediate exceeds 8-bit range | The truncated result and the valid range |
| `"truncated to 16-bit"` | Immediate exceeds 16-bit range | The truncated result and the valid range |
| `"ORG directive after code"` | ORG used after code/data emitted | That ORG doesn't move code; place at start |
| `"redefined"` | Label defined more than once | Previous definition line; suggests local labels |
| `"Local label"` + `"outside procedure"` | `.label` used without enclosing PROC | To use PROC/ENDP or a global label |
| `"Macro 'X' redefined"` | Macro defined twice | Previous definition line; second definition replaces first |
| `"Macro 'X' invoked with N args, expected M"` | Argument count mismatch | Missing args become empty strings; extras are ignored |

### Verifying Correctness

Even when `success` is `true`, always cross-check the `listing[].decoded` field against your intent:

```json
{ "decoded": "MOV REG(AX), IMM(5)", "src": "    MOV AX, 5" }
```

If `decoded` shows something unexpected (e.g., you wrote `MOV AX, BX` but decoded says `MOV REG(AX), IMM(0)`), the operand was misparsed. This is your primary self-verification mechanism.

**Cross-checking symbols:** After assembly, verify that label addresses in `symbols{}` make sense relative to your code layout. If a label has an unexpected address, an earlier instruction may be a different size than you assumed.

### Using `--explain` for Planning

Before writing an instruction you haven't used before, query the ISA database:

```bash
agent86 --explain IMUL
```

This returns all valid operand forms and any constraints. Use this to avoid trial-and-error assembly cycles.

### Conditional Jump Auto-Promotion

Conditional jumps (Jcc) that target a label beyond the short range (-128 to +127 bytes) are automatically promoted to a near form. The assembler transparently emits an inverted-Jcc that skips over a 3-byte near JMP, expanding the 2-byte short jump into a 5-byte sequence. This is completely transparent -- just write `JZ far_label` and the assembler handles the rest.

LOOP, LOOPZ, LOOPNZ, and JCXZ are NOT auto-promoted because they have no near-form equivalents. If a LOOP target is out of range, replace it with `DEC CX` / `JNZ target`.

The promoted form uses 5 bytes instead of 2, which may affect code layout. Check `listing[].size` in the `--agent` output to see which jumps were promoted.

---

## Common Patterns

### Minimal DOS .COM Program

```asm
ORG 100h              ; .COM files are loaded at offset 100h

start:
    MOV AH, 09h       ; DOS function: print string
    MOV DX, message    ; DS:DX -> string
    INT 21h            ; Call DOS
    INT 20h            ; Terminate program

message DB 'Hello, World!', 0Dh, 0Ah, '$'
```

### Open, Read, and Close a File

```asm
ORG 100h
    ; Open file for reading
    MOV AH, 3Dh
    MOV AL, 0              ; mode 0 = read-only
    MOV DX, filename
    INT 21h
    JC error               ; CF=1 means open failed (AX=error code)
    MOV [handle], AX       ; save handle

    ; Read up to 128 bytes
    MOV BX, [handle]
    MOV AH, 3Fh
    MOV CX, 128            ; max bytes to read
    MOV DX, buffer
    INT 21h
    JC error
    ; AX = actual bytes read (may be < 128 if file is smaller)

    ; Close file
    MOV BX, [handle]
    MOV AH, 3Eh
    INT 21h

    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
error:
    MOV AH, 4Ch
    MOV AL, 1              ; exit code 1 = error
    INT 21h

filename:
    DB 'DATA.TXT', 0
handle:
    DW 0
buffer:
    RESB 128
```

Run with: `agent86 --run-source reader.asm --dos-root ./testdir`

### Create, Write, and Close a File

```asm
ORG 100h
    ; Create file (truncates if exists)
    MOV AH, 3Ch
    MOV CX, 0              ; normal attributes
    MOV DX, filename
    INT 21h
    JC error
    MOV [handle], AX

    ; Write data
    MOV BX, [handle]
    MOV AH, 40h
    MOV CX, 5              ; 5 bytes
    MOV DX, message
    INT 21h
    JC error

    ; Close
    MOV BX, [handle]
    MOV AH, 3Eh
    INT 21h

    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
error:
    MOV AH, 4Ch
    MOV AL, 1
    INT 21h

filename:
    DB 'OUTPUT.TXT', 0
message:
    DB 'Hello'
handle:
    DW 0
```

### Write to STDOUT via File Handle

```asm
ORG 100h
    ; Handle 1 = STDOUT, always available (no --dos-root needed)
    MOV BX, 1              ; stdout handle
    MOV AH, 40h
    MOV CX, 13
    MOV DX, msg
    INT 21h

    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
msg:
    DB 'Hello, World!', 0Dh, 0Ah
```

### Directory Listing with FindFirst/FindNext

```asm
ORG 100h
    ; Set DTA to our buffer
    MOV AH, 1Ah
    MOV DX, dta
    INT 21h

    ; FindFirst: search for *.TXT
    MOV AH, 4Eh
    MOV CX, 0              ; normal files only
    MOV DX, pattern
    INT 21h
    JC done                 ; CF=1 = no files found

next:
    ; DTA+1Eh contains the matched filename (13 bytes, ASCIIZ)
    ; Process the file here...

    ; FindNext
    MOV AH, 4Fh
    INT 21h
    JNC next                ; CF=0 = found another file

done:
    MOV AH, 4Ch
    MOV AL, 0
    INT 21h

pattern:
    DB '*.TXT', 0
dta:
    RESB 43                 ; 43-byte DTA buffer
```

Run with: `agent86 --run-source dirlist.asm --dos-root ./testdir`

### List All Files and Subdirectories

```asm
ORG 100h
    ; Set DTA
    MOV AH, 1Ah
    MOV DX, dta
    INT 21h

    ; FindFirst: *.* with CX=10h to include subdirectories
    MOV AH, 4Eh
    MOV CX, 10h            ; include directories
    MOV DX, pattern
    INT 21h
    JC done

next:
    ; Check if this entry is a directory
    MOV AL, [dta + 15h]    ; attribute byte
    TEST AL, 10h
    JZ not_dir
    ; It's a directory — print "<DIR> " prefix
    MOV AH, 09h
    MOV DX, dir_tag
    INT 21h
    JMP print_name
not_dir:
    ; It's a file — print "     " padding
    MOV AH, 09h
    MOV DX, file_tag
    INT 21h
print_name:
    ; Print the filename from DTA+1Eh using character output
    MOV SI, dta + 1Eh
print_loop:
    MOV DL, [SI]
    CMP DL, 0
    JE print_done
    MOV AH, 02h
    INT 21h
    INC SI
    JMP print_loop
print_done:
    ; Print newline
    MOV DL, 0Dh
    MOV AH, 02h
    INT 21h
    MOV DL, 0Ah
    MOV AH, 02h
    INT 21h

    ; FindNext
    MOV AH, 4Fh
    INT 21h
    JNC next

done:
    MOV AH, 4Ch
    MOV AL, 0
    INT 21h

pattern:
    DB '*.*', 0
dir_tag:
    DB '<DIR> $'
file_tag:
    DB '      $'
dta:
    RESB 43
```

Run with: `agent86 --run-source listall.asm --dos-root ./myfiles`

### File Seek (Read from Specific Offset)

```asm
ORG 100h
    ; Open file
    MOV AH, 3Dh
    MOV AL, 0
    MOV DX, fname
    INT 21h
    JC error
    MOV BX, AX             ; handle in BX

    ; Seek to offset 10 from start
    MOV AH, 42h
    MOV AL, 0              ; SEEK_SET (0=start, 1=current, 2=end)
    MOV CX, 0              ; high word of offset
    MOV DX, 10             ; low word of offset
    INT 21h
    JC error
    ; DX:AX = new absolute position

    ; Read 4 bytes from that position
    MOV AH, 3Fh
    MOV CX, 4
    MOV DX, buf
    INT 21h

    ; Close
    MOV AH, 3Eh
    INT 21h

    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
error:
    MOV AH, 4Ch
    MOV AL, 1
    INT 21h

fname:
    DB 'DATA.BIN', 0
buf:
    RESB 16
```

### Change Directory and Open Relative File

```asm
ORG 100h
    ; Change to subdirectory
    MOV AH, 3Bh
    MOV DX, dirname
    INT 21h
    JC error

    ; Open a file relative to the new current directory
    MOV AH, 3Dh
    MOV AL, 0
    MOV DX, fname
    INT 21h
    JC error
    MOV BX, AX

    ; Read and close...
    MOV AH, 3Fh
    MOV CX, 64
    MOV DX, buf
    INT 21h
    MOV AH, 3Eh
    INT 21h

    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
error:
    MOV AH, 4Ch
    MOV AL, 1
    INT 21h

dirname:
    DB 'SUBDIR', 0
fname:
    DB 'FILE.TXT', 0
buf:
    RESB 64
```

### Direct VRAM Text Output

```asm
ORG 100h

; Set up ES to point to VRAM
MOV AX, 0B800h
MOV ES, AX

; Write 'Hi' at top-left with white-on-blue attribute (1Fh)
; VRAM layout: [char][attr][char][attr]...
; Offset 0 = row 0, col 0. Offset 2 = row 0, col 1.
XOR DI, DI          ; ES:DI -> start of VRAM
MOV AX, 1F48h       ; 'H' (48h) with attr 1Fh
STOSW               ; Write to [0], DI += 2
MOV AX, 1F69h       ; 'i' (69h) with attr 1Fh
STOSW               ; Write to [2], DI += 2

MOV AH, 4Ch
INT 21h
```

Run with `agent86 --run-source vram_hi.asm --viewport 0,0,10,1 --attrs` to see:
```json
"screen": ["Hi        "],
"screenAttrs": ["1F1F0707070707070707"]
```

### Fill Screen with Color using REP STOSW

```asm
ORG 100h

MOV AX, 0B800h
MOV ES, AX
XOR DI, DI
MOV CX, 4000        ; 80 cols * 50 rows = 4000 cells
MOV AX, 1F20h       ; Space (20h) with white-on-blue (1Fh)
CLD
REP STOSW            ; Fill entire screen

MOV AH, 4Ch
INT 21h
```

### Subroutine Call Convention

```asm
; Caller
    PUSH AX            ; Save registers you need preserved
    PUSH BX
    CALL my_subroutine
    POP BX             ; Restore in reverse order
    POP AX

my_subroutine: PROC
    PUSH BP            ; Standard frame setup
    MOV BP, SP
    ; ... body ...
    POP BP
    RET
ENDP
```

### Loop with Counter

```asm
    MOV CX, 10        ; Iterate 10 times
.loop:
    ; ... loop body ...
    LOOP .loop         ; Decrements CX, jumps if CX != 0
```

### Zeroing a Register

```asm
    XOR AX, AX        ; AX = 0 (2 bytes, preferred over MOV AX, 0 which is 3 bytes)
```

### Multi-Precision Arithmetic

```asm
    ; 32-bit addition: DX:AX = DX:AX + CX:BX
    ADD AX, BX        ; Add low words
    ADC DX, CX        ; Add high words + carry from low addition

    ; 32-bit subtraction: DX:AX = DX:AX - CX:BX
    SUB AX, BX        ; Subtract low words
    SBB DX, CX        ; Subtract high words - borrow from low subtraction

    ; Example: Add 0x12345678 + 0x00001000
    MOV DX, 1234h     ; DX:AX = 0x12345678
    MOV AX, 5678h
    ADD AX, 1000h     ; Add to low word
    ADC DX, 0         ; Propagate carry to high word
    ; Result: DX:AX = 0x12355678
```

### Conditional Branching

```asm
    CMP AX, BX
    JG .ax_greater     ; Signed comparison: AX > BX
    JL .ax_less        ; Signed comparison: AX < BX
    ; Fall through: AX == BX
    JMP .done
.ax_greater:
    ; Handle AX > BX
    JMP .done
.ax_less:
    ; Handle AX < BX
.done:
```

### Saving and Restoring Flags

```asm
    PUSHF              ; Save all flags
    ; ... operations that modify flags ...
    POPF               ; Restore original flags
```

### Sign Extension for Division

```asm
    ; 16-bit signed divide: DX:AX / BX
    MOV AX, -100       ; Dividend
    CWD                ; Sign-extend AX into DX:AX
    MOV BX, 7
    IDIV BX            ; AX = quotient, DX = remainder
```

### Exchanging Values

```asm
    MOV AX, 1234h
    MOV BX, 5678h
    XCHG AX, BX       ; AX=5678h, BX=1234h (no temp register needed)
```

### Table Lookup with XLAT

```asm
    ; Convert a nibble (0-15) to its hex ASCII character
hex_table: DB '0123456789ABCDEF'

    MOV BX, hex_table  ; BX = table base address
    MOV AL, 0Ah        ; AL = index (10 = 'A')
    XLAT                ; AL = hex_table[10] = 'A'
    MOV DL, AL
    MOV AH, 02h
    INT 21h             ; Print 'A'
```

### Saving and Restoring All Registers

```asm
    ; PUSHA/POPA save and restore all 8 general registers in one instruction (80186+)
    PUSHA               ; Push AX, CX, DX, BX, SP, BP, SI, DI
    ; ... do work that clobbers registers ...
    POPA                ; Restore all registers (SP value from stack is discarded)
```

> **Note:** PUSHA saves the original SP value, but POPA discards it — SP is restored naturally by the 8 pops. PUSHA/POPA are 80186+ instructions (1 byte each vs 16 bytes for 8 individual PUSH/POP).

### Macro: Save/Restore Specific Registers

Use IRP inside a macro to push and pop a chosen set of registers:

```asm
SaveRegs MACRO
    IRP r, <AX, BX, CX, DX>
        PUSH r
    ENDM
ENDM

RestoreRegs MACRO
    IRP r, <DX, CX, BX, AX>
        POP r
    ENDM
ENDM

    SaveRegs
    ; ... work ...
    RestoreRegs
```

### Macro: Print a Character

```asm
PrintChar MACRO ch
    MOV DL, ch
    MOV AH, 02h
    INT 21h
ENDM

NewLine MACRO
    PrintChar 0Dh
    PrintChar 0Ah
ENDM

    PrintChar 'H'
    PrintChar 'i'
    NewLine
```

### Macro: Define Data with Label

```asm
DefString MACRO name, text
name:
    DB text, '$'
ENDM

DefString greeting, 'Hello World!'

    MOV AH, 09h
    MOV DX, greeting
    INT 21h
```

### Macro: Fill Memory with Non-Zero Value

```asm
; RESB fills with zeros; use REPT inside a macro for any value
FillMem MACRO count, val
    REPT count
        DB val
    ENDM
ENDM

buffer:
FillMem 16, 0FFh    ; 16 bytes of FFh
```

### Macro: Power-of-Two Table via IRP

```asm
powers:
IRP val, <01h, 02h, 04h, 08h, 10h, 20h, 40h, 80h>
    DB val
ENDM
; powers is an 8-byte table: 1, 2, 4, 8, 16, 32, 64, 128
```

---

## Emulator Internals

### Memory Layout

The emulator initializes a flat 64KB memory space plus a separate VRAM buffer:
- **0000h-0001h**: `CD 20` (INT 20h) — PSP termination stub. If a program does `RET` with an empty stack, IP goes to 0000h and executes INT 20h for a clean exit.
- **0100h onwards**: Program binary loaded here (standard .COM load address).
- **FFFEh**: Initial stack pointer (grows downward).
- **Segment B800h (B8000h–B9F3Fh)**: 8KB VRAM buffer for 80×50 CGA text-mode display. Each cell is 2 bytes: `[character][attribute]`. Reads and writes via segment `B800h` are intercepted and routed to this buffer. See the VRAM section below.

### VRAM and Screen Capture

The emulator maintains a separate 8KB VRAM buffer mapped at segment `B800h` (physical addresses `B8000h–B9F3Fh`). This is a faithful model of CGA text-mode video memory: 80 columns × 50 rows × 2 bytes per cell = 8000 bytes.

Programs write to VRAM in two ways, both fully supported:

1. **Direct VRAM writes** — Set `ES` to `B800h` and write character/attribute pairs via `MOV`, `STOSW`, `REP STOSW`, etc. This is how high-performance DOS programs (games, demos, TUI apps) render their displays.

2. **INT 10h services** — Use BIOS video interrupts for cursor positioning, teletype output, scrolling, and screen clearing. See the INT 10h section under Emulated Interrupts.

Additionally, all DOS text output (INT 21h functions 02h, 06h, 09h) is mirrored to VRAM automatically — characters appear at the current cursor position and the cursor advances. This means `--screen` captures output from both DOS and BIOS paths.

**Screen capture** is controlled by `--screen` and `--viewport`:
- Without either flag: no `screen[]` array in JSON output. The VRAM is still maintained internally (INT 10h works correctly), but its contents are not serialized. This keeps output compact for programs that don't use VRAM.
- `--screen`: Captures the full 80×50 grid as 50 strings of 80 characters each.
- `--viewport 0,0,40,25`: Captures only the specified rectangle (col, row, width, height). Useful for focusing on the active area of a program that only uses part of the screen.
- `--attrs`: Adds a parallel `screenAttrs[]` array containing hex-encoded attribute bytes (2 hex digits per cell). Omitted by default since most debugging only needs the text.

**VRAM pre-fill** is controlled by `--vram-fill [text]`:
- `--vram-fill "ABCD"`: Tiles the string `ABCD` cyclically across all 4000 character cells (cell 0 = `A`, cell 1 = `B`, ..., cell 4 = `A`, ...). Attribute bytes remain at the default `07h` (light gray on black). This makes it easy to identify which cells a program overwrites.
- `--vram-fill` (no argument): Fills each cell with a random printable character from `[a-zA-Z0-9!@#$%&*+-=.:~ ]`. Useful for detecting programs that assume VRAM starts as spaces.
- Without this flag (default): VRAM initializes to spaces (`20h`) with attribute `07h`, matching real CGA hardware power-on state.

**Dual output** means a "Hello World" program using INT 21h/09h will show `"Hello World"` in both the `output` field and the `screen[]` array (at the cursor position). The `output` field captures the raw byte stream; `screen[]` captures the spatial layout on the virtual monitor.

### Self-Modifying Code

The emulator detects memory writes and resyncs its internal code buffer. Programs that modify their own instructions at runtime will execute correctly.

### Cycle Limit

Default: 1,000,000 cycles. Override with `--max-cycles`. When the limit is reached, execution halts with `haltReason: "Cycle limit reached"`. If your program needs more cycles, increase the limit. If it hits the limit unexpectedly, you likely have an infinite loop — check the `finalState` to see where execution was stuck.

### Output Safety

All program output is JSON-safe regardless of what bytes the program produces:
- Bytes 0x00-0x1F (control chars) are escaped as `\u00XX`
- Bytes 0x80-0xFF (non-ASCII) are escaped as `\u00XX`
- Printable ASCII (0x20-0x7E) passes through normally
- The `outputHex` field provides the raw bytes for binary-exact comparison
- Output is capped at 4096 bytes with a diagnostic warning if truncated

---

## Known Limitations

1. **No linker.** This assembler produces flat binaries only. There is no object file output and no linking. Multiple source files are supported via the `INCLUDE` directive.
2. **No 32-bit mode.** Only 16-bit real-mode instructions are supported (8086/8088, with 80186 shift extensions).
3. **No segment directives.** There is no `.code`, `.data`, `.stack`, or `SEGMENT`/`ENDS` support. All code and data share one flat segment.
4. ~~No macro system.~~ **Resolved** — `MACRO`/`ENDM`, `LOCAL`, `REPT`, and `IRP` are now supported. See [Macros](#macros).
5. **No `BYTE PTR` / `WORD PTR` syntax.** Use `BYTE` or `WORD` directly before the memory operand (e.g., `MOV BYTE [BX], 5`, not `MOV BYTE PTR [BX], 5`).
6. **No 8086-strict mode.** The assembler will produce 80186 opcodes (C0h/C1h for shift-by-immediate) with a warning rather than rejecting them.
7. **No floating-point (8087) instructions.**
8. **Duplicate labels emit a warning.** Redefining a label overwrites the previous value but emits a `WARNING` diagnostic with the previous definition line.
9. **No `TIMES` directive.** Use `RESB` for zero-fill, `REPT N` / `DB val` / `ENDM` for non-zero fill, or `DB` with comma-separated repeated values.
10. **Emulator: I/O ports not emulated.** `IN` and `OUT` instructions are logged as skipped.
11. **Emulator: Limited DOS/BIOS interrupt support.** INT 20h, INT 21h (console I/O: 01h, 02h, 06h, 09h; system: 0Eh, 19h, 25h, 2Ah, 2Ch, 2Fh, 30h, 35h, 62h; file I/O: 1Ah, 3Bh–3Fh, 40h, 42h–44h, 47h, 4Ch, 4Eh, 4Fh, 57h — file functions require `--dos-root`), INT 10h (functions 00h, 02h, 03h, 06h, 07h, 08h, 09h, 0Ah, 0Eh, 0Fh), and INT 33h (mouse, functions 00h–08h, 0Bh, 0Ch; requires `--mouse` or `--events` with mouse actions) are handled. INT 10h font services (AH=11h) are not implemented. Other interrupts are logged as skipped.
12. **Emulator: No hardware interrupt simulation.** Timer, keyboard, and other hardware interrupts are not generated.

---

## Error Codes Reference

All diagnostics include an actionable `hint` field. For programmatic handling, pattern-match on these key substrings:

### Errors (`level: "ERROR"`)

| Substring in `msg` | Meaning | Hint Contains |
|---|---|---|
| `"Undefined label"` | Symbol not found in symbol table | Hex prefix fix, fuzzy match to closest symbol, or register/local-label guidance |
| `"Size mismatch"` | Operand widths don't match | Both operand names and their bit widths |
| `"Conditional jump auto-promoted"` | *(INFO)* Jcc target exceeded short range; assembler auto-emitted inverted-Jcc + near JMP (5 bytes) | The expanded encoding details |
| `"Loop jump out of range"` | LOOP displacement exceeds +/-127 | `DEC CX / JNZ` replacement pattern |
| `"Invalid operands"` | Operand types don't match any valid form | All valid forms from ISA DB + what you provided |
| `"Invalid register in memory"` | Used AX, CX, DX, SP inside `[]` | Lists the four valid registers (BX, BP, SI, DI) |
| `"Invalid addressing mode"` | Illegal register combination in `[]` | All valid 8086 addressing mode combinations |
| `"Invalid numeric literal"` | Malformed number | Specific invalid character and valid digits for the base |
| `"Division by zero"` | Expression evaluated `/ 0` | To check divisor value or EQU constant |
| `"Expected ')'"` | Unclosed parenthesis in expression | Parentheses check guidance |
| `"Expected comma"` | Missing comma in DB/DW/DD list | Example syntax for the directive |
| `"Unexpected token in expression"` | Invalid token in arithmetic expression | Whether it's a register, directive, bracket, or unknown |
| `"Stack ops require 16-bit"` | Attempted PUSH/POP with 8-bit register | The specific 16-bit counterpart (e.g., AX for AL) |
| `"POP CS"` | Architecturally invalid instruction | To use far JMP or far CALL instead |
| `"IN dest must be AL/AX"` | IN with wrong destination | Valid IN forms with examples |
| `"OUT src must be AL/AX"` | OUT with wrong source | Valid OUT forms with examples |
| `"LEA requires 16-bit"` | LEA with 8-bit destination | Lists valid 16-bit registers |
| `"PROC without label"` | PROC directive with no label | Example syntax |
| `"Extra tokens"` | Stray content after instruction | Common causes |
| `"Internal: mnemonic"` | Assembler bug: no encoder for ISA entry | To report the bug |
| `"Cannot open include file"` | INCLUDE references a file that doesn't exist | The resolved path and base directory used for resolution |
| `"Circular include detected"` | File A includes B which includes A (directly or indirectly) | The canonical path of the file already in the include chain |
| `"Include nesting depth exceeded"` | INCLUDE chain deeper than 16 levels | To check for deeply nested or recursive chains |
| `"INCLUDE directive missing filename"` | INCLUDE with no filename after it | Usage syntax examples |
| `"Unterminated string in INCLUDE"` | Opening quote without matching close quote | The line containing the unterminated string |
| `"Cannot define macro with reserved name"` | MACRO name shadows an instruction, register, or directive | — |
| `"MACRO ... without matching ENDM"` | Macro definition has no closing ENDM | — |
| `"ENDM without matching MACRO, REPT, or IRP"` | Orphan ENDM with no opening block | — |
| `"REPT directive missing repeat count"` | REPT with no count argument | Usage syntax |
| `"REPT count must be a non-negative numeric literal"` | REPT count is not a valid number | The invalid token |
| `"REPT without matching ENDM"` | REPT block has no closing ENDM | — |
| `"IRP directive missing parameter name"` | IRP with no parameter | Usage syntax |
| `"IRP directive missing angle-bracket list"` | IRP without `<...>` item list | Usage syntax |
| `"IRP without matching ENDM"` | IRP block has no closing ENDM | — |
| `"Macro expansion iteration limit exceeded"` | Infinite or deeply recursive macro expansion | Check for recursive macros |

### Warnings (`level: "WARNING"`)

| Substring in `msg` | Meaning | Hint Contains |
|---|---|---|
| `"uses 80186+ encoding"` | Shift/rotate imm>1 produces non-8086 opcode | Concrete `MOV CL, N / SHL reg, CL` replacement |
| `"No size prefix on memory-immediate"` | `MOV [BX], 5` without BYTE/WORD | Example with BYTE and WORD prefix |
| `"No size prefix on memory shift/rotate"` | `SHL [BX], 1` without BYTE/WORD | Example with BYTE and WORD prefix |
| `"truncated to 8-bit"` | Immediate exceeds 8-bit range | The truncated result and valid range |
| `"truncated to 16-bit"` | Immediate exceeds 16-bit range | The truncated result and valid range |
| `"ORG directive after code"` | ORG used after code/data emitted | That ORG doesn't move code; place at start |
| `"redefined"` | Label defined more than once | Previous definition line; suggests local labels |
| `"Local label"` + `"outside procedure"` | `.label` used without enclosing PROC | To use PROC/ENDP or a global label |
| `"Macro ... redefined"` | Macro name defined more than once | Previous definition line |
| `"Macro ... invoked with N args, expected M"` | Argument count mismatch on macro invocation | Whether missing args become empty or extras are ignored |

---

## Appendix: Operand Encoding Reference

For advanced debugging, here is how the assembler encodes operands in the `decoded` field:

| Decoded Format | Meaning | Example |
|---|---|---|
| `REG(AX)` | General register | `MOV REG(AX), IMM(5)` |
| `SREG(DS)` | Segment register | `MOV SREG(DS), REG(AX)` |
| `IMM(100)` | Immediate value (decimal) | `INT IMM(33)` (= INT 21h) |
| `MEM(WORD [BX])` | Memory, word-sized, via BX | `MOV REG(AX), MEM(WORD [BX])` |
| `MEM(BYTE [100])` | Memory, byte-sized, direct address | `MOV MEM(BYTE [100]), IMM(5)` |
| `MEM(WORD [BX+SI+8])` | Memory with base+index+disp | `LEA REG(DI), MEM(WORD [BX+SI+8])` |
| `MEM(WORD SEG:[BX])` | Memory with segment override | Segment prefix byte emitted before opcode |
