#!/usr/bin/env bash
# Build self-contained musl proxchunk-gui in /mnt/alpine.
# Tries a fully static GTK+VTE link first. If that fails, links dynamically
# with -Wl,--as-needed, bundles needed .so plus ld-musl under lib/, and a
# fully static trampoline named proxchunk-gui (runs on a glibc host).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALPINE=${ALPINE:-/mnt/alpine}
PASS=${PASS:-}
BUNDLE=${BUNDLE:-"$ROOT/gui-bundle"}
LAUNCH=${LAUNCH:-"$ROOT/proxchunk-gui"}

if [[ ! -x $ALPINE/sbin/apk ]]; then
    echo "Alpine chroot not found at $ALPINE" >&2
    exit 1
fi

sudo_run() {
    if [[ -n ${PASS:-} && -f $PASS ]]; then
        sudo -S "$@" < "$PASS"
    else
        sudo "$@"
    fi
}

sudo_run mkdir -p "$ALPINE/src/proxchunk"
sudo_run rsync -a --delete \
    --exclude build --exclude build-profile --exclude build-release \
    --exclude build-release-static --exclude .git --exclude '*.zip' \
    --exclude '*.dat' --exclude shots --exclude tmpcfg \
    --exclude proxchunk-musl-static --exclude dist --exclude gui-bundle \
    "$ROOT/" "$ALPINE/src/proxchunk/"

sudo_run chroot "$ALPINE" /bin/sh /src/proxchunk/scripts/alpine-gui-link.sh

sudo_run rm -rf "$BUNDLE"
sudo_run cp -a "$ALPINE/src/proxchunk/gui-out" "$BUNDLE"
sudo_run chown -R "$(id -u):$(id -g)" "$BUNDLE"
install -m 0755 "$BUNDLE/proxchunk-gui" "$LAUNCH"
echo "wrote $LAUNCH and $BUNDLE"
file "$LAUNCH"
ls -la "$BUNDLE" "$BUNDLE/libexec" 2>/dev/null | head
echo "lib files: $(ls "$BUNDLE/lib" 2>/dev/null | wc -l)"
