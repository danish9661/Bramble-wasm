/*
 * RTC Test - program the RP2040 real-time clock, read it back.
 *
 * Sets 2026-09-06 (Sunday) 12:00:00 via SETUP + LOAD strobe, enables,
 * then reads back CTRL/RTC_1/RTC_0 as hex.
 *
 * Memory Layout (RP2040): RAM_TOP 0x20042000, UART0 0x40034000,
 * RTC 0x4005C000.
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

.equ RTC_BASE,          0x4005C000
.equ RTC_SETUP_0,       0x04
.equ RTC_SETUP_1,       0x08
.equ RTC_CTRL,          0x0C
.equ RTC_RTC_1,         0x18
.equ RTC_RTC_0,         0x1C
.equ RTC_ENABLE,        0x01
.equ RTC_LOAD,          0x10

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

    ldr r6, =RTC_BASE

    /* SETUP_0: year/month/day (opaque to the test, echoed back) */
    ldr r0, =0x07E20906
    str r0, [r6, #RTC_SETUP_0]

    /* SETUP_1: hour/min/sec */
    ldr r0, =0x000C0000
    str r0, [r6, #RTC_SETUP_1]

    /* LOAD strobe + ENABLE */
    ldr r0, =(RTC_ENABLE | RTC_LOAD)
    str r0, [r6, #RTC_CTRL]

    /* Read back CTRL (ACTIVE bit should be set) */
    ldr r0, =msg_ctrl
    bl print_string
    ldr r0, [r6, #RTC_CTRL]
    bl print_hex32

    /* Read back RTC_1 / RTC_0 */
    ldr r0, =msg_rtc1
    bl print_string
    ldr r0, [r6, #RTC_RTC_1]
    bl print_hex32

    ldr r0, =msg_rtc0
    bl print_string
    ldr r0, [r6, #RTC_RTC_0]
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
    push {r1, r2, r4, lr}
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
    pop {r1, r2, r4, pc}

.section .rodata
.align 2

msg_start:
    .asciz "RTC Test Starting (RTC 0x4005C000)\n"
msg_ctrl:
    .asciz "RTC CTRL register = "
msg_rtc1:
    .asciz "RTC datetime 1 register = "
msg_rtc0:
    .asciz "RTC datetime 0 register = "
msg_done:
    .asciz "RTC Test Complete!\n"

.align 2
