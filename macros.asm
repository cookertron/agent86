; ============================================================
;  macros.asm — agent86 Macro System Showcase
;
;  A colorful VRAM display built entirely with macros,
;  demonstrating every macro feature in one program.
;
;  Run:    agent86 --run-source macros.asm --screen
;  Also:   agent86 --run-source macros.asm --viewport 0,0,80,25 --attrs
;
;  Features:
;    MACRO/ENDM — parameterized code templates
;    LOCAL      — unique labels per expansion
;    REPT       — repeat blocks for data generation
;    IRP        — list iteration for table building
;    &          — token concatenation for label synthesis
;    Nesting    — macros that invoke other macros
; ============================================================

ORG 100h

; ========================= MACRO LIBRARY =========================

; Initialize ES to VRAM segment B800h
VramInit MACRO
    MOV AX, 0B800h
    MOV ES, AX
    CLD
ENDM

; Print null-terminated string at (col, row) with color attribute
;   Uses the WriteStr subroutine
Print MACRO col, row, attr, msg
    MOV DI, (row) * 160 + (col) * 2
    MOV SI, msg
    MOV AH, attr
    CALL WriteStr
ENDM

; Fill a horizontal span with char + attribute
HFill MACRO col, row, count, char, attr
    MOV DI, (row) * 160 + (col) * 2
    MOV AL, char
    MOV AH, attr
    MOV CX, count
    REP STOSW
ENDM

; Fill a rectangle — LOCAL keeps the loop label unique per expansion
FillRect MACRO col, row, w, h, char, attr
    LOCAL rowloop
    MOV DI, (row) * 160 + (col) * 2
    MOV AL, char
    MOV AH, attr
    MOV DX, h
rowloop:
    MOV CX, w
    PUSH DI
    REP STOSW
    POP DI
    ADD DI, 160
    DEC DX
    JNZ rowloop
ENDM

; Clear entire 80x25 screen — nests into FillRect
ClearScreen MACRO attr
    FillRect 0, 0, 80, 25, 20h, attr
ENDM

; Draw a single-line box border — LOCAL keeps vloop unique
Box MACRO col, row, w, h, attr
    LOCAL vloop
    MOV AH, attr
    ; Top: DA C4 C4 ... BF
    MOV DI, (row) * 160 + (col) * 2
    MOV AL, 0DAh
    STOSW
    MOV AL, 0C4h
    MOV CX, (w) - 2
    REP STOSW
    MOV AL, 0BFh
    STOSW
    ; Bottom: C0 C4 C4 ... D9
    MOV DI, ((row) + (h) - 1) * 160 + (col) * 2
    MOV AL, 0C0h
    STOSW
    MOV AL, 0C4h
    MOV CX, (w) - 2
    REP STOSW
    MOV AL, 0D9h
    STOSW
    ; Vertical sides: B3 ... B3
    MOV DI, ((row) + 1) * 160 + (col) * 2
    MOV DX, (h) - 2
    MOV AL, 0B3h
vloop:
    PUSH DI
    STOSW
    ADD DI, ((w) - 2) * 2
    STOSW
    POP DI
    ADD DI, 160
    DEC DX
    JNZ vloop
ENDM

; Horizontal separator: C3 C4 C4 ... B4
HSep MACRO col, row, w, attr
    MOV DI, (row) * 160 + (col) * 2
    MOV AH, attr
    MOV AL, 0C3h
    STOSW
    MOV AL, 0C4h
    MOV CX, (w) - 2
    REP STOSW
    MOV AL, 0B4h
    STOSW
ENDM

; Four-row shading gradient — nests four HFill calls
Gradient MACRO col, row, w, attr
    HFill col, (row),     w, 0B0h, attr
    HFill col, (row) + 1, w, 0B1h, attr
    HFill col, (row) + 2, w, 0B2h, attr
    HFill col, (row) + 3, w, 0DBh, attr
ENDM

; Define a null-terminated string with a synthesized label
;   & concatenation joins "str_" with the id parameter
DefStr MACRO id, text
    str_&id:
        DB text, 0
ENDM

; ========================= CODE START =========================

    JMP main

; --------------- String data via & concatenation ---------------

DefStr title,    ' agent86 MACRO SYSTEM DEMO '
DefStr palette,  ' Color Palette (built with IRP):'
DefStr gradient, ' Shading Gradient (nested macros):'
DefStr feat,     ' MACRO  LOCAL  REPT  IRP  &  nesting'
DefStr credit,   ' Assembled entirely with macros.'
DefStr how,      ' Every box, fill, and string call above'
DefStr how2,     ' is a single macro invocation.'

; --------------- Color table built with IRP ---------------
; Each byte is a CGA attribute with matching foreground + background,
; producing a solid color block when paired with char DBh.

palette_lo:
IRP c, <00h, 11h, 22h, 33h, 44h, 55h, 66h, 77h>
    DB c
ENDM

palette_hi:
IRP c, <88h, 99h, 0AAh, 0BBh, 0CCh, 0DDh, 0EEh, 0FFh>
    DB c
ENDM

; --------------- Decorative bar via REPT ---------------

deco_bar:
REPT 46
    DB 0CDh
ENDM
DB 0

; ========================= SUBROUTINES =========================

; WriteStr — write null-terminated string to VRAM
;   In:  ES:DI = VRAM position, DS:SI = string, AH = attribute
WriteStr: PROC
.next:
    LODSB
    OR AL, AL
    JZ .done
    STOSW
    JMP .next
.done:
    RET
ENDP

; DrawPaletteRow — draw 8 colored swatches from a table
;   In:  ES:DI = VRAM position, DS:SI = 8-byte attribute table
DrawPaletteRow: PROC
    MOV BX, 8
.swatch:
    LODSB
    MOV AH, AL
    MOV AL, 0DBh
    MOV CX, 5
    REP STOSW
    ADD DI, 2
    DEC BX
    JNZ .swatch
    RET
ENDP

; ========================= MAIN PROGRAM =========================
; The payoff: the main code reads like a high-level script
; while generating a full colorful display.

main:
    VramInit

    ; Clear screen — deep blue background
    ClearScreen 17h

    ; Outer window frame
    Box 8, 1, 64, 23, 1Bh

    ; Title bar
    Print 26, 2, 1Eh, str_title

    ; Double-line decoration (REPT-generated string data)
    Print 17, 3, 1Bh, deco_bar

    ; --- Color palette section ---
    Print 10, 5, 1Fh, str_palette

    ; Two rows of 8 color swatches (IRP-generated tables)
    MOV DI, 6 * 160 + 12 * 2
    MOV SI, palette_lo
    CALL DrawPaletteRow

    MOV DI, 7 * 160 + 12 * 2
    MOV SI, palette_hi
    CALL DrawPaletteRow

    ; --- Gradient section ---
    HSep 8, 9, 64, 1Bh
    Print 10, 10, 1Fh, str_gradient

    ; Four-row gradient (Gradient macro nests four HFill calls)
    Gradient 12, 11, 48, 1Bh

    ; --- Feature summary ---
    HSep 8, 16, 64, 1Bh
    Print 10, 17, 1Fh, str_feat

    ; --- Credits ---
    HSep 8, 19, 64, 1Bh
    Print 10, 20, 1Ah, str_credit
    Print 10, 21, 1Ah, str_how
    Print 10, 22, 1Ah, str_how2

    ; Exit
    MOV AH, 4Ch
    MOV AL, 0
    INT 21h
