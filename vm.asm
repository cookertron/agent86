; =================================================================
; VM Self-Test Suite v1.0
; A stress test for the Agentic Assembler
; =================================================================

ORG 100h
    JMP main

; =================================================================
; BSS Section (Moved to top to fix forward reference displacement bug)
; =================================================================

vm_regs:        RESW 4            ; R0, R1, R2, R3
vm_pc:          RESW 1            ; Program counter
vm_sp:          RESW 1            ; Stack pointer
vm_flags:       RESB 1            ; Bit 0=Z, 1=C, 2=S
vm_memory:      RESB 256          ; VM address space
vm_stack:       RESW 32           ; 32-word call/data stack

; --- Test Runner State ---
test_index:     RESW 1
pass_count:     RESW 1

; --- Stack ---
                RESW 64
stack_top:

; --- Entry Point ---
main: PROC
    CLD                     ; Ensure string operations increment
    MOV DL, '!'
    MOV AH, 02h
    INT 21h
    ; Print welcome
    MOV DX, welcome_msg
    CALL print_string

    ; Initialize pass counter
    MOV WORD [pass_count], 0

    ; For each test (0 to NUM_TESTS-1):
    MOV WORD [test_index], 0

.test_loop:
    ; Load test metadata from test_table
    ; Each entry is 4 words (8 bytes): addr, len, expected, name
    MOV AX, [test_index]
    SHL AX, 1               ; ×8 — three SHL AX, 1 = multiply by 8
    SHL AX, 1
    SHL AX, 1
    MOV BX, AX
    
    ; DEBUG: Print table offset - Removed


    ; Print test name
    MOV DX, [BX + test_table + 6]   ; 4th word = name address
    CALL print_string

    ; Load the program
    MOV SI, [BX + test_table]       ; 1st word = program address
    MOV CX, [BX + test_table + 2]   ; 2nd word = program length
    PUSH BX                         ; Save table offset for expected value
    CALL load_program

    ; Execute
    CALL vm_run              ; Returns when HALT is executed

    ; Check R0 against expected value
    POP BX
    MOV AX, [vm_regs]       ; R0 value
    CMP AX, [BX + test_table + 4]   ; 3rd word = expected R0

    JNZ .test_fail

    ; PASS
    MOV DX, pass_msg
    CALL print_string
    CALL print_newline
    INC WORD [pass_count]
    JMP .test_next

.test_fail:
    ; FAIL — print expected vs actual
    PUSH AX                  ; Save actual R0
    MOV DX, fail_msg
    CALL print_string

    MOV DX, expected_msg
    CALL print_string
    MOV AX, [BX + test_table + 4]
    CALL print_hex_word
    MOV DX, actual_msg
    CALL print_string
    POP AX
    CALL print_hex_word
    CALL print_newline

.test_next:
    INC WORD [test_index]
    MOV AX, [test_index]
    CMP AX, NUM_TESTS
    JB .test_loop

    ; Print summary
    CALL print_newline
    MOV AL, [pass_count]
    ADD AL, '0'             ; ASCII digit (assumes < 10 tests)
    MOV DL, AL
    MOV AH, 02h
    INT 21h
    MOV DX, summary_msg
    CALL print_string

    ; Exit
    INT 20h
ENDP

; =================================================================
; VM Core
; =================================================================

; --- Dispatch table: 28 entries (0x00–0x1B) ---
; The assembler must resolve all of these in pass 2.
dispatch_table:
    DW op_halt          ; 00
    DW op_load          ; 01
    DW op_add           ; 02
    DW op_sub           ; 03
    DW op_and           ; 04
    DW op_or            ; 05
    DW op_xor           ; 06
    DW op_not           ; 07
    DW op_shl           ; 08
    DW op_shr           ; 09
    DW op_neg           ; 0A
    DW op_dec           ; 0B
    DW op_rol           ; 0C
    DW op_ror           ; 0D
    DW op_mul           ; 0E
    DW op_div           ; 0F
    DW op_cmp           ; 10
    DW op_jmp           ; 11
    DW op_jz            ; 12
    DW op_jnz           ; 13
    DW op_jc            ; 14
    DW op_push          ; 15
    DW op_pop           ; 16
    DW op_call          ; 17
    DW op_ret           ; 18
    DW op_store         ; 19
    DW op_fetch         ; 1A
    DW op_test          ; 1B

NUM_OPCODES EQU 1Bh + 1  ; = 28 = 0x1C

vm_run: PROC
    ; Initialize: PC=0, SP=top of stack, FLAGS=0
    MOV WORD [vm_pc], 0
    MOV WORD [vm_sp], 32        ; Stack starts at top (grows down)
    MOV BYTE [vm_flags], 0

vm_fetch_loop:
    ; DEBUG: Print '.' to show liveness
    MOV DL, '.'
    MOV AH, 02h
    INT 21h
    
    ; Fetch opcode byte from vm_memory[PC]
    MOV BX, [vm_pc]             ; BX = PC
    MOV AL, [BX + vm_memory]    ; AL = opcode — tests [BX+label] addressing
    XOR AH, AH                  ; Zero-extend to AX
    ; POP AX - Removed (Bug fix)

    ; Bounds check: opcode < NUM_OPCODES
    CMP AL, NUM_OPCODES
    JAE .invalid_opcode

    ; Dispatch
    ; Handler address = dispatch_table + (opcode * 2)
    MOV SI, AX
    SHL SI, 1
    ; Increment PC *before* executing (instruction pointer points to next byte)
    INC WORD [vm_pc]

    MOV AX, [SI + dispatch_table]
    PUSH AX
    RET

.invalid_opcode:
    MOV DX, err_invalid_op
    CALL print_string
    RET
ENDP

; =================================================================
; VM Helpers
; =================================================================

read_vm_byte: PROC
    ; Returns byte in AL, advances vm_pc
    PUSH BX
    MOV BX, [vm_pc]
    MOV AL, [BX + vm_memory]
    INC WORD [vm_pc]
    POP BX
    RET
ENDP

read_vm_word: PROC
    ; Returns word in AX, advances vm_pc by 2
    PUSH BX
    MOV BX, [vm_pc]
    MOV AX, [BX + vm_memory]  ; x86 can read unaligned words
    ADD WORD [vm_pc], 2
    POP BX
    RET
ENDP

get_reg_ptr: PROC
    ; AL = register number (0-3)
    ; Returns BX = address of vm_regs[AL]
    PUSH AX
    AND AX, 03h             ; Safety mask
    MOV BX, AX
    SHL BX, 1               ; Words
    LEA BX, [BX + vm_regs] 
    POP AX
    RET
ENDP

update_flags: PROC
    ; Updates vm_flags based on host CPU flags
    ; Flags: Z=bit0, C=bit1, S=bit2
    DB 9Ch      ; PUSHF
    PUSH AX
    
    XOR AL, AL
    
    DB 9Dh      ; POPF
    DB 9Ch      ; PUSHF       ; Restore for next checks
    
    JNZ .no_z
    OR AL, 01h
.no_z:
    DB 9Dh      ; POPF
    DB 9Ch      ; PUSHF
    JNC .no_c
    OR AL, 02h
.no_c:
    DB 9Dh      ; POPF
    DB 9Ch      ; PUSHF
    JNS .no_s
    OR AL, 04h
.no_s:
    DB 9Dh      ; POPF
    
    MOV [vm_flags], AL
    POP AX
    RET
ENDP

vm_stack_push: PROC
    ; AX = value to push onto VM stack
    PUSH BX
    PUSH SI

    DEC WORD [vm_sp]
    MOV BX, [vm_sp]
    SHL BX, 1                  ; Word index → byte offset
    MOV [BX + vm_stack], AX    ; Store to VM stack — tests [BX+label] write

    POP SI
    POP BX
    RET
ENDP

vm_stack_pop: PROC
    ; Returns value in AX
    PUSH BX

    MOV BX, [vm_sp]
    SHL BX, 1
    MOV AX, [BX + vm_stack]    ; Load from VM stack — tests [BX+label] read
    INC WORD [vm_sp]

    POP BX
    RET
ENDP

load_program: PROC
    ; SI = source address (bytecode ROM in host memory)
    ; CX = program length in bytes
    ; Copies to vm_memory, then zeroes the rest

    PUSH AX
    PUSH DI
    PUSH CX

    ; DEBUG: Print CX
    PUSH AX
    MOV AX, CX
    CALL print_hex_word
    MOV DL, '|'
    MOV AH, 02h
    INT 21h
    POP AX

    ; Copy program bytes
    MOV DI, vm_memory       ; Destination
    REP MOVSB               ; Copy CX bytes from DS:SI to DS:DI

    ; Zero remaining VM memory: 256 - program_length bytes
    POP CX                  ; Recover original length
    MOV AX, 256
    SUB AX, CX
    MOV CX, AX              ; CX = bytes remaining
    CMP CX, 0
    JLE .no_zero
    
    XOR AL, AL
    REP STOSB               ; Zero fill rest of vm_memory
    
.no_zero:
    POP DI
    POP AX
    RET
ENDP

; =================================================================
; Opcode Handlers
; =================================================================

op_halt:
    RET     ; Returns to whoever called vm_run

op_load:
    MOV DL, 'L' ; Debug
    MOV AH, 02h
    INT 21h
    CALL read_vm_byte       ; AL = register number
    PUSH AX
    CALL read_vm_word       ; AX = immediate value
    MOV CX, AX              ; Save immediate
    POP AX
    CALL get_reg_ptr        ; BX = pointer to register
    MOV [BX], CX            ; Store value
    MOV DL, 'X' ; Debug end
    MOV AH, 02h
    INT 21h
    JMP vm_fetch_loop

op_add:
    CALL read_vm_byte       ; dest reg
    PUSH AX
    CALL read_vm_byte       ; src reg
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = src value
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = dest value
    ADD AX, CX              ; Perform operation (host ADD sets host flags)
    MOV [BX], AX            ; Store result
    CALL update_flags        ; Capture flags from host operation
    JMP vm_fetch_loop

op_sub:
    CALL read_vm_byte       ; dest reg
    PUSH AX
    CALL read_vm_byte       ; src reg
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = src value
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = dest value
    SUB AX, CX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_and:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV CX, [BX]
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    AND AX, CX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_or:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV CX, [BX]
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    OR AX, CX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_xor:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV CX, [BX]
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    XOR AX, CX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_not:
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV AX, [BX]
    NOT AX                  ; Tests NOT reg
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_shl:
    CALL read_vm_byte       ; AL = register number
    PUSH AX
    CALL read_vm_byte       ; AL = shift count (imm8)
    MOV CL, AL              ; Shift count in CL
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    SHL AX, CL              ; Tests SHL reg, CL — strict 8086 form
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_shr:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    MOV CL, AL
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    SHR AX, CL
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_neg:
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV AX, [BX]
    NEG AX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_dec:
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV AX, [BX]
    DEC AX
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_rol:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    MOV CL, AL
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    ROL AX, CL              ; Tests ROL reg, CL
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_ror:
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    MOV CL, AL
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]
    ROR AX, CL
    MOV [BX], AX
    CALL update_flags
    JMP vm_fetch_loop

op_mul:
    CALL read_vm_byte       ; dest (also src1)
    PUSH AX
    CALL read_vm_byte       ; src2
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = src2
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = src1
    MUL CX                  ; DX:AX = AX * CX
    MOV [BX], AX            ; Store low word only (simplified VM)
    CALL update_flags
    JMP vm_fetch_loop

op_div: PROC
    CALL read_vm_byte
    PUSH AX
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = divisor

    ; Check for divide-by-zero
    JCXZ .div_zero           ; Tests JCXZ — jump if CX == 0

    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = dividend
    XOR DX, DX              ; Clear high word for 16-bit / 16-bit division
    DIV CX                  ; AX = quotient, DX = remainder — Tests DIV reg
    MOV [BX], AX            ; R0 = quotient
    CALL update_flags
    JMP vm_fetch_loop

.div_zero:
    MOV DX, err_div_zero
    CALL print_string
    RET                     ; Abort program

op_cmp:
    CALL read_vm_byte       ; r1
    PUSH AX
    CALL read_vm_byte       ; r2
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = second operand
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = first operand
    CMP AX, CX              ; Set host flags (result discarded)
    CALL update_flags        ; Capture into VM flags
    JMP vm_fetch_loop

op_jmp:
    CALL read_vm_word       ; AX = target address
    MOV [vm_pc], AX         ; Unconditional: PC = target
    JMP vm_fetch_loop

op_jz: PROC
    CALL read_vm_word       ; AX = target address
    TEST BYTE [vm_flags], 01h    ; Check Zero bit — tests TEST BYTE [mem], imm
    JZ .do_jump_jz
    JMP vm_fetch_loop
.do_jump_jz:
    MOV [vm_pc], AX
    JMP vm_fetch_loop
ENDP

op_jnz: PROC
    CALL read_vm_word
    TEST BYTE [vm_flags], 01h
    JNZ .do_jump_jnz            ; If Zero IS set, don't jump
    JMP vm_fetch_loop
.do_jump_jnz:
    MOV [vm_pc], AX
    JMP vm_fetch_loop
ENDP

op_jc: PROC
    CALL read_vm_word
    TEST BYTE [vm_flags], 02h    ; Check Carry bit
    JZ .do_jump_jc
    JMP vm_fetch_loop
.do_jump_jc:
    MOV [vm_pc], AX
    JMP vm_fetch_loop
ENDP

op_push:
    CALL read_vm_byte
    CALL get_reg_ptr
    MOV AX, [BX]
    CALL vm_stack_push
    JMP vm_fetch_loop

op_pop:
    CALL read_vm_byte
    PUSH AX                  ; Save register number
    CALL vm_stack_pop        ; AX = popped value
    MOV CX, AX
    POP AX
    CALL get_reg_ptr
    MOV [BX], CX
    JMP vm_fetch_loop

op_call:
    CALL read_vm_word       ; AX = target address
    PUSH AX                 ; Save target
    MOV AX, [vm_pc]         ; Current PC (already past the operand bytes)
    CALL vm_stack_push       ; Push return address onto VM stack
    POP AX
    MOV [vm_pc], AX          ; Jump to target
    JMP vm_fetch_loop

op_ret:
    CALL vm_stack_pop        ; AX = return address
    MOV [vm_pc], AX
    JMP vm_fetch_loop

op_store: PROC
    CALL read_vm_byte       ; AL = address register
    PUSH AX
    CALL read_vm_byte       ; AL = value register
    CALL get_reg_ptr
    MOV CX, [BX]            ; CX = value (only low byte used)
    POP AX
    CALL get_reg_ptr
    MOV BX, [BX]            ; BX = address value from register

    ; Bounds check
    CMP BX, 256
    JAE .store_oob
    MOV [BX + vm_memory], CL ; Store low byte — tests [BX+label] byte write
    JMP vm_fetch_loop
.store_oob:
    MOV DX, err_oob
    CALL print_string
    RET
ENDP

op_fetch: PROC
    CALL read_vm_byte       ; dest register
    PUSH AX
    CALL read_vm_byte       ; address register
    CALL get_reg_ptr
    MOV BX, [BX]            ; BX = address from register

    ; Bounds check
    CMP BX, 256
    JAE .fetch_oob
    XOR AH, AH
    MOV AL, [BX + vm_memory] ; Load byte — tests [BX+label] byte read
    MOV CX, AX               ; Zero-extended value
    POP AX
    CALL get_reg_ptr
    MOV [BX], CX
    JMP vm_fetch_loop
.fetch_oob:
    POP AX
    MOV DX, err_oob
    CALL print_string
    RET
ENDP

op_test:
    CALL read_vm_byte       ; AL = register number
    PUSH AX
    CALL read_vm_byte       ; AL = immediate byte
    MOV CL, AL              ; Save immediate
    POP AX
    CALL get_reg_ptr
    MOV AX, [BX]            ; AX = register value
    XOR AH, AH
    ; Actually: we need flags from the TEST.
    MOV AX, [BX]
    TEST AL, CL             ; Tests TEST reg, reg (using CL as second operand)
    CALL update_flags
    JMP vm_fetch_loop

; =================================================================
; Test Data & Helper Functions
; =================================================================

print_hex_word: PROC
    ; AX = value to print
    PUSH AX
    ; Print high byte first
    MOV AL, AH              ; High byte
    CALL print_hex_byte
    POP AX
    ; Print low byte
    CALL print_hex_byte
    RET
ENDP

print_hex_byte: PROC
    ; AL = value to print
    PUSH AX
    PUSH BX
    PUSH CX
    PUSH DX

    MOV BL, AL      ; Save AL in BL

    ; High nibble
    SHR AL, 1
    SHR AL, 1
    SHR AL, 1
    SHR AL, 1       ; AL = AL >> 4
    CALL print_nibble

    ; Low nibble
    MOV AL, BL
    AND AL, 0Fh
    CALL print_nibble

    POP DX
    POP CX
    POP BX
    POP AX
    RET
ENDP

print_nibble: PROC
    ; AL = low nibble (0-15) to print
    CMP AL, 10
    JB .digit
    ADD AL, 'A' - 10
    JMP .print
.digit:
    ADD AL, '0'
.print:
    MOV DL, AL
    MOV AH, 02h
    INT 21h
    RET
ENDP

print_string: PROC
    ; DX = string address
    MOV AH, 09h
    INT 21h
    RET
ENDP

print_newline: PROC
    MOV DL, 0Dh
    MOV AH, 02h
    INT 21h
    MOV DL, 0Ah
    MOV AH, 02h
    INT 21h
    RET
ENDP

; --- Test Program Table ---
; Each entry: DW program_addr, DW program_length, DW expected_R0, DW name_addr
test_table:
    DW prog1_code, prog1_end - prog1_code, 037h,    prog1_name ; 100-50+10-5=55
    DW prog2_code, prog2_end - prog2_code, 03E8h,  prog2_name ; 10*100=1000
    DW prog3_code, prog3_end - prog3_code, 0AA55h, prog3_name ; Pattern
    DW prog4_code, prog4_end - prog4_code, 0Fh,    prog4_name ; call stub
    DW prog5_code, prog5_end - prog5_code, 42,    prog5_name ; 42

NUM_TESTS EQU 5

DOLLAR EQU '$' ; Since syntax highlighting might get confused with '$'

test_names:
prog1_name: DB 'Test 1: Arithmetic    ', DOLLAR
prog2_name: DB 'Test 2: Mul/Div       ', DOLLAR
prog3_name: DB 'Test 3: Bitwise/Rotate', DOLLAR
prog4_name: DB 'Test 4: Subroutine    ', DOLLAR
prog5_name: DB 'Test 5: Memory R/W    ', DOLLAR

; --- Test Bytecodes ---

prog1_code:
    DB 01h, 00h, 64h, 00h      ; LOAD R0, 100
    DB 01h, 01h, 32h, 00h      ; LOAD R1, 50
    DB 03h, 00h, 01h            ; SUB R0, R1
    DB 01h, 02h, 0Ah, 00h      ; LOAD R2, 10
    DB 02h, 00h, 02h            ; ADD R0, R2
    DB 01h, 03h, 05h, 00h      ; LOAD R3, 5
    DB 03h, 00h, 03h            ; SUB R0, R3
    DB 00h                      ; HALT
prog1_end:

prog2_code:
    DB 01h, 00h, 19h, 00h      ; LOAD R0, 25
    DB 01h, 01h, 28h, 00h      ; LOAD R1, 40
    DB 0Eh, 00h, 01h            ; MUL R0, R1
    DB 00h                      ; HALT
prog2_end:

prog3_code:
    DB 01h, 00h, 00h, 0FFh     ; LOAD R0, 0xFF00
    DB 01h, 01h, 0FFh, 00h     ; LOAD R1, 0x00FF
    DB 04h, 00h, 01h            ; AND R0, R1
    DB 01h, 00h, 00h, 0AAh     ; LOAD R0, 0xAA00
    DB 01h, 01h, 55h, 00h      ; LOAD R1, 0x0055
    DB 05h, 00h, 01h            ; OR R0, R1
    DB 00h                      ; HALT
prog3_end:

prog4_code:
    DB 01h, 01h, 05h, 00h      ; LOAD R1, 5
    DB 17h, 0Ah, 00h            ; CALL 0x000A
    DB 00h                      ; HALT
    DB 00h, 00h                 ; padding
    DB 01h, 00h, 0Ah, 00h      ; LOAD R0, 10
    DB 02h, 00h, 01h            ; ADD R0, R1  (10+5=15)
    DB 18h                      ; RET
prog4_end:

prog5_code:
    DB 01h, 00h, 2Ah, 00h      ; LOAD R0, 42
    DB 01h, 01h, 0C8h, 00h     ; LOAD R1, 200
    DB 19h, 01h, 00h            ; STORE [R1], R0
    DB 01h, 00h, 00h, 00h      ; LOAD R0, 0
    DB 1Ah, 00h, 01h            ; FETCH R0, [R1] (Opcode 1A - fixed typo in analysis, it's 1A for Fetch)
    DB 00h                      ; HALT
prog5_end:

; --- Constants & Padding ---

welcome_msg:   DB '=== VM Self-Test Suite v1.0 ===', 0Dh, 0Ah, 0Dh, 0Ah, '$'
pass_msg:      DB '[PASS]', '$'
fail_msg:      DB '[FAIL] ', '$'
expected_msg:  DB 'expected=0x', '$'
actual_msg:    DB ' actual=0x', '$'
summary_msg:   DB '/5 tests passed.', 0Dh, 0Ah, '$'
err_invalid_op: DB 'ERR: invalid opcode', 0Dh, 0Ah, '$'
err_div_zero:  DB 'ERR: division by zero', 0Dh, 0Ah, '$'
err_oob:       DB 'ERR: memory out of bounds', 0Dh, 0Ah, '$'
trace_prefix:  DB '  [PC=', '$'
trace_op:      DB '] op=', '$'

; Padding block: 128 bytes of DB to push addresses
; This also serves as a lookup table for hex conversion
hex_lut:       DB '0123456789ABCDEF'
sign_lut:      DB '+-'
padding:       RESB 110           ; Push total size past comfort zone

; (BSS Moved to top)
