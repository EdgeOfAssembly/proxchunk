#!/usr/bin/env bash
# Binary package: proxchunk-<ver>-<arch>.tar.gz (runnable tree only).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.1}
ARCH=${ARCH:-$(uname -m)}
BIN=${BIN:-$ROOT/proxchunk-musl-static}
BIND=${BIND:-$ROOT/proxchunkd-musl-static}
NAME=proxchunk-${VER}-${ARCH}
STAGE=$ROOT/dist/$NAME
TGZ=$ROOT/dist/${NAME}.tar.gz

if [[ ! -x $BIN ]]; then
    echo "missing $BIN — run scripts/build-musl-static.sh first" >&2
    exit 1
fi
if [[ ! -x $BIND ]]; then
    echo "missing $BIND — run scripts/build-musl-static.sh first" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/icons" "$STAGE/doc"

install -m 0755 "$BIN" "$STAGE/proxchunk"
install -m 0755 "$BIND" "$STAGE/proxchunkd"
if [[ -x $ROOT/gui-bundle/proxchunk-gui ]]; then
    install -m 0755 "$ROOT/gui-bundle/proxchunk-gui" "$STAGE/proxchunk-gui"
elif [[ -x $ROOT/proxchunk-gui ]]; then
    install -m 0755 "$ROOT/proxchunk-gui" "$STAGE/proxchunk-gui"
fi
if [[ -d $ROOT/gui-bundle/lib ]]; then
    cp -a "$ROOT/gui-bundle/lib" "$STAGE/lib"
fi
install -m 0755 "$ROOT/scripts/install.sh" "$STAGE/install.sh"
install -m 0755 "$ROOT/scripts/uninstall.sh" "$STAGE/uninstall.sh"
install -m 0644 "$ROOT/desktop/proxchunk.desktop" "$STAGE/proxchunk.desktop"
install -m 0644 "$ROOT/README.user.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 0644 "$ROOT/doc/proxchunk.1" "$STAGE/doc/proxchunk.1"
install -m 0644 "$ROOT/doc/proxchunkd.1" "$STAGE/doc/proxchunkd.1"
install -m 0644 "$ROOT/icons/proxchunk.svg" "$STAGE/icons/proxchunk.svg"
install -m 0644 "$ROOT/icons/proxchunk.png" "$STAGE/icons/proxchunk.png"
if [[ -f $ROOT/icons/proxchunk.xpm ]]; then
    install -m 0644 "$ROOT/icons/proxchunk.xpm" "$STAGE/icons/proxchunk.xpm"
fi
if [[ -d $ROOT/icons/hicolor ]]; then
    cp -a "$ROOT/icons/hicolor" "$STAGE/icons/"
fi
if [[ -d $ROOT/icons/pixmaps ]]; then
    cp -a "$ROOT/icons/pixmaps" "$STAGE/icons/"
fi

rm -f "$TGZ"
tar -C "$ROOT/dist" -czf "$TGZ" "$NAME"
echo "wrote $TGZ"
tar -tzf "$TGZ"
ls -l "$TGZ"
