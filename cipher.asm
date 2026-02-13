; ============================================================
; Substitution Cipher Engine with Self-Test
; ============================================================
; Builds a 256-byte cipher table, encrypts a message, builds
; the inverse table, decrypts, and verifies the round-trip.
; ============================================================

ORG 100h

    JMP main

; ============================================================
; Data Section
; ============================================================

; "Hello, World! This is a cipher test." = 36 bytes
PLAINLEN EQU 36

plaintext:   DB 'Hello, World! This is a cipher test.'
ciphertext:  RESB PLAINLEN           ; encrypted output buffer
decrypted:   RESB PLAINLEN           ; decrypted output buffer

cipher_tab:  RESB 256                ; substitution table
decipher_tab: RESB 256               ; inverse substitution table

hex_chars:   DB '0123456789ABCDEF'

msg_orig:    DB 'Original:  $'
msg_enc:     DB 'Encrypted: $'
msg_dec:     DB 'Decrypted: $'
msg_pass:    DB 0Dh, 0Ah, 'PASS: Decrypt matches original', 0Dh, 0Ah, '$'
msg_fail:    DB 0Dh, 0Ah, 'FAIL at offset $'
msg_newline: DB 0Dh, 0Ah, '$'

; ============================================================
; main — Entry point
; ============================================================
main:
    ; Step 1: Build the cipher and inverse tables
    CALL build_cipher_table
    CALL build_inverse_table

    ; Step 2: Encrypt the plaintext
    CALL encrypt_message

    ; Step 3: Decrypt the ciphertext
    CALL decrypt_message

    ; Step 4: Print original plaintext
    MOV AH, 09h
    MOV DX, msg_orig
    INT 21h
    MOV SI, plaintext
    MOV CX, PLAINLEN
    CALL print_buffer
    MOV AH, 09h
    MOV DX, msg_newline
    INT 21h

    ; Step 5: Print ciphertext as hex pairs
    MOV AH, 09h
    MOV DX, msg_enc
    INT 21h
    CALL print_cipher_hex
    MOV AH, 09h
    MOV DX, msg_newline
    INT 21h

    ; Step 6: Print decrypted text
    MOV AH, 09h
    MOV DX, msg_dec
    INT 21h
    MOV SI, decrypted
    MOV CX, PLAINLEN
    CALL print_buffer
    MOV AH, 09h
    MOV DX, msg_newline
    INT 21h

    ; Step 7: Verify decrypted matches original
    CALL verify_match

    ; Terminate cleanly via INT 21h/4Ch
    MOV AX, 4C00h
    INT 21h

; ============================================================
; build_cipher_table PROC
; Populates cipher_tab[i] = ((i * 7) + 13) XOR 0xAA
; Multiplication by 7 uses SHL + ADD:
;   i*2 -> +i = i*3 -> *2 = i*6 -> +i = i*7
; ============================================================
build_cipher_table: PROC
    PUSHA
    XOR SI, SI              ; i = 0
    MOV DI, cipher_tab      ; destination pointer

.loop:
    MOV AX, SI              ; AX = i (16-bit for overflow safety)
    MOV DX, AX              ; DX = i (save original for adds)
    SHL AX, 1               ; AX = i * 2
    ADD AX, DX              ; AX = i * 3
    SHL AX, 1               ; AX = i * 6
    ADD AX, DX              ; AX = i * 7
    ADD AX, 13              ; AX = (i * 7) + 13
    XOR AL, 0AAh            ; AL = ((i*7)+13) XOR 0xAA  (byte result)
    MOV [DI], AL            ; cipher_tab[i] = AL
    INC DI
    INC SI
    CMP SI, 256
    JNE .loop

    POPA
    RET
ENDP

; ============================================================
; build_inverse_table PROC
; Builds decipher_tab so that decipher_tab[cipher_tab[i]] = i
; Uses XCHG to write values at computed offsets
; ============================================================
build_inverse_table: PROC
    PUSHA
    XOR CX, CX              ; i = 0 (CL = low byte = i)

.loop:
    ; Read cipher_tab[i]
    MOV BX, cipher_tab
    ADD BX, CX              ; BX -> cipher_tab[i]
    MOV AL, [BX]            ; AL = cipher_tab[i]

    ; Compute &decipher_tab[ cipher_tab[i] ]
    XOR AH, AH              ; zero-extend AL into AX
    MOV BX, decipher_tab
    ADD BX, AX              ; BX -> decipher_tab[cipher_tab[i]]

    ; Store i at that location using XCHG (swaps AL with [BX])
    MOV AL, CL              ; AL = i (low byte of counter)
    XCHG AL, [BX]           ; decipher_tab[cipher_tab[i]] = i

    INC CX
    CMP CX, 256
    JNE .loop

    POPA
    RET
ENDP

; ============================================================
; encrypt_message PROC
; Encrypts plaintext -> ciphertext via XLAT lookup
; ============================================================
encrypt_message: PROC
    PUSHA
    CLD                      ; forward direction for LODSB/STOSB
    MOV SI, plaintext
    MOV DI, ciphertext
    MOV BX, cipher_tab       ; XLAT table base
    MOV CX, PLAINLEN

.loop:
    LODSB                    ; AL = DS:[SI], SI++
    XLAT                     ; AL = cipher_tab[AL]  (encrypt)
    STOSB                    ; ES:[DI] = AL, DI++
    LOOP .loop

    POPA
    RET
ENDP

; ============================================================
; decrypt_message PROC
; Decrypts ciphertext -> decrypted via XLAT lookup
; ============================================================
decrypt_message: PROC
    PUSHA
    CLD
    MOV SI, ciphertext
    MOV DI, decrypted
    MOV BX, decipher_tab     ; XLAT inverse table base
    MOV CX, PLAINLEN

.loop:
    LODSB                    ; AL = DS:[SI], SI++
    XLAT                     ; AL = decipher_tab[AL]  (decrypt)
    STOSB                    ; ES:[DI] = AL, DI++
    LOOP .loop

    POPA
    RET
ENDP

; ============================================================
; verify_match PROC
; Compares decrypted buffer against original plaintext.
; Uses REPE CMPSB for comparison.
; Uses PUSHF/POPF to preserve comparison flags across the
; offset calculation (SUB/DEC modify flags).
; ============================================================
verify_match: PROC
    PUSHA
    CLD                      ; forward direction for CMPSB
    MOV SI, plaintext
    MOV DI, decrypted
    MOV CX, PLAINLEN

    REPE CMPSB               ; compare CX bytes: DS:[SI] vs ES:[DI]

    ; --- PUSHF: preserve ZF from CMPSB across arithmetic ---
    PUSHF

    ; Pre-compute mismatch offset (SUB and DEC clobber flags)
    MOV AX, SI
    SUB AX, plaintext
    DEC AX                   ; AX = byte offset of first mismatch
    MOV BP, AX               ; stash in BP

    ; --- POPF: restore the CMPSB flags so JNE works correctly ---
    POPF
    JNE .mismatch

    ; All bytes matched
    MOV AH, 09h
    MOV DX, msg_pass
    INT 21h
    JMP .done

.mismatch:
    ; Print "FAIL at offset XX"
    MOV AH, 09h
    MOV DX, msg_fail
    INT 21h
    MOV AX, BP              ; recover offset
    CALL print_hex_byte      ; print low byte as two hex digits
    MOV AH, 09h
    MOV DX, msg_newline
    INT 21h

.done:
    POPA
    RET
ENDP

; ============================================================
; print_buffer PROC
; Prints CX bytes starting at DS:SI using INT 21h/02h
; ============================================================
print_buffer: PROC
    PUSHA

.loop:
    MOV DL, [SI]
    MOV AH, 02h
    INT 21h
    INC SI
    LOOP .loop

    POPA
    RET
ENDP

; ============================================================
; print_cipher_hex PROC
; Prints the ciphertext buffer as space-separated hex pairs
; ============================================================
print_cipher_hex: PROC
    PUSHA
    MOV SI, ciphertext
    MOV CX, PLAINLEN

.loop:
    MOV AL, [SI]
    CALL print_hex_byte      ; print byte as two hex digits
    MOV DL, ' '
    MOV AH, 02h
    INT 21h                  ; print space separator
    INC SI
    LOOP .loop

    POPA
    RET
ENDP

; ============================================================
; print_hex_byte PROC
; Prints AL as two hexadecimal digits.
; Uses SAR (arithmetic shift right) to extract the high nibble.
; SAR sign-extends, so we AND 0Fh to clean up — this is a
; deliberate choice to demonstrate SAR on potentially signed data.
; ============================================================
print_hex_byte: PROC
    PUSHA
    MOV DH, AL              ; save original byte in DH

    ; --- High nibble via SAR ---
    MOV CL, 4
    SAR AL, CL              ; arithmetic shift right by 4
    AND AL, 0Fh             ; mask off sign-extended bits
    MOV BX, hex_chars
    XLAT                    ; AL = hex digit char
    MOV DL, AL
    MOV AH, 02h
    INT 21h                 ; print high hex digit

    ; --- Low nibble ---
    MOV AL, DH              ; restore original byte
    AND AL, 0Fh             ; isolate low nibble
    MOV BX, hex_chars
    XLAT                    ; AL = hex digit char
    MOV DL, AL
    MOV AH, 02h
    INT 21h                 ; print low hex digit

    POPA
    RET
ENDP
