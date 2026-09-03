#!/usr/bin/env python3
"""COOP/COEP server for SharedArrayBuffer threads (python http.server with headers)."""
import http.server, functools, sys

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()
    def log_message(self, *a):
        pass

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    d = sys.argv[2] if len(sys.argv) > 2 else "web"
    print(f"Serving {d} on http://localhost:{port} with COOP/COEP (SAB enabled)")
    http.server.ThreadingHTTPServer(("0.0.0.0", port), functools.partial(H, directory=d)).serve_forever()
