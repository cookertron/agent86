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
| `--input <string>` | Provide stdin input for the program (consumed by INT 21h/01h and 06h). | Empty |
| `--mem-dump <addr,len>` | Include memory dump in breakpoint snapshots. Address in hex, length in decimal. | None |
| `--screen` | **Capture full 80×50 screen** from VRAM into JSON output. | Off |
| `--viewport <col,row,w,h>` | **Capture a rectangular region** of the screen. Implies `--screen` behavior but only for the specified window. | Off |
| `--attrs` | **Include attribute bytes** in screen output. Emits `screenAttrs[]` alongside `screen[]`. | Off |
| `--output-file <path>` | **Write JSON to file** instead of stdout. Bypasses shell encoding issues (no BOM, no re-encoding). | stdout |

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

**Write output to file (avoids PowerShell encoding issues):**
```bash
agent86 --run-source life.asm --screen --output-file result.json
```

---

## JSON Output Schema (`--agent`)

Every `--agent` invocation produces a single JSON object on stdout with this structure:

```json
{
  "success": true,

  "diagnostics": [
    {
      "level": "ERROR",
      "line": 5,
      "msg": "Conditional jump out of range (-200)",
      "hint": "Displacement is -200 bytes (range: -128 to +127). Restructure as: JNZ .skip / JMP target / .skip:"
    }
  ],

  "symbols": {
    "START": { "val": 0, "type": "LABEL", "line": 1 },
    "BUFSIZE": { "val": 256, "type": "EQU", "line": 2 }
  },

  "listing": [
    {
      "addr": 0,
      "line": 3,
      "size": 3,
      "decoded": "MOV REG(AX), IMM(5)",
      "src": "    MOV AX, 5",
      "bytes": [184, 5, 0]
    }
  ]
}
```

### Field Reference

**`success`** — `true` if a binary was written. `false` if any errors occurred. The binary file is NOT created on failure.

**`diagnostics[]`** — All errors and warnings from pass 2. Each entry contains:
- `level`: Severity. `ERROR` means assembly failed. `WARNING` means the binary was produced but something may be wrong (e.g., 80186-only encoding used). `INFO` is advisory.
- `line`: 1-indexed source line number.
- `msg`: Human-readable description of the problem.
- `hint`: Agent-actionable context. Contains a concrete fix, valid alternatives, or a code pattern you can apply directly. **Always read this field first — every diagnostic now includes an actionable hint written specifically for you.**

**`symbols{}`** — The complete symbol table after assembly. Each key is an uppercase symbol name.
- `val`: Resolved integer value (address for labels, constant for EQU).
- `type`: `"LABEL"` (address in code) or `"EQU"` (compile-time constant).
- `line`: Source line where the symbol was defined.

**`listing[]`** — One entry per instruction/directive that emitted bytes. This is your primary debugging view.
- `addr`: Starting byte address of this instruction in the binary.
- `line`: Source line number.
- `size`: Number of bytes emitted.
- `decoded`: How the assembler *interpreted* your instruction (e.g., `MOV REG(AX), IMM(5)`). **Compare this against your intent to detect misparses.**
- `src`: Original source text.
- `bytes`: Raw machine code bytes as decimal integers.

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

**`screen[]`** — *(Only present when `--screen` or `--viewport` is used.)* Array of strings, one per row. Each string contains the visible text characters from the VRAM viewport. Non-printable bytes are replaced with `.` for clean JSON. Without `--screen`/`--viewport`, this field is omitted entirely to keep output compact.

**`screenAttrs[]`** — *(Only present when `--attrs` is also used.)* Array of strings, one per row, parallel to `screen[]`. Each string contains two hex digits per cell representing the CGA text-mode attribute byte (e.g., `"07"` = light grey on black, `"1F"` = white on blue). Omitted unless `--attrs` is specified.

**`skipped[]`** — Instructions that were encountered but not fully emulated (e.g., unimplemented interrupts, I/O ports). Each entry:
- `instruction`: Disassembly text of the skipped instruction.
- `reason`: Why it was skipped.

**`diagnostics[]`** — Runtime diagnostics (e.g., output truncation warnings). Distinct from assembly diagnostics.

---

## JSON Output Schema (`--run`)

The `--run` flag runs a pre-compiled `.COM` binary. The JSON output is the same as the `emulation` section of `--run-source`, but as a top-level object. It also includes additional detail:

- `finalState.sregs`: Segment registers (ES, CS, SS, DS) as hex strings.
- `finalState.flagBits`: Individual flags as booleans.
- `finalState.cursor`: VRAM cursor position as `{"row": N, "col": N}`.
- `snapshots[]`: Breakpoint and watchpoint snapshots (see Breakpoints section).
- `screen[]`: Screen text (when `--screen` or `--viewport` is used).
- `screenAttrs[]`: Attribute hex strings (when `--attrs` is also used).

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
| `JMP` | Near (E9, 3 bytes) | Full 16-bit range |
| `CALL` | Near (E8, 3 bytes) | Full 16-bit range |
| `RET` | C3 (1 byte) | Returns to caller |

**Conditional (all short jumps, 2 bytes, range: -128 to +127 bytes):**

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

> **Critical range limitation:** All conditional jumps and loops are short-range only (-128 to +127 bytes from the *end* of the instruction). If you get a "jump out of range" error, restructure your code by inverting the condition and using a near JMP. For example, replace `JZ far_label` with: `JNZ .skip` / `JMP far_label` / `.skip:`

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

### INT 20h — Program Terminate

Halts emulation with `exitCode: 0`. This is the standard .COM termination method. A program that returns with an empty stack (RET when SP=FFFEh) will execute the `INT 20h` at address 0000h (placed there by the emulator as a PSP stub).

### INT 21h — DOS Function Calls

| AH | Function | Behavior |
|---|---|---|
| `01h` | Read character with echo | Reads one byte from `--input` string (or 0Dh if exhausted). Returns in AL. Echoes to output. |
| `02h` | Write character | Writes DL to output buffer. |
| `06h` | Direct console I/O | If DL=FFh: reads input (ZF=0 and AL=char if available, ZF=1 if not). Otherwise: writes DL to output. |
| `09h` | Write $-terminated string | Reads bytes from DS:DX until `$` terminator. Writes to output buffer. **Capped at 4096 bytes** with diagnostic if truncated (possible bad pointer). |
| `2Ah` | Get date (stub) | Returns CX=year, DH=month, DL=day, AL=day-of-week. |
| `2Ch` | Get time (stub) | Returns CH=hour, CL=minute, DH=second, DL=centisecond. |
| `30h` | Get DOS version (stub) | Returns AL=5 (major), AH=0 (minor). |
| `4Ch` | Exit with return code | Halts emulation with `exitCode` = AL. |

Any other AH value is logged in `skipped[]` as `"Unimplemented DOS function"`.

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
| `"Conditional jump out of range (N)"` | Target too far for short jump | The exact inverted condition and restructure pattern (e.g., `JNZ .skip / JMP target / .skip:`) |
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

### Working with Conditional Jump Limits

Conditional jumps are limited to -128 to +127 bytes. When planning loops or branching over large code blocks, calculate byte distances using the `listing[].addr` and `listing[].size` fields from a previous assembly attempt.

**Inversion pattern for out-of-range conditionals:**

```asm
; BEFORE (fails if far_handler is too far away):
    CMP AX, 0
    JZ far_handler

; AFTER (always works):
    CMP AX, 0
    JNZ .skip
    JMP far_handler
.skip:
```

**Condition inversion table:**

| Original | Inverted |
|---|---|
| `JZ` / `JE` | `JNZ` / `JNE` |
| `JL` | `JGE` |
| `JG` | `JLE` |
| `JB` / `JC` | `JAE` / `JNC` |
| `JA` | `JBE` |
| `JS` | `JNS` |
| `JO` | `JNO` |
| `JP` | `JNP` |

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

1. **No linker.** This assembler produces flat binaries only. There is no object file output, no linking, and no support for multiple source files or `INCLUDE`.
2. **No 32-bit mode.** Only 16-bit real-mode instructions are supported (8086/8088, with 80186 shift extensions).
3. **No segment directives.** There is no `.code`, `.data`, `.stack`, or `SEGMENT`/`ENDS` support. All code and data share one flat segment.
4. **No macro system.** No `MACRO`/`ENDM`, no `REPT`, no `IRP`. Use EQU for constants and PROC/ENDP for structuring.
5. **No `BYTE PTR` / `WORD PTR` syntax.** Use `BYTE` or `WORD` directly before the memory operand (e.g., `MOV BYTE [BX], 5`, not `MOV BYTE PTR [BX], 5`).
6. **No 8086-strict mode.** The assembler will produce 80186 opcodes (C0h/C1h for shift-by-immediate) with a warning rather than rejecting them.
7. **No floating-point (8087) instructions.**
8. **Duplicate labels emit a warning.** Redefining a label overwrites the previous value but emits a `WARNING` diagnostic with the previous definition line.
9. **No `TIMES` directive.** Use `RESB` for zero-fill or `DB` with comma-separated repeated values.
10. **Emulator: I/O ports not emulated.** `IN` and `OUT` instructions are logged as skipped.
11. **Emulator: Limited DOS/BIOS interrupt support.** INT 20h, INT 21h (functions 01h, 02h, 06h, 09h, 2Ah, 2Ch, 30h, 4Ch), and INT 10h (functions 00h, 02h, 03h, 06h, 07h, 08h, 09h, 0Ah, 0Eh, 0Fh) are handled. INT 10h font services (AH=11h) are not implemented. Other interrupts are logged as skipped.
12. **Emulator: No hardware interrupt simulation.** Timer, keyboard, and other hardware interrupts are not generated.

---

## Error Codes Reference

All diagnostics include an actionable `hint` field. For programmatic handling, pattern-match on these key substrings:

### Errors (`level: "ERROR"`)

| Substring in `msg` | Meaning | Hint Contains |
|---|---|---|
| `"Undefined label"` | Symbol not found in symbol table | Hex prefix fix, fuzzy match to closest symbol, or register/local-label guidance |
| `"Size mismatch"` | Operand widths don't match | Both operand names and their bit widths |
| `"Conditional jump out of range"` | Jcc displacement exceeds +/-127 | Inverted condition + JMP restructure pattern |
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
