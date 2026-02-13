ORG 100h

; ===================================================
;  CREEP - A Haunted House Text Adventure
;  8086 real-mode assembly for agent86
; ===================================================

    JMP main

; --- Game State ---
current_room:   DB 0
game_flags:     DB 0
; Bit 0 = candle lit      (F_CANDLE)
; Bit 1 = bookshelf moved (F_SHELF)
; Bit 2 = ghost defeated  (F_GHOST)
; Bit 3 = door unlocked   (F_DOOR)
new_room_flag:  DB 1
score:          DW 0
turn_count:     DW 0
candle_timer:   DB 0
ghost_timer:    DB 0

; Object locations (room index, FFh=inventory, FEh=gone)
obj_loc:
    DB 0    ; 0: MATCHES  - Entrance Hall
    DB 3    ; 1: CANDLE   - Kitchen
    DB 4    ; 2: KEY      - Cellar
    DB 2    ; 3: BOOK     - Library
    DB 7    ; 4: MIRROR   - Secret Room
    DB 0FEh ; 5: CRUCIFIX - Chapel (hidden behind case)
    DB 9    ; 6: NOTE     - Garden

; --- Constants ---
NUM_OBJ   EQU 7
INV_LOC   EQU 0FFh
GONE_LOC  EQU 0FEh
F_CANDLE  EQU 1
F_SHELF   EQU 2
F_GHOST   EQU 4
F_DOOR    EQU 8
F_CASE    EQU 16
NOUN_SHELF EQU 7
NOUN_DOOR  EQU 8
NOUN_CASE  EQU 9

; ===================================================
;  MAIN PROGRAM
; ===================================================
main:
    MOV DX, str_intro
    MOV AH, 09h
    INT 21h

game_loop:
    CMP BYTE [new_room_flag], 0
    JE gl_prompt
    MOV BYTE [new_room_flag], 0
    CALL do_look

gl_prompt:
    MOV DX, str_prompt
    MOV AH, 09h
    INT 21h
    CALL read_line
    CALL print_nl

    ; --- Skip empty input early ---
    MOV SI, input_buf
    CALL skip_spaces
    MOV AL, [SI]
    CMP AL, 0
    JE game_loop

    ; --- Increment turn counter ---
    INC WORD [turn_count]

    ; --- Atmospheric message every 7 turns ---
    PUSH SI
    MOV AX, [turn_count]
    XOR DX, DX
    MOV CX, 7
    DIV CX
    CMP DX, 0
    JNE gl_no_atmos
    ; AX = turn_count / 7, use mod 6 to pick message
    XOR DX, DX
    MOV CX, 6
    DIV CX
    ; DX = (turn_count/7) mod 6
    MOV BX, DX
    SHL BX, 1
    ADD BX, atmos_table
    MOV DX, [BX]
    MOV AH, 09h
    INT 21h
gl_no_atmos:
    POP SI

    ; --- Candle timer ---
    CMP BYTE [candle_timer], 0
    JE gl_no_candle_tick
    DEC BYTE [candle_timer]
    CMP BYTE [candle_timer], 10
    JE gl_candle_warn
    CMP BYTE [candle_timer], 5
    JE gl_candle_warn
    CMP BYTE [candle_timer], 1
    JE gl_candle_warn
    CMP BYTE [candle_timer], 0
    JE gl_candle_dies
    JMP gl_no_candle_tick
gl_candle_warn:
    PUSH SI
    MOV DX, str_candle_flicker
    MOV AH, 09h
    INT 21h
    POP SI
    JMP gl_no_candle_tick
gl_candle_dies:
    AND BYTE [game_flags], 0FEh
    PUSH SI
    MOV DX, str_candle_dies
    MOV AH, 09h
    INT 21h
    POP SI
    ; Death if in cellar (4) or bedroom (6)
    MOV AL, [current_room]
    CMP AL, 4
    JNE gl_not_cel_death
    JMP gl_dark_death
gl_not_cel_death:
    CMP AL, 6
    JNE gl_no_candle_tick
    JMP gl_dark_death
gl_no_candle_tick:

    ; --- Ghost timer ---
    MOV AL, [current_room]
    CMP AL, 6
    JNE gl_ghost_reset
    TEST BYTE [game_flags], F_GHOST
    JNZ gl_ghost_reset
    INC BYTE [ghost_timer]
    CMP BYTE [ghost_timer], 1
    JE gl_ghost_angry
    CMP BYTE [ghost_timer], 3
    JE gl_ghost_kill
    JMP gl_after_timers
gl_ghost_angry:
    PUSH SI
    MOV DX, str_ghost_angry
    MOV AH, 09h
    INT 21h
    POP SI
    JMP gl_after_timers
gl_ghost_kill:
    MOV DX, str_ghost_death
    MOV AH, 09h
    INT 21h
    JMP do_game_over
gl_ghost_reset:
    MOV BYTE [ghost_timer], 0
gl_after_timers:

    ; Get first char of input
    MOV SI, input_buf
    CALL skip_spaces
    MOV AL, [SI]

    ; --- Command dispatch on first character ---
    ; (all targets exceed short-jump range, use JNE+JMP trampolines)
    CMP AL, 'N'
    JNE d_not_n
    JMP cmd_north
d_not_n:
    CMP AL, 'W'
    JNE d_not_w
    JMP cmd_west
d_not_w:
    CMP AL, 'D'
    JNE d_not_d
    JMP try_d
d_not_d:
    CMP AL, 'L'
    JNE d_not_l
    JMP cmd_look_j
d_not_l:
    CMP AL, 'I'
    JNE d_not_i
    JMP cmd_inv_j
d_not_i:
    CMP AL, 'H'
    JNE d_not_h
    JMP cmd_help_j
d_not_h:
    CMP AL, 'Q'
    JNE d_not_q
    JMP cmd_quit_j
d_not_q:
    ; Commands that need disambiguation or are far away
    CMP AL, 'S'
    JNE d_not_s
    JMP try_south
d_not_s:
    CMP AL, 'E'
    JNE d_not_e
    JMP try_east
d_not_e:
    CMP AL, 'U'
    JNE d_not_u
    JMP try_up
d_not_u:
    CMP AL, 'G'
    JNE d_not_g
    JMP do_get_cmd
d_not_g:
    CMP AL, 'T'
    JNE d_not_t
    JMP do_get_cmd
d_not_t:
    CMP AL, 'P'
    JNE d_not_p
    JMP do_push_cmd
d_not_p:
    CMP AL, 'R'
    JNE d_not_r
    JMP do_exam_cmd
d_not_r:
    CMP AL, 'X'
    JNE d_not_x
    JMP do_exam_cmd
d_not_x:
    CMP AL, 'O'
    JNE d_not_o
    JMP do_open_cmd
d_not_o:
    CMP AL, 'A'
    JE d_violence
    CMP AL, 'F'
    JE d_violence
    JMP d_unknown

d_violence:
    MOV DX, str_violence
    MOV AH, 09h
    INT 21h
    JMP game_loop

d_unknown:
    MOV DX, str_huh
    MOV AH, 09h
    INT 21h
    JMP game_loop

gl_dark_death:
    MOV DX, str_dark_death
    MOV AH, 09h
    INT 21h
do_game_over:
    MOV DX, str_game_over
    MOV AH, 09h
    INT 21h
    MOV DX, str_score_pre
    MOV AH, 09h
    INT 21h
    MOV AX, [score]
    CALL print_number
    MOV DX, str_score_post
    MOV AH, 09h
    INT 21h
    MOV AX, 4C00h
    INT 21h

; --- Disambiguators (kept near directions for short jumps) ---
try_d:
    MOV AL, [SI+1]
    CMP AL, 0
    JE cmd_down
    CMP AL, ' '
    JE cmd_down
    CMP AL, 'O'
    JE cmd_down
    CMP AL, 'R'
    JNE td_not_drop
    JMP do_drop_cmd
td_not_drop:
    JMP d_unknown

try_south:
    MOV AL, [SI+1]
    CMP AL, 0
    JE cmd_south
    CMP AL, ' '
    JE cmd_south
    CMP AL, 'O'
    JE cmd_south
    CMP AL, 'C'
    JNE ts_not_score
    JMP do_score_cmd
ts_not_score:
    JMP d_unknown

try_east:
    MOV AL, [SI+1]
    CMP AL, 'X'
    JNE te_east
    JMP do_exam_cmd
te_east:
    JMP cmd_east_go

try_up:
    MOV AL, [SI+1]
    CMP AL, 'S'
    JNE tu_up
    JMP do_use_cmd
tu_up:
    JMP cmd_up_go

; --- Direction commands ---
cmd_north:
    MOV BL, 0
    JMP do_move
cmd_south:
    MOV BL, 1
    JMP do_move
cmd_east_go:
    MOV BL, 2
    JMP do_move
cmd_west:
    MOV BL, 3
    JMP do_move
cmd_up_go:
    MOV BL, 4
    JMP do_move
cmd_down:
    MOV BL, 5
    JMP do_move

; --- Simple command jumps ---
cmd_look_j:
    CALL do_look
    JMP game_loop

cmd_inv_j:
    JMP do_inv_cmd

cmd_help_j:
    MOV DX, str_help
    MOV AH, 09h
    INT 21h
    JMP game_loop

cmd_quit_j:
    MOV DX, str_quit
    MOV AH, 09h
    INT 21h
    MOV AX, 4C00h
    INT 21h

; ===================================================
;  MOVEMENT HANDLER
;  BL = direction (0=N,1=S,2=E,3=W,4=U,5=D)
; ===================================================
do_move:
    MOV AL, [current_room]

    ; Special: Room 0 going South = front door
    CMP AL, 0
    JNE mv_not_r0s
    CMP BL, 1
    JNE mv_not_r0s
    TEST BYTE [game_flags], F_DOOR
    JNZ mv_win
    MOV DX, str_door_locked
    MOV AH, 09h
    INT 21h
    JMP game_loop
mv_win:
    MOV BX, 30
    CALL add_score
    ; Choose ending based on score
    CMP WORD [score], 100
    JAE mv_win_perfect
    CMP WORD [score], 80
    JAE mv_win_good
    ; Basic escape (score < 50)
    MOV DX, str_win_basic
    MOV AH, 09h
    INT 21h
    JMP mv_win_score
mv_win_good:
    MOV DX, str_win
    MOV AH, 09h
    INT 21h
    JMP mv_win_score
mv_win_perfect:
    MOV DX, str_win_perfect
    MOV AH, 09h
    INT 21h
mv_win_score:
    MOV DX, str_score_pre
    MOV AH, 09h
    INT 21h
    MOV AX, [score]
    CALL print_number
    MOV DX, str_score_post
    MOV AH, 09h
    INT 21h
    MOV AX, 4C00h
    INT 21h

mv_not_r0s:
    ; Special: Room 2 going North = bookshelf
    CMP AL, 2
    JNE mv_not_r2n
    CMP BL, 0
    JNE mv_not_r2n
    TEST BYTE [game_flags], F_SHELF
    JNZ mv_to_secret
    MOV DX, str_shelf_blocks
    MOV AH, 09h
    INT 21h
    JMP game_loop
mv_to_secret:
    MOV AL, 7
    JMP mv_enter

mv_not_r2n:
    ; Look up exit from room table
    MOV AL, [current_room]
    XOR AH, AH
    MOV CL, 3
    SHL AX, CL
    ADD AX, room_table
    ADD AX, 2
    MOV BH, 0
    ADD AX, BX
    MOV DI, AX
    MOV AL, [DI]

    CMP AL, 0FFh
    JNE mv_check
    MOV DX, str_cant_go
    MOV AH, 09h
    INT 21h
    JMP game_loop

mv_check:
    ; Cellar (4): need lit candle
    CMP AL, 4
    JNE mv_not_cel
    TEST BYTE [game_flags], F_CANDLE
    JNZ mv_enter
    MOV DX, str_too_dark
    MOV AH, 09h
    INT 21h
    JMP game_loop

mv_not_cel:
    ; Bedroom (6): need lit candle
    CMP AL, 6
    JNE mv_enter
    TEST BYTE [game_flags], F_CANDLE
    JNZ mv_enter
    MOV DX, str_ghost_blocks
    MOV AH, 09h
    INT 21h
    JMP game_loop

mv_enter:
    MOV [current_room], AL
    MOV BYTE [new_room_flag], 1
    JMP game_loop

; ===================================================
;  DO_LOOK - Describe current room and list objects
; ===================================================
do_look:
    PUSH AX
    PUSH BX
    PUSH DX

    ; Dark corridor check: room 1 without candle
    MOV AL, [current_room]
    CMP AL, 1
    JNE look_not_dark
    TEST BYTE [game_flags], F_CANDLE
    JNZ look_not_dark
    MOV DX, str_dark_corridor
    MOV AH, 09h
    INT 21h
    JMP look_done

look_not_dark:
    MOV AL, [current_room]
    XOR AH, AH
    MOV CL, 3
    SHL AX, CL
    ADD AX, room_table
    MOV BX, AX
    MOV DX, [BX]
    MOV AH, 09h
    INT 21h

    ; Room 0: show door state
    MOV AL, [current_room]
    CMP AL, 0
    JNE look_not_r0
    TEST BYTE [game_flags], F_DOOR
    JZ look_check_more
    MOV DX, str_door_open_desc
    MOV AH, 09h
    INT 21h
    JMP look_check_more

look_not_r0:
    ; Room 1: show candle light flavor
    CMP AL, 1
    JNE look_not_r1
    TEST BYTE [game_flags], F_CANDLE
    JZ look_check_more
    MOV DX, str_corridor_lit
    MOV AH, 09h
    INT 21h
    JMP look_check_more

look_not_r1:
    ; Library (room 2): show passage if shelf moved
    CMP AL, 2
    JNE look_not_lib
    TEST BYTE [game_flags], F_SHELF
    JZ look_check_more
    MOV DX, str_passage_open
    MOV AH, 09h
    INT 21h
    JMP look_check_more

look_not_lib:
    ; Room 4: show glint only if key still here
    CMP AL, 4
    JNE look_check_more
    MOV DI, obj_loc
    CMP BYTE [DI+2], 4
    JNE look_check_more
    MOV DX, str_cellar_glint
    MOV AH, 09h
    INT 21h

look_check_more:
    ; Chapel (room 8): show case state
    MOV AL, [current_room]
    CMP AL, 8
    JNE look_not_chapel
    TEST BYTE [game_flags], F_CASE
    JZ look_objects
    MOV DX, str_case_open_desc
    MOV AH, 09h
    INT 21h
    JMP look_objects

look_not_chapel:
    ; Bedroom (room 6): show ghost state
    CMP AL, 6
    JNE look_objects
    TEST BYTE [game_flags], F_GHOST
    JNZ look_ghost_gone
    MOV DX, str_ghost_here
    MOV AH, 09h
    INT 21h
    JMP look_objects
look_ghost_gone:
    MOV DX, str_ghost_cleared
    MOV AH, 09h
    INT 21h

look_objects:
    CALL print_room_obj
look_done:
    POP DX
    POP BX
    POP AX
    RET

; ===================================================
;  PRINT_ROOM_OBJ - List visible objects in room
; ===================================================
print_room_obj: PROC
    PUSH SI
    PUSH BX
    PUSH DI
    PUSH DX
    XOR SI, SI
.check:
    CMP SI, NUM_OBJ
    JAE .done
    MOV DI, obj_loc
    ADD DI, SI
    MOV AL, [DI]
    CMP AL, [current_room]
    JNE .next
    PUSH SI
    MOV DX, str_you_see
    MOV AH, 09h
    INT 21h
    POP SI
    PUSH SI
    MOV BX, SI
    SHL BX, 1
    MOV DI, obj_names
    ADD DI, BX
    MOV DX, [DI]
    MOV AH, 09h
    INT 21h
    MOV DX, str_here
    MOV AH, 09h
    INT 21h
    POP SI
.next:
    INC SI
    JMP .check
.done:
    POP DX
    POP DI
    POP BX
    POP SI
    RET
ENDP

; ===================================================
;  DO_GET_CMD - Pick up an object
; ===================================================
do_get_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces

    MOV AL, [SI]
    CMP AL, 0
    JNE dg_parse
    MOV DX, str_get_what
    MOV AH, 09h
    INT 21h
    JMP game_loop

dg_parse:
    CALL parse_noun
    CMP AL, NUM_OBJ
    JB dg_valid
    MOV DX, str_cant_get
    MOV AH, 09h
    INT 21h
    JMP game_loop

dg_valid:
    MOV BL, AL
    XOR BH, BH
    MOV DI, obj_loc
    ADD DI, BX

    ; Already in inventory?
    CMP BYTE [DI], INV_LOC
    JNE dg_not_inv
    MOV DX, str_already_have
    MOV AH, 09h
    INT 21h
    JMP game_loop

dg_not_inv:
    ; In current room?
    MOV AL, [DI]
    CMP AL, [current_room]
    JE dg_here
    MOV DX, str_not_here
    MOV AH, 09h
    INT 21h
    JMP game_loop

dg_here:
    MOV BYTE [DI], INV_LOC
    PUSH BX
    MOV BX, 5
    CALL add_score
    MOV DX, str_pick_up
    MOV AH, 09h
    INT 21h
    POP BX
    SHL BX, 1
    MOV DI, obj_names
    ADD DI, BX
    MOV DX, [DI]
    MOV AH, 09h
    INT 21h
    MOV DX, str_period_nl
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_USE_CMD - Use an object
; ===================================================
do_use_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces

    MOV AL, [SI]
    CMP AL, 0
    JNE du_parse
    MOV DX, str_use_what
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_parse:
    CALL parse_noun
    CMP AL, NOUN_DOOR
    JNE du_not_door
    JMP du_door
du_not_door:
    CMP AL, NUM_OBJ
    JB du_valid
    MOV DX, str_cant_use
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_valid:
    MOV BL, AL
    XOR BH, BH
    MOV DI, obj_loc
    ADD DI, BX
    CMP BYTE [DI], INV_LOC
    JE du_have
    MOV DX, str_dont_have
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_have:
    CMP BL, 0
    JE du_matches
    CMP BL, 1
    JE du_candle
    CMP BL, 2
    JNE du_not_key
    JMP du_key
du_not_key:
    CMP BL, 3
    JNE du_not_book
    JMP du_book
du_not_book:
    CMP BL, 4
    JE du_mirror_go
    CMP BL, 5
    JE du_crucifix_go
    JMP du_nothing
du_mirror_go:
    JMP du_mirror
du_crucifix_go:
    JMP du_crucifix

du_matches:
    ; Need candle in inventory too
    MOV DI, obj_loc
    CMP BYTE [DI+1], INV_LOC
    JE du_light
    MOV DX, str_match_fizzle
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_light:
    OR BYTE [game_flags], F_CANDLE
    MOV BYTE [candle_timer], 60
    MOV DI, obj_loc
    MOV BYTE [DI], GONE_LOC
    MOV BX, 10
    CALL add_score
    MOV DX, str_candle_lit
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_candle:
    TEST BYTE [game_flags], F_CANDLE
    JNZ du_already_lit
    MOV DI, obj_loc
    CMP BYTE [DI], INV_LOC
    JE du_light2
    MOV DX, str_need_matches
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_light2:
    OR BYTE [game_flags], F_CANDLE
    MOV DI, obj_loc
    MOV BYTE [DI], GONE_LOC
    MOV DX, str_candle_lit
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_already_lit:
    MOV DX, str_already_lit
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_key:
    CMP BYTE [current_room], 0
    JE du_unlock
    MOV DX, str_key_where
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_unlock:
    TEST BYTE [game_flags], F_DOOR
    JNZ du_already_unlk
    OR BYTE [game_flags], F_DOOR
    MOV BX, 10
    CALL add_score
    MOV DX, str_door_unlocked
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_already_unlk:
    MOV DX, str_door_already
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_book:
    MOV DX, str_use_book
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_mirror:
    CMP BYTE [current_room], 6
    JNE du_mirror_gen
    TEST BYTE [game_flags], F_GHOST
    JNZ du_mirror_gen
    OR BYTE [game_flags], F_GHOST
    MOV BX, 15
    CALL add_score
    MOV DX, str_mirror_ghost
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_mirror_gen:
    MOV DX, str_mirror_face
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_crucifix:
    CMP BYTE [current_room], 6
    JNE du_crucifix_gen
    TEST BYTE [game_flags], F_GHOST
    JNZ du_crucifix_gen
    OR BYTE [game_flags], F_GHOST
    MOV BX, 15
    CALL add_score
    MOV DX, str_crucifix_ghost
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_crucifix_gen:
    MOV DX, str_crucifix_nothing
    MOV AH, 09h
    INT 21h
    JMP game_loop

du_door:
    CMP BYTE [current_room], 0
    JNE du_nothing
    TEST BYTE [game_flags], F_DOOR
    JNZ du_door_open
    MOV DX, str_door_locked
    MOV AH, 09h
    INT 21h
    JMP game_loop
du_door_open:
    JMP mv_win

du_nothing:
    MOV DX, str_cant_use
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_PUSH_CMD - Push something
; ===================================================
do_push_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces

    MOV AL, [SI]
    CMP AL, 0
    JNE dp_parse
    MOV DX, str_push_what
    MOV AH, 09h
    INT 21h
    JMP game_loop

dp_parse:
    CALL parse_noun
    CMP AL, NOUN_SHELF
    JE dp_shelf
    ; Also treat BOOK as bookshelf for PUSH
    CMP AL, 3
    JE dp_shelf
    CMP AL, NOUN_CASE
    JNE dp_not_case
    JMP dp_case
dp_not_case:
    MOV DX, str_push_nothing
    MOV AH, 09h
    INT 21h
    JMP game_loop

dp_shelf:
    CMP BYTE [current_room], 2
    JE dp_in_lib
    MOV DX, str_push_nothing
    MOV AH, 09h
    INT 21h
    JMP game_loop

dp_in_lib:
    TEST BYTE [game_flags], F_SHELF
    JNZ dp_already
    OR BYTE [game_flags], F_SHELF
    MOV BX, 10
    CALL add_score
    MOV DX, str_shelf_moved
    MOV AH, 09h
    INT 21h
    JMP game_loop

dp_already:
    MOV DX, str_shelf_done
    MOV AH, 09h
    INT 21h
    JMP game_loop

dp_case:
    CMP BYTE [current_room], 8
    JE dp_in_chapel
    MOV DX, str_push_nothing
    MOV AH, 09h
    INT 21h
    JMP game_loop
dp_in_chapel:
    TEST BYTE [game_flags], F_CASE
    JNZ dp_case_done
    OR BYTE [game_flags], F_CASE
    ; Reveal crucifix
    MOV BYTE [obj_loc+5], 8
    MOV DX, str_case_broken
    MOV AH, 09h
    INT 21h
    JMP game_loop
dp_case_done:
    MOV DX, str_case_already
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_EXAM_CMD - Examine an object
; ===================================================
do_exam_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces

    MOV AL, [SI]
    CMP AL, 0
    JNE de_parse
    MOV DX, str_exam_what
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_parse:
    CALL parse_noun
    CMP AL, NOUN_DOOR
    JE de_door
    CMP AL, NOUN_SHELF
    JE de_shelf
    CMP AL, NOUN_CASE
    JNE de_not_case
    JMP de_case
de_not_case:
    CMP AL, NUM_OBJ
    JB de_obj
    MOV DX, str_nothing_special
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_obj:
    MOV BL, AL
    XOR BH, BH
    MOV DI, obj_loc
    ADD DI, BX
    MOV AL, [DI]
    CMP AL, INV_LOC
    JE de_show
    CMP AL, [current_room]
    JE de_show
    MOV DX, str_not_here
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_show:
    SHL BX, 1
    MOV DI, obj_descs
    ADD DI, BX
    MOV DX, [DI]
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_door:
    CMP BYTE [current_room], 0
    JNE de_nothing
    MOV DX, str_exam_door
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_shelf:
    CMP BYTE [current_room], 2
    JNE de_nothing
    MOV DX, str_exam_shelf
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_nothing:
    MOV DX, str_nothing_special
    MOV AH, 09h
    INT 21h
    JMP game_loop

de_case:
    CMP BYTE [current_room], 8
    JNE de_nothing
    TEST BYTE [game_flags], F_CASE
    JNZ de_case_broken
    MOV DX, str_exam_case
    MOV AH, 09h
    INT 21h
    JMP game_loop
de_case_broken:
    MOV DX, str_exam_case_open
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_OPEN_CMD - Open something
; ===================================================
do_open_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces
    CALL parse_noun
    CMP AL, NOUN_DOOR
    JE do_open_door
    MOV DX, str_cant_open
    MOV AH, 09h
    INT 21h
    JMP game_loop

do_open_door:
    CMP BYTE [current_room], 0
    JNE do_open_no
    TEST BYTE [game_flags], F_DOOR
    JNZ do_open_yes
    MOV DX, str_door_locked
    MOV AH, 09h
    INT 21h
    JMP game_loop
do_open_yes:
    JMP mv_win
do_open_no:
    MOV DX, str_no_door_here
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_DROP_CMD - Drop an object
; ===================================================
do_drop_cmd:
    MOV SI, input_buf
    CALL skip_spaces
    CALL skip_word
    CALL skip_spaces

    MOV AL, [SI]
    CMP AL, 0
    JNE dd_parse
    MOV DX, str_drop_what
    MOV AH, 09h
    INT 21h
    JMP game_loop

dd_parse:
    CALL parse_noun
    CMP AL, NUM_OBJ
    JB dd_valid
    MOV DX, str_cant_drop
    MOV AH, 09h
    INT 21h
    JMP game_loop

dd_valid:
    MOV BL, AL
    XOR BH, BH
    MOV DI, obj_loc
    ADD DI, BX
    CMP BYTE [DI], INV_LOC
    JE dd_have
    MOV DX, str_dont_have
    MOV AH, 09h
    INT 21h
    JMP game_loop

dd_have:
    MOV AL, [current_room]
    MOV [DI], AL
    PUSH BX
    MOV DX, str_drop_ok
    MOV AH, 09h
    INT 21h
    POP BX
    SHL BX, 1
    MOV DI, obj_names
    ADD DI, BX
    MOV DX, [DI]
    MOV AH, 09h
    INT 21h
    MOV DX, str_period_nl
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_SCORE_CMD - Show current score
; ===================================================
do_score_cmd:
    MOV DX, str_score_pre
    MOV AH, 09h
    INT 21h
    MOV AX, [score]
    CALL print_number
    MOV DX, str_score_post
    MOV AH, 09h
    INT 21h
    JMP game_loop

; ===================================================
;  DO_INV_CMD - Show inventory
; ===================================================
do_inv_cmd: PROC
    MOV DX, str_carrying
    MOV AH, 09h
    INT 21h
    XOR SI, SI
    XOR CX, CX
.check:
    CMP SI, NUM_OBJ
    JAE .done_check
    MOV DI, obj_loc
    ADD DI, SI
    CMP BYTE [DI], INV_LOC
    JNE .next
    PUSH SI
    PUSH CX
    MOV DX, str_inv_indent
    MOV AH, 09h
    INT 21h
    POP CX
    POP SI
    PUSH SI
    PUSH CX
    MOV BX, SI
    SHL BX, 1
    MOV DI, obj_names
    ADD DI, BX
    MOV DX, [DI]
    MOV AH, 09h
    INT 21h
    CALL print_nl
    POP CX
    POP SI
    INC CX
.next:
    INC SI
    JMP .check
.done_check:
    CMP CX, 0
    JNE .has_items
    MOV DX, str_nothing
    MOV AH, 09h
    INT 21h
.has_items:
    JMP game_loop
ENDP

; ===================================================
;  UTILITY: Read line into input_buf
; ===================================================
read_line: PROC
    PUSH DI
    PUSH CX
    MOV DI, input_buf
    XOR CX, CX
.read:
    MOV AH, 01h
    INT 21h
    CMP AL, 0Dh
    JE .done
    CMP AL, 08h
    JE .bksp
    CMP CX, 39
    JAE .read
    CMP AL, 'a'
    JB .store
    CMP AL, 'z'
    JA .store
    SUB AL, 20h
.store:
    MOV [DI], AL
    INC DI
    INC CX
    JMP .read
.bksp:
    CMP CX, 0
    JE .read
    DEC DI
    DEC CX
    JMP .read
.done:
    MOV BYTE [DI], 0
    POP CX
    POP DI
    RET
ENDP

; ===================================================
;  UTILITY: Print newline
; ===================================================
print_nl:
    PUSH DX
    PUSH AX
    MOV DX, str_nl
    MOV AH, 09h
    INT 21h
    POP AX
    POP DX
    RET

; ===================================================
;  UTILITY: Print number in AX as decimal
; ===================================================
print_number: PROC
    PUSH AX
    PUSH BX
    PUSH CX
    PUSH DX
    XOR CX, CX
    MOV BX, 10
.div_loop:
    XOR DX, DX
    DIV BX
    PUSH DX
    INC CX
    CMP AX, 0
    JNE .div_loop
.print_loop:
    POP DX
    ADD DL, '0'
    MOV AH, 02h
    INT 21h
    DEC CX
    JNZ .print_loop
    POP DX
    POP CX
    POP BX
    POP AX
    RET
ENDP

; ===================================================
;  UTILITY: Add BX points to score (idempotent via DI flag check)
; ===================================================
add_score: PROC
    ADD [score], BX
    RET
ENDP

; ===================================================
;  UTILITY: Skip spaces (SI advances past spaces)
; ===================================================
skip_spaces: PROC
.lp:
    CMP BYTE [SI], ' '
    JNE .dn
    INC SI
    JMP .lp
.dn:
    RET
ENDP

; ===================================================
;  UTILITY: Skip word (SI advances to space or null)
; ===================================================
skip_word: PROC
.lp:
    MOV AL, [SI]
    CMP AL, 0
    JE .dn
    CMP AL, ' '
    JE .dn
    INC SI
    JMP .lp
.dn:
    RET
ENDP

; ===================================================
;  UTILITY: Parse noun from word at SI
;  Returns AL: 0=matches,1=candle,2=key,3=book,
;              4=mirror,5=shelf,6=door,FFh=unknown
; ===================================================
parse_noun: PROC
    MOV AL, [SI]
    CMP AL, 0
    JE .unk
    CMP AL, 'M'
    JE .chk_m
    CMP AL, 'C'
    JE .chk_c
    CMP AL, 'K'
    JE .key
    CMP AL, 'B'
    JE .chk_b
    CMP AL, 'S'
    JE .shelf
    CMP AL, 'D'
    JE .door
    CMP AL, 'N'
    JE .note
    CMP AL, 'G'
    JE .case
    JMP .unk
.chk_m:
    MOV AL, [SI+1]
    CMP AL, 'A'
    JE .match
    CMP AL, 'I'
    JE .mirror
    JMP .unk
.match:
    MOV AL, 0
    RET
.chk_c:
    ; CA=candle/case, CR=crucifix
    MOV AL, [SI+1]
    CMP AL, 'R'
    JE .crucifix
    CMP AL, 'A'
    JNE .candle
    ; CAN=candle, CAS=case
    CMP BYTE [SI+2], 'S'
    JE .case
.candle:
    MOV AL, 1
    RET
.crucifix:
    MOV AL, 5
    RET
.key:
    MOV AL, 2
    RET
.chk_b:
    CMP BYTE [SI+4], 'S'
    JE .shelf
    CMP BYTE [SI+4], 'C'
    JE .shelf
    MOV AL, 3
    RET
.mirror:
    MOV AL, 4
    RET
.note:
    MOV AL, 6
    RET
.shelf:
    MOV AL, NOUN_SHELF
    RET
.door:
    MOV AL, NOUN_DOOR
    RET
.case:
    MOV AL, NOUN_CASE
    RET
.unk:
    MOV AL, 0FFh
    RET
ENDP

; ===================================================
;  DATA: Room Table
;  8 bytes per room: desc_ptr(2), exits N,S,E,W,U,D(6)
;  FFh = no exit
; ===================================================
room_table:
    ; Room 0: Entrance Hall
    DW desc_r0
    DB 1, 0FFh, 8, 0FFh, 0FFh, 0FFh
    ; Room 1: Dark Corridor
    DW desc_r1
    DB 0FFh, 0, 2, 3, 5, 0FFh
    ; Room 2: Library
    DW desc_r2
    DB 0FFh, 0FFh, 0FFh, 1, 0FFh, 0FFh
    ; Room 3: Kitchen
    DW desc_r3
    DB 0FFh, 9, 1, 0FFh, 0FFh, 4
    ; Room 4: Cellar
    DW desc_r4
    DB 0FFh, 0FFh, 0FFh, 0FFh, 3, 0FFh
    ; Room 5: Attic
    DW desc_r5
    DB 0FFh, 0FFh, 6, 0FFh, 0FFh, 1
    ; Room 6: Master Bedroom
    DW desc_r6
    DB 0FFh, 0FFh, 0FFh, 5, 0FFh, 0FFh
    ; Room 7: Secret Room
    DW desc_r7
    DB 0FFh, 2, 0FFh, 0FFh, 0FFh, 0FFh
    ; Room 8: Chapel
    DW desc_r8
    DB 0FFh, 0FFh, 0FFh, 0, 0FFh, 0FFh
    ; Room 9: Garden
    DW desc_r9
    DB 3, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh

; ===================================================
;  DATA: Object name and description pointer tables
; ===================================================
obj_names:
    DW str_obj0, str_obj1, str_obj2, str_obj3, str_obj4
    DW str_obj5, str_obj6

obj_descs:
    DW str_desc0, str_desc1, str_desc2, str_desc3, str_desc4
    DW str_desc5, str_desc6

; ===================================================
;  DATA: All strings (single-quoted, no apostrophes)
; ===================================================
str_nl: DB 0Dh, 0Ah, '$'
str_prompt: DB 0Dh, 0Ah, '> $'
str_period_nl: DB '.', 0Dh, 0Ah, '$'
str_inv_indent: DB '  $'

str_intro:
    DB 0Dh, 0Ah
    DB '========================================', 0Dh, 0Ah
    DB '            C R E E P', 0Dh, 0Ah
    DB '    A Haunted House Text Adventure', 0Dh, 0Ah
    DB '========================================', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB 'You awaken on the floor of a crumbling', 0Dh, 0Ah
    DB 'old house. The air reeks of mold and', 0Dh, 0Ah
    DB 'decay. Shadows twist in the corners.', 0Dh, 0Ah
    DB 'You must find a way to ESCAPE!', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB 'Type HELP for commands.', 0Dh, 0Ah
    DB '$'

str_help:
    DB 0Dh, 0Ah
    DB 'Commands:', 0Dh, 0Ah
    DB '  NORTH (N)  SOUTH (S)  EAST (E)', 0Dh, 0Ah
    DB '  WEST (W)   UP (U)     DOWN (D)', 0Dh, 0Ah
    DB '  LOOK (L)    - Describe the room', 0Dh, 0Ah
    DB '  GET <item>  - Pick up an item', 0Dh, 0Ah
    DB '  DROP <item> - Put down an item', 0Dh, 0Ah
    DB '  USE <item>  - Use an item', 0Dh, 0Ah
    DB '  PUSH <obj>  - Push something', 0Dh, 0Ah
    DB '  EXAMINE <x> - Look at something', 0Dh, 0Ah
    DB '  READ <item> - Read something', 0Dh, 0Ah
    DB '  OPEN <obj>  - Open something', 0Dh, 0Ah
    DB '  INVENTORY (I) - Check your items', 0Dh, 0Ah
    DB '  SCORE       - Check your score', 0Dh, 0Ah
    DB '  HELP (H)    - This help text', 0Dh, 0Ah
    DB '  QUIT (Q)    - Give up and quit', 0Dh, 0Ah
    DB '$'

str_quit:
    DB 0Dh, 0Ah
    DB 'The house claims another victim...', 0Dh, 0Ah
    DB '$'

str_win_basic:
    DB 0Dh, 0Ah
    DB 'You stumble through the front door', 0Dh, 0Ah
    DB 'into the cold night air. The house', 0Dh, 0Ah
    DB 'looms behind you, still watching.', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB 'You escaped, but at what cost?', 0Dh, 0Ah
    DB 'Many secrets remain undiscovered.', 0Dh, 0Ah
    DB '$'

str_win:
    DB 0Dh, 0Ah
    DB 'You burst through the front door into', 0Dh, 0Ah
    DB 'the cool night air! The house groans', 0Dh, 0Ah
    DB 'behind you as if it were alive. You', 0Dh, 0Ah
    DB 'run into the darkness and never look', 0Dh, 0Ah
    DB 'back.', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB '*** CONGRATULATIONS! You escaped! ***', 0Dh, 0Ah
    DB '$'

str_win_perfect:
    DB 0Dh, 0Ah
    DB 'You stride through the front door as', 0Dh, 0Ah
    DB 'dawn breaks over the horizon. Behind', 0Dh, 0Ah
    DB 'you the house lets out a final groan', 0Dh, 0Ah
    DB 'and collapses into rubble and dust!', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB 'You have conquered every secret, faced', 0Dh, 0Ah
    DB 'every danger, and emerged victorious!', 0Dh, 0Ah
    DB 0Dh, 0Ah
    DB '*** PERFECT ESCAPE! ***', 0Dh, 0Ah
    DB '$'

desc_r0:
    DB 0Dh, 0Ah
    DB '=== ENTRANCE HALL ===', 0Dh, 0Ah
    DB 'You stand in a dusty entrance hall.', 0Dh, 0Ah
    DB 'A grand staircase leads NORTH into', 0Dh, 0Ah
    DB 'darkness. The front door is to the', 0Dh, 0Ah
    DB 'SOUTH. An archway leads EAST to a', 0Dh, 0Ah
    DB 'chapel. Cobwebs hang from the ceiling.', 0Dh, 0Ah
    DB '$'

desc_r1:
    DB 0Dh, 0Ah
    DB '=== DARK CORRIDOR ===', 0Dh, 0Ah
    DB 'A long gloomy corridor stretches', 0Dh, 0Ah
    DB 'before you. Doors lead EAST to a', 0Dh, 0Ah
    DB 'library and WEST to a kitchen.', 0Dh, 0Ah
    DB 'A staircase leads UP. The entrance', 0Dh, 0Ah
    DB 'hall is back SOUTH.', 0Dh, 0Ah
    DB '$'

desc_r2:
    DB 0Dh, 0Ah
    DB '=== LIBRARY ===', 0Dh, 0Ah
    DB 'Bookshelves line every wall from', 0Dh, 0Ah
    DB 'floor to ceiling. A large bookshelf', 0Dh, 0Ah
    DB 'on the north wall looks slightly', 0Dh, 0Ah
    DB 'out of place. The corridor is WEST.', 0Dh, 0Ah
    DB '$'

desc_r3:
    DB 0Dh, 0Ah
    DB '=== KITCHEN ===', 0Dh, 0Ah
    DB 'A decrepit kitchen. Rusted pots and', 0Dh, 0Ah
    DB 'pans hang from hooks. A wooden', 0Dh, 0Ah
    DB 'staircase leads DOWN to the cellar.', 0Dh, 0Ah
    DB 'The corridor is EAST. A door leads', 0Dh, 0Ah
    DB 'SOUTH to an overgrown garden.', 0Dh, 0Ah
    DB '$'

desc_r4:
    DB 0Dh, 0Ah
    DB '=== CELLAR ===', 0Dh, 0Ah
    DB 'A damp cold cellar. Water drips from', 0Dh, 0Ah
    DB 'the stone ceiling. It smells of earth', 0Dh, 0Ah
    DB 'and rot. Stairs lead UP.', 0Dh, 0Ah
    DB '$'

desc_r5:
    DB 0Dh, 0Ah
    DB '=== ATTIC ===', 0Dh, 0Ah
    DB 'A creaky attic crammed with old', 0Dh, 0Ah
    DB 'furniture draped in white sheets', 0Dh, 0Ah
    DB 'like silent ghosts. A heavy door', 0Dh, 0Ah
    DB 'leads EAST. Stairs lead DOWN.', 0Dh, 0Ah
    DB '$'

desc_r6:
    DB 0Dh, 0Ah
    DB '=== MASTER BEDROOM ===', 0Dh, 0Ah
    DB 'A grand bedroom with a four-poster', 0Dh, 0Ah
    DB 'bed draped in tattered curtains.', 0Dh, 0Ah
    DB 'The air is bitterly cold. The attic', 0Dh, 0Ah
    DB 'is WEST.', 0Dh, 0Ah
    DB '$'

desc_r7:
    DB 0Dh, 0Ah
    DB '=== SECRET ROOM ===', 0Dh, 0Ah
    DB 'A hidden chamber behind the bookshelf!', 0Dh, 0Ah
    DB 'Ancient symbols cover the walls. A', 0Dh, 0Ah
    DB 'strange energy crackles in the air.', 0Dh, 0Ah
    DB 'The library is SOUTH.', 0Dh, 0Ah
    DB '$'

desc_r8:
    DB 0Dh, 0Ah
    DB '=== CHAPEL ===', 0Dh, 0Ah
    DB 'A small private chapel. Dusty pews', 0Dh, 0Ah
    DB 'face a cracked stone altar. A glass', 0Dh, 0Ah
    DB 'display case sits beside the altar.', 0Dh, 0Ah
    DB 'The entrance hall is WEST.', 0Dh, 0Ah
    DB '$'

desc_r9:
    DB 0Dh, 0Ah
    DB '=== GARDEN ===', 0Dh, 0Ah
    DB 'An overgrown garden choked with dead', 0Dh, 0Ah
    DB 'vines and gnarled trees. A crumbling', 0Dh, 0Ah
    DB 'stone wall surrounds it. The moonlight', 0Dh, 0Ah
    DB 'casts eerie shadows. Kitchen is NORTH.', 0Dh, 0Ah
    DB '$'

str_passage_open:
    DB 'A dark passage leads NORTH behind the', 0Dh, 0Ah
    DB 'moved bookshelf.', 0Dh, 0Ah, '$'

str_ghost_here:
    DB 'A spectral figure hovers by the window,', 0Dh, 0Ah
    DB 'its hollow eyes fixed upon you!', 0Dh, 0Ah, '$'

str_ghost_cleared:
    DB 'The room feels strangely peaceful now.', 0Dh, 0Ah, '$'

str_obj0: DB 'some matches$'
str_obj1: DB 'a candle$'
str_obj2: DB 'a rusty key$'
str_obj3: DB 'an old book$'
str_obj4: DB 'a silver mirror$'
str_obj5: DB 'a crucifix$'
str_obj6: DB 'a crumpled note$'

str_desc0:
    DB 'A small box of matches. A few remain.', 0Dh, 0Ah, '$'
str_desc1:
    DB 'A thick white wax candle.', 0Dh, 0Ah, '$'
str_desc2:
    DB 'A large rusty iron key. It looks very', 0Dh, 0Ah
    DB 'old. What lock might it fit?', 0Dh, 0Ah, '$'
str_desc3:
    DB 'An old leather journal. One passage', 0Dh, 0Ah
    DB 'reads: "The north wall of the library', 0Dh, 0Ah
    DB 'hides a secret. PUSH and ye shall', 0Dh, 0Ah
    DB 'find what lies beyond."', 0Dh, 0Ah, '$'
str_desc4:
    DB 'An ornate silver mirror in a tarnished', 0Dh, 0Ah
    DB 'frame. Your pale frightened face stares', 0Dh, 0Ah
    DB 'back at you.', 0Dh, 0Ah, '$'
str_desc5:
    DB 'A wooden crucifix, old but well-carved.', 0Dh, 0Ah
    DB 'It feels warm to the touch.', 0Dh, 0Ah, '$'
str_desc6:
    DB 'A crumpled note in faded ink. It reads:', 0Dh, 0Ah
    DB '"Spirits fear their own reflection,', 0Dh, 0Ah
    DB 'and the symbols of faith repel them."', 0Dh, 0Ah, '$'

str_exam_door:
    DB 'A heavy oak door with a rusty iron', 0Dh, 0Ah
    DB 'lock. It leads outside.', 0Dh, 0Ah, '$'
str_exam_shelf:
    DB 'A tall oak bookshelf, heavier on one', 0Dh, 0Ah
    DB 'side. It looks like it could slide.', 0Dh, 0Ah, '$'

str_huh:
    DB 'I do not understand. Type HELP for', 0Dh, 0Ah
    DB 'commands.', 0Dh, 0Ah, '$'
str_cant_go:
    DB 'You cannot go that way.', 0Dh, 0Ah, '$'
str_door_locked:
    DB 'The front door is locked tight!', 0Dh, 0Ah, '$'
str_shelf_blocks:
    DB 'A massive bookshelf blocks the way', 0Dh, 0Ah
    DB 'north. Perhaps you could PUSH it?', 0Dh, 0Ah, '$'
str_too_dark:
    DB 'It is pitch dark down there! You', 0Dh, 0Ah
    DB 'stumble on the stairs and retreat.', 0Dh, 0Ah
    DB 'You need a light source.', 0Dh, 0Ah, '$'
str_ghost_blocks:
    DB 'An icy hand grabs you from the', 0Dh, 0Ah
    DB 'darkness and hurls you back! You', 0Dh, 0Ah
    DB 'need light to enter that room.', 0Dh, 0Ah, '$'

str_you_see:    DB 'You see $'
str_here:       DB ' here.', 0Dh, 0Ah, '$'

str_get_what:   DB 'Get what?', 0Dh, 0Ah, '$'
str_cant_get:   DB 'You cannot get that.', 0Dh, 0Ah, '$'
str_already_have: DB 'You already have that.', 0Dh, 0Ah, '$'
str_not_here:   DB 'You do not see that here.', 0Dh, 0Ah, '$'
str_pick_up:    DB 'You pick up $'

str_use_what:   DB 'Use what?', 0Dh, 0Ah, '$'
str_cant_use:   DB 'You cannot use that here.', 0Dh, 0Ah, '$'
str_dont_have:  DB 'You do not have that.', 0Dh, 0Ah, '$'

str_match_fizzle:
    DB 'You strike a match but it quickly', 0Dh, 0Ah
    DB 'burns out. You need something to', 0Dh, 0Ah
    DB 'light.', 0Dh, 0Ah, '$'
str_candle_lit:
    DB 'You strike a match and light the', 0Dh, 0Ah
    DB 'candle! Its warm glow pushes back', 0Dh, 0Ah
    DB 'the shadows.', 0Dh, 0Ah, '$'
str_need_matches:
    DB 'You need matches to light it.', 0Dh, 0Ah, '$'
str_already_lit:
    DB 'The candle is already lit.', 0Dh, 0Ah, '$'

str_key_where:
    DB 'There is nothing to use that on', 0Dh, 0Ah
    DB 'here. The front door is to the', 0Dh, 0Ah
    DB 'SOUTH from the entrance hall.', 0Dh, 0Ah, '$'
str_door_unlocked:
    DB 'You insert the rusty key into the', 0Dh, 0Ah
    DB 'lock. With a grinding screech, it', 0Dh, 0Ah
    DB 'turns! The front door is unlocked!', 0Dh, 0Ah, '$'
str_door_already:
    DB 'The door is already unlocked.', 0Dh, 0Ah, '$'

str_use_book:
    DB 'You flip through the yellowed pages.', 0Dh, 0Ah
    DB 'Try EXAMINE BOOK to read it closely.', 0Dh, 0Ah, '$'

str_mirror_ghost:
    DB 'You hold the silver mirror before the', 0Dh, 0Ah
    DB 'ghost. It sees its own reflection and', 0Dh, 0Ah
    DB 'lets out a terrible shriek! The spirit', 0Dh, 0Ah
    DB 'dissolves into wisps of cold mist and', 0Dh, 0Ah
    DB 'vanishes forever!', 0Dh, 0Ah, '$'
str_mirror_face:
    DB 'You gaze into the silver mirror. Your', 0Dh, 0Ah
    DB 'tired face stares back at you.', 0Dh, 0Ah, '$'

str_push_what:
    DB 'Push what?', 0Dh, 0Ah, '$'
str_push_nothing:
    DB 'Nothing happens.', 0Dh, 0Ah, '$'
str_shelf_moved:
    DB 'You throw your weight against the', 0Dh, 0Ah
    DB 'bookshelf. With a grinding groan,', 0Dh, 0Ah
    DB 'it slides aside revealing a dark', 0Dh, 0Ah
    DB 'passage leading NORTH!', 0Dh, 0Ah, '$'
str_shelf_done:
    DB 'The bookshelf has already been moved.', 0Dh, 0Ah, '$'

str_exam_what:
    DB 'Examine what?', 0Dh, 0Ah, '$'
str_nothing_special:
    DB 'You see nothing special.', 0Dh, 0Ah, '$'

str_carrying:
    DB 0Dh, 0Ah, 'You are carrying:', 0Dh, 0Ah, '$'
str_nothing:
    DB '  Nothing.', 0Dh, 0Ah, '$'

str_cant_open:
    DB 'You cannot open that.', 0Dh, 0Ah, '$'
str_no_door_here:
    DB 'There is no door here.', 0Dh, 0Ah, '$'

str_drop_what:  DB 'Drop what?', 0Dh, 0Ah, '$'
str_cant_drop:  DB 'You cannot drop that.', 0Dh, 0Ah, '$'
str_drop_ok:    DB 'You drop $'
str_score_pre:  DB 'Your score is $'
str_score_post: DB ' out of 110.', 0Dh, 0Ah, '$'

str_candle_flicker:
    DB 'The candle flame flickers and sputters!', 0Dh, 0Ah, '$'
str_candle_dies:
    DB 'The candle gutters and goes out! You', 0Dh, 0Ah
    DB 'are plunged into darkness!', 0Dh, 0Ah, '$'
str_dark_death:
    DB 'Something cold wraps around you in the', 0Dh, 0Ah
    DB 'darkness. You scream but no one hears.', 0Dh, 0Ah, '$'
str_game_over:
    DB 0Dh, 0Ah
    DB '*** You have perished! ***', 0Dh, 0Ah, '$'
str_ghost_angry:
    DB 'The ghost turns toward you with hollow', 0Dh, 0Ah
    DB 'burning eyes...', 0Dh, 0Ah, '$'
str_ghost_death:
    DB 'The ghost shrieks and passes through', 0Dh, 0Ah
    DB 'you! An icy wave stops your heart.', 0Dh, 0Ah, '$'

str_door_open_desc:
    DB 'The front door stands open to the', 0Dh, 0Ah
    DB 'SOUTH, revealing the night beyond.', 0Dh, 0Ah, '$'
str_corridor_lit:
    DB 'Your candle casts dancing shadows', 0Dh, 0Ah
    DB 'along the corridor walls.', 0Dh, 0Ah, '$'
str_cellar_glint:
    DB 'Something glints in a dark corner.', 0Dh, 0Ah, '$'

str_case_broken:
    DB 'You smash the glass case! Shards fly', 0Dh, 0Ah
    DB 'everywhere, revealing a wooden', 0Dh, 0Ah
    DB 'crucifix inside!', 0Dh, 0Ah, '$'
str_case_already:
    DB 'The case is already smashed.', 0Dh, 0Ah, '$'
str_case_open_desc:
    DB 'The shattered glass case lies open.', 0Dh, 0Ah, '$'
str_exam_case:
    DB 'A glass display case beside the altar.', 0Dh, 0Ah
    DB 'Inside you can see a wooden crucifix.', 0Dh, 0Ah
    DB 'The glass looks brittle.', 0Dh, 0Ah, '$'
str_exam_case_open:
    DB 'The shattered remains of a glass case.', 0Dh, 0Ah, '$'
str_crucifix_ghost:
    DB 'You raise the crucifix before the', 0Dh, 0Ah
    DB 'ghost! It recoils in terror, letting', 0Dh, 0Ah
    DB 'out a wail that shakes the walls!', 0Dh, 0Ah
    DB 'The spirit crumbles to dust and', 0Dh, 0Ah
    DB 'vanishes!', 0Dh, 0Ah, '$'
str_crucifix_nothing:
    DB 'You hold up the crucifix. Nothing', 0Dh, 0Ah
    DB 'happens.', 0Dh, 0Ah, '$'
str_dark_corridor:
    DB 0Dh, 0Ah
    DB '=== DARK CORRIDOR ===', 0Dh, 0Ah
    DB 'Too dark to see clearly. Openings', 0Dh, 0Ah
    DB 'in several directions.', 0Dh, 0Ah
    DB '$'

str_violence:
    DB 'Violence is not the answer here.', 0Dh, 0Ah, '$'

; --- Atmospheric messages ---
atmos_table:
    DW str_atm0, str_atm1, str_atm2, str_atm3, str_atm4, str_atm5

str_atm0: DB 'A cold draft chills your neck.', 0Dh, 0Ah, '$'
str_atm1: DB 'A distant creak echoes through the', 0Dh, 0Ah
          DB 'house.', 0Dh, 0Ah, '$'
str_atm2: DB 'The shadows seem to shift around you.', 0Dh, 0Ah, '$'
str_atm3: DB 'You hear a faint whisper behind you...', 0Dh, 0Ah, '$'
str_atm4: DB 'Something scurries inside the walls.', 0Dh, 0Ah, '$'
str_atm5: DB 'The floorboards groan beneath you.', 0Dh, 0Ah, '$'

; ===================================================
;  Uninitialized data
; ===================================================
input_buf: RESB 42
