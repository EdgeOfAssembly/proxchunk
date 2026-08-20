#!/usr/bin/env bash
# Install proxchunk. Default PREFIX=/usr/local (sudo if needed).
#   ./install.sh           system
#   ./install.sh --user    ~/.local (no sudo)
#
# Icons: standard hicolor sizes + pixmaps fallback so LXDE/XFCE/GNOME
# all find Icon=proxchunk without requiring rsvg on the target machine.
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
    PREFIX=$HOME/.local
fi

if [[ $USER_INSTALL -eq 0 && ${EUID} -ne 0 ]]; then
    if [[ ! -d $PREFIX/bin || ! -w $PREFIX/bin ]]; then
        exec sudo "$0" --prefix "$PREFIX"
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

HICOLOR=$PREFIX/share/icons/hicolor
if [[ -f $DIR/icons/hicolor/index.theme ]]; then
    install -d "$HICOLOR"
    install -m 0644 "$DIR/icons/hicolor/index.theme" "$HICOLOR/index.theme"
fi
if [[ -f $DIR/icons/proxchunk.svg ]]; then
    install -d "$HICOLOR/scalable/apps"
    install -m 0644 "$DIR/icons/proxchunk.svg" "$HICOLOR/scalable/apps/proxchunk.svg"
fi
for sz in 16 22 24 32 48 64 128 256 512; do
    src=$DIR/icons/hicolor/${sz}x${sz}/apps/proxchunk.png
    if [[ -f $src ]]; then
        install -d "$HICOLOR/${sz}x${sz}/apps"
        install -m 0644 "$src" "$HICOLOR/${sz}x${sz}/apps/proxchunk.png"
    fi
done
# 1024 optional extra
if [[ -f $DIR/icons/proxchunk.png ]]; then
    install -d "$HICOLOR/1024x1024/apps"
    install -m 0644 "$DIR/icons/proxchunk.png" "$HICOLOR/1024x1024/apps/proxchunk.png"
fi
# Classic pixmap fallback (LXDE / older GTK menus)
if [[ -f $DIR/icons/pixmaps/proxchunk.png ]]; then
    install -d "$PREFIX/share/pixmaps"
    install -m 0644 "$DIR/icons/pixmaps/proxchunk.png" "$PREFIX/share/pixmaps/proxchunk.png"
fi

command -v update-desktop-database >/dev/null \
    && update-desktop-database "$PREFIX/share/applications" || true
if command -v gtk-update-icon-cache >/dev/null && [[ -d $HICOLOR ]]; then
    gtk-update-icon-cache -f -t "$HICOLOR" 2>/dev/null || true
fi
command -v gtk4-update-icon-cache >/dev/null && [[ -d $HICOLOR ]] \
    && gtk4-update-icon-cache -f -t "$HICOLOR" 2>/dev/null || true
command -v lxpanelctl >/dev/null && lxpanelctl restart 2>/dev/null || true
command -v xfce4-panel >/dev/null && xfce4-panel -r 2>/dev/null || true

echo "installed PREFIX=$PREFIX"
echo "  $PREFIX/bin/proxchunk"
echo "  $PREFIX/share/pixmaps/proxchunk.png"
echo "  $PREFIX/share/icons/hicolor/{16x16..512x512,scalable}/apps/proxchunk.*"
echo "  $PREFIX/share/applications/proxchunk.desktop"
