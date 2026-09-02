/*
 * RP2350 RISC-V Bootrom
 *
 * Provides:
 *   - Reset vector at 0x00000000 (jump to boot code)
 *   - Boot code: set SP, GP, mtvec, jump to flash
 *   - ROM function table with lookup mechanism
 *   - Function stubs at well-known addresses (intercepted by rv_cpu_step)
 *   - Trap handler for unhandled exceptions
 *
 * ROM Function Table Layout (RP2350-compatible):
 *   0x0010: Magic ('R', 'P', 0x02) — RP2350 identifier
 *   0x0014: Pointer to function table
 *   0x0018: Pointer to data table
 *   0x001C: Pointer to table lookup function
 *   0x0100: Function table entries [32-bit code, 32-bit func_ptr] ...
 *   0x0200: Data table entries
 *   0x0400: ROM function stubs (JALR x0, ra, 0 — intercepted by PC check)
 *
 * ROM Function Interception:
 *   Functions at 0x0400-0x04FF are stub RET instructions.
 *   rv_rom_intercept() checks PC and performs native C operations
 *   for memcpy, memset, flash operations, etc.
 */

#ifndef RV_BOOTROM_H
#define RV_BOOTROM_H

#include <stdint.h>
#include "rp2350_rv/rv_cpu.h"

/* RP2350 bootrom well-known offsets */
#define RV_ROM_WELL_KNOWN_PTR_SIZE      2
#define RV_ROM_WELL_KNOWN_LOOKUP_PTR    0x7DF8
#define RV_ROM_WELL_KNOWN_LOOKUP_ENTRY  0x7DFA
#define RV_ROM_WELL_KNOWN_ENTRY         0x7DFC

/* ROM function addresses (stubs placed here, intercepted by PC) */
#define RV_ROM_FN_TABLE_LOOKUP        0x0300
#define RV_ROM_FN_TABLE_LOOKUP_ENTRY  0x0304
#define RV_ROM_FN_MEMCPY              0x0400
#define RV_ROM_FN_MEMSET              0x0404
#define RV_ROM_FN_MEMCPY4             0x0408
#define RV_ROM_FN_MEMSET4             0x040C
#define RV_ROM_FN_POPCOUNT32          0x0410
#define RV_ROM_FN_CLZ32               0x0414
#define RV_ROM_FN_CTZ32               0x0418
#define RV_ROM_FN_REVERSE32           0x041C
#define RV_ROM_FN_FLASH_ENTER         0x0420
#define RV_ROM_FN_FLASH_EXIT          0x0424
#define RV_ROM_FN_FLASH_ERASE         0x0428
#define RV_ROM_FN_FLASH_PROGRAM       0x042C
#define RV_ROM_FN_FLASH_FLUSH_CACHE   0x0430
#define RV_ROM_FN_CONNECT_INTERNAL_FLASH 0x0434
#define RV_ROM_FN_BOOTROM_STATE_RESET 0x0438
#define RV_ROM_FN_GET_SYS_INFO        0x043C
#define RV_ROM_FN_REBOOT              0x0440
#define RV_ROM_FN_SET_STACK           0x0444
#define RV_ROM_FN_LAST                0x0448

/* RP2350 table lookup flags */
#define RV_ROM_RT_FLAG_FUNC_RISCV      0x0001
#define RV_ROM_RT_FLAG_FUNC_RISCV_FAR  0x0003
#define RV_ROM_RT_FLAG_DATA            0x0040

/* RP2350 bootrom error codes */
#define RV_BOOTROM_OK                     0
#define RV_BOOTROM_ERROR_INVALID_ARG     -5
#define RV_BOOTROM_ERROR_BUFFER_TOO_SMALL -13

/* ROM function/data table codes (matches Pico SDK bootrom_constants.h) */
#define RV_ROM_TABLE_CODE(c1, c2)     ((uint32_t)(c1) | ((uint32_t)(c2) << 8))
#define RV_ROM_CODE_CONNECT_INTERNAL_FLASH RV_ROM_TABLE_CODE('I', 'F')
#define RV_ROM_CODE_FLASH_EXIT_XIP        RV_ROM_TABLE_CODE('E', 'X')
#define RV_ROM_CODE_FLASH_FLUSH_CACHE     RV_ROM_TABLE_CODE('F', 'C')
#define RV_ROM_CODE_FLASH_ENTER_CMD_XIP   RV_ROM_TABLE_CODE('C', 'X')
#define RV_ROM_CODE_FLASH_ERASE           RV_ROM_TABLE_CODE('R', 'E')
#define RV_ROM_CODE_FLASH_PROG            RV_ROM_TABLE_CODE('R', 'P')
#define RV_ROM_CODE_REBOOT                RV_ROM_TABLE_CODE('R', 'B')
#define RV_ROM_CODE_BOOTROM_STATE_RESET   RV_ROM_TABLE_CODE('S', 'R')
#define RV_ROM_CODE_SET_BOOTROM_STACK     RV_ROM_TABLE_CODE('S', 'S')
#define RV_ROM_CODE_GET_SYS_INFO          RV_ROM_TABLE_CODE('G', 'S')
#define RV_ROM_DATA_SOFTWARE_GIT_REVISION RV_ROM_TABLE_CODE('G', 'R')
#define RV_ROM_DATA_FLASH_DEVINFO16_PTR   RV_ROM_TABLE_CODE('F', 'D')

/* SYS_INFO flags */
#define RV_SYS_INFO_CHIP_INFO       0x0001
#define RV_SYS_INFO_CRITICAL        0x0002
#define RV_SYS_INFO_CPU_INFO        0x0004
#define RV_SYS_INFO_FLASH_DEV_INFO  0x0008
#define RV_SYS_INFO_BOOT_RANDOM     0x0010
#define RV_SYS_INFO_NONCE           0x0020
#define RV_SYS_INFO_BOOT_INFO       0x0040

/* Boot type / CPU identifiers used in synthesized sys info */
#define RV_BOOT_TYPE_NORMAL         0x00
#define RV_BOOT_PARTITION_NONE      0xFF
#define RV_PICOBIN_CPU_ARM          0x00
#define RV_PICOBIN_CPU_RISCV        0x01

/* Populate the RP2350 ROM buffer with RISC-V bootrom code and function table. */
uint32_t rv_bootrom_init(uint8_t *rom, uint32_t rom_size,
                         uint32_t flash_base, uint32_t sram_end);

/* Check if PC is at a ROM function stub and execute it natively.
 * Returns 1 if intercepted (PC advanced to return address), 0 if not a ROM function. */
int rv_rom_intercept(rv_cpu_state_t *cpu);

#endif /* RV_BOOTROM_H */
