; Haunted House - Text Adventure Game
; formatting: flat .com binary

ORG 100h


    jmp main

; ---------------------------------------------------------------------------
; DATA
; ---------------------------------------------------------------------------




msg_welcome:    DB 'Welcome to the Haunted House!', 0Dh, 0Ah, '$'
rng_seed:       DW 1234h

spooky_table:
    DW msg_spooky_1
    DW msg_spooky_2
    DW msg_spooky_3
    DW msg_spooky_4

msg_spooky_1: DB 0Dh, 0Ah, 'You hear a floorboard creak behind you.', 0Dh, 0Ah, '$'
msg_spooky_2: DB 0Dh, 0Ah, 'A cold draft chills you to the bone.', 0Dh, 0Ah, '$'
msg_spooky_3: DB 0Dh, 0Ah, 'Did something just move in the shadows?', 0Dh, 0Ah, '$'
msg_spooky_4: DB 0Dh, 0Ah, 'You feel like you are being watched.', 0Dh, 0Ah, '$'


msg_prompt:     DB '> $'
msg_echo_prefix: DB 'You said: $'
msg_unknown:    DB 'I do not understand that command.', 0Dh, 0Ah, '$'

msg_crlf:       DB 0Dh, 0Ah, '$'

cmd_n:          DB 'N', 0
cmd_s:          DB 'S', 0
cmd_e:          DB 'E', 0
cmd_w:          DB 'W', 0
msg_no_exit:    DB 'You cannot go that way.', 0Dh, 0Ah, '$'

; Room Data
; Struct: Desc(2), N(2), S(2), E(2), W(2)
current_room:   DW room_hallway

room_hallway:
    DW msg_desc_hallway
    DW room_living
    DW 0            ; South - Locked door?
    DW room_kitchen
    DW room_library

room_living:
    DW msg_desc_living
    DW 0
    DW room_hallway
    DW 0
    DW 0

room_kitchen:
    DW msg_desc_kitchen
    DW 0
    DW 0
    DW 0
    DW room_hallway

room_library:
    DW msg_desc_library
    DW 0
    DW 0
    DW room_hallway
    DW 0

; Room Descriptions
msg_desc_hallway: DB 'You are in a spooky hallway. Exits are North, East, West.', 0Dh, 0Ah, '$'
msg_desc_living:  DB 'You are in the living room. It is cold here.', 0Dh, 0Ah, '$'
msg_desc_kitchen: DB 'You are in the kitchen. A knife rests on the table.', 0Dh, 0Ah, '$'
msg_desc_library: DB 'You are in the library. Many books are rotton.', 0Dh, 0Ah, '$'

cmd_quit:       DB 'QUIT', 0
cmd_look:       DB 'LOOK', 0
cmd_inv:        DB 'INV', 0

cmd_get:        DB 'GET', 0
cmd_drop:       DB 'DROP', 0

command_arg:    DW 0

cmd_use:        DB 'USE', 0

msg_nounlock:   DB 'You cannot use that here.', 0Dh, 0Ah, '$'
msg_unlock:     DB 'You unlock the front door! The south exit is now open.', 0Dh, 0Ah, '$'
msg_no_key:     DB 'You do not have the KEY.', 0Dh, 0Ah, '$'

room_garden:
    DW msg_desc_garden
    DW 0
    DW 0
    DW 0
    DW 0

msg_desc_garden: DB 'You have escaped the Haunted House! YOU WIN!', 0Dh, 0Ah, '$'

; ... (previous code) ...


; (Code removed)



; Item Data
; Struct: Name(2), Desc(2), Location(2)
; Location: Room Ptr, 0 (Void), FFFFh (Inventory)

items_start:
    DW item_key_name, item_key_desc, room_library
    DW item_flashlight_name, item_flashlight_desc, room_kitchen
items_end:


item_key_name:  DB 'KEY', 0
item_key_desc:  DB 'A shiny brass key.', 0Dh, 0Ah, '$'
item_flashlight_name: DB 'FLASHLIGHT', 0
item_flashlight_desc: DB 'A heavy usage flashlight.', 0Dh, 0Ah, '$'

input_buffer:   RESB 64     ; Reserve 64 bytes for input

msg_item_taken: DB 'You take the ', '$'
msg_item_dropped: DB 'You drop the ', '$'


msg_item_not_found: DB 'I don', 27h, 't see that item here.', 0Dh, 0Ah, '$'
msg_item_not_in_inv: DB 'You don', 27h, 't have that item.', 0Dh, 0Ah, '$'


msg_inv_empty:  DB 'Your inventory is empty.', 0Dh, 0Ah, '$'
msg_inv_has:    DB 'You have: ', '$'
msg_nothing_to_take: DB 'There is nothing to take here.', 0Dh, 0Ah, '$'

msg_what_to_get: DB 'What do you want to get?', 0Dh, 0Ah, '$'
msg_what_to_drop: DB 'What do you want to drop?', 0Dh, 0Ah, '$'
msg_see_item:   DB 'You see a ', '$'



; ---------------------------------------------------------------------------
; CODE
; ---------------------------------------------------------------------------

main:
    ; Initialize data segment (DS=CS in .COM)
    mov ax, cs
    mov ds, ax
    mov es, ax

    ; Print welcome message
    mov dx, msg_welcome
    call print_string
    
    ; Look initially
    call do_look_impl


game_loop:
    ; Print prompt
    mov dx, msg_prompt
    call print_string


    ; Read input
    mov dx, input_buffer
    call read_line

    ; --- Atmosphere ---
    ; Simple RNG: Increment a seed based on user input length/time (simulated by just incrementing chance)
    ; Actually, simpler: Use low byte of time system clock? Or just a counter.
    inc word [rng_seed]
    mov ax, [rng_seed]
    and ax, 0Fh     ; Mask to 0-15
    cmp ax, 0       ; 1 in 16 chance

    jmp skip_spooky
    

    ; Print random spooky message
    mov ax, [rng_seed]
    ; shr ax, 4 
    mov cx, 4
shr_loop:
    shr ax, 1
    loop shr_loop
    
    and ax, 03h     ; 0-3

    
    mov bx, ax
    shl bx, 1       ; Multiply by 2 for word offset
    mov si, spooky_table
    add si, bx
    mov dx, [si]
    call print_string

skip_spooky:

    ; Convert input to uppercase


    mov bx, input_buffer
    call to_upper

    ; --- Tokenize ---
    ; Find first space and replace with 0
    mov di, input_buffer
    mov word [command_arg], 0   ; Default no arg
    

tokenize_loop:
    mov al, [di]
    cmp al, 0
    je tokenize_done
    cmp al, ' '
    je found_space
    inc di
    jmp tokenize_loop

found_space:
    mov byte [di], 0        ; Terminate command
    inc di                  ; Point to start of arg
    ; Skip extra spaces? (optional)
    mov [command_arg], di   ; Store pointer to arg
    
tokenize_done:



    ; --- Check QUIT ---
    mov si, input_buffer
    mov di, cmd_quit
    call strcmp
    jne chk_look
    jmp exit_game

chk_look:
    mov si, input_buffer
    mov di, cmd_look
    call strcmp
    jne chk_inv
    jmp do_look

chk_inv:
    mov si, input_buffer
    mov di, cmd_inv
    call strcmp
    jne chk_get
    jmp do_inv

chk_get:
    mov si, input_buffer
    mov di, cmd_get
    call strcmp
    jne chk_drop
    jmp do_get

chk_drop:
    mov si, input_buffer
    mov di, cmd_drop
    call strcmp
    jne chk_use
    jmp do_drop

chk_use:
    mov si, input_buffer
    mov di, cmd_use
    call strcmp
    jne chk_n
    jmp do_use

chk_n:
    mov si, input_buffer
    mov di, cmd_n
    call strcmp
    jne chk_s
    jmp go_north

chk_s:
    mov si, input_buffer
    mov di, cmd_s
    call strcmp
    jne chk_e
    jmp go_south

chk_e:
    mov si, input_buffer
    mov di, cmd_e
    call strcmp
    jne chk_w
    jmp go_east

chk_w:
    mov si, input_buffer
    mov di, cmd_w
    call strcmp
    jne unknown_cmd
    jmp go_west

unknown_cmd:

    mov dx, msg_unknown
    call print_string
    jmp game_loop

go_north:
    mov bx, 2
    jmp move_player

go_south:
    mov bx, 4
    jmp move_player

go_east:
    mov bx, 6
    jmp move_player

go_west:
    mov bx, 8
    jmp move_player


; Input: BX = offset in room struct for direction (2, 4, 6, 8)
move_player:
    mov si, [current_room]
    add si, bx              ; SI = Address of exit pointer
    mov dx, [si]            ; DX = Exit room pointer
    
    cmp dx, 0
    je move_no_exit
    
    mov [current_room], dx
    call do_look_impl
    jmp game_loop

move_no_exit:
    mov dx, msg_no_exit
    call print_string
    jmp game_loop

do_look:
    call do_look_impl
    jmp game_loop



msg_pitch_black: DB 'It is pitch black. You are likely to be eaten by a grue.', 0Dh, 0Ah, '$'
msg_cant_see:   DB 'It is too dark to see anything here.', 0Dh, 0Ah, '$'

; Update do_look to show items
do_look_impl: PROC
    ; Check Darkness
    mov ax, [current_room]
    cmp ax, room_library

    jne do_look_light_ok



    ; In library, check for flashlight
    mov bx, items_start 
    ; Flashlight is second item
    add bx, 6 
    mov ax, [bx+4] ; Get location
    cmp ax, 0FFFFh ; In inventory?
    je do_look_light_ok
    
    ; Also check if flashlight is IN the room (dropped here)
    cmp ax, [current_room]
    je do_look_light_ok

    ; It is dark
    mov dx, msg_pitch_black
    call print_string
    ret

do_look_light_ok:
    mov bx, [current_room]
    mov dx, [bx]
    call print_string
    
    ; Loop through items to see what's here
    mov bx, items_start
look_item_loop:
    cmp bx, items_end
    jae look_done_items
    
    mov ax, [bx+4]          ; Get location
    cmp ax, [current_room]
    jne look_next_item
    
    ; Found item in room
    mov dx, msg_echo_prefix ; Recycle "You said: " ?? No.
    ; "You see a " ...
    mov dx, msg_see_item
    call print_string
    mov si, [bx]            ; Name pointer
    call print_string_sz
    call print_newline
    
look_next_item:
    add bx, 6               ; Struct size is 6
    jmp look_item_loop

look_done_items:
    ret
ENDP

do_get:
    mov bx, [command_arg]
    cmp bx, 0
    jne get_has_arg
    mov dx, msg_what_to_get
    call print_string
    jmp game_loop
    

get_has_arg:
    ; Check Darkness for GET
    mov ax, [current_room]
    cmp ax, room_library
    jne get_light_ok

    ; In library, check for flashlight
    mov bx, items_start 
    add bx, 6 
    mov ax, [bx+4] 
    cmp ax, 0FFFFh 
    je get_light_ok
    cmp ax, [current_room]
    je get_light_ok

    mov dx, msg_cant_see
    call print_string
    jmp game_loop

get_light_ok:
    mov bx, items_start
get_loop:
    cmp bx, items_end
    jae get_not_found
    
    mov ax, [bx+4]          ; Location
    cmp ax, [current_room]
    jne get_next
    
    ; Check name
    mov si, [command_arg]   ; User input
    mov di, [bx]            ; Item name
    call strcmp
    je get_take_it
    
get_next:

    add bx, 6
    jmp get_loop

get_take_it:
    mov word [bx+4], 0FFFFh ; Set location to Inventory
    mov dx, msg_item_taken
    call print_string
    mov si, [bx]
    call print_string_sz
    call print_newline
    jmp game_loop

get_not_found:
    mov dx, msg_item_not_found
    call print_string
    jmp game_loop

do_drop:
    mov bx, [command_arg]
    cmp bx, 0
    jne drop_has_arg
    mov dx, msg_what_to_drop
    call print_string
    jmp game_loop

drop_has_arg:
    mov bx, items_start
drop_loop:
    cmp bx, items_end
    jae drop_not_found
    
    mov ax, [bx+4]          ; Location
    cmp ax, 0FFFFh          ; Inventory?
    jne drop_next
    
    ; Check name
    mov si, [command_arg]
    mov di, [bx]
    call strcmp
    je drop_it
    
drop_next:
    add bx, 6
    jmp drop_loop

drop_it:
    mov ax, [current_room]
    mov [bx+4], ax          ; Set location to current room
    mov dx, msg_item_dropped
    call print_string
    mov si, [bx]
    call print_string_sz
    call print_newline
    jmp game_loop

drop_not_found:
    mov dx, msg_item_not_in_inv
    call print_string
    jmp game_loop

do_use:
    mov bx, [command_arg]
    cmp bx, 0
    jne use_has_arg
    mov dx, msg_unknown ; "What do you want to use?"
    call print_string
    jmp game_loop

use_has_arg:
    ; Only support USE KEY for now
    mov si, [command_arg]
    mov di, item_key_name
    call strcmp
    jne use_cant_use

    ; Check if have key
    mov bx, items_start ; Key is first item
    mov ax, [bx+4]      ; Location
    cmp ax, 0FFFFh
    jne use_no_key

    ; Check if in Hallway
    mov ax, [current_room]
    cmp ax, room_hallway
    jne use_cant_use

    ; Unlock door
    mov word [room_hallway+4], room_garden ; Set South exit
    mov dx, msg_unlock
    call print_string
    jmp game_loop

use_no_key:
    mov dx, msg_no_key
    call print_string
    jmp game_loop

use_cant_use:
    mov dx, msg_nounlock
    call print_string
    jmp game_loop

do_inv:
    mov dx, msg_inv_has
    call print_string
    call print_newline
    
    mov bx, items_start
    mov cx, 0               ; Count of items found
inv_loop:
    cmp bx, items_end
    jae inv_done
    
    mov ax, [bx+4]
    cmp ax, 0FFFFh
    jne inv_next
    
    ; Found item in inventory
    mov si, [bx]
    call print_string_sz
    call print_newline
    inc cx
    
inv_next:
    add bx, 6
    jmp inv_loop

inv_done:
    cmp cx, 0
    jne inv_exit
    mov dx, msg_inv_empty
    call print_string

inv_exit:
    jmp game_loop


exit_game:
    mov ax, 4C00h
    int 21h




; ---------------------------------------------------------------------------
; PROCEDURES
; ---------------------------------------------------------------------------

; print_string: Output $-terminated string at DS:DX
; Input: DX = pointer to string
print_string: PROC
    mov ah, 09h
    int 21h
    ret
ENDP

; print_string_sz: Output 0-terminated string at DS:SI
; Input: SI = pointer to string
print_string_sz: PROC
    push ax
    push dx
    push si

.loop:
    mov dl, [si]
    cmp dl, 0
    je .done
    
    mov ah, 02h
    int 21h
    
    inc si
    jmp .loop

.done:
    pop si
    pop dx
    pop ax
    ret
ENDP


; print_newline: Output CRLF
print_newline: PROC
    mov dx, msg_crlf
    call print_string
    ret
ENDP

; read_line: Read string from stdin into buffer at DS:DX
; Input: DX = pointer to buffer
; Output: Buffer filled with string, 0-terminated.
; Note: This is a simple implementation. Does not handle backspace yet.

; strcmp: Compare two 0-terminated strings
; Input: SI = string 1, DI = string 2
; Output: ZF=1 if equal, ZF=0 if different
strcmp: PROC
    push ax
    push si
    push di

.loop:
    mov al, [si]
    mov ah, [di]
    cmp al, ah
    jne .done       ; If different, ZF=0, exit
    
    cmp al, 0       ; If both are 0 (end of string), match!
    je .done        ; ZF=1 already set by cmp al, 0 (which is 0-0=0)
    
    inc si
    inc di
    jmp .loop

.done:
    pop di
    pop si
    pop ax
    ret
ENDP

; to_upper: Convert 0-terminated string to uppercase
; Input: BX = pointer to string
to_upper: PROC
    push ax
    push bx

.loop:
    mov al, [bx]
    cmp al, 0
    je .done

    cmp al, 'a'
    jb .next
    cmp al, 'z'
    ja .next
    
    sub al, 32      ; Convert to uppercase
    mov [bx], al

.next:
    inc bx
    jmp .loop

.done:
    pop bx
    pop ax
    ret
ENDP

; read_line: Read string from stdin into buffer at DS:DX
; Input: DX = pointer to buffer
; Output: Buffer filled with string, 0-terminated.
; Note: This is a simple implementation. Does not handle backspace yet.
read_line: PROC
    push ax
    push bx
    push cx
    push si

    mov bx, dx          ; BX = buffer pointer
    mov si, 0           ; SI = char count

.read_char:
    mov ah, 01h         ; Read char with echo
    int 21h

    cmp al, 0Dh         ; Check for CR (Enter)
    je .done

    mov [bx+si], al     ; Store char
    inc si
    jmp .read_char

.done:
    mov byte [bx+si], 0     ; Terminate with 0 for strcmp
    
    call print_newline  ; Echo newline since 01h doesn't output LF
    
    pop si
    pop cx
    pop bx
    pop ax
    ret
ENDP




