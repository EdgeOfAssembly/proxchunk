#!/usr/bin/env bash
# Pack musl-static proxchunk + desktop/icons/docs into proxchunk-<ver>.zip
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.7}
BIN=${BIN:-$ROOT/proxchunk-musl-static}
STAGE=$ROOT/dist/proxchunk-$VER
ZIP=$ROOT/dist/proxchunk-$VER.zip

if [[ ! -x $BIN ]]; then
    echo "missing $BIN — run scripts/build-musl-static.sh first" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/icons" "$STAGE/doc"

install -m 0755 "$BIN" "$STAGE/proxchunk"
install -m 0755 "$ROOT/scripts/install-desktop.sh" "$STAGE/install-desktop.sh"
install -m 0644 "$ROOT/desktop/proxchunk.desktop" "$STAGE/proxchunk.desktop"
install -m 0644 "$ROOT/README.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 0644 "$ROOT/doc/proxchunk.1" "$STAGE/doc/proxchunk.1"
install -m 0644 "$ROOT/icons/proxchunk.svg" "$STAGE/icons/proxchunk.svg"
install -m 0644 "$ROOT/icons/proxchunk.png" "$STAGE/icons/proxchunk.png"

# Portable desktop: Exec/Icon filled in by install-desktop.sh.
# Keep a copy that still works if proxchunk is on PATH after extract.
cat > "$STAGE/README-PORTABLE.txt" <<EOF
proxchunk $VER — portable tree

  ./proxchunk --help
  ./install-desktop.sh     # menu entry with this folder's binary and icon

Extract path does not matter. Do not move the binary away from icons/
if you want the menu icon to keep working after install-desktop.sh.
EOF

rm -f "$ZIP"
( cd "$ROOT/dist" && zip -r -9 "proxchunk-$VER.zip" "proxchunk-$VER" )
echo "wrote $ZIP"
unzip -l "$ZIP"
ls -l "$ZIP"
