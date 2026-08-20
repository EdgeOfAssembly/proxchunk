#!/usr/bin/env bash
# Binary package: proxchunk-<ver>-<arch>.tar.gz (runnable tree only).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.0}
ARCH=${ARCH:-$(uname -m)}
BIN=${BIN:-$ROOT/proxchunk-musl-static}
NAME=proxchunk-${VER}-${ARCH}
STAGE=$ROOT/dist/$NAME
TGZ=$ROOT/dist/${NAME}.tar.gz

if [[ ! -x $BIN ]]; then
    echo "missing $BIN — run scripts/build-musl-static.sh first" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/icons" "$STAGE/doc"

install -m 0755 "$BIN" "$STAGE/proxchunk"
if [[ -x $ROOT/proxchunk-gui ]]; then
    install -m 0755 "$ROOT/proxchunk-gui" "$STAGE/proxchunk-gui"
fi
if [[ -d $ROOT/gui-bundle/lib ]]; then
    cp -a "$ROOT/gui-bundle/lib" "$STAGE/lib"
fi
if [[ -d $ROOT/gui-bundle/libexec ]]; then
    cp -a "$ROOT/gui-bundle/libexec" "$STAGE/libexec"
fi
if [[ -d $ROOT/gui-bundle/share ]]; then
    cp -a "$ROOT/gui-bundle/share" "$STAGE/share"
fi
# Also accept a pre-assembled tree next to the CLI.
if [[ -d $ROOT/lib && ! -d $STAGE/lib ]]; then
    cp -a "$ROOT/lib" "$STAGE/lib"
fi
if [[ -d $ROOT/libexec && ! -d $STAGE/libexec ]]; then
    cp -a "$ROOT/libexec" "$STAGE/libexec"
fi
install -m 0755 "$ROOT/scripts/install.sh" "$STAGE/install.sh"
install -m 0755 "$ROOT/scripts/uninstall.sh" "$STAGE/uninstall.sh"
install -m 0644 "$ROOT/desktop/proxchunk.desktop" "$STAGE/proxchunk.desktop"
install -m 0644 "$ROOT/README.user.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 0644 "$ROOT/doc/proxchunk.1" "$STAGE/doc/proxchunk.1"
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
