#!/usr/bin/env python3
"""仅绑定板端回环地址的演示 HTTP 服务：强 ETag、Range、If-Range。

python3 tests/demo_server.py --file demo/source.bin --port 8765
然后下载 http://127.0.0.1:8765/file.bin。
不指定 --file 时提供可重现的 8 MiB 测试数据。
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_HEAD(self):
        self.respond(head=True)

    def do_GET(self):
        self.respond(head=False)

    def respond(self, head):
        if self.path != "/file.bin":
            self.send_error(404)
            return
        data = self.server.source.read_bytes() if self.server.source else self.server.data
        tag = '"' + hashlib.sha256(data).hexdigest() + '"'
        offset = 0
        partial = False
        if not head and self.headers.get("Range") and self.headers.get("If-Range") == tag:
            r = self.headers["Range"]
            try:
                if not r.startswith("bytes=") or not r.endswith("-"):
                    raise ValueError()
                offset = int(r[6:-1])
                if offset < 0 or offset >= len(data):
                    raise ValueError()
                partial = True
            except ValueError:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % len(data))
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
        self.send_response(206 if partial else 200)
        self.send_header("Content-Length", str(len(data) - offset))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", tag)
        self.send_header("Connection", "close")
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (offset, len(data)-1, len(data)))
        self.end_headers()
        if not head:
            try:
                self.wfile.write(data[offset:])
            except (BrokenPipeError, ConnectionResetError):
                pass


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--file", type=Path)
    p.add_argument("--port", type=int, default=8765)
    a = p.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    server.source = a.file.resolve() if a.file else None
    server.data = bytes(range(256)) * (8 * 1024 * 1024 // 256)
    data = server.source.read_bytes() if server.source else server.data
    print("Serving http://127.0.0.1:%d/file.bin" % server.server_port, flush=True)
    print("bytes=%d sha256=%s" % (len(data), hashlib.sha256(data).hexdigest()), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
