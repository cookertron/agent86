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

### Keyboard input simulation

```bash
./agent86 --run-source program.asm --input "Hello"
```

Programs that read from stdin (INT 21h AH=01h, AH=06h) consume characters from the `--input` string, making interactive programs fully testable in automation.

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

This is not a replacement for NASM, MASM, or TASM. It doesn't support macros, segments, relocatable object files, or linking. It produces flat `.COM` binaries only. The emulator covers real-mode 8086 with DOS console I/O — not BIOS, hardware ports, or protected mode.

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

1. **Tokenizer + parser** — front end
2. **Two-pass assembler** — resolves labels, emits machine code
3. **Shared decoder** — used by disassembler and emulator
4. **Emulator** — executes decoded instructions, captures I/O
5. **JSON emitters** — structured output for every mode

If you're adding an instruction, it needs to be added in four places: the ISA database, the encoder, the decoder, and the emulator. The [implementation plans](docs/) document the exact patterns.

## License

MIT
