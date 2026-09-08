/*
 * WS2812 Test - drive a WS2812-style bitstream from PIO0 SM0 using
 * OUT + side-set + JMP. Sends one 24-bit GRB pixel, then lights the LED.
 *
 * PIO program (side_set 1, plain):
 *   0: pull            side 0          ; pixel -> OSR (blocking)
 *   1: set y, 23       side 0          ; 24 bits
 *   2: out x, 1        side 0 [1]      ; shift bit to X, line low
 *   3: jmp !x, 5       side 1 [1]      ; bit set? line high
 *   4: jmp 6           side 1 [2]
 *   5: nop             side 0 [1]      ; bit clear: line low
 *   6: jmp y--, 2      side 1 [1]      ; next bit, line high
 *   7: jmp 7           side 1          ; done loop, line high
 *
 * Firmware polls SM0 ADDR until it reaches 7 (bounded), then prints
 * FIFO state and lights GPIO25.
 *
 * Memory Layout (RP2040): RAM_TOP 0x20042000, UART0 0x40034000,
 * PIO0 0x50200000, SIO 0xD0000000.
 */

.syntax unified
#ifdef RP2350
.cpu cortex-m33
#else
.cpu cortex-m0plus
#endif
.thumb

#ifdef RP2350
.equ UART0_DR,          0x40070000
.equ UART0_CR,          0x40070030
#else
.equ UART0_DR,          0x40034000
.equ UART0_CR,          0x40034030
#endif
.equ UART_CR_EN,        0x0301
#ifdef RP2350
.equ RAM_TOP,           0x20082000
#else
.equ RAM_TOP,           0x20042000
#endif

.equ PIO0_BASE,         0x50200000
.equ PIO_CTRL,          0x000
.equ PIO_TXF0,          0x010
.equ PIO_INSTR0,        0x048
.equ PIO_SM0_CLKDIV,    0x0C8
.equ PIO_SM0_EXECCTRL,  0x0CC
.equ PIO_SM0_SHIFTCTRL, 0x0D0
.equ PIO_SM0_ADDR,      0x0D4
.equ PIO_SM0_PINCTRL,   0x0DC

.equ SIO_GPIO_OE_SET,   0xD0000024
.equ SIO_GPIO_OUT_SET,  0xD0000014

.equ WS_PIN,            16
.equ LED_PIN_BIT,       (1 << 25)
.equ PIXEL_GRB,         0x00FF0000

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

    /* PIO base in r6 */
    ldr r6, =PIO0_BASE

    /* Load program into INSTR_MEM */
    ldr r0, =prog
    movs r1, #0
load_loop:
    lsls r2, r1, #2
    adds r2, r2, #0x48
    adds r2, r6, r2
    lsls r3, r1, #1
    adds r3, r0, r3
    ldrh r3, [r3]
    str r3, [r2]
    adds r1, #1
    cmp r1, #8
    blo load_loop

    /* PINCTRL: SIDESET_COUNT=1, SIDESET_BASE=16 */
    ldr r0, =(1 << 29)
    ldr r1, =0x4000
    orrs r0, r1
    ldr r1, =0xDC
    adds r1, r6, r1
    str r0, [r1]

    /* EXECCTRL: WRAP_TOP=7, plain side-set */
    ldr r0, =(7 << 12)
    ldr r1, =0xCC
    adds r1, r6, r1
    str r0, [r1]

    /* SHIFTCTRL: autopull, OUT shift right */
    ldr r0, =(1 << 17)
    ldr r1, =(1 << 19)
    orrs r0, r1
    ldr r1, =0xD0
    adds r1, r6, r1
    str r0, [r1]

    /* CLKDIV: run every cycle */
    ldr r0, =(1 << 16)
    ldr r1, =0xC8
    adds r1, r6, r1
    str r0, [r1]

    /* Push pixel, enable SM0 */
    ldr r0, =PIXEL_GRB
    ldr r1, =0x10
    adds r1, r6, r1
    str r0, [r1]
    movs r0, #1
    str r0, [r6]

    /* Wait for SM to reach done loop (ADDR==7), bounded */
    ldr r4, =200000
    ldr r5, =0xD4
    adds r5, r6, r5
wait_loop:
    ldr r0, [r5]
    cmp r0, #7
    beq pixel_done
    subs r4, #1
    bne wait_loop
    ldr r0, =msg_timeout
    bl print_string
    b finish

pixel_done:
    ldr r0, =msg_sent
    bl print_string

    /* LED on (GPIO25 via SIO) */
    ldr r0, =LED_PIN_BIT
    ldr r1, =SIO_GPIO_OE_SET
    str r0, [r1]
    ldr r1, =SIO_GPIO_OUT_SET
    str r0, [r1]

finish:
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

/* PIO program words (see header comment) */
prog:
    .hword 0x80A0, 0xE0D7, 0x6121, 0x1125
    .hword 0x1206, 0xA142, 0x1182, 0x1007

msg_start:
    .asciz "WS2812 Test Starting (PIO0 SM0, side-set, pin 16)\n"
msg_sent:
    .asciz "WS2812 24 bits sent (GRB=FF0000), SM idle at done loop\n"
msg_timeout:
    .asciz "WS2812 TIMEOUT waiting for SM\n"
msg_done:
    .asciz "WS2812 Test Complete!\n"

.align 2
