/*
 * Software-Defined EEPROM (24LC256-compatible)
 *
 * I2C serial EEPROM, 32KB (256Kbit), 64-byte pages, 16-bit addresses.
 * Default I2C address: 0x50. Memory initializes erased (0xFF).
 *
 * Protocol (address byte handled by I2C TAR match, only data delivered here):
 *   Byte write:  [mem_hi] [mem_lo] [data] + STOP
 *   Page write:  [mem_hi] [mem_lo] [data...] + STOP (wraps within 64B page)
 *   Random read: [mem_hi] [mem_lo] + RESTART, then reads (auto-increment,
 *                wraps at 32KB). Current-address read also supported.
 *
 * The emulator calls start_fn only on RESTART and stop_fn on STOP, so the
 * address-phase state is armed by stop/init (phase 0) and frozen by
 * start/RESTART (phase 3, latch kept).
 *
 * Usage:
 *   sdd_create_eeprom(0, 0x50, NULL);
 *   -sdd eeprom                       32KB on I2C0 addr 0x50
 *   -sdd eeprom:i2c=1,addr=0x51       Custom bus/address
 *   -sdd eeprom:file=eeprom.bin       File-backed persistence
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdd.h"

#define EEPROM_SIZE       32768u
#define EEPROM_PAGE_SIZE  64u
#define EEPROM_PAGE_MASK  (EEPROM_PAGE_SIZE - 1u)

typedef struct {
    uint8_t  mem[EEPROM_SIZE];
    uint16_t addr;          /* Latched memory address */
    uint8_t  addr_hi;       /* First address byte staging */
    int      phase;         /* 0=expect hi, 1=expect lo, 2=data, 3=read mode */
    int      dirty;         /* 1 if unwritten changes (for file backing) */
    char     path[256];     /* Optional backing file, "" = none */
} sdd_eeprom_state_t;

static void eeprom_flush_to_file(sdd_eeprom_state_t *s) {
    FILE *f;
    if (!s->path[0] || !s->dirty) return;
    f = fopen(s->path, "wb");
    if (!f) {
        fprintf(stderr, "[SDD] EEPROM: failed to save %s\n", s->path);
        return;
    }
    fwrite(s->mem, 1, EEPROM_SIZE, f);
    fclose(f);
    s->dirty = 0;
}

static void eeprom_i2c_start(void *ctx) {
    sdd_eeprom_state_t *s = (sdd_eeprom_state_t *)ctx;
    /* RESTART: freeze latched address, reads follow */
    s->phase = 3;
}

static void eeprom_i2c_stop(void *ctx) {
    sdd_eeprom_state_t *s = (sdd_eeprom_state_t *)ctx;
    eeprom_flush_to_file(s);
    /* Next fresh transaction starts with address bytes */
    s->phase = 0;
}

static int eeprom_i2c_write(void *ctx, uint8_t data) {
    sdd_eeprom_state_t *s = (sdd_eeprom_state_t *)ctx;

    if (s->phase == 3) {
        /* Write after RESTART: restart address machine (combined format) */
        s->phase = 0;
    }
    if (s->phase == 0) {
        s->addr_hi = data;
        s->phase = 1;
    } else if (s->phase == 1) {
        s->addr = (uint16_t)(((uint16_t)s->addr_hi << 8) | data);
        s->addr %= EEPROM_SIZE;
        s->phase = 2;
    } else {
        /* Data byte: page-wrap writes per 24LC256 spec */
        s->mem[s->addr] = data;
        s->addr = (uint16_t)((s->addr & ~EEPROM_PAGE_MASK) |
                             ((s->addr + 1u) & EEPROM_PAGE_MASK));
        s->dirty = 1;
    }
    return 0;  /* ACK */
}

static uint8_t eeprom_i2c_read(void *ctx) {
    sdd_eeprom_state_t *s = (sdd_eeprom_state_t *)ctx;
    uint8_t v = s->mem[s->addr];
    s->addr = (uint16_t)((s->addr + 1u) % EEPROM_SIZE);
    return v;
}

static void eeprom_cleanup(void *ctx) {
    sdd_eeprom_state_t *s = (sdd_eeprom_state_t *)ctx;
    eeprom_flush_to_file(s);
    free(s);
}

/* ========================================================================
 * Factory
 * ======================================================================== */

int sdd_create_eeprom(int i2c_bus, int i2c_addr, const char *file) {
    sdd_eeprom_state_t *state = calloc(1, sizeof(sdd_eeprom_state_t));
    if (!state) return -1;

    memset(state->mem, 0xFF, EEPROM_SIZE);
    state->addr = 0;
    state->phase = 0;
    state->dirty = 0;
    state->path[0] = '\0';

    if (file && file[0]) {
        strncpy(state->path, file, sizeof(state->path) - 1);
        FILE *f = fopen(file, "rb");
        if (f) {
            size_t n = fread(state->mem, 1, EEPROM_SIZE, f);
            fclose(f);
            fprintf(stderr, "[SDD] EEPROM: loaded %zu bytes from %s\n", n, file);
        } else {
            fprintf(stderr, "[SDD] EEPROM: new image %s (%uKB)\n", file,
                    EEPROM_SIZE / 1024u);
        }
    }

    sdd_device_t dev;
    memset(&dev, 0, sizeof(dev));
    strncpy(dev.name, "eeprom", SDD_NAME_LEN - 1);
    dev.i2c_bus = i2c_bus;
    dev.i2c_addr = i2c_addr;
    dev.i2c_write = eeprom_i2c_write;
    dev.i2c_read = eeprom_i2c_read;
    dev.i2c_start = eeprom_i2c_start;
    dev.i2c_stop = eeprom_i2c_stop;
    dev.spi_bus = -1;
    dev.cleanup = eeprom_cleanup;
    dev.ctx = state;

    int idx = sdd_register(&dev);
    if (idx < 0) {
        free(state);
        return -1;
    }

    fprintf(stderr, "[SDD] EEPROM 24LC256 (%uKB) on I2C%d addr 0x%02X%s%s\n",
            EEPROM_SIZE / 1024u, i2c_bus, i2c_addr,
            file && file[0] ? " file=" : "", file && file[0] ? file : "");
    return idx;
}
