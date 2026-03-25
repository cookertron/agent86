# Changelog

All notable changes to agent86 are documented in this file.

---

## [0.20.0] - 2026-03-19

### Added
- **DOS_FAIL / DOS_PARTIAL debug directives** — inject one-shot DOS failures for testing error-handling code paths. `DOS_FAIL <int>, <ah> [, <error_code>]` arms a failure that sets CF=1 and AX=error_code (default 5) on the next matching INT call. `DOS_PARTIAL <int>, <ah>, <count>` arms a partial result that sets CF=0 and AX=count. Both are one-shot: subsequent calls proceed normally. Works with any INT/AH combination (file create 3Ch, write 40h, alloc 48h, etc.). Full pipeline: assembler parses directive, .dbg serialization, JIT loads and arms at address, intercepts before handleDOSInt.

### Test Results
- tests/test_dos_fail.asm: 6 tests pass (default error, custom error, one-shot, write fail, partial write, alloc fail)
- All regressions pass: test_log, test_regs, test_vramout, test_segov, test_ds_io, test_hello25, test_push_pop_es, test_jit_memcorrupt

---

## [0.19.5] - 2026-03-18

### Fixed
- **Debug directives collide with jump targets** — Debug directives (LOG, BREAKPOINT, ASSERT_EQ, etc.) emit 0 bytes of machine code. When placed between a conditional jump's fall-through path and the jump target label, they shared the same address as the target. The JIT fires all directives at a given address, so a BREAKPOINT intended for the fall-through path would also fire when the jump was taken. Fixed by tracking `directive_pending_` state in the assembler: when a runtime debug directive is followed by a label, a 1-byte NOP (0x90) is automatically inserted to separate their addresses. Both pass 1 and pass 2 agree on NOP placement, so all forward references remain correct. The NOP is unreachable on the fall-through path (BREAKPOINT halts before it) and invisible on the jump path (jump targets now resolve to the post-NOP address).

### Test Results
- Bug reproducer passes: JC jumps over LOG+BREAKPOINT on fall-through path, only jump-target LOG fires
- All regressions pass: test_log, test_regs, test_vramout, test_segov, test_ds_io, test_hello25, test_push_pop_es, test_segs, test_jit_memcorrupt, test_offset, test_macro

---

## [0.19.4] - 2026-03-16

### Fixed
- **ASSERT_EQ / LOG do not support segment override syntax** — `ASSERT_EQ BYTE ES:[0], 41h` and `LOG "msg", BYTE ES:[addr]` now parse and honor segment override prefixes (ES, CS, SS, DS). Previously the parser expected `[` immediately after the BYTE/WORD size specifier, rejecting the segment register prefix. The fix spans the full pipeline: assembler parser detects `SREG:` tokens and records `mem_seg` in the directive, all three .dbg serialization paths emit `"mem_seg":N`, the JIT deserializer reads it back, and the runtime computes the physical address as `sregs[seg]*16 + offset` (20-bit, matching the emulator's segmented addressing). Failure messages now include the segment name (e.g., `BYTE ES:[0] == 65`).

### Test Results
- Bug reproducer passes: `ASSERT_EQ BYTE ES:[0], 41h` compiles and verifies at runtime
- Comprehensive: ES byte/word, SS byte, default-segment byte, all pass
- LOG with segment override: `LOG "msg", BYTE ES:[0]` reports correct value from ES segment
- All regressions pass: test_segov, test_macro, test_log

---

## [0.19.3] - 2026-03-11

### Fixed
- **`\C` modifier doesn't convert letters to control codes** — `injectKeys()` set the Ctrl shift flag (bit 2) when `\C` was active but left the ASCII byte unchanged. Real DOS keyboard hardware produces control codes (`letter & 0x1F`, i.e. uppercase - 0x40) when Ctrl is held. Now `\Ca\C` injects ASCII 0x01 (Ctrl+A), `\Cz\C` injects 0x1A (Ctrl+Z), etc. Both lowercase and uppercase letters are handled. The `\u001A` workaround is no longer needed.

### Test Results
- tests/test_ctrl.asm: Ctrl+A=0x01, Ctrl+Z=0x1A, Ctrl+C=0x03, Ctrl+H=0x08, plain letter after toggle-off
- All regressions pass

---

## [0.19.2] - 2026-03-10

### Fixed
- **Mouse lazy barrier fires while keyboard buffer still has unconsumed keys** — `advanceMouseOnQuery()` now checks that the keyboard buffer is empty before applying a mouse barrier event. Previously, INT 33h AX=0003h would fire the barrier regardless of pending keystrokes, causing mouse state to change mid-way through a multi-key sequence. This affected TUI programs whose main loops poll both keyboard and mouse each iteration (e.g., TEDIT: Ctrl+N followed by Ctrl+Tab would have the mouse click applied between the two keystrokes instead of after both).

### Test Results
- TEDIT baseline and bug repro both produce Col 12 (mouse click correctly sequenced after all keys consumed)
- tests/test_mouse_events.asm: passes (single-key events unaffected by the guard)
- All regressions pass: stress (49/49), vm (5/5)

---

## [0.19.1] - 2026-03-08

### Fixed
- **Alt+letter extended key sequences** — Alt+letter keys now produce correct BIOS-compatible behavior across all keyboard interfaces
  - `injectKeys()`: when Alt modifier is active for a letter (a-z/A-Z), the keystroke is stored with `ascii=0x00` and the letter's scan code, matching real BIOS behavior
  - **INT 16h AH=00h**: Alt+Q now returns AX=0x1000 (scancode=0x10, ascii=0x00) instead of the incorrect AX=0x1071 (ascii='q')
  - **INT 21h AH=06h DL=FFh**: now implements the real DOS two-byte extended key protocol — when a key with `ascii=0x00` is consumed, the first call returns AL=0x00 (extended prefix), and the second call returns AL=scan_code. Previously returned the plain ASCII letter in a single call, making it impossible for TUI frameworks to detect Alt+key shortcuts
  - Pending extended byte state (`has_pending_ext_`, `pending_ext_byte_`) on `KeyboardBuffer` persists between calls and is reset on `setEvents()`
- **`\uXXXX` unicode escapes in `--events` JSON** — the JSON parser now supports RFC 8259 `\uXXXX` escape sequences in key strings, enabling injection of null bytes and arbitrary byte values
  - `\u0000` produces a null byte (0x00), essential for extended key prefixes
  - Values 0x0000–0xFFFF are supported; the low byte is emitted (sufficient for all DOS/BIOS byte values)
  - In `injectKeys()`, a null byte followed by another byte creates a single extended keystroke: `{ascii=0x00, scancode=next_byte}` — this enables direct injection of any extended key (e.g., F10 via `\u0000\u0044`)

### Alt+Letter Scan Code Table

The scan codes used for Alt+letter keystrokes (same as the standard letter scan codes):

```
A=1Eh B=30h C=2Eh D=20h E=12h F=21h G=22h H=23h I=17h
J=24h K=25h L=26h M=32h N=31h O=18h P=19h Q=10h R=13h
S=1Fh T=14h U=16h V=2Fh W=11h X=2Dh Y=15h Z=2Ch
```

### Examples

Alt+Q via modifier toggle (recommended for Alt+letter shortcuts):
```json
[{"keys":"\\Aq\\A"}]
```
- INT 16h AH=00h → AX=0x1000 (scancode=0x10, ascii=0x00)
- INT 21h AH=06h → first call: AL=0x00, second call: AL=0x10

F10 via unicode escape (for extended keys without modifier toggles):
```json
[{"keys":"\u0000\u0044"}]
```
- INT 16h AH=00h → AX=0x4400 (scancode=0x44, ascii=0x00)
- INT 21h AH=06h → first call: AL=0x00, second call: AL=0x44

### Test Results
- tests/test_altkey.asm: Alt+Q via INT 21h AH=06h two-byte protocol verified (0x00 prefix + 0x10 scan code)
- tests/test_unicode_esc.asm: F10 via `\u0000\u0044` verified through INT 21h AH=06h
- All regressions pass: stress (49/49), vm (5/5), mouse_events, log, segov, regs

---

## [0.19.0] - 2026-03-03

### Added
- **Unified mouse + keyboard event stream** — `--events` now supports sequential events consumed in order, interleaved with mouse position/button updates
  - **Sequential keys** — `{"keys":"hello"}` (no `"on"` trigger) injects keystrokes when the keyboard buffer is empty, consumed in stream order
  - **Sequential mouse** — `{"mouse":{"x":80,"y":40,"buttons":1}}` updates INT 33h mouse state (`x` and `y` required, `buttons` optional, default 0; bitmask: 1=left, 2=right, 4=middle)
  - **Lazy barrier semantics** — mouse events are NOT applied eagerly; they act as barriers in the event stream and are only applied when the program calls `INT 33h AX=0003h` (get position). This ensures correct ordering: preceding keyboard events are fully consumed before the mouse state changes
  - **Mixed format** — triggered (`{"on":"read:1","keys":"..."}`) and sequential events can coexist in the same JSON array
  - `advanceSequential()` stops at mouse events; `advanceMouseOnQuery()` called from INT 33h AX=0003h handler applies the pending mouse event and advances the stream
- **`InputEvent` / `MouseEvent` structs** in kbd.h for the unified event model
- **`parseJsonNumber()`** — JSON integer parser for mouse coordinate fields
- **`parseMouseObject()`** — parses `{"buttons":N,"x":N,"y":N}` nested objects

### Changed
- `KeyboardBuffer::setEvents()` now accepts triggered events, sequential events, and a `MouseState*` pointer (three parameters instead of one)
- `JitEngine::setEvents()` signature updated to `(vector<KeyEvent>, vector<InputEvent>)` — forwards mouse pointer from `mouse_` member
- `parseEventsJson()` returns two output vectors: `triggered` and `sequential`
- `loadEventsArg()` updated for the new two-vector signature
- `--help events` rewritten with sequential format documentation, mouse event examples, and barrier semantics explanation

### Algorithm
The sequential event stream uses a cursor-based consumption model:
1. `advanceSequential()`: skip past keys events when buffer is empty (inject them), stop at mouse events (barriers) or when buffer is non-empty
2. `advanceMouseOnQuery()`: called from INT 33h AX=0003h — apply the pending mouse event, advance cursor, then call `advanceSequential()` to inject any following keys batch
3. At init (`setEvents`): prime the stream by calling `advanceSequential()` to inject the first keys batch
4. On `blockingRead()`/`poll()`: call `advanceSequential()` before and after key consumption

### Backward Compatibility
- Legacy triggered format (`{"on":"read:1","keys":"..."}`) continues to work unchanged
- Programs that don't use `--events` or don't have sequential events are unaffected
- Idle detection works correctly: if only mouse events remain with no keys, buffer stays empty, polls return ZF=1, and idle detection triggers normally

### Test Results
- tests/test_mouse_events.asm: interleaved keys + mouse with lazy barriers, verifies ordering (mouse applied on INT 33h query, not at injection time)
- All regressions pass: stress (49/49), vm (5/5), test_mouse (static mouse), notepad (IDLE at 121K instructions)

---

## [0.18.0] - 2026-03-02

### Fixed
- **INT 21h segment-aware addressing** — all DOS services now respect segment registers for memory operands
  - All functions using DS:DX (AH=09h, 3Bh, 3Ch, 3Dh, 3Fh, 40h, 41h, 43h, 4Eh) compute physical address as `DS*16 + offset`
  - AH=47h (Get CWD) uses DS:SI correctly
  - AH=56h (Rename) uses DS:DX for old path and ES:DI for new path
  - `readASCIIZ()` and `resolvePath()` accept segment parameter
  - `writeDTARecord()` uses physical DTA address
  - Programs can now set DS to a different segment and perform file I/O into that segment
- **DTA segment tracking** — AH=1Ah (Set DTA) stores both DS and DX; AH=2Fh (Get DTA) returns stored segment in ES
  - DTA stored as seg:offset pair (`dta_seg` + `dta_addr`) instead of offset-only
  - FindFirst/FindNext use physical DTA address for record writes
- **Memory allocator no longer overlaps COM segment** — AH=48h allocations start above physical 0x10000
  - `mem_top` changed from `0x0FFF` (64KB) to `0x9FFF` (640KB conventional memory, below video area at A000:0)
  - `MEM_FLOOR = 0x1000` prevents allocations below physical 0x10000 (the COM program's 64KB space)
  - ~576KB available for program allocations (segments 0x1000–0x9FFF)
  - `maxAvailable()` accounts for the floor
  - Previously, a 32KB allocation at segment 0x0800 (physical 0x8000) would overlap the COM segment's stack

### Test Results
- tests/test_ds_io.asm: allocator returns seg >= 0x1000, AH=09h/40h/3Fh with non-zero DS, file write/read round-trip through allocated segment
- All regressions pass: stress (49/49), segment overrides, notepad (24,815 bytes, IDLE at 121K instructions)

---

## [0.17.0] - 2026-03-02

### Added
- **Segment override syntax** — assembler now supports `ES:[DI]`, `CS:[BX+5]`, `SS:[BP-2]`, `DS:[SI]` notation
  - Emits correct prefix bytes: `0x26` (ES), `0x2E` (CS), `0x36` (SS), `0x3E` (DS)
  - Works with all instruction forms: MOV, ADD, CMP, TEST, PUSH, shifts, unary ops, LEA, XCHG, LDS/LES
  - Combines with BYTE/WORD size overrides: `MOV AL, BYTE ES:[DI+2]`
  - `estimateSize()` accounts for the extra prefix byte via `estimateSizeBase()` wrapper
  - Lexer updated: `ES:` followed by `[` is tokenized as REGISTER + COLON (not as a LABEL)
  - Eliminates the need for `DB 26h` / `SegES MACRO` workarounds
- **`--args` CLI flag** — pass command-line arguments to the running .COM program via the DOS PSP command tail
  - `--args "hello world"` writes to PSP at offset `0x80` (length byte), `0x81+` (space + args + `0x0D` terminator)
  - DOS convention: command tail starts with a space, max 126 bytes, CR-terminated
  - Works with `--run`, `--trace`, `--build_run`, `--build_trace`
  - Without `--args`: PSP at `0x80` = 0 (length), `0x81` = `0x0D` (empty tail)
  - `--help args` topic with PSP memory layout and assembly reading example

### Test Results
- tests/test_segov.asm: ES/SS overrides, displacements, size overrides, hex verification (`26 8B 05`), round-trip via separate segment
- All regressions pass: stress (49/49), vm (5/5), screen, macros, mouse, vramout

---

## [0.16.0] - 2026-03-02

### Added
- **REGS debug directive** — register state snapshots for debugging
  - Standalone `REGS` — non-halting snapshot, accumulates in `"reg_dumps"` JSON array (max 32)
  - Colon modifier on BREAKPOINT: `BREAKPOINT : REGS` — includes full register state in BREAKPOINT JSON
  - Colon modifier on ASSERT_EQ: `ASSERT_EQ AX, 5 : REGS` — includes registers on assertion failure
  - Chainable with VRAMOUT: `BREAKPOINT : VRAMOUT : REGS` — both screen and registers in one JSON
  - `dumpRegsJson()` helper: AX/BX/CX/DX/SP/BP/SI/DI/DS/ES/SS/CS/IP/FL + decoded flag string
- **LOG / LOG_ONCE runtime debug print** — printf-style runtime logging without halting
  - `LOG "message"` — message-only log entry
  - `LOG "message", AX` — message with register value (any 16-bit register)
  - `LOG "message", BYTE [addr]` / `LOG "message", WORD [addr]` — message with memory value
  - `LOG_ONCE label, "message" [, operand]` — fires once per label (deduplicated via `log_once_fired_` set)
  - Accumulates in `"log"` JSON array (max 256 entries per run)
  - Each entry: `{"addr":N,"instr":N,"message":"..."}` with optional `"value"`, `"reg"`/`"mem_addr"` fields
- **Generalized modifier system** — extensible colon-separated modifier parsing
  - `findModifierColon()` replaces single-purpose `findVramOutModifier()`
  - `parseModifiers()` handles arbitrary `: VRAMOUT ... : REGS` chains in any order
  - `Modifiers` struct bundles VramOutParams + regs flag

### Test Results
- tests/test_regs.asm: standalone REGS, BREAKPOINT:REGS, VRAMOUT:REGS chaining verified
- tests/test_log.asm: message-only, register, BYTE memory, WORD memory, LOG_ONCE dedup in loop
- All regressions pass: stress (49/49), vm (5/5), screen, macros, mouse, vramout

---

## [0.15.0] - 2026-03-02

### Added
- **Idle detection** — auto-terminates interactive programs that reach a stable idle state
  - Tracks consecutive keyboard polls (INT 16h AH=01h or INT 21h AH=06h DL=FFh) that return "no key"
  - After 1,000 consecutive idle polls, terminates with `"executed":"IDLE"` status
  - JSON includes `instructions`, `idle_polls`, `screen` data, and any `vram_dumps`
  - Exit code 0 (success) — program reached its event loop with nothing to do
  - With `--events`: idle detection triggers after all injected keys are consumed
- **INT 16h without --events** — keyboard BIOS now works even without the `--events` flag
  - AH=01h (poll): returns ZF=1 (no key available) instead of being unhandled
  - AH=00h (blocking read): terminates with descriptive FAILED JSON
  - AH=02h (shift status): returns 0
### Fixed
- **NOP-bloated binary** — reverted overly pessimistic `estimateSize()` from v0.14.0; now saves pass 1 estimates in `pass1_sizes_` vector and uses those for pass 2 NOP padding. Produces optimal machine code encodings (disp8 when displacement fits, sign-extended byte for small ALU immediates) with minimal NOPs only where needed (Jcc short/near). Notepad binary: 24,815 bytes (down from 26,458)

### Test Results
- Notepad TUI: compiles (24,815 bytes), renders complete 80x25 TUI, reaches IDLE after 121K instructions
- All regressions pass: stress (49/49), vm (5/5), screen, macros, mouse, vramout

---

## [0.14.0] - 2026-03-02

### Added
- **MACRO/ENDM preprocessor** — MASM-compatible text-level macros with parameters, expanded between INCLUDE and pass 1
  - `PushAll MACRO` / `PushAll ENDM` — define named macros with body lines
  - Parameterized macros: `WriteByte MACRO char` / `MOV DL, char` / `ENDM`
  - Invocation replaces macro name with expanded body (case-insensitive)
  - Up to 100 expansion iterations to handle nested macro calls
- **IRP (Indefinite Repeat)** — standalone and within macros
  - `IRP reg, <AX, BX, CX, DX>` / `PUSH reg` / `ENDM` — repeat body for each item
  - IRP blocks inside macro definitions are expanded at invocation time
- **INT 33h mouse driver** — 6 subfunctions for mouse support
  - AX=0000h: reset and detect mouse (returns AX=FFFFh if present)
  - AX=0001h/0002h: show/hide mouse cursor
  - AX=0003h: get position and button state (BX=buttons, CX=x, DX=y)
  - AX=0007h/0008h: set horizontal/vertical range
  - `MouseState` struct in video.h; mouse state persists across calls
- **INT 10h AH=01h** — set cursor shape via CH (start scan line) and CL (end scan line); bit 5 of CH = hide cursor
- **INT 10h AH=03h** — now returns stored cursor shape in CH/CL (previously only returned position)
- **Unquoted INCLUDE paths** — `INCLUDE TUI\tui.inc` now works alongside `INCLUDE "file.inc"`; raw text-level detection instead of tokenizer-based
- **Screen data in FAILED JSON** — instruction limit exceeded, invalid opcode, and INT 16h empty buffer errors now include `"screen":{...}` when `--screen` is active

### Fixed
- **estimateSize() pass 1/pass 2 mismatch** — three value-dependent size optimizations in `encoder.cpp` returned different values depending on whether symbol values were resolved, causing a cumulative address offset. Initial fix (this version) made all estimates pessimistic; refined in v0.15.0 to use pass 1 saved sizes for optimal output.
- **cursor_hidden** added to JSON screen output when cursor is hidden

### Test Results
- Notepad TUI editor: compiles (1,633 symbols), renders complete 80x25 TUI (menu bar, bordered window, status bar) correctly
- stress.asm: 49/49 (no regressions)
- vm.com: 5/5 (no regressions)
- test_screen.asm: screen output correct (no regressions)
- test_macro.asm, test_macro2.asm: PushAll/PopAll round-trip, parameterized macros, standalone IRP

---

## [0.13.0] - 2026-03-02

### Added
- **VRAMOUT debug directive** — on-demand VRAM visibility during execution
  - Standalone: `VRAMOUT` (full screen), `VRAMOUT FULL, ATTRS`, `VRAMOUT PARTIAL x, y, w, h`
  - Accumulates in `"vram_dumps"` JSON array (max 32 snapshots per run)
  - Non-halting — use `BREAKPOINT : VRAMOUT` for halting screen capture
- **Colon modifier on BREAKPOINT** — `BREAKPOINT : VRAMOUT [opts]` includes screen data in BREAKPOINT JSON
- **Colon modifier on ASSERT_EQ** — `ASSERT_EQ reg, val : VRAMOUT [opts]` includes screen data on assertion failure
- **PARTIAL region mode** — `VRAMOUT PARTIAL x, y, w, h` captures a rectangular sub-region; adds `"region":[x,y,w,h]` to JSON
- **ATTRS mode** — `VRAMOUT FULL, ATTRS` includes attribute bytes as hex pairs per character per row
- **CP437 → UTF-8 transcoding** — screen JSON renders box-drawing and block characters correctly for TUI inspection

### Changed
- **BREAKPOINT is inline-only** — `BREAKPOINT name [, N]` with optional name and pass count. Name appears as `"name"` in JSON. Remote label-based breakpoints removed.

### Test Results
- tests/test_vramout.asm: standalone snapshots, BREAKPOINT:VRAMOUT, PARTIAL, ATTRS all verified

---

## [0.12.1] - 2026-03-02

### Added
- **INT 21h text output → video framebuffer** — extracted `videoWriteChar()` helper from INT 10h AH=0Eh teletype; INT 21h AH=01h/02h/06h/09h/40h now write to VRAM at cursor position when `--screen` is active
- Handles CR, LF, BS, TAB, auto-scroll on bottom row

---

## [0.12.0] - 2026-03-02

### Added
- **1MB segmented addressing** — memory expanded from 64KB to 1MB; segment registers are now active (physical = seg*16 + offset, masked to 20 bits)
- **CPU heap-allocated** — `std::unique_ptr<CPU8086>` (~1MB struct, too large for stack)
- **`--screen` flag** — enable video framebuffer with mode selection (MDA, CGA40, CGA80, VGA50)
- **INT 10h video BIOS** — 11 subfunctions:
  - AH=00h set mode, AH=02h/03h cursor position, AH=06h/07h scroll up/down
  - AH=08h read char+attr, AH=09h/0Ah write char, AH=0Eh teletype, AH=0Fh get mode
- **VRAM rendering** — screen state rendered as JSON `"screen"` object at program exit: mode, cols, rows, cursor position, text lines (right-trimmed)
- **Direct VRAM access** — programs can write to B800:0000 (CGA/VGA) or B000:0000 (MDA) via segment register manipulation
- **SCREEN directive** — `SCREEN CGA80` in assembly source enables video mode (alternative to `--screen` CLI flag)

### Changed
- CPU8086 struct expanded: memory[1048576], updated offsets (OFF_MEMORY=28, OFF_PENDING=1048604, OFF_HALTED=1048608)
- `emitApplySegment()` and `emitSegAddr()` in JIT: compute 20-bit physical addresses from seg*16+offset

---

## [0.11.0] - 2026-03-02

### Added
- **Full INT 21h DOS services** — 33 subfunctions:
  - Console I/O: AH=01h (read char), 02h (print char), 06h (direct I/O), 09h (print string)
  - File system: AH=3Ch (create), 3Dh (open), 3Eh (close), 3Fh (read), 40h (write), 42h (seek), 41h (delete), 43h (get/set attrs), 56h (rename), 57h (get/set date/time)
  - FindFirst/FindNext: AH=4Eh/4Fh with DTA record formatting
  - Directory: AH=3Bh (CHDIR), 47h (CWD), 0Eh/19h (drive ops)
  - Memory management: AH=48h (alloc), 49h (free), 4Ah (resize)
  - IOCTL: AH=44h/00 (get device info), 44h/09 (is remote)
  - Stubs: DTA (1Ah/2Fh), IVT (25h/35h), date/time (2Ah/2Ch), version (30h), PSP (62h)
- **DosState class** — persistent DOS state with handle table, path resolution, Win32 FindFirst/FindNext, paragraph memory allocator

---

## [0.10.0] - 2026-03-01

### Added
- **INT 16h keyboard input** — BIOS keyboard services for interactive programs
  - AH=00h: blocking read (dequeue key, AH=scancode, AL=ASCII)
  - AH=01h: poll (peek without consuming, ZF indicates availability)
  - AH=02h: shift status (modifier byte in AL)
- **`--events` CLI flag** — inject keystrokes via JSON event descriptors
  - Inline JSON: `--events '[{"on":"read:1","keys":"Hello\r"}]'`
  - File-based: `--events events.json`
  - Triggers: `read:N` fires on Nth INT 16h AH=00h, `poll:N` fires on Nth AH=01h
- **Modifier toggles** — `\S` (Shift), `\C` (Ctrl), `\A` (Alt) in key strings toggle modifier state
- **KeyboardBuffer class** — deque-based buffer with event-driven injection, ASCII-to-scancode mapping

### Changed
- Empty keyboard buffer on blocking read = FAILED JSON with descriptive error

---

## [0.9.0] - 2026-03-01

### Added
- **ASSERT** — compile-time assertion directive
  - `ASSERT expr` — fail assembly if expression is zero
  - `ASSERT expr1, expr2` — fail assembly if values differ
- **PRINT** — compile-time message directive
  - `PRINT "message"` / `PRINT expr` / `PRINT "message", expr`
  - Output in `"prints"` array of compile JSON
- **HEX_START / HEX_END** — capture emitted bytes between markers
  - Output in `"hex_dumps"` array of compile JSON
- **ASSERT_EQ** — runtime assertion directive (honored by `--trace`)
  - `ASSERT_EQ AX, 42` — halt if register doesn't match expected value
  - `ASSERT_EQ BYTE [addr], val` / `ASSERT_EQ WORD [addr], val` — memory checks
  - Multiple ASSERT_EQ at the same address are all checked
- **ASSERT_FAILED JSON** — `{"executed":"ASSERT_FAILED","addr":N,"assert":"...","actual":N,"expected":N,"instructions":N}` with register and memory dump on stderr

---

## [0.8.0] - 2026-03-01

### Added
- **Debug directives** — three new assembly directives that emit no machine code but control JIT debugging at runtime:
  - `TRACE_START` — begin per-instruction trace output at the current address
  - `TRACE_STOP` — end trace output at the current address
  - `BREAKPOINT` — halt execution at the current address on first hit
  - `BREAKPOINT label` — halt at a label's address on first hit
  - `BREAKPOINT label, N` — halt after N passes (stops on the (N+1)th hit)
- **Directives in `.dbg` file** — new `"directives"` JSON array alongside existing `source_map` and `symbols`, containing type/addr/count/label for each debug directive
- **Optional cycle limit** — `--run N` and `--trace N` accept an optional integer argument to override the default 100M instruction limit (e.g., `--run 500` stops after 500 instructions)
- **BREAKPOINT JSON** — new execution result shape: `{"executed":"BREAKPOINT","addr":N,"instructions":N}`

### Changed
- **`--trace` is now directive-driven** — without TRACE_START/TRACE_STOP directives, `--trace` behaves identically to `--run` (silent execution, JSON only). Trace output only appears between TRACE_START and TRACE_STOP addresses. This replaces the previous behavior of dumping every instruction.
- **Graceful degradation** — if `.dbg` file is missing when running `--trace`, execution proceeds silently (same as `--run`)

### Removed
- **`--step` mode** — interactive single-step execution has been removed. agent86 is a tool for autonomous use; directive-driven debugging via TRACE_START/TRACE_STOP/BREAKPOINT replaces the interactive workflow.
- **`helpFlagStep()`** — removed from help system; `--help step` no longer recognized

### Test Results
- stress.asm: 49/49 checks pass (no regressions)
- vm.com: 5/5 VM self-tests pass (no regressions)
- Directive tests: TRACE_START/TRACE_STOP traces only the targeted section; BREAKPOINT halts at correct address with correct JSON; BREAKPOINT with count passes N times then halts; missing .dbg degrades gracefully; `--run 500` enforces cycle limit

---

## [0.7.1] - 2026-03-01

### Fixed
- **PROC/ENDP parsing** — MASM-style `name PROC` / `name ENDP` no longer misparse
- **ENDP label redefinition** — ENDP no longer redefines the procedure label
- **JIT REX.B prefix for shift-by-CL** — shifts by CL now correctly encode extended registers
- **estimateSize disp8 handling** — displacement estimates now account for disp8 encoding

### Added
- **NOP padding safety net** — pass 2 NOP-pads all instructions when actual encoding is smaller than pass 1 estimate
- **stress.asm** — comprehensive 49-check stress test across 12 sections (expressions, EQU, DB/DW, NOT/NEG, shifts, ALU, LEA, INC/DEC, stack, MUL/DIV, local scoping, misc)

### Test Results
- 49/49 stress checks pass

---

## [0.7.0] - 2026-03-01

### Added
- **Extended expressions** — full 8-level precedence: `|`, `^`, `&`, `<<`, `>>`, `+`/`-`, `*`/`/`/`%`, unary `~`/`-`/`+`
- **Parenthesized expressions** — `(expr)` for explicit grouping
- **Expression error diagnostics** — division/modulo by zero, unmatched parentheses, unexpected tokens

---

## [0.6.0] - 2026-03-01

### Added
- **INCLUDE directive** — `INCLUDE "file.inc"` performs textual inclusion before assembly passes, enabling shared constants, macros, and subroutines across files
- **Recursive includes** — included files can themselves include other files, resolved relative to the including file's directory
- **Circular include detection** — `seen_files` set prevents infinite recursion; each file is included at most once (built-in include guard)
- **Depth limit** — 16 levels maximum to catch runaway recursion
- **Source origin tracking** — `SourceOrigin` struct maps each expanded line back to its original file and line number
- **File-aware error reporting** — errors in included files report the correct `file:line`; JSON output includes `"file"` field when the error originates from an included file
- **`--help [topic]` system** — `--help` prints concise flag overview with JSON shapes; `--help run`, `--help trace`, `--help o` give per-flag detail with working shell examples. Topics accept optional leading dashes (`--help --run` also works).
- **`manual.md`** — comprehensive assembly language and JIT reference: all directives, instructions, addressing modes, DOS emulation, JSON output field reference, and explicit limitations section

### Changed
- `AsmError` now has a `file` field (between `line` and `source`)
- `assemble()` accepts an optional `source_file` parameter for path-relative include resolution
- stderr errors show `file:line` format when a file is known

### Test Results
- `test_include.asm` + `test_inc.inc` — verifies included constants and data assemble and run correctly
- All existing tests produce identical output (no regressions)

---

## [0.5.1] - 2026-02-28

### Changed
- Structured assembler JSON — success returns `size` + `symbols` map, failure returns error array with `line`/`source`/`message`

---

## [0.5.0] - 2026-02-28

### Added
- **Per-instruction JIT emulator** — generates native x64 code at runtime to execute .COM binaries, integrated into agent86.exe via `--run`, `--trace`, and `--step` flags
- **Full 8086 opcode decoder** — decodes all 256 primary opcodes plus ModR/M, prefix handling (segment overrides, REP, LOCK), all 24 addressing modes
- **x64 code emission** — MOV, all ALU ops (ADD/ADC/SUB/SBB/AND/OR/XOR/CMP/TEST), INC/DEC/NEG/NOT, shifts/rotates (SHL/SHR/SAR/ROL/ROR/RCL/RCR by 1/CL/imm8), MUL/IMUL/DIV/IDIV (8-bit and 16-bit), all Jcc conditions, JMP/CALL/RET (near, indirect, far), LOOP/LOOPE/LOOPNE/JCXZ, PUSH/POP/PUSHA/POPA/PUSHF/POPF, string ops (MOVSB/W, STOSB/W, LODSB/W, CMPSB/W, SCASB/W) with REP prefix, LEA, XCHG, CBW, CWD, XLAT, LAHF, SAHF, flag manipulation (CLC/STC/CLD/STD/CLI/STI/CMC), BCD (DAA/DAS/AAA/AAS/AAM/AAD), LDS/LES, IN/OUT, NOP, HLT, IRET
- **Native flag capture** — x64 ALU ops produce 8086-compatible RFLAGS; `pushfq/pop` captures CF/PF/AF/ZF/SF/OF directly. INC/DEC correctly preserve CF.
- **Minimal DOS emulation** — INT 20h (exit), INT 21h AH=02 (print char), AH=09 (print '$'-terminated string), AH=4Ch (exit with return code)
- **Debug modes** — `--trace` prints instruction + register dump per step; `--step` adds interactive pause; `--run` outputs JSON `{"executed":"OK","instructions":N}`
- **CPU8086 struct** with `static_assert` on all field offsets, 64KB flat memory, loadCOM()

### Architecture
- Per-instruction JIT: decode one 8086 instruction → emit x64 into VirtualAlloc buffer → call as `void(*)(CPU8086*)` → repeat
- RCX holds CPU struct pointer throughout (Win64 ABI)
- REP string ops handled in C++ dispatch loop (one JIT iteration per element)
- BCD operations handled in C++ via pending_int negative markers

### Test Results
| Test File | Instructions | Result |
|-----------|-------------|--------|
| test_hello.com | 4 | Prints "Hello!" |
| test_loop.com | 23 | AX=0037h (correct) |
| test_addressing.com | 62 | OK |
| test_adc_sbb.com | 17 | OK |
| test_newops.com | 33 | OK |
| test_pusha.com | 3 | OK |
| test_far_jcc.com | 4 | OK |
| vm.com | 3557 | 5/5 VM self-tests pass |

---

## [0.4.0] - 2026-02-27

### Added
- **Segment register support** — MOV to/from segment registers (ES, CS, SS, DS), PUSH/POP segment registers
- **XCHG** — all forms: AX/r16 short (90+r), r8/r8, r16/r16, reg/mem, mem/reg
- **IN/OUT** — port I/O with immediate port (E4-E7) and DX port (EC-EF), both AL and AX
- **LDS/LES** — load far pointer from memory (C4, C5)
- **Indirect JMP/CALL** — via register (FF /4, FF /2) and memory operand
- **RET imm16 / RETF imm16** — near/far return with stack adjustment (C2, CA)
- **RETF** — far return (CB)
- **IRET** — interrupt return (CF)
- **INT 3 special case** — encodes as single-byte CC instead of CD 03
- **Additional shift/rotate** — RCL (/2), RCR (/3), SAR (/7), all with reg/1 and reg/CL forms, plus memory operand support for all shift/rotate ops
- **IMUL/IDIV** — signed multiply/divide on reg and memory operands
- **NOT/NEG/MUL/DIV on memory** — unary operations now support memory operand forms
- **PUSH/POP memory** — PUSH [mem] (FF /6), POP [mem] (8F /0)
- **All string operations** — MOVSW (A5), STOSW (AB), LODSB (AC), LODSW (AD), CMPSB (A6), CMPSW (A7), SCASB (AE), SCASW (AF)
- **REP prefix variants** — REPE/REPZ (F3), REPNE/REPNZ (F2)
- **Flag manipulation** — CLI (FA), STI (FB), CMC (F5), LAHF (9F), SAHF (9E), PUSHF (9C), POPF (9D)
- **BCD arithmetic** — DAA (27), DAS (2F), AAA (37), AAS (3F), AAM (D4 0A), AAD (D5 0A)
- **Miscellaneous** — HLT (F4), WAIT (9B), CBW (98), CWD (99), INTO (CE), XLAT/XLATB (D7), LOCK prefix (F0)

### Test Coverage
- `test_newops.asm` — 76 instruction encodings verified byte-perfect (105 bytes)

---

## [0.3.0] - 2025-02-27

### Added
- **All 16 conditional jump opcodes (70-7F)** — added JO (70), JNO (71), JP/JPE (7A), JNP/JPO (7B) and all aliases
- **LOOPE/LOOPZ, LOOPNE/LOOPNZ instructions** — opcodes E1, E0
- **JMP SHORT optimization** — JMP now emits `EB rel8` (2 bytes) when target is in short range, falls back to `E9 rel16` (3 bytes) for near

### Fixed
- **Conditional jump size mismatch between passes** — pass 1 estimated all Jcc at 2 bytes but pass 2 could emit 5 bytes (inverted trampoline), corrupting all downstream addresses. Now uses pessimistic 5-byte estimate in pass 1; short jumps are NOP-padded to match.
- **JO opcode collision** — was incorrectly mapped to 0x78 (JS). Now correctly 0x70.
- **LOOP/JCXZ range validation** — these are short-only (rel8) instructions. Out-of-range targets now produce an error instead of silently truncating.

---

## [0.2.0] - 2025-02-27

### Added
- **ADC/SBB instructions** — full ALU encoding for Add-with-Carry and Subtract-with-Borrow (reg/reg, reg/imm, reg/mem, mem/reg, mem/imm forms)
- **CLC, STC, STD instructions** — single-byte flag manipulation opcodes (F8, F9, FD)

### Fixed
- **`[BP]` addressing mode** — was incorrectly encoded as `mod=00 rm=110` (direct address `[disp16]`). Now correctly emits `mod=01 rm=110 disp8=0` per Intel specification. This also covers the `[BP+0]` case.

---

## [0.1.0] - 2025-02-27

Initial implementation. Two-pass 8086 16-bit assembler targeting .COM binaries.

### Core Architecture
- **Two-pass assembly engine** — Pass 1 builds symbol table and calculates sizes; Pass 2 resolves all symbols and emits machine code
- **Lexer** — tokenizes labels, mnemonics, registers, numbers (decimal, hex `0FFh`/`0xFF`, binary `1010b`), strings, character literals, operators, size keywords
- **Expression evaluator** — arithmetic on labels/constants with `+`, `-`, `*`, `/` precedence, `$` (current address), character literals (`'A'`)
- **Symbol table** — global labels, local label scoping (`.name` qualified by nearest global label), PROC/ENDP scope tracking
- **Encoder** — full ModR/M byte construction for all 8086 16-bit addressing modes
- **JSON-only output** — stdout emits `{"compiled":"OK"}` or `{"compiled":"FAILED"}`, nothing else

### Directives
- `ORG` — set origin address
- `DB` — define bytes (strings, byte expressions, mixed)
- `DW` — define words (immediates, labels, label arithmetic expressions)
- `RESB` / `RESW` — reserve uninitialized bytes/words (emitted as zeros)
- `EQU` — define constant values
- `PROC` / `ENDP` — procedure scoping for local labels

### Instructions
- **MOV** — reg/reg, reg/imm, reg/mem, mem/reg, mem/imm, reg/label
- **ALU** — ADD, SUB, AND, OR, XOR, CMP (all operand combinations)
- **TEST** — reg/reg, reg/imm, BYTE/WORD mem/imm
- **Unary** — NOT, NEG, INC, DEC (reg8, reg16, BYTE/WORD mem)
- **Multiply/Divide** — MUL, DIV (reg)
- **Shift/Rotate** — SHL, SHR, ROL, ROR (reg/1, reg/CL)
- **Stack** — PUSH, POP (reg16)
- **Control flow** — JMP near, CALL near, RET, LOOP, JCXZ, INT
- **Conditional jumps** — JZ/JE, JNZ/JNE, JB/JC, JAE/JNC, JBE, JA, JL, JGE, JLE, JG, JNS, JS (short rel8, with automatic reverse+JMP near for far targets)
- **LEA** — reg, mem
- **String ops** — CLD, REP MOVSB, REP STOSB
- **Misc** — NOP, raw bytes via DB (e.g., PUSHF/POPF as `DB 9Ch`/`DB 9Dh`)

### Addressing Modes (all 24 effective forms)
- `[BX+SI]`, `[BX+DI]`, `[BP+SI]`, `[BP+DI]`, `[SI]`, `[DI]`, `[BP]`, `[BX]`
- Each with no displacement, disp8, or disp16 variants
- `[disp16]` direct addressing
- Register direct (`mod=11`)
- Label-as-displacement (e.g., `[BX + vm_memory]`, `[BX + label + imm]`)
- BYTE / WORD size override prefixes

### Test Coverage
- `test_hello.asm` — MOV, INT, DB strings (16 bytes)
- `test_loop.asm` — XOR, MOV, ADD, LOOP with local labels (11 bytes)
- `vm.asm` — 845-line VM with PROC/ENDP, forward references, dispatch tables, expression arithmetic, all addressing modes (2181 bytes)
