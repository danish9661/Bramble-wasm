/*
 * FP Test - double-precision 0.1 + 0.2 via softfloat __aeabi_dadd,
 * print result words as hex. Exact IEEE-754 result:
 *   0.1 + 0.2 = 0x3FD5555555555555 (0.30000000000000004)
 *
 * Memory Layout (RP2040):
 *   Vector table:   0x10000100 - 0x10000140 (after boot2)
 *   RAM_TOP:        0x20042000 (264 KB)
 *   UART0:          0x40034000
 */

.syntax unified
.cpu cortex-m0plus
.thumb

.equ UART0_DR,          0x40034000
.equ UART0_CR,          0x40034030
.equ UART_CR_EN,        0x0301
.equ RAM_TOP,           0x20042000

.section .vectors, "a", %progbits
.align 2
.global _vectors

_vectors:
    .word RAM_TOP
    .word reset_handler + 1
    .word 0xFFFFFFFF
    .word 0xFFFFFFFF
    .rept 12
    .word 0xFFFFFFFF
    .endr

.section .text
.align 2

.thumb_func
.global reset_handler

reset_handler:
    ldr r7, =RAM_TOP
    mov sp, r7

    ldr r4, =UART0_CR
    ldr r5, =UART_CR_EN
    str r5, [r4]

    ldr r0, =msg_start
    bl print_string

    /* r0+r1 = 0.1 (0x3FB999999999999A), r2+r3 = 0.2 (0x3FC999999999999A) */
    ldr r0, =0x9999999A
    ldr r1, =0x3FB99999
    ldr r2, =0x9999999A
    ldr r3, =0x3FC99999
    bl __aeabi_dadd
    /* r0 = low word, r1 = high word of 0.1+0.2 */
    push {r0, r1}

    ldr r0, =msg_hi
    bl print_string
    pop {r0, r1}
    push {r0, r1}
    movs r0, r1
    bl print_hex32

    ldr r0, =msg_lo
    bl print_string
    pop {r0, r1}
    bl print_hex32

    ldr r0, =msg_done
    bl print_string

    bkpt #0
    b . - 2

/* Print string via UART0 (r0 = ptr) */
.thumb_func
print_string:
    push {r1, r2, r3, lr}
    movs r1, r0
    ldr r2, =UART0_DR
print_loop:
    ldrb r3, [r1]
    cmp r3, #0
    beq print_done
    str r3, [r2]
    adds r1, #1
    b print_loop
print_done:
    pop {r1, r2, r3, pc}

/* Print r0 as 8 hex digits + newline */
.thumb_func
print_hex32:
    push {r1, r2, r3, r4, lr}
    movs r1, r0
    ldr r2, =UART0_DR
    movs r4, #8
hex_digit:
    lsrs r0, r1, #28
    lsls r1, r1, #4
    cmp r0, #10
    blo hex_num
    adds r0, #0x37
    b hex_out
hex_num:
    adds r0, #0x30
hex_out:
    str r0, [r2]
    subs r4, #1
    bne hex_digit
    movs r0, #0x0A
    str r0, [r2]
    pop {r1, r2, r3, r4, pc}

.section .rodata
.align 2

msg_start:
    .asciz "FP Test Starting (0.1 + 0.2 softfloat double)\n"
msg_hi:
    .asciz "Result high word = "
msg_lo:
    .asciz "Result low word = "
msg_done:
    .asciz "FP Test Complete!\n"

.align 2
