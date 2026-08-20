#!/usr/bin/env bash
# Local Range integrity and no-Range rejection.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-"$ROOT/build/proxchunk"}
PORT=${PORT:-18765}
MB=${MB:-4}
OUT=/tmp/proxchunk-edge.bin
REF=/tmp/proxchunk-edge.ref

python3 "$ROOT/tests/range_server.py" --port "$PORT" --mb "$MB" &
srv=$!
cleanup() { kill "$srv" 2>/dev/null || true; rm -f "$OUT" "$REF"; }
trap cleanup EXIT
sleep 0.3

python3 - "$MB" "$REF" <<'PY'
import sys
mb = int(sys.argv[1])
open(sys.argv[2], "wb").write(bytes([0x5A]) * mb * 1024 * 1024)
PY

ncpu=$(nproc)
log=$("$BIN" --direct --no-progress --no-tor --no-cache --no-user-proxies \
  -o "$OUT" "http://127.0.0.1:${PORT}/blob" 2>&1) || {
    echo "$log" >&2
    echo "FAIL: default N-way download" >&2
    exit 1
}
echo "$log" | grep -q "concurrent=${ncpu} " \
    || { echo "$log" >&2; echo "FAIL: default concurrent want ${ncpu}" >&2; exit 1; }
echo "$log" | grep -q "pieces=${ncpu}" \
    || { echo "$log" >&2; echo "FAIL: default pieces want ${ncpu}" >&2; exit 1; }
echo "$log" | grep -q "Split into ${ncpu} equal pieces" \
    || { echo "$log" >&2; echo "FAIL: default should N-way split" >&2; exit 1; }
test "$(stat -c %s "$OUT")" -eq "$(stat -c %s "$REF")"
cmp -s "$OUT" "$REF"

log2=$("$BIN" --direct --no-progress --no-tor --no-cache --no-user-proxies \
  -c 2 -o "$OUT" "http://127.0.0.1:${PORT}/blob" 2>&1) || {
    echo "$log2" >&2
    echo "FAIL: -c 2 download" >&2
    exit 1
}
echo "$log2" | grep -q "concurrent=2 " \
    || { echo "$log2" >&2; echo "FAIL: -c 2 concurrent override" >&2; exit 1; }
echo "$log2" | grep -q "Split into 2 equal pieces" \
    || { echo "$log2" >&2; echo "FAIL: -c 2 should split into 2 pieces" >&2; exit 1; }
cmp -s "$OUT" "$REF"

log3=$("$BIN" --direct --no-progress --no-tor --no-cache --no-user-proxies \
  -c 2 -s 1 -o "$OUT" "http://127.0.0.1:${PORT}/blob" 2>&1) || {
    echo "$log3" >&2
    echo "FAIL: -s 1 size-split download" >&2
    exit 1
}
echo "$log3" | grep -q "chunk=1MB" \
    || { echo "$log3" >&2; echo "FAIL: -s 1 should log chunk=1MB" >&2; exit 1; }
echo "$log3" | grep -q "Split into 4 chunks of ~1 MB" \
    || { echo "$log3" >&2; echo "FAIL: 4MiB file -s 1 should be 4 chunks" >&2; exit 1; }
cmp -s "$OUT" "$REF"

# Local server without Range must fail.
python3 - "$PORT" <<'PY' &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port = int(sys.argv[1]) + 1
class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        return
    def do_GET(self):
        body = b"no-range"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    do_HEAD = do_GET
HTTPServer(("127.0.0.1", port), H).serve_forever()
PY
nr=$!
sleep 0.2
if "$BIN" --direct --no-progress --no-tor --no-cache --no-user-proxies \
  -o /tmp/proxchunk-norange.bin "http://127.0.0.1:$((PORT + 1))/x" >/dev/null 2>&1; then
    kill "$nr" 2>/dev/null || true
    echo "FAIL: no-Range URL should fail" >&2
    exit 1
fi
kill "$nr" 2>/dev/null || true

echo "test_download ok ($(stat -c %s "$OUT") bytes)"
