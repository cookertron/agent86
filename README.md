# agent86

An 8086 assembler, disassembler, and emulator in a single C++ file. Built for AI agents.

```
$ ./agent86 --run-source hello.asm
```
```json
{
  "assembly": { "success": true, "size": 22 },
  "emulation": {
    "halted": true,
    "haltReason": "INT 21h/4Ch exit (code=0)",
    "output": "Hello, World!",
    "fidelity": 1
  }
}
```

No linker. No DOS environment. No dependencies. Write assembly, get structured JSON back.

---

## What is this?

agent86 is a self-contained x86 real-mode toolchain that produces flat `.COM` binaries. It was designed from the ground up for AI agents to use autonomously — every output is machine-readable JSON with actionable diagnostics, and the built-in emulator means agents can test their code without ever leaving the tool.

The complete agent workflow:

1. **Write** `.asm` source
2. **Run** `agent86 --run-source program.asm --breakpoints 0x105`
3. **Parse** the JSON — check `assembly.success`, read `emulation.output`
4. **Debug** — if something's wrong, breakpoint snapshots show register state, stack contents, and memory dumps at any address
5. **Fix and retry** — diagnostics include fix hints, not just error messages

An agent can go from a blank file to a verified, working binary with zero human intervention.

## Quick start

**Build** (no dependencies beyond a C++17 compiler):

```bash
g++ -std=c++17 -O2 -o agent86 main.cpp
```

**Assemble:**

```bash
./agent86 program.asm                    # produces program.com
./agent86 --agent program.asm            # JSON output with diagnostics + listing
```

**Disassemble:**

```bash
./agent86 --disassemble program.com      # human-readable disassembly
```

**Emulate:**

```bash
./agent86 --run program.com              # run a .COM binary, JSON output
./agent86 --run-source program.asm       # assemble + run in one step
```

## Features

### Structured JSON output

Every mode produces JSON that agents (or scripts) can parse directly. Assembly output includes a full listing with addresses, decoded instructions, and a symbol table. Emulation output includes final register state, captured stdout, and halt reason.

### Diagnostics with fix hints

Errors don't just say what's wrong — they say how to fix it:

```json
{
  "level": "ERROR",
  "line": 7,
  "msg": "Operand size mismatch",
  "hint": "Op1 is 8-bit (AL), Op2 is 16-bit (BX). Use same-size registers: e.g., AL,BL or AX,BX"
}
```

The assembler also provides ISA-driven auto-hints that list all valid forms of an instruction when you use it incorrectly.

### Built-in emulator

The emulator executes `.COM` binaries in a 64KB flat memory space with emulated DOS interrupts (INT 20h, INT 21h for console I/O). No external tools needed.

```bash
# Run with breakpoints and register watches
./agent86 --run-source program.asm \
    --breakpoints 0x105,0x10A \
    --watch-regs AX,CX \
    --max-cycles 100000 \
    --mem-dump 0x200,16
```

Breakpoint snapshots capture registers, flags, stack contents, the next instruction to execute, and optional memory dumps — everything an agent needs to find the first point of divergence from expected behaviour.

### Execution trace

```bash
./agent86 --run program.com --trace
```

For small programs, a full execution trace is more useful than manually placing breakpoints. Every instruction is logged with the register state after execution.

### Screenshot rendering

Export the emulated VRAM as a 24-bit BMP image using CP437 fonts and the CGA 16-color palette:

```bash
./agent86 --run-source life.asm --screenshot screen.bmp
./agent86 --run-source life.asm --screenshot screen.bmp --font 8x8   # 640x400 instead of 640x800
```

The `--font` flag selects between `8x16` (default VGA, 640x800) and `8x8` (640x400). When the BMP is written successfully, the JSON output includes a `"screenshot"` field with the file path.

### Keyboard input simulation

```bash
./agent86 --run-source program.asm --input "Hello"
```

Programs that read from stdin (INT 21h AH=01h, AH=06h) consume characters from the `--input` string, making interactive programs fully testable in automation.

### Multi-file includes

Split your source across multiple files with the `INCLUDE` directive:

```asm
INCLUDE 'lib/strings.asm'
INCLUDE "constants.inc"
```

Relative paths resolve from the including file's directory. Nesting up to 16 levels deep, with circular-include detection. All three quoting styles work (`'single'`, `"double"`, or bare filename). Diagnostics trace back to the original file and line.

### Macro preprocessor

A full MASM-style macro system eliminates repetitive code:

```asm
; Define a parameterized macro
PrintChar MACRO ch
    MOV DL, ch
    MOV AH, 02h
    INT 21h
ENDM

PrintChar 'A'               ; expands to 3 instructions
PrintChar 'B'
```

The macro system supports:

- **`MACRO`/`ENDM`** — parameterized code templates with comma-separated arguments
- **`LOCAL`** — unique labels per expansion (no collisions when a macro is invoked twice)
- **`REPT`** — repeat a block N times
- **`IRP`** — iterate over an angle-bracket list (`IRP reg, <AX,BX,CX>`)
- **`&` concatenation** — join parameter text with surrounding tokens to synthesize labels
- **Nesting** — macros can invoke other macros, REPT/IRP can appear inside macro bodies and vice versa

Macros expand as a text-level preprocessing step before the two-pass assembler, so they work with any instruction or directive.

### Shared decoder

The assembler, disassembler, and emulator all share a single instruction decoder. This means the disassembly you see in breakpoint snapshots is always consistent with how the emulator interprets the code, and round-trip verification (`assemble → disassemble → compare`) is built in.

## Instruction set

The assembler supports the practical 8086 instruction set plus useful 80186 additions:

| Category | Instructions |
|---|---|
| **Data movement** | MOV, XCHG, LEA, PUSH, POP, PUSHA, POPA, PUSHF, POPF, XLAT, IN, OUT |
| **Arithmetic** | ADD, ADC, SUB, SBB, CMP, INC, DEC, NEG, MUL, IMUL, DIV, IDIV |
| **Logic** | AND, OR, XOR, NOT, TEST |
| **Shifts & rotates** | SHL, SHR, SAL, SAR, ROL, ROR, RCL, RCR |
| **String operations** | MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W + REP/REPE/REPNE |
| **Control flow** | JMP, CALL, RET, INT, HLT, 16 conditional jumps, LOOP/LOOPE/LOOPNE, JCXZ |
| **Flags & misc** | CLC, STC, CMC, CLD, STD, CLI, STI, CBW, CWD, LAHF, SAHF, NOP |

Full documentation of every instruction, directive, addressing mode, and JSON schema is in [AGENT_MANUAL.md](AGENT_MANUAL.md).

## Emulated DOS interrupts

| Interrupt | Function | Description |
|---|---|---|
| INT 20h | — | Program terminate |
| INT 21h | AH=01h | Read character with echo |
| INT 21h | AH=02h | Write character (DL) |
| INT 21h | AH=06h | Direct console I/O |
| INT 21h | AH=09h | Write $-terminated string (DS:DX) |
| INT 21h | AH=4Ch | Exit with return code (AL) |
| INT 21h | AH=2Ah | Get date (stub) |
| INT 21h | AH=2Ch | Get time (stub) |
| INT 21h | AH=30h | Get DOS version (stub) |

Unsupported interrupts are logged in the JSON `skipped[]` array with a count and reason. The `fidelity` field tells you whether skipped operations might have affected the result.

## Example: the 30-second test

Create `hello.asm`:

```asm
ORG 100h
    MOV AH, 09h
    MOV DX, msg
    INT 21h
    MOV AX, 4C00h
    INT 21h
msg: DB 'Hello, World!$'
```

```bash
./agent86 --run-source hello.asm
```

You'll get JSON confirming assembly succeeded, the emulator ran 4 instructions, and the output was `Hello, World!`.

## Demo: macros in action

[`macros.asm`](macros.asm) builds a colorful 80x25 text-mode display using nothing but macros. It showcases every macro feature in one program:

```asm
; Macro library turns VRAM programming into high-level calls
ClearScreen 17h
Box 8, 1, 64, 23, 1Bh
Print 26, 2, 1Eh, str_title
Gradient 12, 11, 48, 1Bh
```

The file defines 11 macros (`VramInit`, `Print`, `HFill`, `FillRect`, `ClearScreen`, `Box`, `HSep`, `Gradient`, `DefStr`, and more) that demonstrate:

- **`MACRO`/`ENDM`** with parameters — `Print col, row, attr, msg` expands to a positioned VRAM string write
- **`LOCAL`** labels — `FillRect` and `Box` use loop labels that stay unique across multiple invocations
- **`REPT`** — generates a 46-character decorative bar as raw data bytes
- **`IRP`** — builds two 8-entry CGA color palette tables by iterating over attribute values
- **`&` concatenation** — `DefStr title, 'text'` synthesizes the label `str_title` at definition time
- **Nested macros** — `ClearScreen` calls `FillRect`, `Gradient` calls `HFill` four times

Run it:

```bash
./agent86 --run-source macros.asm --viewport 0,0,80,25 --attrs
./agent86 --run-source macros.asm --screenshot macros.bmp    # render as BMP image
```

## For AI agents

If you're integrating agent86 into an agent workflow:

1. **Read [AGENT_MANUAL.md](AGENT_MANUAL.md) first** — it's written specifically for agents and covers the full JSON schema, every instruction, and the recommended debug workflow.

2. **Use `--run-source`** for the common case — it assembles and runs in one step, returning both assembly diagnostics and emulation results.

3. **Use `--agent`** when you only need to assemble — it gives you the listing, symbol table, and diagnostics without running the code.

4. **Set `--max-cycles`** to avoid hangs on infinite loops.

5. **Run the executable directly** — do not wrap in `cmd.exe /c` on Windows. The cmd startup banner will contaminate the JSON output.

```
PowerShell:  .\agent86.exe --run-source program.asm
cmd:         agent86.exe --run-source program.asm
Linux/Mac:   ./agent86 --run-source program.asm
```

## What it's not

This is not a replacement for NASM, MASM, or TASM. It doesn't support segments, relocatable object files, or linking. It produces flat `.COM` binaries only. The emulator covers real-mode 8086 with DOS console I/O — not BIOS, hardware ports, or protected mode.

That said, it does include multi-file `INCLUDE` support and a full MASM-style macro preprocessor (`MACRO`, `LOCAL`, `REPT`, `IRP`, `&` concatenation), so non-trivial programs are practical to write.

The deliberate constraints are the point. A single file, a single command, a single JSON response. No configuration, no toolchain, no environment setup. That's what makes it usable by an agent without human help.

## Building

agent86 is a single C++ file with no external dependencies. Any C++17 compiler will build it:

```bash
# Linux / macOS
g++ -std=c++17 -O2 -o agent86 main.cpp

# Windows (MSVC)
cl /std:c++17 /O2 /Fe:agent86.exe main.cpp

# Windows (MinGW)
g++ -std=c++17 -O2 -o agent86.exe main.cpp
```

Pre-built binaries for Linux, macOS, and Windows are available in [Releases](../../releases).

## Contributing

Bug reports, test cases, and improvements are welcome. The codebase is intentionally a single file — please keep it that way. The architecture is:

1. **Preprocessor** — `INCLUDE` expansion, then macro expansion (`MACRO`/`REPT`/`IRP`)
2. **Tokenizer + parser** — front end
3. **Two-pass assembler** — resolves labels, emits machine code
4. **Shared decoder** — used by disassembler and emulator
5. **Emulator** — executes decoded instructions, captures I/O
6. **JSON emitters** — structured output for every mode

If you're adding an instruction, it needs to be added in four places: the ISA database, the encoder, the decoder, and the emulator. The [implementation plans](docs/) document the exact patterns.

## License

MIT
