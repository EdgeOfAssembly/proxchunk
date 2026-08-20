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
    parser.add_argument("--bytes", type=int, default=0, help="Payload size in bytes (overrides --mb)")
    parser.add_argument(
        "--require-proxy-header",
        action="store_true",
        help="HTTP 500 on Range unless X-Proxchunk-Proxy is set (simulates IA blocks)",
    )
    parser.add_argument(
        "--direct-probe-500",
        action="store_true",
        help="HTTP 500 on Range 0-0 unless X-Proxchunk-Proxy is set",
    )
    args = parser.parse_args()
    size = args.bytes if args.bytes > 0 else args.mb * 1024 * 1024
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
            via = self.headers.get("X-Proxchunk-Proxy") == "1"
            if rng and args.require_proxy_header and not via:
                self.send_error(500, "direct range blocked")
                return
            if rng and args.direct_probe_500 and not via:
                spec = rng[len("bytes=") :].strip() if rng.startswith("bytes=") else ""
                if spec == "0-0" or spec.startswith("0-0"):
                    self.send_error(500, "direct range blocked")
                    return
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
