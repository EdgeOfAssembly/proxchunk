#!/usr/bin/env bash
# Remove files installed by install.sh. Does not delete the extracted tree.
#   ./uninstall.sh              PREFIX=/usr/local (sudo if needed)
#   ./uninstall.sh --user       ~/.local
#   ./uninstall.sh --purge      also delete ~/.config/proxchunk and ~/.cache/proxchunk
set -euo pipefail
USER_INSTALL=0
PURGE=0
PREFIX=${PREFIX:-/usr/local}

usage() {
    echo "Usage: $0 [--user] [--prefix DIR] [--purge]"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --user) USER_INSTALL=1; shift ;;
        --purge) PURGE=1; shift ;;
        --prefix) PREFIX=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done

if [[ $USER_INSTALL -eq 1 ]]; then
    PREFIX=$HOME/.local
fi

if [[ $USER_INSTALL -eq 0 && ${EUID} -ne 0 ]]; then
    if [[ -e $PREFIX/bin/proxchunk && ! -w $PREFIX/bin/proxchunk ]]; then
        exec sudo "$0" ${PURGE:+--purge} --prefix "$PREFIX"
    fi
fi

rm_one() {
    local f=$1
    if [[ -e $f || -L $f ]]; then
        rm -f "$f"
        echo "removed $f"
    fi
}

rm_one "$PREFIX/bin/proxchunk"
rm_one "$PREFIX/share/man/man1/proxchunk.1"
rm_one "$PREFIX/share/applications/proxchunk.desktop"
rm_one "$PREFIX/share/icons/hicolor/scalable/apps/proxchunk.svg"
rm_one "$PREFIX/share/icons/hicolor/1024x1024/apps/proxchunk.png"

command -v update-desktop-database >/dev/null \
    && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null \
    && gtk-update-icon-cache -f "$PREFIX/share/icons/hicolor" 2>/dev/null || true

if [[ $PURGE -eq 1 ]]; then
    # Only the invoking user's config/cache (not root's, if sudo).
    if [[ -n ${SUDO_USER:-} ]]; then
        home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    else
        home=$HOME
    fi
    if [[ -n $home && $home != / ]]; then
        rm -rf "$home/.config/proxchunk" "$home/.cache/proxchunk"
        echo "purged $home/.config/proxchunk $home/.cache/proxchunk"
    fi
fi

echo "uninstalled PREFIX=$PREFIX"
