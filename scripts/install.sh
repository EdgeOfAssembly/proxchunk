#!/usr/bin/env bash
# Install proxchunk. Default PREFIX=/usr/local (sudo if needed).
#   ./install.sh           system
#   ./install.sh --user    ~/.local (no sudo)
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
USER_INSTALL=0
PREFIX=${PREFIX:-/usr/local}

usage() {
    echo "Usage: $0 [--user] [--prefix DIR]"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --user) USER_INSTALL=1; shift ;;
        --prefix) PREFIX=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done

if [[ $USER_INSTALL -eq 1 ]]; then
    PREFIX=${PREFIX:-$HOME/.local}
    if [[ $PREFIX == /usr/local ]]; then
        PREFIX=$HOME/.local
    fi
fi

if [[ $USER_INSTALL -eq 0 && ${EUID} -ne 0 ]]; then
    if [[ ! -d $PREFIX/bin || ! -w $PREFIX/bin ]]; then
        exec sudo "$0" ${USER_INSTALL:+--user} --prefix "$PREFIX"
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
