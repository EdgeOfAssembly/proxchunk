#!/usr/bin/env bash
# Pack musl-static proxchunk into proxchunk-<ver>.tar.gz (unix permissions).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.7}
BIN=${BIN:-$ROOT/proxchunk-musl-static}
STAGE=$ROOT/dist/proxchunk-$VER
TGZ=$ROOT/dist/proxchunk-$VER.tar.gz

if [[ ! -x $BIN ]]; then
    echo "missing $BIN — run scripts/build-musl-static.sh first" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/icons" "$STAGE/doc"

install -m 0755 "$BIN" "$STAGE/proxchunk"
install -m 0755 "$ROOT/scripts/install.sh" "$STAGE/install.sh"
install -m 0755 "$ROOT/scripts/install-desktop.sh" "$STAGE/install-desktop.sh"
install -m 0644 "$ROOT/desktop/proxchunk.desktop" "$STAGE/proxchunk.desktop"
install -m 0644 "$ROOT/README.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 0644 "$ROOT/doc/proxchunk.1" "$STAGE/doc/proxchunk.1"
install -m 0644 "$ROOT/icons/proxchunk.svg" "$STAGE/icons/proxchunk.svg"
install -m 0644 "$ROOT/icons/proxchunk.png" "$STAGE/icons/proxchunk.png"

cat > "$STAGE/README-PORTABLE.txt" <<EOF
proxchunk $VER — portable tree

  tar -xzf proxchunk-$VER.tar.gz
  cd proxchunk-$VER
  ./proxchunk --help              # use immediately, no install
  sudo ./install.sh               # /usr/local/bin, man, desktop, icons
  ./install-desktop.sh            # user menu only (no sudo)

After sudo ./install.sh, "proxchunk" and "man proxchunk" work from any directory.
EOF
chmod 0644 "$STAGE/README-PORTABLE.txt"

rm -f "$TGZ"
tar -C "$ROOT/dist" -czf "$TGZ" "proxchunk-$VER"
echo "wrote $TGZ"
tar -tzf "$TGZ"
ls -l "$TGZ"
