"""Range-capable test server for the qmgr shim: byte i of /blob is (i*31)%251.
Usage: rangesrv.py [port] [size]. GET /stats returns and resets the connection counts."""
import http.server, socketserver, threading, sys, json, re
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 64 << 20
lock = threading.Lock(); stats = {"conns": 0, "reqs": 0}
PATTERN = bytes((i * 31) % 251 for i in range(251 * 4096))
def data(start, end):
    out = bytearray(); i = start
    while i <= end:
        o = i % 251; n = min(end - i + 1, len(PATTERN) - o)
        out += PATTERN[o:o + n]; i += n
    return bytes(out)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def setup(self):
        super().setup()
        with lock: stats["conns"] += 1
    def log_message(self, *a): pass
    def do_GET(self):
        with lock: stats["reqs"] += 1
        if self.path.startswith("/stats"):
            with lock: body = json.dumps(stats).encode(); stats["conns"] = stats["reqs"] = 0
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if m:
            a = int(m.group(1)); b = min(int(m.group(2)) if m.group(2) else SIZE - 1, SIZE - 1)
            if a >= SIZE: self.send_response(416); self.send_header("Content-Length", "0"); self.end_headers(); return
            body = data(a, b)
            self.send_response(206); self.send_header("Content-Range", f"bytes {a}-{b}/{SIZE}")
        else:
            body = data(0, SIZE - 1); self.send_response(200)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
S(("127.0.0.1", PORT), H).serve_forever()
