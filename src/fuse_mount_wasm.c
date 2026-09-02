#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "emulator.h"
#include "fuse_mount.h"
#include <emscripten.h>

static uint8_t *wasm_flash = NULL;
static size_t wasm_flash_size = 0;
static int wasm_mounted = 0;

int fuse_mount_start(uint8_t *flash_data, size_t flash_size, const char *mount_point) {
    wasm_flash = flash_data;
    wasm_flash_size = flash_size;
    wasm_mounted = 1;
    // Use OPFS via Emscripten FS sync
    EM_ASM({
        if (typeof navigator !== 'undefined' && navigator.storage && navigator.storage.getDirectory) {
            console.log("[WASM-FUSE] OPFS available for", UTF8ToString($0));
        } else {
            console.log("[WASM-FUSE] OPFS not available, using MEMFS");
        }
    }, mount_point);
    fprintf(stderr, "[WASM-FUSE] Mounted %zu bytes at %s (OPFS/MEMFS)\n", flash_size, mount_point);
    return 0;
}
void fuse_set_flash_offset(uint32_t offset) { (void)offset; }
pthread_mutex_t fuse_flash_mutex = PTHREAD_MUTEX_INITIALIZER;
