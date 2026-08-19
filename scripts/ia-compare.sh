#!/usr/bin/env bash
# Single-connection wget baseline, then proxchunk. Do not use wget2 (it chunks).
export HOME=/home/wizard
export XDG_CACHE_HOME=/home/wizard/.cache
export TERM=xterm-256color
cd /tmp/proxchunk
URL='https://archive.org/download/doom-wads/Maximum%20Doom%202.zip'

echo "=== $(wget --version | head -1)  — one TCP connection ==="
echo "Line cap ~30 Mbps = 3.75 MB/s"
echo "File: Maximum Doom 2.zip  131284120 bytes (~125 MiB)  Range: yes"
echo

rm -f /tmp/proxchunk/ia-wget.zip /tmp/proxchunk/ia-proxy.zip

echo "=== 1) wget (single IP) ==="
wget --progress=bar:force:noscroll -O /tmp/proxchunk/ia-wget.zip "$URL"
echo
echo "wget exit $?  size=$(stat -c %s /tmp/proxchunk/ia-wget.zip 2>/dev/null || echo 0)"
echo

echo "=== 2) proxchunk (multi-proxy Range) ==="
./build/proxchunk --show-proxies --limit-mb 125 -s 8 -c 8 -p 24 \
  -o /tmp/proxchunk/ia-proxy.zip "$URL"
echo
echo "=== compare ==="
ls -l /tmp/proxchunk/ia-wget.zip /tmp/proxchunk/ia-proxy.zip 2>/dev/null
echo DONE
exec bash
