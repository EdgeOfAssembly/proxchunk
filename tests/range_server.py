#!/usr/bin/env python3
"""Local HTTP server with Range support for reproducible proxchunk profiles."""

from __future__ import annotations

import argparse
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main() -> int:
    parser = argparse.ArgumentParser(description="Range-capable blob HTTP server")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--mb", type=int, default=32, help="Payload size in MiB")
    args = parser.parse_args()
    size = args.mb * 1024 * 1024
    blob = bytes([0x5A]) * size

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *a: object) -> None:
            return

        def _send_full_headers(self, code: int, length: int, extra: list[tuple[str, str]] | None = None) -> None:
            self.send_response(code)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Type", "application/octet-stream")
            if extra:
                for k, v in extra:
                    self.send_header(k, v)
            self.end_headers()

        def do_HEAD(self) -> None:
            self._send_full_headers(200, size)

        def do_GET(self) -> None:
            rng = self.headers.get("Range")
            if not rng or not rng.startswith("bytes="):
                self._send_full_headers(200, size)
                self.wfile.write(blob)
                return
            spec = rng[len("bytes=") :].strip()
            start_s, _, end_s = spec.partition("-")
            try:
                start = int(start_s) if start_s else 0
                end = int(end_s) if end_s else size - 1
            except ValueError:
                self.send_error(400)
                return
            start = max(0, start)
            end = min(size - 1, end)
            if start > end:
                self.send_error(416)
                return
            length = end - start + 1
            self._send_full_headers(
                206,
                length,
                extra=[("Content-Range", f"bytes {start}-{end}/{size}")],
            )
            self.wfile.write(blob[start : end + 1])

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"range_server {size} bytes on 127.0.0.1:{args.port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
