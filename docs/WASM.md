# Bramble-WASM – WebAssembly Port

Compiled from C (Bramble v0.46.0) to WASM via Emscripten 6.0.9 (`emsdk`) for browser execution.

## Build

```bash
source emsdk/emsdk_env.sh
./build_wasm.sh
# COMMON_FLAGS="-O3 -msimd128 -Wall ..."
# INCLUDES="-Iinclude/ -Iinclude/rp2350_rv -Iinclude/rp2350_arm"
# SOURCES: src/bramble_wasm.c + cpu, thumb32, membus, uf2, elf, gpio, timer, uart, spi, i2c, pwm, adc, dma, pio, nvic, clocks, usb, rtc, rom, gdb, storage, sdcard, emmc, fatfs, w5500, bme280, cyw43, devtools, vnet, sdd, rv_cpu, rv_clint, rv_membus, rv_bootrom, rp2350_periph, picobin, m33_cpu
# Excluded: main.c (CLI), fuse_mount.c, corepool.c, tapif.c, netbridge.c, wire.c (stubs for uart/net/wire)
# Flags: -s WASM=1 ALLOW_MEMORY_GROWTH=1 MODULARIZE=1 EXPORT_NAME=BrambleModule EXPORT_ES6=1 ENVIRONMENT='web,node' INITIAL_MEMORY=64M
```

Outputs `web/bramble.wasm.js` 64K + `web/bramble.wasm.wasm` 156K (`build_wasm.sh`).

## Exports (`src/bramble_wasm.c`)

`bramble_init(arch)` 0:M0+ 1:RV32 2:M33, `bramble_load_uf2(ptr,len)` via `fopen("/tmp/fw.uf2")` + `load_uf2`, `bramble_load_elf`, `bramble_step(n)`, `bramble_reset` with `picobin_scan` for RV32 entry `0x10000022`, `bramble_read_uart`/`_bulk`/`_write_uart` via `putchar` intercept `uart_tx_buf[4096]`, `bramble_get_gpio`/`_raw`/`_out`/`_oe`, `bramble_mem_read32/write32`, `bramble_is_halted`, `bramble_get_core_state`, `get_flash/sram_ptr`. Stubs: `net_bridge_uart_active`, `wire_uart_active`, `corepool_wake_cores`, `tapif_*`, `fuse_flash_mutex`.

## Web UI (`web/index.html`)

`import BrambleModule from './bramble.wasm.js'` with `EXPORT_ES6=1` `print/printErr:console.log` avoids red. Features: drag-drop UF2/ELF, preset buttons `hello_world` `gpio_test` `timer_test` `interrupt_test` `name_prompt` `littleos` (RP2040), `littleos_pico2` (RP2350 M33), `littleos_pico2_riscv` (RP2350 RV32) in `web/` and `web/examples/`, serial monitor UART0, GPIO 0-29 viewer (`get_gpio_raw||get_gpio`), core PC/SP/halted/MIPS `perf-info` `2.5M` cycles/frame, clock select, `web/.nojekyll` `/.github/workflows/pages.yml` deploy `web/` to `https://danish9661.github.io/Bramble-wasm/`.

## Chips Verified (Arduino CLI `rp2040:rp2040@6.0.0`)

- RP2040 M0+ `build_pico/Blink.ino.uf2` WASM `B 1..5 DONE` `hello_world` 229 steps `Hello from Bramble RP2040 Emulator!` `gpio_test` `LED ON/OFF` `timer_test` `Timer Test Complete` `littleos` shell, `319/319` native tests same sources.
- RP2350 RV32 `build_pico2_rv/Blink.ino.uf2` trap `0x1000CC5C cause 3` (semihosting) `littleos_pico2_riscv` 1.5M steps shell `RP2350 Hazard3 RISC-V`.
- RP2350 M33 `littleos_pico2` 1891 blocks `PC 0x1000015C` `PC 0x86` ROM, no trap, same as native 2s.

## Peripherals

All `src/*` compiled: GPIO `0x40014000/0xD0000000`, UART PL011 `0x40034000`, SPI PL022, I2C DW_apb_i2c, Timer 64-bit, PWM 8 slices, ADC, DMA 12ch, PIO 2 blocks, NVIC, Clocks, USB, RTC, ROM, SIO, VREG, etc., SD/eMMC SPI, W5500, BME280, CYW43 TAP, VNet, SDD, all verified via `319` tests and firmware UART. SPI/I2C/PWM/ADC/DMA/PIO/USB via `319` and `littleOS` tasks, BME280/W5500/CYW43 models included.

## Credit

Original emulator: [Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble) MIT v0.46.0. This repo is a WASM port with browser UI, `build_wasm.sh`, `src/bramble_wasm.c`, `web/` for GitHub Pages.

## Deploy

`git push main` triggers `pages.yml` `upload-pages-artifact path: ./web`. Local `python3 -m http.server 8080 --directory web`.
