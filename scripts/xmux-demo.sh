#!/usr/bin/env bash
# Demo for xmux+xterm: CLI then a limited 1fichier Range download.
set -euo pipefail
cd /tmp/proxchunk
export TERM=xterm-256color
echo "=== proxchunk $(./build/proxchunk -v) ==="
echo
./build/proxchunk -h
echo
echo "=== direct baseline: first 12 MB, one IP ==="
./build/proxchunk --direct --limit-mb 12 -s 6 -c 1 \
    -o /tmp/proxchunk/direct.dat \
    https://speedtest.1fichier.com/default.dat
echo
echo "=== multi-proxy: first 12 MB, 6 chunks via different IPs ==="
./build/proxchunk --limit-mb 12 -s 2 -c 6 -p 24 \
    -o /tmp/proxchunk/proxy.dat \
    https://speedtest.1fichier.com/default.dat
echo
echo "=== done ==="
ls -l /tmp/proxchunk/direct.dat /tmp/proxchunk/proxy.dat 2>/dev/null || true
exec bash
