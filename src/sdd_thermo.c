/*
 * Software-Defined Thermometer (TMP102-compatible)
 *
 * I2C temperature sensor compatible with the TI TMP102 register layout.
 * Default I2C address: 0x48. Register pointer auto-selects which register
 * is returned on reads.
 *
 * Registers:
 *   0x00 = Temperature (16-bit, signed, 12-bit resolution in normal mode)
 *          [15:4] = temperature in 0.0625°C units, sign-extended
 *   0x01 = Configuration (16-bit)
 *   0x02 = T_LOW limit (16-bit)
 *   0x03 = T_HIGH limit (16-bit)
 *
 * I2C protocol:
 *   Write: [addr_byte] [register_pointer] [optional data MSB] [data LSB]
 *   Read:  Returns data from current register pointer, MSB first
 *
 * Usage:
 *   sdd_create_thermometer(25.0f, 0, 0x48);
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdd.h"

/* TMP102 register indices */
#define TMP102_REG_TEMP    0x00
#define TMP102_REG_CONFIG  0x01
#define TMP102_REG_TLOW    0x02
#define TMP102_REG_THIGH   0x03
#define TMP102_NUM_REGS    4

typedef struct {
    uint16_t regs[TMP102_NUM_REGS];   /* 16-bit registers */
    uint8_t  reg_ptr;                  /* Current register pointer */
    int      ptr_set;                  /* 1 after first write byte sets pointer */
    int      byte_idx;                 /* Which byte of the 16-bit register (0=MSB, 1=LSB) */
    float    temperature;              /* Current temperature in °C */
} sdd_thermo_state_t;

/* Convert temperature to TMP102 raw register value.
 * Normal mode (12-bit): value = temp / 0.0625, left-shifted by 4 */
static uint16_t temp_to_raw(float temp_c) {
    int16_t raw = (int16_t)(temp_c / 0.0625f);
    return (uint16_t)(raw << 4);
}

/* Set the temperature reading */
static void thermo_set_temp(sdd_thermo_state_t *s, float temp_c) {
    s->temperature = temp_c;
    s->regs[TMP102_REG_TEMP] = temp_to_raw(temp_c);
}

/* I2C callbacks */
static void thermo_i2c_start(void *ctx) {
    sdd_thermo_state_t *s = (sdd_thermo_state_t *)ctx;
    s->ptr_set = 0;
    s->byte_idx = 0;
}

static void thermo_i2c_stop(void *ctx) {
    (void)ctx;
}

static int thermo_i2c_write(void *ctx, uint8_t data) {
    sdd_thermo_state_t *s = (sdd_thermo_state_t *)ctx;

    if (!s->ptr_set) {
        /* First byte after START: register pointer */
        s->reg_ptr = data & 0x03;  /* Only 4 registers */
        s->ptr_set = 1;
        s->byte_idx = 0;
    } else {
        /* Subsequent bytes: write to register (MSB first) */
        if (s->reg_ptr == TMP102_REG_TEMP) {
            return 0;  /* Temperature register is read-only, ACK anyway */
        }
        if (s->byte_idx == 0) {
            s->regs[s->reg_ptr] = (uint16_t)data << 8;
            s->byte_idx = 1;
        } else {
            s->regs[s->reg_ptr] |= data;
            s->byte_idx = 0;
        }
    }

    return 0;  /* ACK */
}

static uint8_t thermo_i2c_read(void *ctx) {
    sdd_thermo_state_t *s = (sdd_thermo_state_t *)ctx;

    uint16_t val = s->regs[s->reg_ptr];
    uint8_t result;

    if (s->byte_idx == 0) {
        result = (uint8_t)(val >> 8);
        s->byte_idx = 1;
    } else {
        result = (uint8_t)(val & 0xFF);
        s->byte_idx = 0;
        /* Auto-increment not typical for TMP102, but reset byte index */
    }

    return result;
}

static void thermo_cleanup(void *ctx) {
    free(ctx);
}

/* ========================================================================
 * Factory
 * ======================================================================== */

int sdd_create_thermometer(float initial_temp, int i2c_bus, int i2c_addr) {
    sdd_thermo_state_t *state = calloc(1, sizeof(sdd_thermo_state_t));
    if (!state) return -1;

    /* Default config: 12-bit resolution, continuous conversion */
    state->regs[TMP102_REG_CONFIG] = 0x60A0;  /* OS=0, R1:R0=11 (12-bit), F1:F0=00 */
    state->regs[TMP102_REG_TLOW]  = temp_to_raw(75.0f);   /* Default T_LOW = 75°C */
    state->regs[TMP102_REG_THIGH] = temp_to_raw(80.0f);   /* Default T_HIGH = 80°C */

    thermo_set_temp(state, initial_temp);

    sdd_device_t dev;
    memset(&dev, 0, sizeof(dev));
    strncpy(dev.name, "thermometer", SDD_NAME_LEN - 1);
    dev.i2c_bus = i2c_bus;
    dev.i2c_addr = i2c_addr;
    dev.i2c_write = thermo_i2c_write;
    dev.i2c_read = thermo_i2c_read;
    dev.i2c_start = thermo_i2c_start;
    dev.i2c_stop = thermo_i2c_stop;
    dev.spi_bus = -1;
    dev.cleanup = thermo_cleanup;
    dev.ctx = state;

    int idx = sdd_register(&dev);
    if (idx < 0) {
        free(state);
        return -1;
    }

    fprintf(stderr, "[SDD] Thermometer: %.1f°C on I2C%d addr 0x%02X\n",
            initial_temp, i2c_bus, i2c_addr);
    return idx;
}
