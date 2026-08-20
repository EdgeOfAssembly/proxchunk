#!/usr/bin/env bash
# proxchunkd CLI contract, lifecycle, IPC, and download-via-daemon.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DAEMON=${1:-"$ROOT/build/proxchunkd"}
CLI=${2:-"$ROOT/build/proxchunk"}

fail() { echo "FAIL: $*" >&2; exit 1; }

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

wait_ping() {
    local sock=$1
    local i
    for i in $(seq 1 80); do
        if "$DAEMON" --status --socket "$sock" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_live() {
    local sock=$1
    local i out
    for i in $(seq 1 100); do
        out=$("$DAEMON" --status --socket "$sock" 2>/dev/null) || true
        if echo "$out" | grep -qE 'live=[1-9]'; then
            return 0
        fi
        sleep 0.1
    done
    echo "status: ${out:-none}" >&2
    return 1
}

# --- A. CLI contract ---
help=$("$DAEMON" -h 2>&1) || fail "-h exit"
echo "$help" | grep -q Usage || fail "help Usage"
echo "$help" | grep -q -- '--foreground' || fail "help --foreground"
echo "$help" | grep -q -- '--stop' || fail "help --stop"
echo "$help" | grep -q -- '--status' || fail "help --status"
echo "$help" | grep -q -- '--no-fetch' || fail "help --no-fetch"
echo "$help" | grep -qi 'no arguments' || fail "help should say no-args daemonizes"

"$DAEMON" --help >/dev/null 2>&1 || fail "--help"
"$DAEMON" --foreground -h >/dev/null 2>&1 || fail "--foreground -h still help"

ver=$("$DAEMON" -v)
echo "$ver" | grep -q '^proxchunkd ' || fail "-v prefix"
test "$ver" = "$("$DAEMON" --version)" || fail "-v/--version mismatch"
test "$ver" = "proxchunkd 1.1" || fail "version want 'proxchunkd 1.1' got '$ver'"

"$DAEMON" --not-a-flag >/dev/null 2>&1 && fail "unknown option should fail"
"$DAEMON" -p 0 >/dev/null 2>&1 && fail "-p 0"
"$DAEMON" -r -1 >/dev/null 2>&1 && fail "-r -1"

tmp=$(mktemp -d)
rsrv=0
prx=0
fgpid=0
dpid=0
trap 'kill $rsrv $prx $fgpid $dpid 2>/dev/null || true; "$DAEMON" --stop --socket "$tmp/d.sock" --pid-file "$tmp/d.pid" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT

SOCK=$tmp/d.sock
PIDF=$tmp/d.pid
LOG=$tmp/d.log
PF=$tmp/proxies.txt

# --status when down
if "$DAEMON" --status --socket "$SOCK" >/dev/null 2>&1; then
    fail "--status down should exit 1"
fi
stout=$("$DAEMON" --status --socket "$SOCK" 2>&1) || true
echo "$stout" | grep -qi 'not running' || fail "--status down message"

# --stop when down is idempotent exit 0
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" >/dev/null 2>&1 || fail "--stop when down"

RPORT=$(free_port)
PPORT=$(free_port)
python3 "$ROOT/tests/range_server.py" --port "$RPORT" --bytes 65536 &
rsrv=$!
python3 "$ROOT/tests/tiny_http_proxy.py" --port "$PPORT" &
prx=$!
sleep 0.3
echo "http://127.0.0.1:${PPORT}" > "$PF"

start_fg() {
    "$DAEMON" --foreground --no-fetch --no-tor --no-cache --no-user-proxies \
        --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT}/blob" \
        --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 &
    fgpid=$!
    wait_ping "$SOCK" || { echo "log:"; cat "$LOG" 2>/dev/null || true; fail "PING after --foreground"; }
}

# --- B. Foreground + SIGINT ---
start_fg
wait_live "$SOCK" || fail "live>=1 after score"
stat=$("$DAEMON" --status --socket "$SOCK")
echo "$stat" | grep -q 'live=' || fail "status format: $stat"
kill -INT "$fgpid"
st=0
wait "$fgpid" || st=$?
[[ $st -eq 0 || $st -eq 127 ]] || fail "SIGINT exit want 0 got $st"
test ! -e "$SOCK" || fail "SIGINT leftover socket"
test ! -e "$PIDF" || fail "SIGINT leftover pid"
if grep -qiE 'Aborted|core dumped|Fatal' "$LOG" 2>/dev/null; then
    fail "SIGINT log looks like abort"
fi

# SIGTERM
start_fg
kill -TERM "$fgpid"
st=0
wait "$fgpid" || st=$?
[[ $st -eq 0 || $st -eq 127 ]] || fail "SIGTERM exit want 0 got $st"
test ! -e "$SOCK" || fail "SIGTERM leftover socket"
test ! -e "$PIDF" || fail "SIGTERM leftover pid"

# SIGQUIT
start_fg
kill -QUIT "$fgpid"
st=0
wait "$fgpid" || st=$?
[[ $st -eq 0 || $st -eq 127 ]] || fail "SIGQUIT exit want 0 got $st"
test ! -e "$SOCK" || fail "SIGQUIT leftover socket"

# --- C. Daemonize + --stop ---
"$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 \
    || fail "daemonize start"
wait_ping "$SOCK" || fail "PING after daemonize"
test -e "$SOCK" || fail "daemonize socket missing"
test -e "$PIDF" || fail "daemonize pid missing"
dpid=$(cat "$PIDF")
kill -0 "$dpid" || fail "pidfile pid not live"
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" || fail "--stop"
test ! -e "$SOCK" || fail "--stop leftover socket"
test ! -e "$PIDF" || fail "--stop leftover pid"
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" || fail "idempotent --stop"

# --- D. Stale socket + dead pidfile ---
sleep 30 &
deadpid=$!
kill "$deadpid" 2>/dev/null || true
wait "$deadpid" 2>/dev/null || true
mkdir -p "$(dirname "$PIDF")"
echo "$deadpid" > "$PIDF"
python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.bind('$SOCK')"
"$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 \
    || fail "start with stale sock/pid"
wait_ping "$SOCK" || fail "PING after stale recovery"
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" || fail "stop after stale"

# --- E. Double start ---
"$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 \
    || fail "double-start first"
wait_ping "$SOCK" || fail "PING before double start"
out=$("$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 2>&1) \
    || fail "double start should exit 0"
echo "$out" | grep -qi 'already running' || fail "double start message: $out"

# --- I. IPC busy leak / protocol ---
wait_live "$SOCK" || fail "live before IPC tests"
python3 - "$SOCK" <<'PY' || fail "IPC acquire/drop"
import socket, sys, time
sock = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sock)
s.sendall(b"HELLO 1\n")
hello = s.recv(4096)
if not hello.startswith(b"OK 1"):
    raise SystemExit(f"hello {hello!r}")
s.sendall(b"ACQUIRE\n")
acq = s.recv(4096)
if not acq.startswith(b"OK "):
    raise SystemExit(f"acquire {acq!r}")
s.close()
PY
sleep 0.3
stat=$("$DAEMON" --status --socket "$SOCK")
echo "$stat" | grep -q 'busy=0' || fail "busy leak after drop: $stat"

python3 - "$SOCK" <<'PY' || fail "IPC acquire/release"
import socket, sys
sock = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sock)
s.sendall(b"HELLO 1\n")
s.recv(4096)
s.sendall(b"ACQUIRE\n")
acq = s.recv(4096).decode()
url = acq.split()[1]
s.sendall(f"RELEASE {url} ok 1.0\n".encode())
rel = s.recv(4096)
if not rel.startswith(b"OK"):
    raise SystemExit(f"release {rel!r}")
s.sendall(b"STATS\n")
print(s.recv(4096).decode(), end="")
s.close()
PY
stat=$("$DAEMON" --status --socket "$SOCK")
echo "$stat" | grep -q 'busy=0' || fail "busy after RELEASE: $stat"

python3 - "$SOCK" <<'PY' || fail "unknown command / oversize"
import socket, sys
sock = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sock)
s.sendall(b"HELLO 1\n")
s.recv(4096)
s.sendall(b"NOPE\n")
r = s.recv(4096)
if b"unknown" not in r:
    raise SystemExit(f"unknown {r!r}")
s.sendall(b"x" * 5000)
s.close()
PY
"$DAEMON" --status --socket "$SOCK" >/dev/null || fail "daemon died after oversize"

# --- G. Download via daemon ---
kill "$rsrv" 2>/dev/null || true
wait "$rsrv" 2>/dev/null || true
RPORT2=$(free_port)
python3 "$ROOT/tests/range_server.py" --port "$RPORT2" --mb 1 &
rsrv=$!
sleep 0.3
# re-score against the new origin
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" >/dev/null 2>&1 || true
"$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT2}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 \
    || fail "restart daemon for download"
wait_live "$SOCK" || fail "live before download"

OUT=$tmp/out.bin
REF=$tmp/ref.bin
python3 - "$REF" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes([0x5A]) * 1024 * 1024)
PY
log=$("$CLI" --socket "$SOCK" --no-progress --no-tor --no-cache --no-user-proxies \
    -c 2 -o "$OUT" "http://127.0.0.1:${RPORT2}/blob" 2>&1) || {
    echo "$log" >&2
    fail "download via daemon"
}
cmp -s "$OUT" "$REF" || fail "download bytes mismatch"

# --- H. Probe 500 direct / 206 via proxy ---
kill "$rsrv" 2>/dev/null || true
wait "$rsrv" 2>/dev/null || true
RPORT3=$(free_port)
python3 "$ROOT/tests/range_server.py" --port "$RPORT3" --mb 1 --direct-probe-500 &
rsrv=$!
sleep 0.3
"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" >/dev/null 2>&1 || true
"$DAEMON" --no-fetch --no-tor --no-cache --no-user-proxies \
    --proxy-file "$PF" --test-url "http://127.0.0.1:${RPORT3}/blob" \
    --socket "$SOCK" --pid-file "$PIDF" --log-file "$LOG" --refresh 0 \
    || fail "restart daemon for probe-500"
wait_live "$SOCK" || fail "live before probe-500"
if "$CLI" --direct --no-progress --no-tor --no-cache --no-user-proxies \
    -o "$OUT" "http://127.0.0.1:${RPORT3}/blob" >/dev/null 2>&1; then
    fail "--direct should fail when tiny Range probe is 500"
fi
log=$("$CLI" --socket "$SOCK" --no-progress --no-tor --no-cache --no-user-proxies \
    -c 2 -o "$OUT" "http://127.0.0.1:${RPORT3}/blob" 2>&1) || {
    echo "$log" >&2
    fail "proxied download should succeed when direct probe 500s"
}
cmp -s "$OUT" "$REF" || fail "probe-500 download mismatch"

"$DAEMON" --stop --socket "$SOCK" --pid-file "$PIDF" || fail "final --stop"

echo "test_proxchunkd ok"
