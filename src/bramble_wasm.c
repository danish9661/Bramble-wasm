#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "emulator.h"
#include "gpio.h"
#include "timer.h"
#include "nvic.h"
#include "clocks.h"
#include "adc.h"
#include "rom.h"
#include "uart.h"
#include "spi.h"
#include "i2c.h"
#include "pwm.h"
#include "dma.h"
#include "pio.h"
#include "usb.h"
#include "rtc.h"
#include "storage.h"
#include "devtools.h"
#include "gdb.h"
#include "rp2350_rv/rv_cpu.h"
#include "rp2350_rv/rv_clint.h"
#include "rp2350_rv/rv_membus.h"
#include "rp2350_rv/rv_bootrom.h"
#include "rp2350_rv/rp2350_periph.h"
#include "rp2350_rv/rv_icache.h"
#include "rp2350_rv/rp2350_memmap.h"
#include "rp2350_rv/picobin.h"
#include "rp2350_arm/m33_cpu.h"
#include <pthread.h>
pthread_mutex_t fuse_flash_mutex = PTHREAD_MUTEX_INITIALIZER;

/* UF2 block structure (copied from uf2.c) */
typedef struct {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;
    uint8_t  data[476];
    uint32_t magic_end;
} __attribute__((packed)) uf2_block_t;

#define UF2_MAGIC_START0 0x0A324655
#define UF2_MAGIC_START1 0x9E5D5157
#define UF2_MAGIC_END    0x0AB16F30

typedef enum { ARCH_M0PLUS, ARCH_RV32, ARCH_M33 } arch_t;

static rv_cpu_state_t rv_cores[2];
static rv_membus_state_t rv_bus;
static rv_icache_t rv_icache;
static int current_arch = 1;

/* UART TX buffer: firmware → browser */
#define UART_TX_BUF_SIZE 4096
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static int uart_tx_head = 0;
static int uart_tx_tail = 0;

/* UART RX buffer: browser → firmware */
static uint8_t uart_rx_buf[256];
static int uart_rx_head = 0;
static int uart_rx_tail = 0;

static void reset_runtime_peripherals(void) {
    gpio_init();
    timer_init();
    nvic_init();
    rom_init();
    uart_init();
    spi_init();
    i2c_init();
    pwm_init();
    dma_init();
    pio_init();
    clocks_init();
    adc_init();
    usb_init();
    rtc_init();
}

/* Override putchar to capture UART TX output from emulator */
int __attribute__((used)) putchar(int c) {
    uint8_t ch = (uint8_t)c;
    int next = (uart_tx_head + 1) % UART_TX_BUF_SIZE;
    if (next != uart_tx_tail) {
        uart_tx_buf[uart_tx_head] = ch;
        uart_tx_head = next;
    }
    return c;
}

int bramble_init(int arch) {
    current_arch = arch;
    cpu_init();
    memset(cpu.flash, 0xFF, FLASH_SIZE_MAX);
    timing_set_clock_mhz(1);
    reset_runtime_peripherals();
    dual_core_init();

    memset(&rv_cores[0], 0, sizeof(rv_cpu_state_t));
    memset(&rv_cores[1], 0, sizeof(rv_cpu_state_t));

    if (arch == ARCH_RV32) {
        membus_rp2350_mode = 1;
        rv_membus_init(&rv_bus, cpu.flash, FLASH_SIZE_MAX, timing_config.cycles_per_us);
        rv_bootrom_init(rv_bus.rom, rv_bus.rom_size, RP2350_FLASH_BASE, RP2350_SRAM_END);
        rv_icache_init(&rv_icache);
        rv_cpu_init(&rv_cores[0], 0);
        rv_cpu_init(&rv_cores[1], 1);
        rv_cores[0].bus = &rv_bus;
        rv_cores[1].bus = &rv_bus;
        rv_cores[0].icache = &rv_icache;
        rv_cores[1].icache = &rv_icache;
        gdb_is_riscv = 1;
        gdb_rv_harts[0] = &rv_cores[0];
        gdb_rv_harts[1] = &rv_cores[1];
    } else {
        membus_rp2350_mode = (arch == ARCH_M33) ? 1 : 0;
        gdb_is_riscv = 0;
    }

    uart_tx_head = 0;
    uart_tx_tail = 0;
    uart_rx_head = 0;
    uart_rx_tail = 0;

    return 1;
}

int bramble_load_uf2(const uint8_t *data, int len) {
    FILE *f = fopen("/tmp/fw.uf2", "wb");
    if (!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    int ret = load_uf2("/tmp/fw.uf2");
    // load_uf2 sets detected_arch and flash; sync membus mode if RP2350
    if (loader_detected_arch() == FW_ARCH_RV32) membus_rp2350_mode = 1;
    return ret;
}

int bramble_load_elf(const uint8_t *data, int len) {
    FILE *f = fopen("/tmp/fw.elf", "wb");
    if (!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    return load_elf("/tmp/fw.elf");
}

void bramble_reset(void) {
    if (current_arch == ARCH_RV32) {
        picobin_info_t pbi = picobin_scan(cpu.flash, 4096);
        if (pbi.found && pbi.entry_pc != 0) {
            rv_cpu_reset(&rv_cores[0], pbi.entry_pc);
            if (pbi.entry_sp != 0) rv_cores[0].x[2] = pbi.entry_sp;
        } else {
            rv_cpu_reset(&rv_cores[0], 0x00000000);
        }
        rv_cpu_reset(&rv_cores[1], 0x00000000);
        rv_cores[1].is_halted = 1;
        rv_cores[0].bus = &rv_bus;
        rv_cores[1].bus = &rv_bus;
        rv_cores[0].icache = &rv_icache;
        rv_cores[1].icache = &rv_icache;
    } else {
        cpu_reset_core(CORE0);
        cpu_reset_core(CORE1);
    }
}

void bramble_set_clock(int freq_mhz) {
    timing_set_clock_mhz((uint32_t)freq_mhz);
}

/* Read one byte from UART TX buffer (firmware output). Returns -1 if empty. */
int bramble_read_uart(void) {
    if (uart_tx_tail != uart_tx_head) {
        int ch = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
        return ch;
    }
    return -1;
}

/* Read up to max_len bytes from UART TX buffer. Returns bytes read. */
int bramble_read_uart_bulk(uint8_t *dest, int max_len) {
    int count = 0;
    while (count < max_len && uart_tx_tail != uart_tx_head) {
        dest[count++] = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
    }
    return count;
}

/* Push a byte into UART RX FIFO (firmware input). */
void bramble_write_uart(int ch) {
    int next = (uart_rx_head + 1) % 256;
    if (next != uart_rx_tail) {
        uart_rx_buf[uart_rx_head] = (uint8_t)ch;
        uart_rx_head = next;
    }
}

/* Called periodically by the step loop to feed UART RX from our buffer. */
static void feed_uart_rx(void) {
    while (uart_rx_tail != uart_rx_head) {
        int ch = uart_rx_buf[uart_rx_tail];
        uart_rx_tail = (uart_rx_tail + 1) % 256;
        uart_rx_push(0, (uint8_t)ch);
    }
}

int bramble_get_gpio(int pin) {
    return gpio_get_pin((uint8_t)pin);
}

void bramble_set_gpio(int pin, int val) {
    gpio_set_pin((uint8_t)pin, (uint8_t)val);
}

uint32_t bramble_mem_read32(uint32_t addr) {
    return mem_read32(addr);
}

void bramble_mem_write32(uint32_t addr, uint32_t val) {
    mem_write32(addr, val);
}

int bramble_step(int n_instructions) {
    if (timing_config.cycles_per_us == 0)
        timing_set_clock_mhz(1);

    if (current_arch == ARCH_RV32) {
        int total = 0;
        while (total < n_instructions) {
            if (rv_cpu_is_halted(&rv_cores[0]))
                break;
            if (!rv_cores[0].is_wfi) {
                if (!rv_rom_intercept(&rv_cores[0])) {
                    rv_cpu_step(&rv_cores[0]);
                    total++;
                }
            }
            if (!rv_cores[1].is_halted && !rv_cores[1].is_wfi) {
                if (!rv_rom_intercept(&rv_cores[1]))
                    rv_cpu_step(&rv_cores[1]);
            }
            rv_clint_tick(&rv_bus.clint, 1);
            if (rv_bus.clint.cycle_accum == 0) {
                timer_tick(1);
                rp2350_timer1_tick(&rv_bus.periph, 1);
            }
            rv_clint_check_interrupts(&rv_bus.clint, &rv_cores[0]);
            if (!rv_cores[1].is_halted)
                rv_clint_check_interrupts(&rv_bus.clint, &rv_cores[1]);
            if (rv_cores[1].is_halted) {
                uint32_t h1_entry, h1_sp, h1_arg;
                if (rv_membus_check_hart1_launch(&rv_bus, &h1_entry, &h1_sp, &h1_arg)) {
                    rv_cpu_reset(&rv_cores[1], h1_entry);
                    rv_cores[1].x[2] = h1_sp;
                    rv_cores[1].x[10] = h1_arg;
                }
            }
            if ((total & 0x3FF) == 0)
                feed_uart_rx();
            if (rv_cores[0].csr[CSR_MCAUSE] == MCAUSE_BREAKPOINT && rv_cores[0].x[10] == 0x20026)
                break;
            if (total >= n_instructions)
                break;
        }
        feed_uart_rx();
        return total;
    } else {
        int total = 0;
        while (total < n_instructions) {
            if (cpu_is_halted_core(0))
                break;
            cpu_step_core(0);
            total++;
            if (!cpu_is_halted_core(1))
                cpu_step_core(1);
            if ((total & 0x3FF) == 0) {
                timer_tick(1024);
                feed_uart_rx();
            }
            if (total >= n_instructions)
                break;
        }
        feed_uart_rx();
        return total;
    }
}

int bramble_is_halted(void) {
    if (current_arch == ARCH_RV32)
        return rv_cpu_is_halted(&rv_cores[0]);
    return cpu_is_halted_core(0);
}

void bramble_get_core_state(int core, uint32_t *pc, uint32_t *sp) {
    if (current_arch == ARCH_RV32) {
        if (core == 0) { *pc = rv_cores[0].pc; *sp = rv_cores[0].x[2]; }
        else { *pc = rv_cores[1].pc; *sp = rv_cores[1].x[2]; }
    } else {
        extern cpu_state_dual_t cores[2];
        if (core == 0) { *pc = cores[0].r[15]; *sp = cores[0].r[13]; }
        else { *pc = cores[1].r[15]; *sp = cores[1].r[13]; }
    }
}

uint8_t *bramble_get_flash_ptr(void) {
    return cpu.flash;
}

uint8_t *bramble_get_sram_ptr(void) {
    return rv_bus.sram;
}

/* Stubs for excluded files (netbridge.c, wire.c, corepool.c) */
int net_bridge_uart_active(int uart_num) { (void)uart_num; return 0; }
void net_bridge_uart_tx(int uart_num, uint8_t byte) { (void)uart_num; (void)byte; }
int wire_uart_active(int uart_num) { (void)uart_num; return 0; }
void wire_send_uart(int uart_num, uint8_t byte) { (void)uart_num; (void)byte; }
void corepool_start_core_thread(int core_id) { (void)core_id; }
void corepool_wake_cores(void) {}
int tapif_open(const char *name) { (void)name; return -1; }
void tapif_close(int fd) { (void)fd; }
int tapif_read(int fd, uint8_t *buf, int maxlen) { (void)fd; (void)buf; (void)maxlen; return 0; }
int tapif_write(int fd, const uint8_t *buf, int len) { (void)fd; (void)buf; (void)len; return len; }