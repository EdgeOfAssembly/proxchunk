#!/usr/bin/env bash
# Audit 1.0 binary and source release tarballs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.0}
ARCH=${ARCH:-$(uname -m)}
BIN_TGZ=$ROOT/dist/proxchunk-${VER}-${ARCH}.tar.gz
SRC_TGZ=$ROOT/dist/proxchunk-${VER}-src.tar.gz
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

[[ -f $BIN_TGZ ]] || fail "missing $BIN_TGZ"
[[ -f $SRC_TGZ ]] || fail "missing $SRC_TGZ"

echo "==> binary tarball $BIN_TGZ"
tar -tzf "$BIN_TGZ" > "$STAGE/bin.list"
prefix=proxchunk-${VER}-${ARCH}

grep -qx "${prefix}/proxchunk" "$STAGE/bin.list" || fail "binary missing proxchunk"
grep -qx "${prefix}/proxchunk-gui" "$STAGE/bin.list" || fail "binary missing proxchunk-gui"
grep -q "${prefix}/libexec/proxchunk-gui.bin" "$STAGE/bin.list" || fail "binary missing libexec GUI"
grep -q "${prefix}/lib/ld-musl-x86_64.so.1" "$STAGE/bin.list" || fail "binary missing bundled ld-musl"
grep -qx "${prefix}/install.sh" "$STAGE/bin.list" || fail "binary missing install.sh"
grep -qx "${prefix}/uninstall.sh" "$STAGE/bin.list" || fail "binary missing uninstall.sh"
grep -qx "${prefix}/README.md" "$STAGE/bin.list" || fail "binary missing README.md"
grep -qx "${prefix}/LICENSE" "$STAGE/bin.list" || fail "binary missing LICENSE"
grep -qx "${prefix}/proxchunk.desktop" "$STAGE/bin.list" || fail "binary missing desktop"
grep -qx "${prefix}/doc/proxchunk.1" "$STAGE/bin.list" || fail "binary missing man page"
grep -qx "${prefix}/icons/proxchunk.svg" "$STAGE/bin.list" || fail "binary missing svg"
grep -qx "${prefix}/icons/pixmaps/proxchunk.png" "$STAGE/bin.list" || fail "binary missing pixmap"

# Binary package must not ship source/build tree.
while IFS= read -r p; do
    case $p in
        */CMakeLists.txt|*/Makefile|*/src/*|*/tests/*|*/.git*|*/README.user.md)
            fail "binary tarball has source/build path: $p"
            ;;
        *build-instructions*|*BUILD.md*)
            fail "binary tarball has build doc: $p"
            ;;
    esac
done < "$STAGE/bin.list"

mkdir -p "$STAGE/bin"
tar -xzf "$BIN_TGZ" -C "$STAGE/bin"
B=$STAGE/bin/$prefix
[[ -x $B/proxchunk ]] || fail "packed binary not executable"
[[ -x $B/proxchunk-gui ]] || fail "packed proxchunk-gui not executable"
[[ -x $B/install.sh ]] || fail "install.sh not executable"
[[ -x $B/uninstall.sh ]] || fail "uninstall.sh not executable"
grep -q 'Exec=proxchunk-gui' "$B/proxchunk.desktop" || fail "desktop should launch proxchunk-gui"

info=$(file "$B/proxchunk")
echo "file: $info"
echo "$info" | grep -qi 'statically linked' || fail "packed binary is not static"
echo "$info" | grep -qi 'BuildID' && fail "packed binary still has ELF BuildID: $info"
if command -v readelf >/dev/null; then
    if readelf -n "$B/proxchunk" 2>/dev/null | grep -qi 'Build ID'; then
        fail "readelf still reports Build ID"
    fi
fi
ldd_out=$(ldd "$B/proxchunk" 2>&1 || true)
echo "$ldd_out" | grep -qiE 'not a dynamic|statically linked' \
    || fail "ldd thinks packed binary is dynamic: $ldd_out"

ver=$("$B/proxchunk" -v)
test "$ver" = "proxchunk ${VER}" || fail "packed -v: $ver"

readme=$B/README.md
grep -qiE '^## Build$|g\+\+|libcurl|cmake|make test' "$readme" \
    && fail "binary README still has build instructions"
grep -q 'logical CPU' "$readme" || fail "binary README missing CPU concurrent default"
grep -q 'log out' "$B/install.sh" && fail "install.sh still mentions log out"
grep -q 'restart the panel' "$B/install.sh" && fail "install.sh still mentions restart the panel"

echo "==> CLI + Range on packed binary"
bash "$ROOT/tests/test_cli.sh" "$B/proxchunk"
bash "$ROOT/tests/test_download.sh" "$B/proxchunk"

echo "==> source tarball $SRC_TGZ"
tar -tzf "$SRC_TGZ" > "$STAGE/src.list"
sprefix=proxchunk-${VER}
grep -qx "${sprefix}/CMakeLists.txt" "$STAGE/src.list" || fail "src missing CMakeLists.txt"
grep -qx "${sprefix}/Makefile" "$STAGE/src.list" || fail "src missing Makefile"
grep -qx "${sprefix}/src/proxchunk.cpp" "$STAGE/src.list" || fail "src missing proxchunk.cpp"
grep -qx "${sprefix}/README.md" "$STAGE/src.list" || fail "src missing README.md"
grep -qx "${sprefix}/README.user.md" "$STAGE/src.list" || fail "src missing README.user.md"
grep -qx "${sprefix}/tests/test_cli.sh" "$STAGE/src.list" || fail "src missing tests"

# Host-only / generated paths must not appear.
while IFS= read -r p; do
    case $p in
        */scripts/demo-install.sh|*/scripts/shot24.py|*/scripts/xmux-demo.sh|*/scripts/ia-compare.sh|*/scripts/run-proxy.sh)
            fail "src tarball has host-only script: $p"
            ;;
        */dist/*|*/proxchunk-musl-static|*/.git/*)
            fail "src tarball has generated/host path: $p"
            ;;
    esac
done < "$STAGE/src.list"

mkdir -p "$STAGE/src"
tar -xzf "$SRC_TGZ" -C "$STAGE/src"
grep -q '^## Build' "$STAGE/src/$sprefix/README.md" || fail "source README missing Build section"

echo "test_package ok"
echo "  $BIN_TGZ"
echo "  $SRC_TGZ"
