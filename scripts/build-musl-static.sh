#!/usr/bin/env bash
# Build a portable musl-static proxchunk inside an Alpine chroot.
#   ALPINE  Alpine root (default: /mnt/alpine)
#   PASS    Optional file for `sudo -S` (non-interactive)
#   OUT     Output path (default: ./proxchunk-musl-static)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALPINE=${ALPINE:-/mnt/alpine}
PASS=${PASS:-}
OUT=${OUT:-"$ROOT/proxchunk-musl-static"}

if [[ ! -x $ALPINE/sbin/apk ]]; then
    echo "Alpine chroot not found at $ALPINE" >&2
    exit 1
fi

sudo_run() {
    if [[ -n $PASS && -f $PASS ]]; then
        sudo -S "$@" < "$PASS"
    else
        sudo "$@"
    fi
}

sudo_run mkdir -p "$ALPINE/src/proxchunk" "$ALPINE/usr/local/include"
sudo_run rsync -a --delete \
    --exclude build --exclude build-profile --exclude build-release \
    --exclude build-release-static --exclude .git --exclude '*.zip' \
    --exclude '*.dat' --exclude shots --exclude tmpcfg \
    --exclude proxchunk-musl-static --exclude dist \
    "$ROOT/" "$ALPINE/src/proxchunk/"
sudo_run rm -rf "$ALPINE/usr/local/include/libsf"
sudo_run cp -a /usr/local/include/libsf "$ALPINE/usr/local/include/"

sudo_run chroot "$ALPINE" /bin/sh -c '
set -e
cd /src/proxchunk
g++ -std=gnu++23 -O3 -DNDEBUG -ffunction-sections -fdata-sections \
  -fno-pie -no-pie -static -static-libgcc -static-libstdc++ \
  -Iinclude -I/usr/local/include \
  -DPROXCHUNK_VERSION=\"1.0\" -DCURL_STATICLIB \
  src/proxchunk.cpp \
  -Wl,--gc-sections -Wl,-static -Wl,--build-id=none \
  $(pkg-config --static --libs libcurl) \
  -o /src/proxchunk/proxchunk-musl-static
strip --strip-all -R .note.gnu.build-id /src/proxchunk/proxchunk-musl-static
file /src/proxchunk/proxchunk-musl-static
'

sudo_run cp "$ALPINE/src/proxchunk/proxchunk-musl-static" "$OUT"
sudo_run chown "$(id -u):$(id -g)" "$OUT"
chmod +x "$OUT"
file "$OUT"
ldd "$OUT" 2>&1 | head -3 || true
"$OUT" -v
echo "wrote $OUT"
