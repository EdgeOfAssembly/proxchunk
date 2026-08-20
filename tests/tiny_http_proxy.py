#!/usr/bin/env python3
"""Minimal HTTP forward proxy for local proxchunkd tests (no CONNECT)."""

from __future__ import annotations

import argparse
import http.client
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


def main() -> int:
    parser = argparse.ArgumentParser(description="Tiny HTTP forward proxy")
    parser.add_argument("--port", type=int, default=18780)
    args = parser.parse_args()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt: str, *a: object) -> None:
            return

        def do_HEAD(self) -> None:
            self._forward()

        def do_GET(self) -> None:
            self._forward()

        def _forward(self) -> None:
            parsed = urlparse(self.path)
            if parsed.scheme != "http" or not parsed.hostname:
                self.send_error(400, "absolute http URL required")
                return
            port = parsed.port or 80
            path = parsed.path or "/"
            if parsed.query:
                path = path + "?" + parsed.query
            extra = []
            for key, value in self.headers.items():
                lk = key.lower()
                if lk in {"host", "proxy-connection", "connection"}:
                    continue
                extra.append((key, value))
            extra.append(("X-Proxchunk-Proxy", "1"))
            conn = http.client.HTTPConnection(parsed.hostname, port, timeout=30)
            try:
                conn.request(self.command, path, headers=dict(extra))
                resp = conn.getresponse()
                body = resp.read()
            except OSError:
                self.send_error(502, "upstream failed")
                return
            finally:
                conn.close()
            self.send_response(resp.status, resp.reason)
            for key, value in resp.getheaders():
                if key.lower() in {"transfer-encoding", "connection"}:
                    continue
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD" and body:
                self.wfile.write(body)

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"tiny_http_proxy on 127.0.0.1:{args.port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
