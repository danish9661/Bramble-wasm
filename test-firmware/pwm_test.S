.syntax unified
.cpu cortex-m0plus
.thumb
.equ UART0_DR, 0x40034000
.equ UART0_CR, 0x40034030
.equ UART_CR_EN, 0x0301
.equ RAM_TOP, 0x20042000
.section .vectors, "a", %progbits
.align 2
_vectors:
.word RAM_TOP
.word reset_handler+1
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
ldr r7, =RAM_TOP; mov sp, r7
ldr r4, =UART0_CR; ldr r5, =UART_CR_EN; str r5, [r4]
ldr r0, =msg_start; bl print_string
ldr r0, =0x40050000; ldr r1, =0x1; str r1, [r0]
ldr r0, =msg_ok; bl print_string
ldr r0, =msg_done; bl print_string
bkpt #0; b . -2
.thumb_func
print_string:
push {r1,r2,r3,lr}; movs r1, r0; ldr r2, =UART0_DR
print_loop: ldrb r3, [r1]; cmp r3, #0; beq print_done; str r3, [r2]; adds r1, #1; b print_loop
print_done: pop {r1,r2,r3,pc}
.section .rodata
msg_start: .asciz "PWM Test Starting (PWM 8 slices 0x40050000)\n"
msg_ok: .asciz "PWM register 0x40050000 write OK\n"
msg_done: .asciz "PWM Test Complete!\n"
.align 2
