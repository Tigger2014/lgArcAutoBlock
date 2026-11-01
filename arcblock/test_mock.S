/* test_mock.S — mock Thumb-2 targets exercising every engine rewrite path.
 * Each window is exactly 8 bytes (verified by the test passing: the engine
 * grows the window to >= 8 on an insn boundary, so any overrun would refuse).
 * Assembled with the real gas: the literal/BL encodings are ground truth.
 */
    .syntax unified
    .thumb
    .text

    .global helper_add_one
    .thumb_func
helper_add_one:
    adds r0, r0, #1
    bx lr

    .align 2
    .global mock_add
    .thumb_func
mock_add:                       @ int (int a, int b) = a + b + 100
    push {r4, lr}               @ 0-1
    ldr r4, .Lval_add           @ 2-3  16-bit LDR literal in window
    adds r0, r0, r1             @ 4-5
    adds r0, r0, r4             @ 6-7
    pop {r4, pc}                @ 8-9
.Lval_add: .word 100

    .align 2
    .global mock_branch
    .thumb_func
mock_branch:                    @ int (int x) = x ? 1 : 0
    push {lr}                   @ 0-1
    cmp r0, #0                  @ 2-3
    beq .Lbr_zero               @ 4-5  16-bit Bcc in window
    movs r0, #1                 @ 6-7
    pop {pc}                    @ 8-9
.Lbr_zero:
    movs r0, #0
    pop {pc}

    .align 2
    .global mock_bl
    .thumb_func
mock_bl:                        @ int (int x) = helper_add_one(x) + 1
    push {r4, lr}               @ 0-1
    bl helper_add_one           @ 2-5  32-bit BL in window
    adds r0, r0, #1             @ 6-7
    pop {r4, pc}                @ 8-9

    .align 2
    .global mock_litw
    .thumb_func
mock_litw:                      @ int (void) = 0x1234
    push {r4, lr}               @ 0-1
    ldr.w r4, .Lval_w           @ 2-5  32-bit LDR literal (T3 or T4: gas decides)
    mov r0, r4                  @ 6-7
    pop {r4, pc}                @ 8-9
.Lval_w: .word 0x1234

    .align 2
    .global mock_adr
    .thumb_func
mock_adr:                       @ int (void) = 0x5678
    adr r0, .Ltab_adr           @ 0-1  ADR in window
    ldr r0, [r0]                @ 2-3
    bx lr                       @ 4-5
    nop                         @ 6-7  pad window to 8 (never executes)
.Ltab_adr: .word 0x5678

    .align 2
    .global mock_lit16
    .thumb_func
mock_lit16:                 @ int (int a) = a + 0xA5 via 16-bit LDR literal
    push {r3, lr}           @ 0-1
    ldr.n r3, .Lp16         @ 2-3  T1 LDR literal in window (base=4, pool=8)
    adds r0, r0, r3         @ 4-5
    pop {r3, pc}            @ 6-7
.Lp16: .word 0xA5           @ 8-11

    .align 2
    .global mock_refuse
    .thumb_func
mock_refuse:                    @ CBZ in window: engine must REFUSE, fn untouched
    push {lr}                   @ 0-1
    cbz r0, .Lrz                @ 2-3
    movs r0, #7                 @ 4-5
    pop {pc}                    @ 6-7
.Lrz:
    movs r0, #0
    pop {pc}
