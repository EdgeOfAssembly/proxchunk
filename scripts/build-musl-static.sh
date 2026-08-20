#!/usr/bin/env bash
# Build a portable musl-static proxchunk inside /mnt/alpine (chroot).
# Requires: Alpine root at /mnt/alpine, sudo, /tmp/password.txt
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALPINE=${ALPINE:-/mnt/alpine}
PASS=${PASS:-/tmp/password.txt}
OUT=${OUT:-"$ROOT/proxchunk-musl-static"}

if [[ ! -x $ALPINE/sbin/apk ]]; then
    echo "Alpine chroot not found at $ALPINE" >&2
    exit 1
fi
if [[ ! -f $PASS ]]; then
    echo "sudo password file missing: $PASS" >&2
    exit 1
fi

sudo -S mkdir -p "$ALPINE/src/proxchunk" "$ALPINE/usr/local/include" < "$PASS"
sudo -S rsync -a --delete \
    --exclude build --exclude build-profile --exclude build-release \
    --exclude build-release-static --exclude .git --exclude '*.zip' \
    --exclude '*.dat' --exclude shots --exclude tmpcfg \
    --exclude proxchunk-musl-static \
    "$ROOT/" "$ALPINE/src/proxchunk/" < "$PASS"
sudo -S rm -rf "$ALPINE/usr/local/include/libsf" < "$PASS"
sudo -S cp -a /usr/local/include/libsf "$ALPINE/usr/local/include/" < "$PASS"

sudo -S chroot "$ALPINE" /bin/sh -c '
set -e
cd /src/proxchunk
g++ -std=gnu++23 -O3 -DNDEBUG -ffunction-sections -fdata-sections \
  -fno-pie -no-pie -static -static-libgcc -static-libstdc++ \
  -Iinclude -I/usr/local/include \
  -DPROXCHUNK_VERSION="1.7" -DCURL_STATICLIB \
  src/proxchunk.cpp \
  -Wl,--gc-sections -Wl,-static \
  $(pkg-config --static --libs libcurl) \
  -o /src/proxchunk/proxchunk-musl-static
strip --strip-all /src/proxchunk/proxchunk-musl-static
file /src/proxchunk/proxchunk-musl-static
' < "$PASS"

sudo -S cp "$ALPINE/src/proxchunk/proxchunk-musl-static" "$OUT" < "$PASS"
sudo -S chown "$(id -u):$(id -g)" "$OUT" < "$PASS"
chmod +x "$OUT"
file "$OUT"
ldd "$OUT" 2>&1 | head -3 || true
"$OUT" -v
echo "wrote $OUT"
