#!/usr/bin/env bash
set -euo pipefail
export TERM=xterm-256color
TGZ=/tmp/proxchunk/dist/proxchunk-1.7.tar.gz
DEST=/tmp/proxchunk-extract
rm -rf "$DEST"
mkdir -p "$DEST"
echo "=== extract $TGZ ==="
tar -xzf "$TGZ" -C "$DEST"
cd "$DEST/proxchunk-1.7"
echo
echo "=== run in place (no install) ==="
./proxchunk -v
./proxchunk -h | head -8
echo
echo "=== sudo ./install.sh → /usr/local ==="
if [[ -r /tmp/password.txt ]]; then
    sudo -S ./install.sh < /tmp/password.txt
else
    sudo ./install.sh
fi
echo
echo "=== standard locations ==="
command -v proxchunk
proxchunk -v
ls -l /usr/local/bin/proxchunk
ls -l /usr/local/share/man/man1/proxchunk.1
ls -l /usr/local/share/applications/proxchunk.desktop
ls -l /usr/local/share/icons/hicolor/scalable/apps/proxchunk.svg
ls -l /usr/local/share/icons/hicolor/1024x1024/apps/proxchunk.png
echo
echo "=== man proxchunk (first lines) ==="
man -l /usr/local/share/man/man1/proxchunk.1 | sed -n '1,18p'
echo
echo DEMO_OK
exec bash
