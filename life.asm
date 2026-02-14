; ============================================================
; Conway's Game of Life — agent86 x86 assembly
; 40x40 grid, direct VRAM output, double-buffered
; ============================================================
ORG 100h

; --- Grid geometry ---
GRID_W      EQU 40
GRID_H      EQU 40
GRID_SIZE   EQU GRID_W * GRID_H

; --- Simulation ---
NUM_GENS    EQU 450

; --- Display ---
ALIVE_CHAR  EQU 0DBh
DEAD_CHAR   EQU 20h
ALIVE_ATTR  EQU 0Ah
DEAD_ATTR   EQU 00h

; --- VRAM ---
VRAM_SEG    EQU 0B800h
VRAM_COLS   EQU 80
VRAM_START  EQU 5 * 160 + 20 * 2

; ============================================================
; Phase 1: Initialization
; ============================================================
    ; Set 80x25 text mode (mode 3)
    MOV AX, 0003h       ; AH=00 (set video mode), AL=03 (80x25 text)
    INT 10h

    ; Load 8x8 font to get 80x50 character cells on VGA
    MOV AX, 1112h       ; AH=11h (character generator), AL=12h (load 8x8 font)
    XOR BL, BL          ; BL=0 (load block 0)
    INT 10h

    ; Clear Buffer A (ES=DS at .COM startup)
    MOV DI, buf_a
    MOV CX, GRID_SIZE
    XOR AL, AL
    CLD
    REP STOSB

    ; Clear Buffer B
    MOV DI, buf_b
    MOV CX, GRID_SIZE
    REP STOSB

    ; Stamp seed pattern into Buffer A
    MOV SI, pattern_rpentomino
    MOV BX, buf_a
    CALL stamp_pattern

    ; Set ES = VRAM for rest of program
    MOV AX, VRAM_SEG
    MOV ES, AX

; ============================================================
; Main generation loop
; ============================================================
gen_loop:
    CALL display
    CALL compute

    ; Swap buffer pointers
    MOV AX, [ptr_cur]
    XCHG AX, [ptr_nxt]
    MOV [ptr_cur], AX

    ; Increment and check termination
    INC WORD [gen_count]
    MOV AX, [gen_count]
    CMP AX, NUM_GENS
    JB gen_loop

; ============================================================
; Phase 5: Exit
; ============================================================
    CALL display
    MOV AH, 09h
    MOV DX, msg_done
    INT 21h
    INT 20h

; ============================================================
; Data
; ============================================================
ptr_cur:    DW buf_a
ptr_nxt:    DW buf_b
gen_count:  DW 0
msg_done:   DB 'Life complete', 0Dh, 0Ah, '$'

; ============================================================
; stamp_pattern: SI=pattern data, BX=buffer base
; ============================================================
stamp_pattern: PROC
.loop:
    LODSB
    CMP AL, 0FFh
    JE .done
    XOR AH, AH
    MOV DX, GRID_W
    MUL DX
    MOV DI, AX
    LODSB
    XOR AH, AH
    ADD DI, AX
    MOV BYTE [BX + DI], 1
    JMP .loop
.done:
    RET
ENDP

; ============================================================
; display: Read ptr_cur buffer → write to VRAM (ES:DI)
; ============================================================
display: PROC
    MOV SI, [ptr_cur]
    MOV DI, VRAM_START
    MOV DX, GRID_H
.row:
    MOV CX, GRID_W
.cell:
    LODSB
    TEST AL, AL
    JZ .dead
    MOV AX, ALIVE_ATTR * 256 + ALIVE_CHAR
    JMP .write
.dead:
    MOV AX, DEAD_ATTR * 256 + DEAD_CHAR
.write:
    STOSW
    LOOP .cell
    ADD DI, (VRAM_COLS - GRID_W) * 2
    DEC DX
    JNZ .row
    RET
ENDP

; ============================================================
; compute: Conway's rules, ptr_cur → ptr_nxt
; ============================================================
compute: PROC
    MOV BX, [ptr_cur]
    MOV BP, [ptr_nxt]
    MOV SI, BX
    ADD SI, GRID_W + 1
    MOV DI, BP
    ADD DI, GRID_W + 1
    MOV DH, GRID_H - 2
.row:
    MOV CX, GRID_W - 2
.cell:
    ; Count 8 neighbors
    XOR DL, DL
    MOV AL, [SI - GRID_W - 1]
    ADD DL, AL
    MOV AL, [SI - GRID_W]
    ADD DL, AL
    MOV AL, [SI - GRID_W + 1]
    ADD DL, AL
    MOV AL, [SI - 1]
    ADD DL, AL
    MOV AL, [SI + 1]
    ADD DL, AL
    MOV AL, [SI + GRID_W - 1]
    ADD DL, AL
    MOV AL, [SI + GRID_W]
    ADD DL, AL
    MOV AL, [SI + GRID_W + 1]
    ADD DL, AL

    ; Apply Conway's rules
    CMP DL, 3
    JE .alive
    CMP DL, 2
    JNE .dead
    CMP BYTE [SI], 1
    JE .alive
.dead:
    MOV BYTE [DI], 0
    JMP .next
.alive:
    MOV BYTE [DI], 1
.next:
    INC SI
    INC DI
    DEC CX
    JNZ .cell
    ADD SI, 2
    ADD DI, 2
    DEC DH
    JNZ .row
    RET
ENDP

; ============================================================
; Seed patterns (row, col pairs, FFh terminated)
; ============================================================
pattern_blinker:
    DB 19, 20, 20, 20, 21, 20
    DB 0FFh

pattern_glider:
    DB 2, 3
    DB 3, 4
    DB 4, 2, 4, 3, 4, 4
    DB 0FFh

pattern_rpentomino:
    DB 19, 21, 19, 22
    DB 20, 20, 20, 21
    DB 21, 21
    DB 0FFh

pattern_lwss:
    DB 18, 4, 18, 7
    DB 19, 3
    DB 20, 3, 20, 7
    DB 21, 3, 21, 4, 21, 5, 21, 6
    DB 0FFh

pattern_acorn:
    DB 18, 19
    DB 19, 21
    DB 20, 18, 20, 19, 20, 22, 20, 23, 20, 24
    DB 0FFh

; ============================================================
; Buffers (zero-initialized by RESB)
; ============================================================
buf_a:  RESB GRID_SIZE
buf_b:  RESB GRID_SIZE
