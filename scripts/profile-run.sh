#!/usr/bin/env bash
# Reproducible local Range download for gprof + wall time.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-"$ROOT/build-profile/proxchunk"}
PORT=${PORT:-8765}
MB=${MB:-32}
OUT=${OUT:-/tmp/proxchunk-profile.bin}
PROF=${PROF:-"$ROOT/profile.txt"}

if [[ ! -x $BIN ]]; then
    echo "missing $BIN (run make profile)" >&2
    exit 1
fi

python3 "$ROOT/tests/range_server.py" --port "$PORT" --mb "$MB" &
srv=$!
trap 'kill $srv 2>/dev/null || true' EXIT
sleep 0.3

rm -f "$OUT" "$ROOT/gmon.out" gmon.out
echo "==> $BIN --direct $MB MiB from 127.0.0.1:$PORT"
/usr/bin/time -f 'wall %e s  rss %M KB' "$BIN" --direct --no-progress \
    -c 8 -s 4 -o "$OUT" "http://127.0.0.1:${PORT}/blob"
sz=$(stat -c %s "$OUT")
echo "output $sz bytes"
gmon=gmon.out
[[ -f $ROOT/gmon.out ]] && gmon=$ROOT/gmon.out
[[ -f gmon.out ]] && gmon=gmon.out
if [[ -f $gmon ]]; then
    gprof "$BIN" "$gmon" > "$PROF"
    echo "gprof -> $PROF"
    head -n 40 "$PROF"
else
    echo "no gmon.out (binary not built with -pg?)"
fi
