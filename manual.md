# agent86 Manual

Two-pass 8086 assembler and per-instruction JIT emulator targeting .COM binaries.
Version 0.19.3.

For CLI usage, run `agent86 --help` or `agent86 --help <flag>`.

---

## Table of Contents

- [CLI Reference](#cli-reference)
  - [Modes](#modes)
  - [Flags](#flags)
  - [Help Topics](#help-topics)
  - [Examples](#cli-examples)
- [Assembly Language](#assembly-language)
  - [Source Format](#source-format)
  - [Labels](#labels)
  - [Number Formats](#number-formats)
  - [Expressions](#expressions)
  - [Directives](#directives)
  - [INCLUDE](#include)
  - [Macros](#macros)
- [Debug Directives](#debug-directives)
  - [Compile-Time](#compile-time-directives)
  - [Runtime](#runtime-directives)
  - [VRAMOUT](#vramout)
  - [REGS](#regs)
  - [LOG / LOG_ONCE](#log--log_once)
  - [Modifier Chaining](#modifier-chaining)
- [Instruction Reference](#instruction-reference)
  - [Data Movement](#data-movement)
  - [Arithmetic](#arithmetic)
  - [Logic](#logic)
  - [Shifts and Rotates](#shifts-and-rotates)
  - [Control Flow](#control-flow)
  - [Conditional Jumps](#conditional-jumps)
  - [Loops](#loops)
  - [String Operations](#string-operations)
  - [Flag Control](#flag-control)
  - [Miscellaneous](#miscellaneous)
- [Addressing Modes](#addressing-modes)
- [DOS Emulation](#dos-emulation)
  - [INT 20h — Terminate](#int-20h--terminate)
  - [INT 21h — DOS Services](#int-21h--dos-services)
  - [INT 10h — Video BIOS](#int-10h--video-bios)
  - [INT 16h — Keyboard BIOS](#int-16h--keyboard-bios)
  - [INT 33h — Mouse Driver](#int-33h--mouse-driver)
- [Video Framebuffer](#video-framebuffer)
- [Keyboard and Mouse Input](#keyboard-and-mouse-input)
- [JSON Output Reference](#json-output-reference)
- [Limitations](#limitations)

---

## CLI Reference

All output goes to **stdout as JSON**. DOS text output (INT 21h print, INT 10h teletype) goes to **stderr**. This separation enables machine parsing of results while preserving human-readable program output.

### Modes

**Assemble only** — produce a `.com` binary and `.dbg` debug file:
```
agent86 <file.asm>
agent86 <file.asm> -o <output.com>
```

**Assemble + execute** (recommended workflow) — assemble, then immediately run or trace. Emits compile JSON on the first line and execution JSON on the second:
```
agent86 <file.asm> --build_run [N]
agent86 <file.asm> --build_trace [N]
```

**Execute pre-compiled** — run or trace an already-assembled `.com` file:
```
agent86 <file.com> --run [N]
agent86 <file.com> --trace [N]
```

**Help**:
```
agent86 --help
agent86 --help <topic>
```

The optional `[N]` on execution modes sets the instruction cycle limit (default: 100,000,000). Programs terminate with an error if they exceed this limit.

The difference between `--run` and `--trace`: `--run` executes silently (ignores `.dbg`). `--trace` loads the `.dbg` file and honors runtime debug directives (TRACE_START/TRACE_STOP, BREAKPOINT, ASSERT_EQ, VRAMOUT, REGS, LOG). If no directives are present, `--trace` behaves identically to `--run`.

### Flags

| Flag | Description |
|------|-------------|
| `--build_run [N]` | Assemble `.asm` then execute (recommended) |
| `--build_trace [N]` | Assemble `.asm` then trace with debug directives |
| `--run [N]` | Execute a pre-compiled `.com` binary |
| `--trace [N]` | Trace a pre-compiled `.com` binary with debug directives |
| `-o <path>` | Override output `.com` path (assemble modes only) |
| `--args <string>` | Set PSP command tail (program arguments at 0x80) |
| `--events <json\|file>` | Inject keyboard/mouse input (inline JSON or file path) |
| `--screen <mode>` | Enable video framebuffer (MDA, CGA40, CGA80, VGA50) |
| `--help [topic]` | Show help (overview or per-flag detail) |

### Help Topics

`agent86 --help <topic>` for detailed usage:

| Topic | Aliases | Description |
|-------|---------|-------------|
| `asm` | `assembly`, `syntax` | Assembly language syntax |
| `run` | | `--run` flag details |
| `trace` | | `--trace` flag details |
| `build_run` | `build_trace`, `build` | Combined assemble+execute |
| `directives` | `directive`, `dir` | Debug directive reference |
| `events` | `keyboard`, `keys`, `input` | Event injection format |
| `screen` | `video`, `vram`, `framebuffer` | Video framebuffer modes |
| `args` | `arguments`, `psp` | PSP command tail |
| `o` | | Output path override |

### CLI Examples

Assemble a file:
```bash
agent86 hello.asm
```

Assemble and run in one step (recommended for iterative development):
```bash
agent86 hello.asm --build_run
```

Assemble and trace with debug directives:
```bash
agent86 prog.asm --build_trace
```

Assemble and run with a 500-cycle limit:
```bash
agent86 prog.asm --build_run 500
```

Run a pre-compiled binary:
```bash
agent86 hello.com --run
```

Run with keyboard input:
```bash
agent86 prog.com --run --events '[{"keys":"Hello\r"}]'
```

Run with video output:
```bash
agent86 prog.com --run --screen CGA80
```

Run with program arguments:
```bash
agent86 prog.com --run --args "arg1 arg2"
```

Full TUI application test (assemble + run + video + keyboard + mouse):
```bash
agent86 app.asm --build_run --screen CGA80 --events '[{"keys":"text"},{"mouse":{"buttons":1,"x":80,"y":40}},{"keys":"\r"}]'
```

Assemble + trace with debug directives, video, and events:
```bash
agent86 app.asm --build_trace --screen CGA80 --events '[{"keys":"A"}]'
```

Run with events from a file:
```bash
agent86 prog.com --run --events events.json
```

---

## Assembly Language

### Source Format

- One instruction or directive per line
- Comments start with `;` and extend to end of line
- Case-insensitive mnemonics and directives (`MOV` = `mov` = `Mov`)
- Labels are case-sensitive (`MyLabel` != `mylabel`)
- Whitespace is ignored except inside strings

```asm
; This is a comment
    ORG 100h              ; directive
start:                    ; global label
    MOV AX, 42            ; instruction
    .loop:                ; local label (scoped to "start")
        DEC AX
        JNZ .loop
    INT 20h
msg: DB 'Hello$'          ; label + data on same line
```

### Labels

**Global labels** — define with trailing colon. Become scope for local labels.

```asm
main:
print_char:
data_section:
```

**Local labels** — prefixed with `.`, scoped to the nearest preceding global label. Internally qualified as `global.local`.

```asm
search PROC
    .loop:              ; search.loop
        CMP BYTE [SI], 0
        JZ .done        ; search.done
        INC SI
        JMP .loop
    .done:              ; search.done
        RET
search ENDP

fill PROC
    .loop:              ; fill.loop — no collision with search.loop
        MOV [DI], AL
        INC DI
        LOOP .loop
        RET
fill ENDP
```

**Forward references** — labels can be referenced before they're defined. Resolved in pass 2.

```asm
    JMP skip_data       ; forward reference — OK
msg: DB 'Hello$'
skip_data:
    MOV DX, msg
```

### Number Formats

| Format | Syntax | Examples |
|--------|--------|---------|
| Decimal | digits | `42`, `255`, `65535` |
| Hexadecimal | `0x` prefix or `h` suffix | `0xFF`, `0FFh`, `1234h`, `0ABCDh` |
| Binary | `b` suffix | `1010b`, `11111111b` |
| Character | single quotes | `'A'` (= 65), `'0'` (= 48), `'$'` (= 36) |

Hex with trailing `h`: the first character must be a digit `0-9`. Use a leading `0` for hex values starting with A-F: write `0FFh`, not `FFh`.

### Expressions

Full 8-level precedence with parentheses:

| Precedence | Operators | Description |
|-----------|-----------|-------------|
| 1 (lowest) | `\|` | Bitwise OR |
| 2 | `^` | Bitwise XOR |
| 3 | `&` | Bitwise AND |
| 4 | `<<` `>>` | Shift left/right |
| 5 | `+` `-` | Add, subtract |
| 6 | `*` `/` `%` | Multiply, divide, modulo |
| 7 (highest) | `~` `-` `+` | Unary NOT, negate, plus |

| Symbol | Meaning |
|--------|---------|
| `$` | Current address (program counter at this point) |
| `label` | Address of label, or value of EQU constant |

```asm
BUFFER_SZ  EQU 256
TABLE_END  EQU table + BUFFER_SZ * 2
msg_len    EQU $ - msg            ; length of msg data
           MOV AX, -1             ; FFFFh
           DB  (data_end - data) & 0FFh  ; low byte of computed size
```

### Directives

**ORG** — set the origin address. Required for .COM files (loaded at 100h).

```asm
ORG 100h
```

**DB** — define bytes. Strings, integers, expressions, character literals.

```asm
DB 0                             ; single zero byte
DB 'Hello, World!', 0Dh, 0Ah, '$'  ; string + CRLF + terminator
DB buffer_end - buffer           ; computed value
```

**DW** — define 16-bit words (little-endian).

```asm
DW 1234h                ; bytes: 34h, 12h
DW handler_a, handler_b ; jump table
DW label + 4            ; label arithmetic
```

**RESB / RESW** — reserve zero-filled bytes/words.

```asm
buffer: RESB 256        ; 256 zero bytes
table:  RESW 16         ; 32 zero bytes (16 words)
```

**EQU** — named constant. No bytes emitted.

```asm
CR  EQU 0Dh
LF  EQU 0Ah
MAX EQU 100
```

**PROC / ENDP** — procedure boundaries. Affect local label scoping only.

```asm
my_func PROC
    .retry:             ; qualified as my_func.retry
        ...
        JNZ .retry
        RET
my_func ENDP
```

**SCREEN** — declare video mode in assembly source. Equivalent to `--screen` on the CLI.

```asm
SCREEN CGA80            ; enable 80x25 color text mode
```

### INCLUDE

Textual file inclusion, expanded before assembly passes.

```asm
INCLUDE "constants.inc"       ; quoted path, relative to including file
INCLUDE lib\helpers.inc       ; unquoted path also supported
```

- **Recursive**: included files can include other files (max depth: 16)
- **Include guard**: each file included at most once by canonical path
- **Circular detection**: two files including each other produces an error
- **Error reporting**: errors in included files show the original file and line

### Macros

**MACRO/ENDM** — define named text macros with optional parameters. Expanded between INCLUDE processing and assembly passes.

```asm
PushAll MACRO
    PUSH AX
    PUSH BX
    PUSH CX
    PUSH DX
    PUSH SI
    PUSH DI
ENDM

PopAll MACRO
    POP DI
    POP SI
    POP DX
    POP CX
    POP BX
    POP AX
ENDM

my_func:
    PushAll                ; expands to 6 PUSH instructions
    ; ... function body ...
    PopAll                 ; expands to 6 POP instructions
    RET
```

**Parameterized macros** — parameters are substituted in the body at expansion time.

```asm
WriteChar MACRO ch
    MOV DL, ch
    MOV AH, 02h
    INT 21h
ENDM

WriteChar 'A'              ; MOV DL, 'A' / MOV AH, 02h / INT 21h
WriteChar 0Dh              ; MOV DL, 0Dh / ...
```

**IRP (Indefinite Repeat)** — repeat body for each item in a list. Can be standalone or inside macros.

```asm
; Standalone IRP
IRP reg, <AX, BX, CX, DX>
    PUSH reg
ENDM
; Expands to: PUSH AX / PUSH BX / PUSH CX / PUSH DX

; IRP inside a macro
PushAll MACRO
    IRP reg, <AX, BX, CX, DX, SI, DI>
        PUSH reg
    ENDM
ENDM
```

---

## Debug Directives

All debug directives emit zero bytes of machine code. They are written to the `.dbg` file during assembly.

### Compile-Time Directives

Evaluated during assembly (pass 2). Results appear in the compile JSON.

**ASSERT** — fail assembly if expression is zero or values differ.

```asm
ASSERT NUM_ENTRIES              ; fail if NUM_ENTRIES is 0
ASSERT data_end - data, 10     ; fail if data section isn't exactly 10 bytes
```

**PRINT** — emit a message to the compile JSON `"prints"` array.

```asm
PRINT "hello"                   ; {"line":5,"text":"hello"}
PRINT 2 + 3                     ; {"line":6,"text":"5"}
PRINT "size = ", end - start    ; {"line":7,"text":"size = 42"}
```

**HEX_START / HEX_END** — capture emitted bytes in the compile JSON `"hex_dumps"` array.

```asm
HEX_START
    MOV AX, 1
    MOV BX, 2
HEX_END
```

### Runtime Directives

Stored in the `.dbg` file at assembly time. Honored by `--trace` (and `--build_trace`) at runtime. Ignored by `--run` (and `--build_run`).

**TRACE_START / TRACE_STOP** — enable/disable per-instruction trace output (source lines, hex, register dumps) on stderr.

```asm
TRACE_START
    MOV AX, 1       ; this section is traced
    ADD AX, BX
TRACE_STOP
    MOV CX, 3       ; this runs silently
```

**BREAKPOINT** — halt execution and return BREAKPOINT JSON. Inline-only.

```asm
BREAKPOINT                ; halt here on first hit
BREAKPOINT init           ; named "init", halt on first hit
BREAKPOINT loop_top, 5    ; named, halt after 5 passes (6th hit)
```

JSON: `{"executed":"BREAKPOINT","addr":N,"name":"init","instructions":N}`

**ASSERT_EQ** — halt if actual value doesn't match expected. Checked before the instruction at that address, so place after the instruction you want to verify.

```asm
MOV AX, 42
ASSERT_EQ AX, 42              ; passes: AX is 42
ASSERT_EQ BYTE [200h], 41h    ; check memory byte
ASSERT_EQ WORD [300h], 1234h  ; check memory word
```

Registers: AX, BX, CX, DX, SP, BP, SI, DI. Multiple ASSERT_EQ at the same address are all checked.

JSON: `{"executed":"ASSERT_FAILED","addr":N,"assert":"AX == 42","actual":99,"expected":42,"instructions":N}`

### VRAMOUT

On-demand VRAM snapshots during execution. Requires `--screen` (silently no-ops without it).

**Standalone** — non-halting, accumulates in `"vram_dumps"` array (max 32):

```asm
VRAMOUT                         ; full screen, text only
VRAMOUT FULL                    ; explicit full
VRAMOUT FULL, ATTRS             ; full screen with attribute bytes
VRAMOUT PARTIAL 0, 0, 40, 12   ; region (x, y, width, height)
VRAMOUT PARTIAL 0, 0, 40, 12, ATTRS  ; region with attributes
```

**Modifier on BREAKPOINT** — include screen data in BREAKPOINT JSON:

```asm
BREAKPOINT : VRAMOUT
BREAKPOINT init, 5 : VRAMOUT FULL, ATTRS
BREAKPOINT : VRAMOUT PARTIAL 0, 0, 40, 12
```

**Modifier on ASSERT_EQ** — include screen data on assertion failure:

```asm
ASSERT_EQ AX, 5 : VRAMOUT
ASSERT_EQ BYTE [0x200], 0x48 : VRAMOUT FULL, ATTRS
```

Defaults: FULL mode, ATTRS=false.

### REGS

On-demand register snapshots during execution. Only honored by `--trace` / `--build_trace`.

**Standalone** — non-halting, accumulates in `"reg_dumps"` array (max 32):

```asm
MOV AX, 42
MOV BX, 100
REGS                            ; snapshot all registers at this point
```

JSON entry: `{"addr":N,"instr":N,"regs":{"AX":42,"BX":100,"CX":0,...,"IP":N,"FL":N,"flags":"-----.--"}}`

The `regs` object contains: AX, BX, CX, DX, SP, BP, SI, DI (GPRs), DS, ES, SS, CS (segment regs), IP (instruction pointer), FL (raw flags word), and `flags` (human-readable flag string: `O D S Z - A P C` where `-` = clear).

**Modifier on BREAKPOINT** — include register state in BREAKPOINT JSON:

```asm
BREAKPOINT : REGS
BREAKPOINT checkpoint, 3 : REGS
```

**Modifier on ASSERT_EQ** — include register state on assertion failure:

```asm
ASSERT_EQ AX, 5 : REGS
```

### LOG / LOG_ONCE

Runtime debug print that accumulates entries in the `"log"` array (max 256). Only honored by `--trace` / `--build_trace`.

**LOG** — emit a log entry every time execution passes this address:

```asm
LOG "entering loop"                     ; message only
LOG "counter", CX                       ; message + register value
LOG "char", BYTE [SI]                   ; message + memory byte
LOG "value", WORD [0x200]               ; message + memory word
```

JSON entries:
- Message only: `{"addr":N,"instr":N,"message":"entering loop"}`
- With register: `{"addr":N,"instr":N,"message":"counter","reg":"CX","value":5}`
- With memory byte: `{"addr":N,"instr":N,"message":"char","mem_addr":N,"size":"byte","value":65}`
- With memory word: `{"addr":N,"instr":N,"message":"value","mem_addr":512,"size":"word","value":1234}`

**LOG_ONCE** — like LOG, but fires only once per label (deduped across all hits):

```asm
.loop:
    LOG_ONCE loop_entry, "entered loop"
    LOG_ONCE loop_val, "AX", AX
    ; ... loop body ...
    JNZ .loop
; "entered loop" and "AX" appear only once in the log, not per iteration
```

The label (first argument) is used for deduplication and does not need to correspond to any assembly label.

### Modifier Chaining

BREAKPOINT and ASSERT_EQ support multiple modifiers separated by colons, in any order:

```asm
BREAKPOINT : VRAMOUT : REGS               ; screen dump + register snapshot
BREAKPOINT init : REGS : VRAMOUT FULL, ATTRS  ; named, with both modifiers
ASSERT_EQ AX, 5 : VRAMOUT : REGS          ; assert with both on failure
BREAKPOINT : VRAMOUT PARTIAL 0, 0, 40, 12 : REGS  ; partial screen + regs
```

---

## Instruction Reference

All 8086 instructions are supported, plus 186 extensions PUSHA/POPA. Operand forms shown as shorthand: `r8` = 8-bit register, `r16` = 16-bit register, `m` = memory, `imm` = immediate, `sreg` = segment register.

### Data Movement

| Instruction | Description | Operand Forms |
|-------------|-------------|---------------|
| `MOV` | Move | r/r, r/imm, r/m, m/r, m/imm, sreg/r, r/sreg, sreg/m, m/sreg |
| `XCHG` | Exchange | r/r, r/m (AX,r16 has short encoding) |
| `LEA` | Load effective address | r16/m |
| `LDS` | Load pointer + DS | r16/m |
| `LES` | Load pointer + ES | r16/m |
| `PUSH` | Push to stack | r16, sreg, m |
| `POP` | Pop from stack | r16, sreg, m |
| `PUSHA` | Push all GPRs | (186) |
| `POPA` | Pop all GPRs | (186) |
| `PUSHF` | Push FLAGS | — |
| `POPF` | Pop FLAGS | — |
| `XLAT`/`XLATB` | Table lookup AL=[BX+AL] | — |
| `CBW` | Sign-extend AL->AX | — |
| `CWD` | Sign-extend AX->DX:AX | — |
| `LAHF` | Load AH from FLAGS | — |
| `SAHF` | Store AH to FLAGS | — |
| `IN` | Port input | AL/AX, imm8/DX |
| `OUT` | Port output | imm8/DX, AL/AX |

### Arithmetic

| Instruction | Description | Notes |
|-------------|-------------|-------|
| `ADD` | Add | All ALU forms: r/r, r/imm, r/m, m/r, m/imm |
| `ADC` | Add with carry | dst + src + CF |
| `SUB` | Subtract | |
| `SBB` | Subtract with borrow | dst - src - CF |
| `INC` | Increment | Preserves CF |
| `DEC` | Decrement | Preserves CF |
| `NEG` | Negate (two's comp) | 0 - dst |
| `MUL` | Unsigned multiply | AX=AL*r8, DX:AX=AX*r16 |
| `IMUL` | Signed multiply | Same layout |
| `DIV` | Unsigned divide | AL=AX/r8 AH=rem, AX=DX:AX/r16 DX=rem |
| `IDIV` | Signed divide | Same layout |
| `CMP` | Compare | SUB without storing result |
| `DAA` | Decimal adjust add | Operates on AL |
| `DAS` | Decimal adjust sub | |
| `AAA` | ASCII adjust add | |
| `AAS` | ASCII adjust sub | |
| `AAM` | ASCII adjust mul | AH=AL/10, AL=AL%10 |
| `AAD` | ASCII adjust div | AL=AH*10+AL, AH=0 |

### Logic

| Instruction | Description | Notes |
|-------------|-------------|-------|
| `AND` | Bitwise AND | Clears CF, OF |
| `OR` | Bitwise OR | Clears CF, OF |
| `XOR` | Bitwise XOR | Clears CF, OF |
| `NOT` | Bitwise complement | Flags unchanged |
| `TEST` | AND without storing | Like CMP for bit testing |

### Shifts and Rotates

All support: reg/1, reg/CL, mem/1, mem/CL.

| Instruction | Description |
|-------------|-------------|
| `SHL` | Shift left (= SAL) |
| `SHR` | Logical shift right (fill with 0) |
| `SAR` | Arithmetic shift right (preserve sign) |
| `ROL` | Rotate left |
| `ROR` | Rotate right |
| `RCL` | Rotate left through carry |
| `RCR` | Rotate right through carry |

### Control Flow

| Instruction | Description |
|-------------|-------------|
| `JMP target` | Unconditional near jump (auto-sized: short EB or near E9) |
| `JMP r16` | Indirect jump via register |
| `JMP [mem]` | Indirect jump via memory |
| `CALL target` | Near call (push IP, jump) |
| `CALL r16` | Indirect call via register |
| `CALL [mem]` | Indirect call via memory |
| `RET` | Near return |
| `RET imm16` | Near return + pop imm16 bytes |
| `RETF` | Far return |
| `RETF imm16` | Far return + pop imm16 bytes |
| `IRET` | Interrupt return (pop IP, CS, FLAGS) |
| `INT num` | Software interrupt (INT 3 encodes as single-byte CC) |
| `INTO` | Interrupt on overflow (INT 4 if OF=1) |

### Conditional Jumps

Short-range (rel8) by default. For targets beyond +/-127 bytes, the assembler emits a reverse-condition jump over a JMP NEAR (5 bytes total, NOP-padded to match pass 1 estimate).

| Mnemonic | Aliases | Condition |
|----------|---------|-----------|
| `JZ` | `JE` | ZF=1 |
| `JNZ` | `JNE` | ZF=0 |
| `JB` | `JC`, `JNAE` | CF=1 |
| `JAE` | `JNC`, `JNB` | CF=0 |
| `JBE` | `JNA` | CF=1 or ZF=1 |
| `JA` | `JNBE` | CF=0 and ZF=0 |
| `JL` | `JNGE` | SF!=OF |
| `JGE` | `JNL` | SF=OF |
| `JLE` | `JNG` | ZF=1 or SF!=OF |
| `JG` | `JNLE` | ZF=0 and SF=OF |
| `JS` | | SF=1 |
| `JNS` | | SF=0 |
| `JO` | | OF=1 |
| `JNO` | | OF=0 |
| `JP` | `JPE` | PF=1 |
| `JNP` | `JPO` | PF=0 |

### Loops

All are short-range only (rel8). Out-of-range targets produce an error.

| Instruction | Description |
|-------------|-------------|
| `LOOP target` | Dec CX, jump if CX!=0 |
| `LOOPE`/`LOOPZ` | Dec CX, jump if CX!=0 and ZF=1 |
| `LOOPNE`/`LOOPNZ` | Dec CX, jump if CX!=0 and ZF=0 |
| `JCXZ target` | Jump if CX==0 (no decrement) |

### String Operations

Use with REP/REPE/REPNE prefixes. Direction flag controls SI/DI direction: `CLD` = forward (increment), `STD` = backward (decrement).

| Instruction | Description |
|-------------|-------------|
| `MOVSB`/`MOVSW` | Copy [DS:SI] -> [ES:DI], advance both |
| `STOSB`/`STOSW` | Store AL/AX -> [ES:DI], advance DI |
| `LODSB`/`LODSW` | Load [DS:SI] -> AL/AX, advance SI |
| `CMPSB`/`CMPSW` | Compare [DS:SI] vs [ES:DI], set flags |
| `SCASB`/`SCASW` | Compare AL/AX vs [ES:DI], set flags |

Prefixes:
- `REP` — repeat CX times (MOVS, STOS, LODS)
- `REPE`/`REPZ` — repeat while CX!=0 and ZF=1 (CMPS, SCAS)
- `REPNE`/`REPNZ` — repeat while CX!=0 and ZF=0 (CMPS, SCAS)

### Flag Control

| Instruction | Description |
|-------------|-------------|
| `CLC` | Clear carry (CF=0) |
| `STC` | Set carry (CF=1) |
| `CMC` | Complement carry |
| `CLD` | Clear direction (DF=0, forward) |
| `STD` | Set direction (DF=1, backward) |
| `CLI` | Clear interrupt (IF=0) |
| `STI` | Set interrupt (IF=1) |

### Miscellaneous

| Instruction | Description |
|-------------|-------------|
| `NOP` | No operation (opcode 90h) |
| `HLT` | Halt (terminates emulation) |
| `WAIT` | Wait for coprocessor (no-op in emulator) |
| `LOCK` | Bus lock prefix (accepted, no effect in emulator) |

---

## Addressing Modes

The 8086 supports these base/index combinations in memory operands:

| Form | ModR/M | Example |
|------|--------|---------|
| `[BX+SI+disp]` | rm=000 | `MOV AX, [BX+SI]` |
| `[BX+DI+disp]` | rm=001 | `MOV AX, [BX+DI+4]` |
| `[BP+SI+disp]` | rm=010 | `MOV AX, [BP+SI+100h]` |
| `[BP+DI+disp]` | rm=011 | `ADD [BP+DI], CX` |
| `[SI+disp]` | rm=100 | `CMP BYTE [SI], 0` |
| `[DI+disp]` | rm=101 | `MOV [DI+2], AX` |
| `[BP+disp]` | rm=110 | `MOV AX, [BP]` (encoded as [BP+0]) |
| `[BX+disp]` | rm=111 | `MOV AX, [BX+10]` |
| `[disp16]` | direct | `MOV AX, [label]` |

**Displacement encoding**: no displacement = mod 00, 8-bit = mod 01, 16-bit = mod 10. Exception: `[BP]` with no displacement uses mod 01 + disp8=0 (because mod 00 rm 110 = direct addressing).

**Segment overrides**: prefix a memory operand with `ES:`, `CS:`, `SS:`, or `DS:` to override the default segment:

```asm
MOV AX, ES:[DI]             ; read from ES segment instead of DS
MOV BYTE CS:[BX+5], 0       ; write to CS segment
ADD AX, SS:[BP-2]            ; add from SS segment
CMP WORD ES:[DI+4], 1234h   ; compare in ES segment
```

Default segments: `DS` for most memory operands, `SS` for `[BP]`-based addresses. Segment overrides emit a prefix byte (ES=`26h`, CS=`2Eh`, SS=`36h`, DS=`3Eh`) before the instruction opcode.

**Size overrides**: use `BYTE` or `WORD` when size is ambiguous:

```asm
MOV BYTE [BX], 0       ; 8-bit store
MOV WORD [BX], 0       ; 16-bit store
INC BYTE [SI+4]         ; 8-bit increment
```

Size overrides combine with segment overrides: `MOV AL, BYTE ES:[DI+2]`.

Size is inferred when a register operand is present (`MOV [BX], AL` = 8-bit).

**Labels as addresses**:

```asm
MOV AX, [my_table]          ; direct addressing with label
MOV AX, [BX + my_table]     ; base + label offset
MOV AX, [BX + my_table + 4] ; base + label + constant
```

---

## DOS Emulation

The JIT emulator provides DOS, BIOS, and mouse services. All print output goes to **stderr** to keep stdout reserved for JSON.

All INT 21h functions respect segment registers: DS:DX for paths and buffers, ES:DI for rename new path, DS:SI for get CWD buffer. Programs can set DS to an allocated segment and perform file I/O directly into it.

### INT 20h — Terminate

Halts execution, exit code 0.

### INT 21h — DOS Services

33 subfunctions are supported:

**Console I/O**

| AH | Function | Description |
|----|----------|-------------|
| 01h | Read char | Read one character with echo (requires `--events`) |
| 02h | Print char | Print character in DL to stderr |
| 06h | Direct I/O | DL=FFh reads (two-byte protocol for extended keys), else writes DL |
| 09h | Print string | Print '$'-terminated string at DS:DX |

**File System**

| AH | Function | Description |
|----|----------|-------------|
| 3Ch | Create file | DS:DX = ASCIIZ path, CX = attrs -> AX = handle |
| 3Dh | Open file | DS:DX = ASCIIZ path, AL = mode -> AX = handle |
| 3Eh | Close file | BX = handle |
| 3Fh | Read file | BX = handle, CX = count, DS:DX = buffer |
| 40h | Write file | BX = handle, CX = count, DS:DX = buffer |
| 42h | Seek | BX = handle, CX:DX = offset, AL = origin |
| 41h | Delete file | DS:DX = ASCIIZ path |
| 43h | Get/set attrs | DS:DX = path, AL = get(0)/set(1) |
| 56h | Rename | DS:DX = old, ES:DI = new |
| 57h | Get/set date | BX = handle, AL = get(0)/set(1) |

**Directory**

| AH | Function | Description |
|----|----------|-------------|
| 0Eh | Set drive | DL = drive (0=A) |
| 19h | Get drive | Returns AL = current drive |
| 3Bh | CHDIR | DS:DX = ASCIIZ path |
| 47h | Get CWD | DS:SI = buffer, DL = drive |

**FindFirst/FindNext**

| AH | Function | Description |
|----|----------|-------------|
| 4Eh | FindFirst | DS:DX = wildcard, CX = attrs -> DTA filled |
| 4Fh | FindNext | Continue from previous FindFirst |

**Memory Management**

| AH | Function | Description |
|----|----------|-------------|
| 48h | Alloc memory | BX = paragraphs -> AX = segment (always >= 0x1000) |
| 49h | Free memory | ES = segment to free |
| 4Ah | Resize | ES = segment, BX = new paragraphs |

Allocations are above the COM segment: segment 0x1000+ (physical 0x10000+), up to 0x9FFF (below video memory). ~576KB available.

**Other**

| AH | Function | Description |
|----|----------|-------------|
| 1Ah/2Fh | DTA | Set/get Disk Transfer Address (DS:DX, returns ES:BX) |
| 25h/35h | IVT | Set/get interrupt vector (stubs) |
| 2Ah/2Ch | Date/time | Get current date/time |
| 30h | DOS version | Returns AL=5 AH=0 (DOS 5.0) |
| 44h | IOCTL | Subfunctions 00h (device info) and 09h (is remote) |
| 4Ch | Exit | Terminate with return code in AL |
| 62h | Get PSP | Returns BX = PSP segment |

With `--screen`, INT 21h text output (AH=01h/02h/06h/09h/40h) also writes to the video framebuffer at the cursor position.

### INT 10h — Video BIOS

Requires `--screen` flag or `SCREEN` assembly directive.

| AH | Function | Description |
|----|----------|-------------|
| 00h | Set mode | AL = mode number |
| 01h | Set cursor shape | CH = start scan (bit 5 = hide), CL = end scan |
| 02h | Set cursor pos | DH = row, DL = col, BH = page (ignored) |
| 03h | Get cursor pos | Returns DH = row, DL = col, CH/CL = shape |
| 06h | Scroll up | AL = lines, BH = fill attr, CX/DX = window |
| 07h | Scroll down | AL = lines, BH = fill attr, CX/DX = window |
| 08h | Read char+attr | Returns AH = attr, AL = char at cursor |
| 09h | Write char+attr | AL = char, BL = attr, CX = count |
| 0Ah | Write char only | AL = char, CX = count (keeps existing attr) |
| 0Eh | Teletype | AL = char (handles CR/LF/BS/TAB, auto-scroll) |
| 0Fh | Get video mode | Returns AL = mode, AH = cols, BH = page |

### INT 16h — Keyboard BIOS

Requires `--events` flag for keyboard input.

| AH | Function | Description |
|----|----------|-------------|
| 00h | Blocking read | Dequeue one key: AH = scancode, AL = ASCII |
| 01h | Poll | Peek without consuming: ZF=0 if key available |
| 02h | Shift status | Return modifier byte in AL |

**Extended keys (Alt+letter, function keys)**: For keys with `ascii=0x00`, INT 16h AH=00h returns the scan code in AH and zero in AL. For example, Alt+Q returns AX=0x1000 (scancode=0x10, ascii=0x00). INT 16h AH=01h peeks the same value.

**INT 21h AH=06h two-byte protocol**: Since AH=06h only returns a single byte in AL, extended keys use a two-call sequence. The first call returns AL=0x00 (extended prefix, ZF=0). The second call returns AL=scan_code (ZF=0). Programs detect extended keys by checking if the first byte is 0x00.

### INT 33h — Mouse Driver

| AX | Function | Description |
|----|----------|-------------|
| 0000h | Reset/detect | Returns AX=FFFFh if mouse present |
| 0001h | Show cursor | Make mouse cursor visible |
| 0002h | Hide cursor | Hide mouse cursor |
| 0003h | Get position | BX = buttons, CX = x, DX = y |
| 0007h | Set H range | CX = min, DX = max |
| 0008h | Set V range | CX = min, DX = max |

### .COM Environment

- Loaded at offset 100h (`ORG 100h` at top of file)
- All segment registers initially point to the same segment (CS=DS=ES=SS=0)
- SP starts at FFFEh
- 1MB address space with segment:offset addressing (physical = seg*16 + offset)
- Segment registers can be changed (e.g., `MOV ES, AX` for VRAM access)
- 100 million instruction safety limit (configurable via `--run N` / `--trace N`)

**PSP Command Tail** — when `--args` is used, the program receives arguments at the PSP:

| Offset | Content |
|--------|---------|
| `0x80` | Length byte (space + args, max 126) |
| `0x81` | `0x20` (leading space, per DOS convention) |
| `0x82+` | Argument characters |
| `0x81+len` | `0x0D` (CR terminator) |

```asm
; Reading command-line args
    MOV SI, 0x80
    LODSB               ; AL = command tail length
    OR  AL, AL
    JZ  no_args
    MOV CL, AL
    XOR CH, CH
    INC SI              ; skip leading space
    DEC CX              ; CX = number of arg chars, SI = first char
```

---

## Video Framebuffer

Enable with `--screen <mode>` or `SCREEN <mode>` in assembly source.

**Modes**:

| Mode | Size | Color | VRAM Segment |
|------|------|-------|-------------|
| MDA | 80x25 | mono | B000h |
| CGA40 | 40x25 | color | B800h |
| CGA80 | 80x25 | color | B800h |
| VGA50 | 80x50 | color | B800h |

**VRAM layout**: each cell is 2 bytes [character][attribute], row-major.

```asm
MOV AX, 0B800h
MOV ES, AX
XOR DI, DI
MOV WORD ES:[DI], 0741h  ; 'A' with white-on-black at (0,0)
```

Programs can use direct VRAM writes, INT 10h services, or INT 21h text output — all three methods write to the framebuffer.

Screen state is rendered as a JSON `"screen"` object at program exit (and in FAILED JSON). CP437 box-drawing characters are transcoded to UTF-8 for TUI compatibility.

**Screen JSON format**:

```json
{
  "mode": "CGA80",
  "cols": 80,
  "rows": 25,
  "cursor": [0, 5],
  "lines": ["Hello World!", "", "", "..."]
}
```

- `cursor`: `[row, col]` array
- `cursor_hidden`: `true` if cursor is hidden (omitted when visible)
- `lines`: array of right-trimmed strings, always exactly `rows` entries
- `region`: `[x, y, w, h]` array (present only for PARTIAL VRAMOUT)
- `attrs`: array of hex attribute strings (present only with ATTRS flag)

---

## Keyboard and Mouse Input

Enable with `--events <json|file>`. Works with `--run`, `--trace`, `--build_run`, `--build_trace`. Combine with `--args` and `--screen` as needed.

The argument is either **inline JSON** (if it starts with `[`) or a **file path** to a JSON file. The format is identical in both cases — a JSON array of event objects.

### Event Formats

There are two modes: **sequential** (no `"on"` trigger) and **triggered** (with `"on"`). Both can coexist in the same array.

#### Sequential Events (recommended for TUI testing)

Events are consumed in stream order. No `"on"` field needed.

| Object | Description |
|--------|-------------|
| `{"keys":"..."}` | Inject keyboard characters when buffer is empty |
| `{"mouse":{"x":N,"y":N}}` | Update mouse position (applied on INT 33h query) |
| `{"mouse":{"buttons":N,"x":N,"y":N}}` | Update mouse position + buttons |

```json
[
  {"keys":"hello"},
  {"mouse":{"buttons":1,"x":80,"y":40}},
  {"keys":"\r"}
]
```

**Mouse fields**: `x` (required), `y` (required), `buttons` (optional, default 0). Button bitmask: 1=left, 2=right, 4=middle. Coordinates are pixel-based (text mode: column*8, row*8).

**Validation**: an event object cannot have both `keys` and `mouse`. Mouse events cannot have an `on` trigger.

#### Triggered Events (legacy)

Fire at specific INT 16h call counts.

| Field | Description |
|-------|-------------|
| `"on"` | Trigger: `read:N` or `poll:N` (1-based) |
| `"keys"` | Characters to inject |

```json
[
  {"on":"read:1","keys":"Y"},
  {"on":"poll:5","keys":"\\SShifted\\S"}
]
```

### Sequential Event Algorithm

Mouse events act as **lazy barriers** — they are NOT applied immediately. Instead:

1. **Keys events** inject into the keyboard buffer when it is empty, then advance the cursor
2. **Mouse events** block the cursor until the program calls `INT 33h AX=0003h` (get position), at which point the mouse state is applied and the cursor advances past the barrier to inject any following keys

This ensures correct ordering in TUI main loops:

```
Stream: [keys:"A", mouse:{click at 80,40}, keys:"B"]

1. Init:     inject "A" -> buffer=["A"], cursor stops at mouse (barrier)
2. Program:  INT 16h read -> consumes "A" -> buffer empty
3. Program:  INT 33h AX=3 -> applies click, returns {1, 80, 40}
                            -> injects "B" -> buffer=["B"]
4. Program:  INT 16h read -> consumes "B"
```

The program sees "A" first, then the click, then "B" — matching the intended order. Without lazy barriers, the click would be visible before "A" was even processed.

### Modifier Toggles

Use escape sequences inside `"keys"` strings to toggle modifier state. Modifiers toggle on first occurrence and off on second — each keystroke is stamped with the active modifier state at injection time.

| Escape | Modifier | Bits |
|--------|----------|------|
| `\S` | Shift | bits 0+1 (Right+Left Shift) |
| `\C` | Ctrl | bit 2 |
| `\A` | Alt | bit 3 |

In JSON strings, backslashes must be double-escaped: `\\S`, `\\C`, `\\A`.

**Ctrl+letter keys**: When the Ctrl modifier is active and a letter (a-z or A-Z) is injected, the character is transformed to the corresponding control code (letter & 0x1F), producing ASCII 0x01-0x1A. For example, `\\Cc\\C` sends Ctrl+C (0x03).

**Alt+letter keys**: When the Alt modifier is active and a letter (a-z/A-Z) is injected, the keystroke is stored with `ascii=0x00` and the letter's scan code, matching real BIOS behavior. This means INT 16h AH=00h returns the scan code in AH and zero in AL, and INT 21h AH=06h produces a proper two-byte extended key sequence (0x00 prefix, then scan code).

### Unicode Escapes

The JSON parser supports `\uXXXX` unicode escapes (RFC 8259) inside key strings. This is primarily useful for injecting null bytes (0x00) and constructing extended key sequences for keys that don't have modifier toggles.

When `injectKeys()` encounters a null byte (0x00) followed by another byte, it creates a single extended keystroke with `ascii=0x00` and `scancode=next_byte`. This enables direct injection of any extended key.

| Extended Key | Scan Code | JSON Escape |
|-------------|-----------|-------------|
| F1 | 3Bh | `\u0000\u003B` |
| F2 | 3Ch | `\u0000\u003C` |
| F10 | 44h | `\u0000\u0044` |
| Home | 47h | `\u0000\u0047` |
| Up Arrow | 48h | `\u0000\u0048` |
| Left Arrow | 4Bh | `\u0000\u004B` |
| Right Arrow | 4Dh | `\u0000\u004D` |
| End | 4Fh | `\u0000\u004F` |
| Down Arrow | 50h | `\u0000\u0050` |
| Delete | 53h | `\u0000\u0053` |

### Inline vs File Usage

**Inline JSON** — pass the array directly on the command line:

```bash
agent86 prog.com --run --events '[{"keys":"Hello\r"}]'
```

**JSON file** — pass a file path containing the same array:

```bash
agent86 prog.com --run --events events.json
```

Detection rule: if the argument starts with `[` it is parsed as inline JSON, otherwise it is read as a file path.

### Examples

Type text and press Enter (sequential — simplest form):
```json
[{"keys":"Hello, world!\r"}]
```

Click at position, then type (sequential with mouse):
```json
[
  {"mouse":{"buttons":1,"x":80,"y":40}},
  {"keys":"text here"}
]
```

Interleaved keys and mouse (TUI interaction):
```json
[
  {"keys":"A"},
  {"mouse":{"buttons":1,"x":80,"y":40}},
  {"keys":"B"},
  {"mouse":{"buttons":0,"x":160,"y":80}},
  {"keys":"C"}
]
```

Legacy triggered format (still works):
```json
[{"on":"read:1","keys":"Y"}]
```

Mixed triggered + sequential:
```json
[
  {"on":"read:1","keys":"init"},
  {"keys":"hello"},
  {"mouse":{"x":10,"y":5}}
]
```

Shifted input:
```json
[{"keys":"\\SHello\\S"}]
```

Send Ctrl+C:
```json
[{"keys":"\\Cc\\C"}]
```

Alt+Q (for menu shortcuts):
```json
[{"keys":"\\Aq\\A"}]
```

Alt+F then Alt+Q (open File menu, then Quit):
```json
[{"keys":"\\Af\\A"},{"keys":"\\Aq\\A"}]
```

F10 key (via unicode escape):
```json
[{"keys":"\u0000\u0044"}]
```

Arrow key sequence (Down, Down, Enter):
```json
[{"keys":"\u0000\u0050\u0000\u0050\r"}]
```

Mouse click then release (separate events):
```json
[
  {"mouse":{"buttons":1,"x":40,"y":16}},
  {"keys":""},
  {"mouse":{"buttons":0,"x":40,"y":16}}
]
```

---

## JSON Output Reference

All output is a single JSON line on stdout. For `--build_run`/`--build_trace`, two JSON lines are emitted: compile result first, execution result second. If assembly fails, only the compile error JSON is emitted (no execution).

### Assemble Success

```json
{"compiled":"OK","size":16,"symbols":{"MSG":{"addr":265,"type":"label"},"CR":{"addr":13,"type":"equ"}}}
```

Optional fields: `"prints":[...]`, `"hex_dumps":[...]`

**Prints array** (from PRINT directives):
```json
{"compiled":"OK","size":16,"symbols":{...},"prints":[{"line":5,"text":"hello"},{"line":6,"text":"size = 42"}]}
```

**Hex dumps array** (from HEX_START/HEX_END):
```json
{"compiled":"OK","size":16,"symbols":{...},"hex_dumps":[{"addr":256,"size":5,"bytes":"B8 01 00 BB 02"}]}
```

### Assemble Failure

```json
{"compiled":"FAILED","errors":[{"line":3,"file":"lib.inc","source":"MOV AX, [UNDEF]","message":"unresolved symbol"}]}
```

Each error has: `line` (line number), `source` (source text), `message` (error description). The `file` field is present only for errors in included files.

### Execute Success

```json
{"executed":"OK","instructions":3557}
```

Optional fields (included when data is present):
- `"screen":{...}` — with `--screen` active
- `"vram_dumps":[...]` — standalone VRAMOUT snapshots
- `"reg_dumps":[...]` — standalone REGS snapshots
- `"log":[...]` — LOG/LOG_ONCE entries

Full example with all optional fields:
```json
{"executed":"OK","instructions":3557,"vram_dumps":[...],"reg_dumps":[...],"log":[...],"screen":{...}}
```

### Idle (Interactive Programs)

```json
{"executed":"IDLE","instructions":121869,"idle_polls":1000,"screen":{...}}
```

Auto-terminates when 1,000 consecutive keyboard polls (INT 16h AH=01h or INT 21h AH=06h DL=FFh) return "no key available". This is normal for interactive programs (TUI editors, menus) that reach their event loop with no input pending. Exit code 0. Screen data, vram_dumps, reg_dumps, and log are included when present.

### Execute Failure

```json
{"executed":"FAILED","error":"instruction limit exceeded","screen":{...}}
```

Screen data is included when `--screen` is active, even on failure. The `vram_dumps`, `reg_dumps`, and `log` arrays are also included if populated.

### Breakpoint

```json
{"executed":"BREAKPOINT","addr":300,"name":"init","instructions":75}
```

Optional fields:
- `"screen":{...}` — with VRAMOUT modifier
- `"regs":{...}` — with REGS modifier (inline register snapshot on the breakpoint itself)
- `"vram_dumps":[...]` — standalone VRAMOUT snapshots accumulated before the breakpoint
- `"reg_dumps":[...]` — standalone REGS snapshots accumulated before the breakpoint
- `"log":[...]` — LOG entries accumulated before the breakpoint

### Assert Failed

```json
{"executed":"ASSERT_FAILED","addr":300,"assert":"AX == 5","actual":3,"expected":5,"instructions":75}
```

Optional fields (same as Breakpoint):
- `"screen":{...}` — with VRAMOUT modifier
- `"regs":{...}` — with REGS modifier
- `"vram_dumps":[...]`, `"reg_dumps":[...]`, `"log":[...]` — accumulated data

### Screen Object

Present in execution JSON when `--screen` is active. Also present in VRAMOUT snapshots.

```json
{
  "mode": "CGA80",
  "cols": 80,
  "rows": 25,
  "cursor": [5, 10],
  "lines": ["Hello World!", "", "", ""]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `mode` | string | Video mode name (MDA, CGA40, CGA80, VGA50) |
| `cols` | number | Columns (40 or 80) |
| `rows` | number | Rows (25 or 50) |
| `cursor` | [row, col] | Cursor position |
| `cursor_hidden` | boolean | Present and `true` when cursor is hidden |
| `lines` | string[] | Right-trimmed text lines, always exactly `rows` entries |
| `region` | [x,y,w,h] | Present only for PARTIAL VRAMOUT |
| `attrs` | string[] | Hex attribute strings, present only with ATTRS flag |

### VRAM Dump Entry

Entries in the `"vram_dumps"` array (from standalone VRAMOUT directives):

```json
{"addr":300,"instr":150,"screen":{"mode":"CGA80","cols":80,"rows":25,"cursor":[0,0],"lines":[...]}}
```

### Register Dump Entry

Entries in the `"reg_dumps"` array (from standalone REGS directives):

```json
{"addr":300,"instr":150,"regs":{"AX":42,"BX":0,"CX":0,"DX":0,"SP":65534,"BP":0,"SI":0,"DI":0,"DS":0,"ES":0,"SS":0,"CS":0,"IP":300,"FL":2,"flags":"-----P-"}}
```

### Log Entry

Entries in the `"log"` array (from LOG/LOG_ONCE directives):

Message only:
```json
{"addr":300,"instr":150,"message":"entering loop"}
```

With register value:
```json
{"addr":300,"instr":150,"message":"counter","reg":"CX","value":5}
```

With memory byte:
```json
{"addr":300,"instr":150,"message":"char","mem_addr":512,"size":"byte","value":65}
```

With memory word:
```json
{"addr":300,"instr":150,"message":"total","mem_addr":512,"size":"word","value":1234}
```

### Build Mode Output

`--build_run` and `--build_trace` emit **two JSON lines** on stdout:

```
{"compiled":"OK","size":16,"symbols":{...}}
{"executed":"OK","instructions":42}
```

If assembly fails, only one line is emitted:
```
{"compiled":"FAILED","errors":[...]}
```

---

## Limitations

Things agent86 does **not** support:

### Assembler
- **No conditional assembly** — no IF/ELSE/ENDIF, no IFDEF
- **No 80186+ instructions beyond PUSHA/POPA** — no ENTER/LEAVE, no BOUND, no immediate PUSH, no shift-by-immediate
- **No 80286+ or 80386+ instructions**
- **No floating-point** — no 8087 FPU instructions
- **No multiple segments** — flat .COM model only, no SEGMENT/ENDS
- **No STRUC/RECORD/UNION** — no structured data types
- **No TIMES/DUP** — no repeat count on DB/DW (use RESB/RESW for zero-fill)
- **No DD** — no 32-bit data directive
- **No listing output** — no .lst file generation
- **Labels are case-sensitive** — `MyLabel` and `mylabel` are different symbols

### JIT Emulator
- **No hardware interrupts** — only software INT with the services listed above
- **No I/O ports** — IN/OUT instructions are decoded but have no effect
- **No self-modifying code detection** — code is JIT-compiled one instruction at a time, so self-modification works naturally, but there's no cache coherency concern
- **100M instruction limit** — infinite loops terminate with an error after 100 million instructions (configurable with `--run N`). Interactive programs with event loops typically reach IDLE status (auto-detected after 1,000 consecutive keyboard polls with no input) well before the limit
- **Windows only** — JIT uses VirtualAlloc for RWX buffers (Win64 ABI, x64 code generation)
