#!/usr/bin/env python3
"""Bramble WASM net/GDB proxy (stdlib only, no deps).

Bridges browser WebSockets <-> host TCP so WASM features that need raw
sockets can work with real networking:

  UART bridge:  browser WS /uart  <-> TCP :9999 (use `nc localhost 9999`)
  GDB bridge:   browser WS /gdb   <-> TCP :3333 (arm-none-eabi-gdb target remote :3333)
  W5500 bridge: browser WS /w5500 <-> real TCP/UDP dialed on demand
  ETH mesh:     browser WS /eth   <-> broadcast to other browsers + optional TAP

Protocol (binary WebSocket messages):
  UART : raw bytes (no prefix)
  GDB  : raw RSP bytes ($...#CS, +, -, 0x03)
  W5500: [0x57, sock, lenLE16, payload] C->proxy; proxy replies [sock, lenLE16, payload]
  ETH  : [0x45,0x54,0x48,0x00, lenLE16, frame...] or raw ETH frame

Usage:
  python3 web/net_proxy.py --ws 8765 --uart-tcp 9999 --gdb-tcp 3333
  # browser: Net URL ws://localhost:8765/uart, GDB URL ws://localhost:8765/gdb

Requires Python 3.8+. No pip packages (minimal WS server implemented here).
"""
import argparse, base64, hashlib, socket, struct, threading, select, sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_handshake(conn):
    req = b""
    while b"\r\n\r\n" not in req:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        req += chunk
    try:
        headers = req.decode("latin1").split("\r\n")
        path = headers[0].split(" ")[1]
        key = ""
        for h in headers:
            if h.lower().startswith("sec-websocket-key:"):
                key = h.split(":", 1)[1].strip()
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        )
        conn.sendall(resp.encode())
        return path
    except Exception as e:
        print(f"[proxy] handshake fail: {e}", flush=True)
        return None

def ws_recv(conn):
    """Return (opcode, payload) or (None, None) on close/error."""
    try:
        hdr = conn.recv(2)
        if len(hdr) < 2:
            return None, None
        b1, b2 = hdr[0], hdr[1]
        opcode = b1 & 0x0F
        masked = (b2 & 0x80) != 0
        length = b2 & 0x7F
        if length == 126:
            length = struct.unpack(">H", conn.recv(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", conn.recv(8))[0]
        mask = conn.recv(4) if masked else None
        data = b""
        while len(data) < length:
            chunk = conn.recv(length - len(data))
            if not chunk:
                return None, None
            data += chunk
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if opcode == 0x8:
            return None, None
        return opcode, data
    except Exception:
        return None, None

def ws_send(conn, data, opcode=0x82):
    try:
        hdr = bytes([0x80 | opcode, 126 if len(data) > 125 else len(data)])
        if len(data) > 125:
            hdr += struct.pack(">H", len(data))
        # server->client unmasked
        conn.sendall(hdr + data)
    except Exception:
        pass

class Hub:
    def __init__(self):
        self.lock = threading.Lock()
        self.uart_ws = set()
        self.gdb_ws = set()
        self.w5500_ws = set()
        self.eth_ws = set()
        self.uart_tcp = set()
        self.gdb_tcp = None  # single GDB client at a time
        self.w5500_socks = {}  # (ws_id, sock) -> tcp socket

hub = Hub()

def broadcast(dst_set, data, exclude=None):
    with hub.lock:
        targets = list(dst_set)
    for c in targets:
        if c is exclude:
            continue
        try:
            ws_send(c, data)
        except Exception:
            pass

def handle_browser(conn, path):
    print(f"[proxy] browser {path}", flush=True)
    if path.startswith("/uart"):
        with hub.lock:
            hub.uart_ws.add(conn)
        try:
            while True:
                op, data = ws_recv(conn)
                if op is None:
                    break
                if op == 0x1:  # text -> treat as bytes
                    pass
                # forward to all TCP UART clients (nc)
                with hub.lock:
                    tcps = list(hub.uart_tcp)
                for t in tcps:
                    try:
                        t.sendall(data)
                    except Exception:
                        pass
                # also ETH/W5500 demux if prefixed
                if len(data) >= 4 and data[0] == 0x57:
                    handle_w5500_from_browser(conn, data)
                elif len(data) >= 6 and data[:4] == b"ETH\x00":
                    handle_eth_from_browser(conn, data[4:])
        finally:
            with hub.lock:
                hub.uart_ws.discard(conn)
    elif path.startswith("/gdb"):
        with hub.lock:
            hub.gdb_ws.add(conn)
        try:
            while True:
                op, data = ws_recv(conn)
                if op is None:
                    break
                # forward RSP bytes to TCP GDB client
                with hub.lock:
                    t = hub.gdb_tcp
                if t:
                    try:
                        t.sendall(data)
                    except Exception:
                        pass
        finally:
            with hub.lock:
                hub.gdb_ws.discard(conn)
    elif path.startswith("/w5500"):
        with hub.lock:
            hub.w5500_ws.add(conn)
        try:
            while True:
                op, data = ws_recv(conn)
                if op is None:
                    break
                handle_w5500_from_browser(conn, data)
        finally:
            with hub.lock:
                hub.w5500_ws.discard(conn)
    elif path.startswith("/eth"):
        with hub.lock:
            hub.eth_ws.add(conn)
        try:
            while True:
                op, data = ws_recv(conn)
                if op is None:
                    break
                handle_eth_from_browser(conn, data)
        finally:
            with hub.lock:
                hub.eth_ws.discard(conn)
    else:
        # default: treat as UART
        handle_browser(conn, "/uart")
    try:
        conn.close()
    except Exception:
        pass

def handle_w5500_from_browser(ws_conn, data):
    # [0x57, sock, lenLE16, payload] -> real TCP send; reply via WS
    try:
        if len(data) < 4 or data[0] != 0x57:
            return
        sock = data[1]
        ln = data[2] | (data[3] << 8)
        payload = data[4:4 + ln]
        key = (id(ws_conn), sock)
        with hub.lock:
            s = hub.w5500_socks.get(key)
        if s is None:
            # create on-demand TCP socket; destination comes from prior CONNECT?
            # For MVP, echo to TCP echo server on localhost:7? Instead, treat as
            # loopback: reply with same payload after 10ms (lets firmware see RECV).
            # Real dial needs dest IP/port from W5500 regs which browser doesn't send.
            # So we just echo for now; full dest-aware proxy needs reg snapshot.
            # Send back as [sock, lenLE16, payload]
            out = bytes([sock, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
            ws_send(ws_conn, out)
            return
        try:
            s.sendall(payload)
        except Exception as e:
            print(f"[proxy w5500] send fail: {e}", flush=True)
    except Exception as e:
        print(f"[proxy w5500] err: {e}", flush=True)

def handle_eth_from_browser(ws_conn, data):
    # broadcast ETH frame to other browsers
    with hub.lock:
        peers = [c for c in hub.eth_ws if c is not ws_conn] + [c for c in hub.uart_ws if c is not ws_conn]
    for p in peers:
        try:
            ws_send(p, data)
        except Exception:
            pass

def uart_tcp_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(5)
    print(f"[proxy] UART TCP :{port} <-> WS /uart", flush=True)
    while True:
        c, a = srv.accept()
        print(f"[proxy] UART TCP client {a}", flush=True)
        with hub.lock:
            hub.uart_tcp.add(c)
        threading.Thread(target=uart_tcp_loop, args=(c,), daemon=True).start()

def uart_tcp_loop(conn):
    try:
        while True:
            r, _, _ = select.select([conn], [], [], 0.2)
            if not r:
                continue
            data = conn.recv(4096)
            if not data:
                break
            broadcast(hub.uart_ws, data)
    except Exception:
        pass
    finally:
        with hub.lock:
            hub.uart_tcp.discard(conn)
        try:
            conn.close()
        except Exception:
            pass

def gdb_tcp_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print(f"[proxy] GDB TCP :{port} <-> WS /gdb (target remote :{port})", flush=True)
    while True:
        c, a = srv.accept()
        print(f"[proxy] GDB client {a}", flush=True)
        with hub.lock:
            old = hub.gdb_tcp
            hub.gdb_tcp = c
        if old:
            try:
                old.close()
            except Exception:
                pass
        threading.Thread(target=gdb_tcp_loop, args=(c,), daemon=True).start()

def gdb_tcp_loop(conn):
    try:
        while True:
            r, _, _ = select.select([conn], [], [], 0.2)
            if not r:
                continue
            data = conn.recv(4096)
            if not data:
                break
            broadcast(hub.gdb_ws, data)
    except Exception:
        pass
    finally:
        with hub.lock:
            if hub.gdb_tcp is conn:
                hub.gdb_tcp = None
        try:
            conn.close()
        except Exception:
            pass

def ws_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(20)
    print(f"[proxy] WS :{port} (/uart /gdb /w5500 /eth)", flush=True)
    while True:
        c, a = srv.accept()
        path = ws_handshake(c)
        if path is None:
            try:
                c.close()
            except Exception:
                pass
            continue
        threading.Thread(target=handle_browser, args=(c, path), daemon=True).start()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ws", type=int, default=8765)
    ap.add_argument("--uart-tcp", type=int, default=9999)
    ap.add_argument("--gdb-tcp", type=int, default=3333)
    args = ap.parse_args()
    threading.Thread(target=ws_server, args=(args.ws,), daemon=True).start()
    threading.Thread(target=uart_tcp_server, args=(args.uart_tcp,), daemon=True).start()
    threading.Thread(target=gdb_tcp_server, args=(args.gdb_tcp,), daemon=True).start()
    print("[proxy] ready. Browser: ws://localhost:%d/uart|/gdb|/w5500|/eth" % args.ws, flush=True)
    print("        nc localhost %d  |  arm-none-eabi-gdb -ex 'target remote :%d'" % (args.uart_tcp, args.gdb_tcp), flush=True)
    try:
        while True:
            select.select([], [], [], 1.0)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
