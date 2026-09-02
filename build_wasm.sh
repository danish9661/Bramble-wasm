#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

source "$SCRIPT_DIR/../emsdk/emsdk_env.sh" 2>/dev/null || {
  export EMSDK="$SCRIPT_DIR/../emsdk"
  export PATH="$EMSDK:$EMSDK/upstream/emscripten:$PATH"
}

echo "=== Bramble WASM Build ==="
echo "Emscripten: $(emcc --version | head -1)"

mkdir -p web

COMMON_FLAGS="-O3 -msimd128 -Wall -Wno-macro-redefined -Wno-logical-not-parentheses -Wno-format"
INCLUDES="-Iinclude/ -Iinclude/rp2350_rv -Iinclude/rp2350_arm"

SOURCES=(
  src/bramble_wasm.c
  src/cpu.c src/instructions.c src/thumb32.c src/membus.c
  src/uf2.c src/elf.c src/gpio.c src/timer.c src/uart.c
  src/spi.c src/i2c.c src/pwm.c src/adc.c src/dma.c
  src/pio.c src/nvic.c src/clocks.c src/usb.c src/rtc.c
  src/rom.c src/gdb.c src/storage.c src/sdcard.c src/emmc.c
  src/fatfs.c src/w5500.c src/bme280.c src/cyw43.c
  src/devtools.c src/vnet.c src/sdd.c src/sdd_thermo.c
  src/rp2350_rv/rv_cpu.c src/rp2350_rv/rv_clint.c
  src/rp2350_rv/rv_membus.c src/rp2350_rv/rv_bootrom.c
  src/rp2350_rv/rp2350_periph.c src/rp2350_rv/picobin.c
  src/rp2350_arm/m33_cpu.c
)

EXPORTS='[
  "_bramble_init","_bramble_reset",
  "_bramble_load_uf2","_bramble_load_elf",
  "_bramble_step","_bramble_set_clock",
  "_bramble_read_uart","_bramble_read_uart_bulk","_bramble_write_uart",
  "_bramble_get_gpio","_bramble_set_gpio",
  "_bramble_mem_read32","_bramble_mem_write32",
  "_bramble_is_halted","_bramble_get_core_state",
  "_bramble_get_flash_ptr","_bramble_get_sram_ptr",
  "_free","_malloc"
]'

echo "Compiling ${#SOURCES[@]} source files..."

emcc \
  "${SOURCES[@]}" \
  $INCLUDES $COMMON_FLAGS \
  -s WASM=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="BrambleModule" \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","getValue","setValue","HEAPU8","HEAPU32","HEAP8"]' \
  -s INITIAL_MEMORY=67108864 \
  -s MAXIMUM_MEMORY=268435456 \
  -s STACK_SIZE=1048576 \
  -s NO_EXIT_RUNTIME=1 \
  -s ENVIRONMENT='web,node' \
  -o web/bramble.wasm.js

echo ""
echo "Build complete!"
echo "  WASM: $(du -h web/bramble.wasm.wasm | cut -f1)"
echo "  JS:   $(du -h web/bramble.wasm.js | cut -f1)"
echo ""
echo "To test: python3 -m http.server 8080 --directory web"
echo "  Then open http://localhost:8080"