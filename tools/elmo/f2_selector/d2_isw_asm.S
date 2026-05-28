.syntax unified
.thumb

.macro D2_ISW_LANE base
    ldr r0, [r4, #(\base + 0)]
    ldr r1, [r5, #(\base + 0)]
    ands r0, r1
    movs r2, #1
    mov r1, r6
    ands r1, r2
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #1
    ands r1, r2
    eors r0, r1
    ands r0, r2
    mov r0, r6
    lsrs r0, r0, #3

    ldr r0, [r4, #(\base + 4)]
    ldr r1, [r5, #(\base + 4)]
    ands r0, r1
    ldr r1, [r4, #(\base + 0)]
    ldr r3, [r5, #(\base + 4)]
    ands r1, r3
    eors r0, r1
    ldr r1, [r4, #(\base + 4)]
    ldr r3, [r5, #(\base + 0)]
    ands r1, r3
    eors r0, r1
    mov r1, r6
    ands r1, r2
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #2
    ands r1, r2
    eors r0, r1
    ands r0, r2
    mov r0, r6
    lsrs r0, r0, #4

    ldr r0, [r4, #(\base + 8)]
    ldr r1, [r5, #(\base + 8)]
    ands r0, r1
    ldr r1, [r4, #(\base + 0)]
    ldr r3, [r5, #(\base + 8)]
    ands r1, r3
    eors r0, r1
    ldr r1, [r4, #(\base + 8)]
    ldr r3, [r5, #(\base + 0)]
    ands r1, r3
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #1
    ands r1, r2
    eors r0, r1
    ldr r1, [r4, #(\base + 4)]
    ldr r3, [r5, #(\base + 8)]
    ands r1, r3
    eors r0, r1
    ldr r1, [r4, #(\base + 8)]
    ldr r3, [r5, #(\base + 4)]
    ands r1, r3
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #2
    ands r1, r2
    eors r0, r1
    ands r0, r2
    mov r0, r6
    lsrs r0, r0, #5

    lsrs r6, r6, #3
.endm

.global trace_isw_and_d2_asm_window
.type trace_isw_and_d2_asm_window, %function
trace_isw_and_d2_asm_window:
    push {r4, r5, r6, r7, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    ldr r7, 1f
    b 2f
    .align 2
1:
    .word 0xE0000004
2:
    movs r0, #1
    str r0, [r7]

    D2_ISW_LANE 0
    D2_ISW_LANE 12
    D2_ISW_LANE 24
    D2_ISW_LANE 36

    movs r0, #0
    ldr r7, =0xE0000004
    str r0, [r7]
    pop {r4, r5, r6, r7, pc}

.size trace_isw_and_d2_asm_window, .-trace_isw_and_d2_asm_window
.ltorg

.macro D2_EQ_MATCH_BIT0 dst lane_xor
    mov \dst, r3
    movs r2, #1
    ands \dst, r2
.if \lane_xor
    eors \dst, r2
.endif
.endm

.macro D2_EQ_MATCH_BIT1 dst lane_xor
    mov \dst, r3
    lsrs \dst, \dst, #1
    movs r2, #1
    ands \dst, r2
.if \lane_xor
    eors \dst, r2
.endif
.endm

.macro D2_EQ_BIT0 dst zreg
    mov \dst, \zreg
    movs r2, #1
    ands \dst, r2
.endm

.macro D2_EQ_BIT1 dst zreg
    mov \dst, \zreg
    lsrs \dst, \dst, #1
    movs r2, #1
    ands \dst, r2
.endm

.macro D2_PRECHARGE reg shift
    mov \reg, r6
.if \shift
    lsrs \reg, \reg, #\shift
.endif
.endm

.macro D2_EQ_LANE flip0 flip1
    D2_EQ_MATCH_BIT0 r0, \flip0
    D2_PRECHARGE r1, 4
    D2_EQ_MATCH_BIT1 r1, \flip1
    ands r0, r1
    movs r2, #1
    mov r1, r6
    ands r1, r2
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #1
    ands r1, r2
    eors r0, r1
    ands r0, r2
    D2_PRECHARGE r0, 3
    D2_PRECHARGE r1, 5

    D2_EQ_BIT0 r0, r4
    D2_PRECHARGE r1, 4
    D2_EQ_BIT1 r1, r4
    ands r0, r1
    D2_PRECHARGE r1, 5
    D2_EQ_MATCH_BIT0 r1, \flip0
    D2_PRECHARGE r2, 6
    D2_EQ_BIT1 r2, r4
    ands r1, r2
    eors r0, r1
    D2_PRECHARGE r1, 7
    D2_EQ_BIT0 r1, r4
    D2_PRECHARGE r2, 8
    D2_EQ_MATCH_BIT1 r2, \flip1
    ands r1, r2
    eors r0, r1
    movs r2, #1
    mov r1, r6
    ands r1, r2
    eors r0, r1
    mov r1, r6
    lsrs r1, r1, #2
    ands r1, r2
    eors r0, r1
    ands r0, r2
    D2_PRECHARGE r0, 4
    D2_PRECHARGE r1, 6

    D2_EQ_BIT0 r0, r5
    D2_PRECHARGE r1, 4
    D2_EQ_BIT1 r1, r5
    ands r0, r1
    D2_PRECHARGE r1, 5
    D2_EQ_MATCH_BIT0 r1, \flip0
    D2_PRECHARGE r2, 6
    D2_EQ_BIT1 r2, r5
    ands r1, r2
    eors r0, r1
    D2_PRECHARGE r1, 7
    D2_EQ_BIT0 r1, r5
    D2_PRECHARGE r2, 8
    D2_EQ_MATCH_BIT1 r2, \flip1
    ands r1, r2
    eors r0, r1
    movs r2, #1
    mov r1, r6
    lsrs r1, r1, #1
    ands r1, r2
    eors r0, r1
    D2_PRECHARGE r1, 5
    D2_EQ_BIT0 r1, r4
    D2_PRECHARGE r2, 6
    D2_EQ_BIT1 r2, r5
    ands r1, r2
    eors r0, r1
    D2_PRECHARGE r1, 7
    D2_EQ_BIT0 r1, r5
    D2_PRECHARGE r2, 8
    D2_EQ_BIT1 r2, r4
    ands r1, r2
    eors r0, r1
    movs r2, #1
    mov r1, r6
    lsrs r1, r1, #2
    ands r1, r2
    eors r0, r1
    ands r0, r2
    D2_PRECHARGE r0, 5
    D2_PRECHARGE r1, 6
    D2_PRECHARGE r2, 7

    lsrs r6, r6, #3
.endm

.global trace_eq2_const_d2_asm_window
.type trace_eq2_const_d2_asm_window, %function
trace_eq2_const_d2_asm_window:
    push {r4, r5, r6, r7, lr}
    ldr r3, [r0, #0]
    ldr r4, [r0, #4]
    ldr r5, [r0, #8]
    mov r6, r1
    ldr r7, 3f
    b 4f
    .align 2
3:
    .word 0xE0000004
4:
    movs r0, #1
    str r0, [r7]

    D2_EQ_LANE 1 1
    D2_EQ_LANE 0 1
    D2_EQ_LANE 1 0
    D2_EQ_LANE 0 0

    movs r0, #0
    str r0, [r7]
    pop {r4, r5, r6, r7, pc}

.size trace_eq2_const_d2_asm_window, .-trace_eq2_const_d2_asm_window
.ltorg
