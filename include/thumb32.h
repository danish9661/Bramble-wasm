#ifndef THUMB32_H
#define THUMB32_H

#include <stdint.h>

/*
 * thumb32_step - dispatch a 32-bit Thumb-2 instruction
 *
 * Called from cpu_step() when a 32-bit Thumb-2 prefix is detected.
 * @pc:    address of the upper halfword (current PC before fetch)
 * @upper: first (upper) 16-bit halfword
 * @lower: second (lower) 16-bit halfword
 *
 * Returns 1 if handled, 0 if unknown instruction.
 * Sets cpu.r[15] and pc_updated as appropriate.
 */
int thumb32_step(uint32_t pc, uint16_t upper, uint16_t lower);

/* Evaluate an ARM condition code (0-15) against current XPSR flags.
 * Shared by the IT-block predication check in cpu_step(). */
int t32_check_cond(uint8_t cond);

#endif /* THUMB32_H */
