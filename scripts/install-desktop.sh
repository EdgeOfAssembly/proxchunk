#!/usr/bin/env bash
# Register a menu entry for this extracted tree (absolute Exec/Icon paths).
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
BIN=$DIR/proxchunk
ICON=$DIR/icons/proxchunk.svg
[[ -x $BIN ]] || { echo "missing $BIN" >&2; exit 1; }
[[ -f $ICON ]] || ICON=$DIR/icons/proxchunk.png

APPDIR=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$APPDIR"
DESK=$APPDIR/proxchunk.desktop
cat > "$DESK" <<EOF
[Desktop Entry]
Type=Application
Version=1.5
Name=proxchunk
GenericName=Range downloader
Comment=Multi-proxy HTTP Range chunked downloader
Exec=$BIN
Icon=$ICON
Path=$DIR
Terminal=true
Categories=Network;FileTransfer;
Keywords=download;proxy;range;http;
StartupNotify=false
EOF
chmod 644 "$DESK"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPDIR" || true
echo "installed $DESK"
echo "Exec=$BIN"
echo "Icon=$ICON"
