#!/usr/bin/env bash
# System install into PREFIX (default /usr/local). Re-execs with sudo if needed.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
PREFIX=${PREFIX:-/usr/local}

if [[ ${EUID} -ne 0 ]]; then
    if [[ ! -d $PREFIX/bin || ! -w $PREFIX/bin ]]; then
        exec sudo "$0" "$@"
    fi
fi

BIN=$DIR/proxchunk
[[ -x $BIN ]] || { echo "missing $BIN" >&2; exit 1; }

install -d "$PREFIX/bin"
install -m 0755 "$BIN" "$PREFIX/bin/proxchunk"

if [[ -f $DIR/doc/proxchunk.1 ]]; then
    install -d "$PREFIX/share/man/man1"
    install -m 0644 "$DIR/doc/proxchunk.1" "$PREFIX/share/man/man1/proxchunk.1"
fi

if [[ -f $DIR/proxchunk.desktop ]]; then
    install -d "$PREFIX/share/applications"
    install -m 0644 "$DIR/proxchunk.desktop" "$PREFIX/share/applications/proxchunk.desktop"
fi

if [[ -f $DIR/icons/proxchunk.svg ]]; then
    install -d "$PREFIX/share/icons/hicolor/scalable/apps"
    install -m 0644 "$DIR/icons/proxchunk.svg" \
        "$PREFIX/share/icons/hicolor/scalable/apps/proxchunk.svg"
fi
if [[ -f $DIR/icons/proxchunk.png ]]; then
    install -d "$PREFIX/share/icons/hicolor/1024x1024/apps"
    install -m 0644 "$DIR/icons/proxchunk.png" \
        "$PREFIX/share/icons/hicolor/1024x1024/apps/proxchunk.png"
fi

command -v update-desktop-database >/dev/null \
    && update-desktop-database "$PREFIX/share/applications" || true
command -v gtk-update-icon-cache >/dev/null \
    && gtk-update-icon-cache -f "$PREFIX/share/icons/hicolor" 2>/dev/null || true

echo "installed PREFIX=$PREFIX"
echo "  $PREFIX/bin/proxchunk"
echo "  $PREFIX/share/man/man1/proxchunk.1"
echo "  $PREFIX/share/applications/proxchunk.desktop"
echo "  $PREFIX/share/icons/hicolor/scalable/apps/proxchunk.svg"
