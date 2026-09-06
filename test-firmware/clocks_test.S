/*
 * Clocks Test - read RP2040 clock-domain registers, print hex values
 *
 * Memory Layout (RP2040):
 *   Vector table:   0x10000100 - 0x10000140 (after boot2)
 *   RAM_TOP:        0x20042000 (264 KB)
 *   UART0:          0x40034000
 *   RESETS:         0x4000C000 (RESET @ +0x00)
 *   CLOCKS:         0x40008000 (CLK_SYS_SELECTED @ +0x44)
 *   PLL_SYS:        0x40028000 (CS @ +0x00)
 */

.syntax unified
.cpu cortex-m0plus
.thumb

.equ UART0_DR,          0x40034000
.equ UART0_CR,          0x40034030
.equ UART_CR_EN,        0x0301
.equ RAM_TOP,           0x20042000

.equ RESETS_RESET,      0x4000C000
.equ CLK_SYS_SELECTED,  0x40008044
.equ PLL_SYS_CS,        0x40028000

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

    /* RESET register */
    ldr r0, =msg_reset
    bl print_string
    ldr r4, =RESETS_RESET
    ldr r0, [r4]
    bl print_hex32

    /* CLK_SYS_SELECTED register */
    ldr r0, =msg_clksys
    bl print_string
    ldr r4, =CLK_SYS_SELECTED
    ldr r0, [r4]
    bl print_hex32

    /* PLL_SYS_CS register */
    ldr r0, =msg_pll
    bl print_string
    ldr r4, =PLL_SYS_CS
    ldr r0, [r4]
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
    .asciz "Clocks Test Starting (RESETS/CLOCKS/PLL_SYS)\n"
msg_reset:
    .asciz "RESETS RESET register 0x4000C000 = "
msg_clksys:
    .asciz "CLK_SYS_SELECTED register 0x40008044 = "
msg_pll:
    .asciz "PLL_SYS_CS register 0x40028000 = "
msg_done:
    .asciz "Clocks Test Complete!\n"

.align 2
