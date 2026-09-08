/*
 * UART Echo Test - read one line from UART0 RX, echo it back.
 *
 * Prints a prompt, polls FR.RXFE until a byte arrives, echoes bytes
 * until CR/LF, then prints the line back with a marker and completes.
 *
 * Memory Layout (RP2040): RAM_TOP 0x20042000, UART0 0x40034000.
 */

.syntax unified
.cpu cortex-m0plus
.thumb

.equ UART0_DR,          0x40034000
.equ UART0_FR,          0x40034018
.equ UART0_CR,          0x40034030
.equ UART_CR_EN,        0x0301
.equ UART_FR_RXFE,      0x10
.equ RAM_TOP,           0x20042000
.equ LINE_BUF,          0x20041000
.equ LINE_MAX,          64

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

    /* r4 = line buffer, r5 = length */
    ldr r4, =LINE_BUF
    movs r5, #0

read_loop:
    /* Wait for RX byte */
    ldr r0, =UART0_FR
rx_wait:
    ldr r1, [r0]
    movs r2, #UART_FR_RXFE
    tst r1, r2
    bne rx_wait

    /* Read byte */
    ldr r0, =UART0_DR
    ldrb r1, [r0]

    /* CR or LF ends the line */
    cmp r1, #0x0D
    beq line_done
    cmp r1, #0x0A
    beq line_done

    /* Store (bounded) and echo */
    cmp r5, #LINE_MAX
    bhs read_loop
    strb r1, [r4, r5]
    adds r5, #1
    str r1, [r0]
    b read_loop

line_done:
    movs r1, #0
    strb r1, [r4, r5]

    ldr r0, =msg_echo
    bl print_string
    movs r0, r4
    bl print_string
    ldr r0, =msg_nl
    bl print_string

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

.section .rodata
.align 2

msg_start:
    .asciz "UART Echo Test: type a line and press Enter\n"
msg_echo:
    .asciz "Echo: "
msg_nl:
    .asciz "\n"
msg_done:
    .asciz "UART Echo Test Complete!\n"

.align 2
