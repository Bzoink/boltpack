/*
 * boltlz_arm.S -- BoltLZ match scan for little-endian ARM
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

    .text
    .arm
    .align 2
    .global bl_arm_count
    .type bl_arm_count, %function

/* r0 and r1 are the matching starts after the verified prefix.
 * r2 bounds both reads. Return the byte count before the first difference.
 * Word reads require equal alignment, and every read stays within the
 * supplied range, including when the inputs overlap. */
bl_arm_count:
    cmp     r2, #0
    moveq   r0, #0
    moveq   pc, lr
    str     r4, [sp, #-4]!
    mov     r3, r2
    eor     r12, r0, r1
    tst     r12, #3
    bne     .Lbytes

.Lalign:
    tst     r0, #3
    beq     .Lwords
    ldrb    r12, [r0], #1
    ldrb    r4, [r1], #1
    cmp     r12, r4
    bne     .Ldone
    subs    r2, r2, #1
    bne     .Lalign
    b       .Ldone

.Lwords:
    cmp     r2, #4
    blo     .Lbytes
    ldr     r12, [r0]
    ldr     r4, [r1]
    cmp     r12, r4
    bne     .Lbytes
    add     r0, r0, #4
    add     r1, r1, #4
    sub     r2, r2, #4
    b       .Lwords

.Lbytes:
    cmp     r2, #0
    beq     .Ldone
.Lbyte_loop:
    ldrb    r12, [r0], #1
    ldrb    r4, [r1], #1
    cmp     r12, r4
    bne     .Ldone
    subs    r2, r2, #1
    bne     .Lbyte_loop

.Ldone:
    sub     r0, r3, r2
    ldr     r4, [sp], #4
    mov     pc, lr
    .size bl_arm_count, . - bl_arm_count

#if defined(__linux__)
    .section .note.GNU-stack,"",%progbits
#endif
