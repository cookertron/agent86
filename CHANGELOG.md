# Changelog

All notable changes to agent86 are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning follows [Semantic Versioning](https://semver.org/).

**Version scheme:** `MAJOR.MINOR.PATCH`
- **MAJOR** — Breaking changes to CLI flags, JSON output schema, or assembly syntax
- **MINOR** — New instructions, new CLI flags, new emulator features (backwards-compatible)
- **PATCH** — Bug fixes, diagnostic improvements, internal refactors

---

## [Unreleased]

### Added
- **`\C` (Ctrl) and `\A` (Alt) input modifiers** — New escape prefixes for `--input` and `--events` `"keys"` strings. Sets INT 16h AH=02h shift-flags bits 2 (Ctrl) and 3 (Alt). Prefixes are stackable: `\A\C\Sa` sends `a` with Alt+Ctrl+Shift held (flags = 0x0E).

### Fixed
- **EQU/label case-insensitive collision bug** — A label like `ed_tabwidth` would silently collide with `ED_TABWIDTH EQU 8` due to case-insensitive symbol table normalization. The two-pass assembler made this worse: EQU definitions ran on both passes while labels only registered on pass 1, so the EQU would overwrite the label in pass 2. Now reports an error on collision and guards EQU assignment to pass 1 only.

---

## [0.9.0] — 2026-02-15

### Added
- **ADC and SBB instruction encoding** — Both instructions were in the ISA database but had no encoder. Added full opcode mappings for all operand combinations (reg/reg, reg/mem, reg/imm, mem/imm).
- **INT 10h video BIOS emulation** — AH=00h (set mode), 02h (set cursor), 03h (get cursor), 05h (set page), 06h/07h (scroll up/down), 08h (read char+attr), 09h (write char+attr), 0Ah (write char), 0Eh (teletype), 0Fh (get mode), 11h (font services).
- **Dual-channel output** — INT 21h character output now simultaneously writes to the text output buffer and VRAM, so agents can verify both program output and visual layout.
- **Screenshot rendering** — `--screenshot <file.bmp>` exports VRAM as a 24-bit BMP using CP437 glyphs and CGA 16-color palette. `--font 8x8` or `--font 8x16` selects glyph size.
- **Viewport capture** — `--viewport col,row,width,height` for windowed VRAM subsets in JSON output. Viewport data also included in breakpoint snapshots for frame-by-frame animation debugging.

### Fixed
- **JSON serialization robustness** — `jsonEscape()` now properly escapes all non-printable bytes (0x00-0x1F, 0x80-0xFF) as `\u00XX`. Added 4KB output buffer cap on INT 21h/09h with diagnostic warning. Ensures valid UTF-8 JSON even when programs have pointer errors.

---

## [0.8.0] — 2026-02-01

### Added
- **VRAM and segment-aware memory** — 8KB VRAM buffer at segment B800h (80x50 CGA text mode). Memory reads/writes now resolve through segment registers with correct 8086 defaults (BP-based -> SS, everything else -> DS).
- **Screen capture flags** — `--screen` (full 80x50), `--viewport` (windowed), `--attrs` (include attribute bytes in hex).
- **Input simulation** — `--input <string>` provides pre-loaded stdin for INT 21h AH=01h and AH=06h. Makes interactive programs fully testable in automation.
- **Event scripting** — `--events` for scripted keyboard/mouse interaction timelines with cycle-based triggers.
- **Mouse emulation** — INT 33h handler with `--mouse` flag for initial mouse state.

---

## [0.7.0] — 2026-01-15

### Added
- **Complete 8086 instruction subset** — Added 18 missing instructions across three groups:
  - New instructions: PUSHF, POPF, XLAT, HLT, PUSHA, POPA
  - ISA database + encoder: NOP, XCHG, CBW, CWD, LAHF, SAHF, SAR, RCL, RCR
  - Aliases: SAL (= SHL), LOOPZ/LOOPNZ (= LOOPE/LOOPNE)
- **Emulator debugging** — `--breakpoints`, `--watch-regs`, `--mem-dump`, `--trace` flags. Breakpoint snapshots capture registers, flags, stack, next instruction, and optional memory dumps.
- **Sandboxed file I/O** — `--dos-root` for INT 21h file operations confined to a directory.
- **Conditional jump auto-promotion** — Short-range conditional jumps automatically promoted to near jumps when target is out of range. Multi-pass stabilization loop handles address changes from promotions.

---

## [0.6.0] — 2025-12-15

### Added
- **8086 CPU emulator** — Full emulation built on the shared decoder: 8 GP registers, 4 segment registers, IP, flags. 64KB flat memory. Executes decoded instructions with cycle counting.
  - ALU: ADD, SUB, AND, OR, XOR, CMP, TEST, NOT, NEG, MUL, IMUL, DIV, IDIV
  - Shifts/rotates: SHL, SHR, SAR, ROL, ROR, RCL, RCR
  - Data movement: MOV, XCHG, LEA, PUSH, POP, IN, OUT
  - Control flow: JMP, CALL, RET, INT, 16 conditional jumps, LOOP/LOOPE/LOOPNE, JCXZ
  - String ops: MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W with REP/REPE/REPNE
  - Flags: full CF, ZF, SF, OF, PF, AF computation
- **DOS interrupt emulation** — INT 20h (terminate), INT 21h AH=01h/02h/06h/09h/4Ch (console I/O and exit).
- **CLI modes** — `--run <file.com>` (run binary), `--run-source <file.asm>` (assemble + run), `--max-cycles`.
- **JSON emulation output** — Structured output with registers, output buffer, halt reason, cycle count, fidelity score, skipped interrupts.

---

## [0.5.0] — 2025-11-15

### Added
- **Diagnostic hint system** — All 29 error emission sites now include actionable fix hints. Features:
  - Smart numeric literal diagnosis (hex/binary/octal validation)
  - Fuzzy label matching with Levenshtein distance for "did you mean?" suggestions
  - ISA-driven auto-hints listing valid operand forms when an instruction is used incorrectly
  - Context-sensitive unexpected-token messages
- **New warnings** — Memory-immediate without size prefix, immediate value truncation, ORG after code emission, duplicate label definition.
- **`--explain <MNEMONIC>`** — Query the ISA knowledge base for a single instruction.
- **`--dump-isa`** — List all supported instructions as JSON.
- **Queryable help system** — `--help` lists all topics, `--help <topic>` returns structured JSON per topic.

---

## [0.4.0] — 2025-10-15

### Changed
- **Shared instruction decoder** — Refactored `disasmInstruction()` into a structured `decodeInstruction()` that returns `DecodedInst` objects. This single decoder now feeds both the disassembler and emulator, eliminating duplicate logic.
  - New types: `OpKind`, `DecodedOperand`, `DecodedInst`
  - New function: `formatInstruction()` for human-readable output
  - Fixed missing flag/misc opcodes: CLC, STC, CMC, CLI, STI, CLD, STD, NOP, XCHG, CBW, CWD, LAHF, SAHF

---

## [0.3.0] — 2025-09-15

### Added
- **Macro preprocessor** — Full MASM-style macro system:
  - `MACRO`/`ENDM` with parameterized arguments
  - `LOCAL` labels (unique per expansion)
  - `REPT` (repeat N times), `IRP` (iterate over list)
  - `&` concatenation for synthesizing labels
  - Nesting support (macros invoking macros, REPT/IRP inside macros)
- **STRUC/ENDS** — Structure definitions with field offset calculation.
- **Multi-file INCLUDE** — `INCLUDE 'file.asm'` with relative path resolution, 16-level nesting, circular-include detection. Three quoting styles supported. Diagnostics trace back to original file and line.

---

## [0.2.0] — 2025-08-15

### Added
- **Disassembler** — `--disassemble <file.com>` decodes .COM binaries back to structured JSON instruction listings.
- **`--agent` mode** — Primary JSON output mode with full listing (addresses, decoded instructions, bytes), symbol table, and diagnostics.
- **Two-pass assembler** — Forward reference resolution, EQU constants, label arithmetic.
- **Local labels** — `.label` syntax inside `PROC`/`ENDP` blocks.

---

## [0.1.0] — 2025-07-15

### Added
- **Initial release** — Single-file 8086 assembler producing flat `.COM` binaries.
- **ISA database** — Instruction validation with operand form checking.
- **Tokenizer + parser** — Front end for 8086 assembly syntax.
- **Data directives** — `DB`, `DW`, `ORG`, `EQU`.
- **Addressing modes** — Register, immediate, direct memory, register-indirect with displacement (`[BX+SI+4]`).
- **Basic diagnostics** — Error messages with line numbers.
- **Output** — Flat binary .COM files and human-readable assembly listing.
