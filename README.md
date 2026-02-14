# agent86

**A complete 8086 assembler, disassembler, and emulator in a single C++ file — built for AI agents.**

```
$ ./agent86 --run-source life.asm --viewport 20,5,40,40 --breakpoints 0x78 --max-cycles 5000000
```
```json
{
  "assembly": { "success": true, "size": 532 },
  "emulation": {
    "halted": true,
    "output": "Life complete\r\n",
    "screen": ["                    ██  ██                      ██  ██          ",
               "                    ██  ██                      ██  ██          ",
               "                      ██ █                        ██ █          ", "..."],
    "cursor": { "row": 45, "col": 14 },
    "breakpointSnapshots": [ { "address": "0x0078", "hitCount": 10, "screen": ["..."] } ],
    "fidelity": 1
  }
}
```

No linker. No DOS environment. No dependencies. Write assembly, get structured JSON — including VRAM screen captures, breakpoint animation frames, and full register snapshots.

---

## What is this?

agent86 is a self-contained x86 real-mode toolchain that produces flat `.COM` binaries. Designed from the ground up for AI agents — every output is machine-readable JSON with actionable diagnostics, and the built-in emulator means agents can write, test, and debug code without ever leaving the tool.

The agent workflow:

1. **Write** `.asm` source
2. **Run** `agent86 --run-source program.asm --viewport 0,0,80,50 --breakpoints 0x105`
3. **Parse** the JSON — check `assembly.success`, read `emulation.output`, inspect `screen[]` frames
4. **Debug** — breakpoint snapshots capture registers, stack, VRAM screen state, and memory dumps at any address
5. **Fix and retry** — diagnostics include fix hints, not just error messages

An agent can go from a blank file to a verified, working binary with zero human intervention.

---

## Quick start

**Build** (no dependencies beyond a C++17 compiler):

```bash
g++ -std=c++17 -O2 -o agent86 agent86.cpp
```

**Assemble:**

```bash
./agent86 program.asm                    # produces program.com
./agent86 --agent program.asm            # JSON output with diagnostics + listing
```

**Disassemble:**

```bash
./agent86 --disassemble program.com      # structured JSON disassembly
```

**Emulate:**

```bash
./agent86 --run program.com              # run a .COM binary, JSON output
./agent86 --run-source program.asm       # assemble + run in one step
```

---

## Features

### Structured JSON everywhere

Every mode produces JSON that agents (or scripts) can parse directly. Assembly output includes a full listing with addresses, decoded instructions, and a symbol table. Emulation output includes final register state, captured stdout, VRAM screen content, cursor position, and halt reason.

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

The assembler provides ISA-driven auto-hints that list all valid forms of an instruction when you use it incorrectly.

### 8KB VRAM with screen capture

The emulator includes an 8KB video RAM buffer at segment `B800h` — a faithful model of CGA text-mode memory. Programs can write directly to VRAM using segment register `ES` or use INT 10h BIOS video services. The agent captures the screen as JSON:

```bash
# Capture the full 80×50 display
./agent86 --run-source game.asm --screen

# Capture just a 40×40 region (row 5, col 20)
./agent86 --run-source life.asm --viewport 20,5,40,40

# Include CGA colour attributes
./agent86 --run-source colors.asm --viewport 0,0,40,25 --attrs
```

| Flag | Effect |
|---|---|
| `--screen` | Full 80×50 VRAM dump as `screen[]` string array |
| `--viewport col,row,w,h` | Rectangular window into VRAM — compact, focused output |
| `--attrs` | Adds `screenAttrs[]` with hex CGA attribute bytes per cell |
| `--output-file path` | Write JSON to file (bypasses PowerShell encoding issues) |

### Breakpoint animation

Place a breakpoint at the top of a loop and each hit captures an independent `screen[]` snapshot. The first 10 hits produce full frames — perfect for watching Conway's Life evolve, debugging TUI rendering, or tracing a text adventure's display state turn by turn.

```bash
./agent86 --run-source life.asm --viewport 20,5,40,40 --breakpoints 0x78
```

Each snapshot in the `breakpointSnapshots[]` array includes registers, flags, stack, cursor position, and the VRAM screen at that moment.

### Segment registers

Full segment register support — `CS`, `DS`, `ES`, `SS` — with segment override prefixes (`ES:`, `CS:`, `SS:`, `DS:`). Programs can set `ES` to `B800h` to write directly to video memory while keeping `DS` pointed at the data segment.

```asm
MOV AX, 0B800h
MOV ES, AX
MOV WORD ES:[0], 0A41h    ; Write 'A' in green at top-left of screen
```

### Debugging toolkit

```bash
./agent86 --run-source program.asm \
    --breakpoints 0x105,0x10A \
    --watch-regs AX,CX \
    --max-cycles 100000 \
    --mem-dump 0x200,16
```

Breakpoint snapshots capture registers, flags, stack contents, cursor position, screen state, and optional memory dumps — everything an agent needs to find the first point of divergence from expected behaviour.

### Execution trace

```bash
./agent86 --run program.com --trace
```

For small programs, a full execution trace is more useful than manually placing breakpoints. Every instruction is logged with the register state after execution.

### Keyboard input simulation

```bash
./agent86 --run-source adventure.asm --input "GET LAMP\rN\rOPEN DOOR\r"
```

Programs that read from stdin (INT 21h AH=01h, AH=06h) consume characters from the `--input` string, making interactive programs fully testable in automation.

### Shared decoder

The assembler, disassembler, and emulator all share a single instruction decoder. The disassembly in breakpoint snapshots is always consistent with how the emulator interprets the code, and round-trip verification (assemble → disassemble → compare) is built in.

---

## Demos

Four programs ship with the repo, each showcasing different capabilities — from cryptography to interactive fiction to cellular automata.

### 🔐 cipher.asm — Substitution Cipher with Self-Test

A complete encrypt–decrypt pipeline with built-in verification. Builds a 256-byte substitution table using shift-and-add multiplication (`i×7 + 13 XOR 0xAA`), encrypts a message via `XLAT` table lookups, constructs the inverse table, decrypts, and uses `REPE CMPSB` to verify the round-trip matches byte-for-byte.

```bash
./agent86 --run-source cipher.asm
```
```
Original:  Hello, World! This is a cipher test.
Encrypted: E5 AB A9 A9 A8 CF C7 ... (hex pairs)
Decrypted: Hello, World! This is a cipher test.
PASS: Decrypt matches original
```

Exercises: `XLAT`, `XCHG`, `LODSB`/`STOSB`, `REPE CMPSB`, `PUSHF`/`POPF`, `SAR`, `PUSHA`/`POPA`, procedures with `PROC`/`ENDP`.


### 👻 creep.asm — Haunted House Text Adventure

A 10-room haunted house with a command parser, inventory system, puzzle chains, timed events, multiple endings, and a scoring system — 1,875 lines of 8086 assembly. Explore the house, light a candle before it burns out, defeat a ghost with a silver mirror, and escape through the locked front door.

```bash
./agent86 --run-source creep.asm --input "GET MATCHES\rN\rN\rGET CANDLE\rUSE MATCHES\rS\rS\rS\r"
```

Features: table-driven room navigation, bitfield game state (`TEST`/`OR`/`AND` on flag bytes), object location tracking, candle timer with death mechanic, ghost aggression timer, command disambiguation (`D` → `DOWN` vs `DROP`, `S` → `SOUTH` vs `SCORE`), atmospheric messages on a 7-turn cycle, three distinct endings.


### 🧬 life.asm — Conway's Game of Life *(new — VRAM showcase)*

A double-buffered Game of Life on a 40×40 grid with direct VRAM output. Cells render as square pixels using an 8×8 font (80×50 character resolution). The simulation writes character+attribute pairs to VRAM segment `B800h` via `STOSW`, swaps buffer pointers each generation instead of copying, and runs up to 450 generations from an R-pentomino seed.

This demo exists to showcase the VRAM viewport and breakpoint system. Place a breakpoint at the generation loop entry and each hit captures an independent screen frame:

```bash
# Get the gen_loop address from the symbol table
./agent86 --agent life.asm
# Run with frame-by-frame capture
./agent86 --run-source life.asm --viewport 20,5,40,40 --breakpoints 0x78 --max-cycles 5000000
```

The first 10 breakpoint snapshots each contain a `screen[]` array — 10 frames of the R-pentomino's chaotic expansion, readable directly from JSON. Ships with five seed patterns: blinker, glider, R-pentomino, lightweight spaceship, and acorn.


### 🖥️ vm.asm — Virtual Machine with Self-Test Suite

A 28-instruction virtual machine written in 8086 assembly, running inside the agent86 emulator — a VM within a VM. It has 4 general registers, a 32-word call stack, 256 bytes of addressable memory, flags (Z/C/S), and a dispatch table with computed jumps.

Five bytecode programs are loaded and executed, each validating its result against a known-good value: arithmetic chains (100−50+10−5 = 55), multiplication (25×40 = 1000), bitwise operations (0xAA00 OR 0x0055 = 0xAA55), subroutine call/return, and memory store/fetch round-trips.

```bash
./agent86 --run-source vm.asm
```
```
=== VM Self-Test Suite v1.0 ===

Test 1: Arithmetic    [PASS]
Test 2: Mul/Div       [PASS]
Test 3: Bitwise/Rotate[PASS]
Test 4: Subroutine    [PASS]
Test 5: Memory R/W    [PASS]

5/5 tests passed.
```

Exercises: `SHL`-based dispatch tables, `JCXZ`, `DIV`/`MUL`, `PUSHF`/`POPF` for host flag preservation, `REP MOVSB`/`REP STOSB`, `[BX+label]` addressing throughout.

---

## Instruction set

The assembler supports the full practical 8086 instruction set plus useful 80186 additions:

| Category | Instructions |
|---|---|
| **Data movement** | MOV, XCHG, LEA, PUSH, POP, PUSHA, POPA, PUSHF, POPF, XLAT, IN, OUT |
| **Arithmetic** | ADD, ADC, SUB, SBB, CMP, INC, DEC, NEG, MUL, IMUL, DIV, IDIV, CBW, CWD |
| **Logic** | AND, OR, XOR, NOT, TEST |
| **Shifts & rotates** | SHL, SHR, SAL, SAR, ROL, ROR, RCL, RCR |
| **String operations** | MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W + REP/REPE/REPNE |
| **Control flow** | JMP, CALL, RET, INT, HLT, 16 conditional jumps, LOOP/LOOPE/LOOPNE, JCXZ |
| **Segment** | MOV to/from CS/DS/ES/SS, segment override prefixes (ES:, CS:, SS:, DS:) |
| **Flags & misc** | CLC, STC, CMC, CLD, STD, CLI, STI, LAHF, SAHF, NOP |

Full documentation of every instruction, directive, addressing mode, and JSON schema is in **[AGENT_MANUAL.md](AGENT_MANUAL.md)**.

---

## Emulated interrupts

### INT 10h — BIOS Video Services

| AH | Function |
|---|---|
| `00h` | Set video mode (clears VRAM) |
| `02h` | Set cursor position (DH=row, DL=col) |
| `03h` | Get cursor position |
| `06h` | Scroll window up |
| `07h` | Scroll window down |
| `08h` | Read char + attribute at cursor |
| `09h` | Write char + attribute at cursor (CX=repeat, no advance) |
| `0Ah` | Write char at cursor (keeps existing attribute, no advance) |
| `0Eh` | Teletype output (advances cursor, handles CR/LF/BS, scrolls) |
| `0Fh` | Get current video mode |

### INT 21h — DOS Services

| AH | Function |
|---|---|
| `01h` | Read character with echo |
| `02h` | Write character (DL) |
| `06h` | Direct console I/O |
| `09h` | Write $-terminated string (DS:DX) |
| `2Ah` | Get date (stub) |
| `2Ch` | Get time (stub) |
| `30h` | Get DOS version (returns 5.0) |
| `4Ch` | Exit with return code (AL) |

### INT 20h — Program Terminate

Halts emulation with exit code 0.

DOS text output (INT 21h) is **dual-routed** — characters appear in both the JSON `output` field and the VRAM buffer at the cursor position. So `--screen` captures everything a program prints, whether it uses DOS calls or direct VRAM writes.

Unsupported interrupts are logged in the JSON `skipped[]` array with a count and reason. The `fidelity` field tells you whether skipped operations might have affected the result.

---

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

JSON comes back confirming assembly succeeded, the emulator ran 4 instructions, and the output was `Hello, World!`.

---

## For AI agents

If you're integrating agent86 into an agent workflow:

1. **Read [AGENT_MANUAL.md](AGENT_MANUAL.md) first** — it's written specifically for agents and covers the full JSON schema, every instruction, and the recommended debug loop.

2. **Use `--run-source`** for the common case — assembles and runs in one step, returning both assembly diagnostics and emulation results.

3. **Use `--agent`** when you only need to assemble — gives you the listing, symbol table, and diagnostics without running the code.

4. **Use `--viewport`** for programs that write to VRAM — capture just the region you care about instead of the full 80×50 screen.

5. **Use `--output-file`** on Windows to avoid PowerShell 5.x encoding mangling.

6. **Set `--max-cycles`** to avoid hangs on infinite loops (default: 1,000,000).

7. **Run the executable directly** — do not wrap in `cmd.exe /c` on Windows. The cmd startup banner will contaminate the JSON output.

```
PowerShell:  .\agent86.exe --run-source program.asm
cmd:         agent86.exe --run-source program.asm
Linux/Mac:   ./agent86 --run-source program.asm
```

---

## Building

agent86 is a single C++ file with no external dependencies. Any C++17 compiler will build it:

```bash
# Linux / macOS
g++ -std=c++17 -O2 -o agent86 agent86.cpp

# Windows (MSVC)
cl /std:c++17 /O2 /Fe:agent86.exe agent86.cpp

# Windows (MinGW)
g++ -std=c++17 -O2 -o agent86.exe agent86.cpp
```

Pre-built binaries for Linux, macOS, and Windows are available in [Releases](../../releases).

---

## What it's not

This is not a replacement for NASM, MASM, or TASM. It doesn't support macros, segments as directives, relocatable object files, or linking. It produces flat `.COM` binaries only. The emulator covers real-mode 8086 with DOS console I/O and BIOS video services — not hardware ports, disk I/O, or protected mode.

The deliberate constraints are the point. A single file, a single command, a single JSON response. No configuration, no toolchain, no environment setup. That's what makes it usable by an agent without human help.

---

## Contributing

Bug reports, test cases, and improvements are welcome. The codebase is intentionally a single file — please keep it that way. The architecture is:

1. **Tokenizer + parser** — front end
2. **Two-pass assembler** — resolves labels, emits machine code
3. **Shared decoder** — used by disassembler and emulator
4. **Emulator** — executes decoded instructions, captures I/O and VRAM state
5. **JSON emitters** — structured output for every mode

If you're adding an instruction, it needs to be added in four places: the ISA database, the encoder, the decoder, and the emulator. The [implementation plans](docs/) document the exact patterns.

## License

MIT
