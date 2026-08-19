#!/usr/bin/env bash
set -euo pipefail
cd /tmp/proxchunk
exec ./build/proxchunk --limit-mb 12 -s 2 -c 6 -p 24 \
    -o /tmp/proxchunk/proxy.dat \
    https://speedtest.1fichier.com/default.dat
