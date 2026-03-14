# agent86

A two-pass **8086 assembler** and **per-instruction x64 JIT emulator** for DOS `.COM` binaries.

agent86 assembles 8086 assembly source into flat `.COM` executables, then optionally runs them immediately via a JIT engine that translates each 8086 instruction into native x64 machine code. All structured output is emitted as **JSON on stdout**, while DOS program text output goes to **stderr** — making it easy to integrate into toolchains, test harnesses, and AI-assisted development workflows.

## Features

- **Two-pass assembler** with full 8086 instruction set plus 186 PUSHA/POPA
- **Per-instruction x64 JIT** — no interpreter loop; 8086 opcodes are compiled to native x64 at runtime
- **DOS service emulation** — INT 21h (33 subfunctions), INT 10h (video BIOS), INT 16h (keyboard BIOS), INT 33h (mouse driver)
- **Video framebuffer** — MDA, CGA40, CGA80, and VGA50 text modes with JSON screen dumps
- **Keyboard and mouse input injection** via `--events` (JSON or file)
- **Rich debugging** — breakpoints, assertions, VRAM snapshots, register dumps, LOG/LOG_ONCE directives
- **Macros** — MACRO/ENDM, IRP/ENDM with parameter substitution
- **Expressions** — full 8-level precedence with `$` (current address), labels, and EQU constants
- **INCLUDE** support with recursive expansion, include guards, and circular detection
- **JSON-first output** — structured compile results, execution traces, assertion outcomes, and screen captures

## Quick Start

```bash
# Assemble a source file
agent86 hello.asm

# Assemble and run in one step (recommended workflow)
agent86 hello.asm --build_run

# Assemble and trace with debug directives
agent86 prog.asm --build_trace

# Run a pre-compiled binary with a cycle limit
agent86 prog.com --run 500

# Run with keyboard input
agent86 prog.com --run --events '[{"keys":"Hello\r"}]'

# Run with video output
agent86 prog.com --run --screen CGA80

# Full TUI application test
agent86 app.asm --build_run --screen CGA80 --events '[{"keys":"text"},{"mouse":{"buttons":1,"x":80,"y":40}}]'
```

## Example

```asm
    ORG 100h
    MOV DX, msg
    MOV AH, 09h
    INT 21h
    INT 20h
msg: DB 'Hello, World!$'
```

```bash
$ agent86 hello.asm --build_run
{"compiled":"OK","bytes":20,"symbols":[{"name":"msg","value":267}]}
```
```
Hello, World!
```
```json
{"status":"OK","instructions":4}
```

## CLI Reference

| Flag | Description |
|------|-------------|
| `--build_run [N]` | Assemble `.asm` then execute (recommended) |
| `--build_trace [N]` | Assemble `.asm` then trace with debug directives |
| `--run [N]` | Execute a pre-compiled `.com` binary |
| `--trace [N]` | Trace a pre-compiled `.com` binary |
| `-o <path>` | Override output `.com` path |
| `--args <string>` | Set PSP command tail (program arguments at 0x80) |
| `--events <json\|file>` | Inject keyboard/mouse input |
| `--screen <mode>` | Enable video framebuffer (MDA, CGA40, CGA80, VGA50) |
| `--help [topic]` | Show help overview or per-topic detail |

The optional `[N]` sets the instruction cycle limit (default: 100,000,000).

Run `agent86 --help <topic>` for detailed usage on: `asm`, `run`, `trace`, `build_run`, `directives`, `events`, `screen`, `args`, `o`.

## DOS Emulation

The JIT emulator provides the following interrupt services:

| Interrupt | Service | Coverage |
|-----------|---------|----------|
| INT 20h | Terminate | Program exit |
| INT 21h | DOS services | Console I/O, file system, directory ops, FindFirst/FindNext, memory management (33 subfunctions) |
| INT 10h | Video BIOS | Teletype output, cursor control, scroll, video mode, character read/write (14 subfunctions) |
| INT 16h | Keyboard BIOS | Read key, check key, get shift flags |
| INT 33h | Mouse driver | Show/hide, position, button state, set cursor, sensitivity |

## Project Structure

```
src/
  main.cpp          CLI entry point and JSON output formatting
  asm.cpp / asm.h   Two-pass assembler (parsing, macros, includes, encoding)
  lexer.cpp / .h    Tokenizer for assembly source
  encoder.cpp / .h  8086 instruction encoding (opcode → machine code)
  expr.cpp / .h     Compile-time expression evaluator
  symtab.cpp / .h   Symbol table (labels, EQU constants)
  types.h           Shared type definitions
  jit/
    jit.cpp / .h      JIT engine (decode → emit x64 → execute loop)
    decoder.cpp / .h  8086 machine code decoder
    emitter.cpp / .h  x64 native code emitter and executable buffer
    dos.cpp / .h      DOS/BIOS interrupt handlers
    dos_state.h       DOS state (file handles, DTA, memory allocator)
    cpu.h             CPU8086 struct (registers, flags, 1MB memory)
    video.h           Video framebuffer state and rendering
    kbd.cpp / .h      Keyboard buffer and input event processing
```

## Building

Requires:

- **Linux x86-64** (the JIT emits native x64 machine code)
- **g++** or **clang++** with C++17 support
- No external libraries or dependencies

### Compile

```bash
g++ -std=c++17 -O2 -static -o agent86 \
  src/main.cpp src/asm.cpp src/lexer.cpp src/encoder.cpp \
  src/expr.cpp src/symtab.cpp src/jit/jit.cpp src/jit/emitter.cpp \
  src/jit/dos.cpp src/jit/decoder.cpp src/jit/kbd.cpp
```

This produces a single statically-linked `agent86` binary with no runtime dependencies.

### Notes

- The JIT code buffer uses `mmap` with `PROT_EXEC`
- All file I/O uses standard POSIX APIs
- FindFirst/FindNext (INT 21h AH=4Eh/4Fh) uses POSIX `glob()`
- Path handling uses `realpath`

## Limitations

### Assembler
- No conditional assembly (IF/ELSE/ENDIF, IFDEF)
- No 80186+ instructions beyond PUSHA/POPA
- No floating-point (8087 FPU)
- No multiple segments — flat .COM model only
- No STRUC/RECORD/UNION, TIMES/DUP, or DD

### JIT Emulator
- No hardware interrupts — software INT only
- No I/O ports — IN/OUT decoded but no-op
- 100M instruction default limit (configurable via `--run N`)

## Documentation

See [`manual.md`](manual.md) for the full reference, including the complete instruction set, addressing modes, debug directives, event injection format, and JSON output schema.

## License

See repository for license details.
