#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <emscripten.h>

// TAP via WebSocket (stub for now, logs)
int tapif_open(const char *name) {
    EM_ASM({ console.log("[WASM-NET] tapif_open", UTF8ToString($0), "-> WebSocket not yet, stub -1"); }, name);
    return -1;
}
void tapif_close(int fd) { (void)fd; EM_ASM({ console.log("[WASM-NET] tapif_close"); }); }
int tapif_read(int fd, uint8_t *buf, int maxlen) { (void)fd;(void)buf;(void)maxlen; return 0; }
int tapif_write(int fd, const uint8_t *buf, int len) { (void)fd;(void)buf; return len; }

// Netbridge via WebSocket
int net_bridge_init(void) { EM_ASM({ console.log("[WASM-NET] net_bridge_init via WebSocket"); }); return 0; }
void net_bridge_poll(void) {}
int net_bridge_uart_active(int uart_num) { (void)uart_num; return 0; }
void net_bridge_uart_tx(int uart_num, uint8_t byte) {
    EM_ASM({ 
        const ch = $1;
        if(typeof window !== 'undefined' && window.brambleNetSocket && window.brambleNetSocket.readyState===1) {
            window.brambleNetSocket.send(new Uint8Array([ch]));
        }
    }, uart_num, byte);
}

// Wire via BroadcastChannel
int wire_init(void) { EM_ASM({ 
    if(typeof BroadcastChannel !== 'undefined') {
        if(!window.brambleWire) window.brambleWire = new BroadcastChannel('bramble-wire');
        console.log("[WASM-NET] wire_init BroadcastChannel");
    }
}); return 0; }
void wire_poll(void) {}
int wire_add_link(const char *path, uint8_t type, uint8_t channel) { (void)path;(void)type;(void)channel; EM_ASM({console.log("[WASM-NET] wire_add_link", UTF8ToString($0));}, path); return 0; }
int wire_uart_active(int uart_num) { (void)uart_num; return 0; }
void wire_send_uart(int uart_num, uint8_t byte) {
    EM_ASM({
        if(window.brambleWire) window.brambleWire.postMessage({type:'uart', ch:$1, uart:$0});
    }, uart_num, byte);
}

// Corepool cooperative (no threads, just yield)
#include <pthread.h>
void corepool_init(void) { EM_ASM({console.log("[WASM-CORE] corepool_init cooperative");}); }
void corepool_set_step_quantum(int q) { (void)q; }
int corepool_query_cores(void) { return 1; }
void corepool_register(int cores) { (void)cores; }
void corepool_unregister(void) {}
void corepool_start_threads(void) {}
void corepool_stop_threads(void) {}
void corepool_start_core_thread(int id) { (void)id; }
void corepool_wake_cores(void) {}
void corepool_lock(void) {}
void corepool_unlock(void) {}
void corepool_cleanup(void) {}
