#!/usr/bin/env bash
# Build proxchunk-gui on the host glibc toolchain.
# GTK has no .a here, so: -static-libgcc -static-libstdc++ -Wl,--as-needed,
# then copy remaining non-glibc .so into gui-bundle/lib with $ORIGIN/lib.
# Do NOT ship libc, ld-linux, or musl — those must come from the user's glibc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUNDLE=${BUNDLE:-"$ROOT/gui-bundle"}
OUT=${OUT:-"$ROOT/proxchunk-gui"}

need() { command -v "$1" >/dev/null || { echo "missing $1" >&2; exit 1; }; }
need g++
need pkg-config
pkg-config --exists vte-2.91 gtk+-3.0 || { echo "need gtk+-3.0 and vte-2.91" >&2; exit 1; }

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/lib"

CFLAGS="-std=gnu++23 -O3 -DNDEBUG -ffunction-sections -fdata-sections -fno-pie -Wall -Wextra"
CPPFLAGS="-I$ROOT/include -DPROXCHUNK_VERSION=\"1.0\" $(pkg-config --cflags vte-2.91 gtk+-3.0)"
# as-needed on the driver so unused pkg-config libs are not DT_NEEDED.
LDFLAGS="-no-pie -static-libgcc -static-libstdc++ -Wl,--as-needed -Wl,--gc-sections -Wl,--build-id=none -Wl,-O1 -Wl,--hash-style=gnu"
RPATH='-Wl,-rpath,$ORIGIN/lib'
LIBS="$(pkg-config --libs vte-2.91 gtk+-3.0)"

echo "==> try fully static glibc GTK+VTE"
if g++ $CFLAGS $CPPFLAGS "$ROOT/src/gui.cpp" -o /tmp/proxchunk-gui-static-try \
    $LDFLAGS -static -Wl,-static $LIBS 2>/tmp/gui-glibc-static.err; then
    echo "STATIC_GUI=yes"
    strip --strip-all -R .note.gnu.build-id -o "$BUNDLE/proxchunk-gui" /tmp/proxchunk-gui-static-try
    install -m 0755 "$BUNDLE/proxchunk-gui" "$OUT"
    file "$OUT"
    exit 0
fi
echo "STATIC_GUI=no (no GTK/VTE .a — bundling shared libs)"
tail -6 /tmp/gui-glibc-static.err || true

echo "==> glibc dynamic GUI + lib/ (host libc)"
g++ $CFLAGS $CPPFLAGS "$ROOT/src/gui.cpp" -o "$BUNDLE/proxchunk-gui" \
    $LDFLAGS "$RPATH" $LIBS
strip --strip-all -R .note.gnu.build-id "$BUNDLE/proxchunk-gui"
file "$BUNDLE/proxchunk-gui"
echo "NEEDED:"
readelf -d "$BUNDLE/proxchunk-gui" | grep NEEDED || true

is_host_glibc() {
    local b
    b=$(basename "$1")
    case "$b" in
        libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|libnss_*.so*|libthread_db.so.1)
            return 0 ;;
        ld-linux-x86-64.so.2|ld-linux.so.2|ld-linux-x86-64.so.*)
            return 0 ;;
        linux-vdso.so.1|linux-gate.so.1)
            return 0 ;;
        ld-musl-*.so.1|libc.musl-*.so.1)
            return 0 ;;
    esac
    case "$1" in
        /lib/ld-linux*| /lib64/ld-linux*| /lib/libc.so.6| /lib64/libc.so.6)
            return 0 ;;
    esac
    return 1
}

copy_one() {
    local src=$1 dest base real
    [[ -e $src ]] || return 1
    file -bL "$src" | grep -q ELF || return 1
    base=$(basename "$src")
    dest=$BUNDLE/lib/$base
    if [[ -e $dest ]]; then
        return 1
    fi
    cp -a "$src" "$dest"
    if [[ -L $src ]]; then
        real=$(readlink -f "$src")
        if [[ -n $real && -f $real ]]; then
            cp -a "$real" "$BUNDLE/lib/$(basename "$real")"
        fi
    fi
    return 0
}

: > /tmp/gui-glibc-deps.list
echo "$BUNDLE/proxchunk-gui" >> /tmp/gui-glibc-deps.list
added=1
n=0
while [[ $added -eq 1 && $n -lt 40 ]]; do
    n=$((n + 1))
    added=0
    : > /tmp/gui-glibc-next
    while read -r bin; do
        [[ -f $bin ]] || continue
        ldd "$bin" 2>/dev/null | awk '
            /vdso|linux-gate/ { next }
            /=>/ { print $3; next }
            /^\t\// { print $1 }
        '
    done < /tmp/gui-glibc-deps.list >> /tmp/gui-glibc-next
    sort -u /tmp/gui-glibc-next -o /tmp/gui-glibc-next
    while read -r path; do
        [[ -n $path && -e $path ]] || continue
        is_host_glibc "$path" && continue
        if copy_one "$path"; then
            echo "$path" >> /tmp/gui-glibc-deps.list
            added=1
        fi
    done < /tmp/gui-glibc-next
done

# libvte is C++; the GUI binary is -static-libstdc++ but vte still needs these.
for extra in "$(g++ -print-file-name=libstdc++.so.6)" "$(g++ -print-file-name=libgcc_s.so.1)"; do
    [[ -e $extra ]] || continue
    copy_one "$extra" || true
    real=$(readlink -f "$extra")
    if [[ -n $real && $real != "$extra" ]]; then
        copy_one "$real" || true
    fi
done

# Bundled .so must search this directory, not the host GTK.
if command -v patchelf >/dev/null; then
    while IFS= read -r -d '' so; do
        file -bL "$so" | grep -q ELF || continue
        # DT_RPATH is searched before LD_LIBRARY_PATH so a user's CUDA/gcc
        # path cannot shadow the bundled GTK stack.
        patchelf --force-rpath --set-rpath '$ORIGIN' "$so"
    done < <(find "$BUNDLE/lib" -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
    patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$BUNDLE/proxchunk-gui"
else
    echo "patchelf missing — bundled libs may still bind to system GTK" >&2
    exit 1
fi

# Fail the build if we accidentally pulled musl or glibc into lib/.
if ls "$BUNDLE/lib"/ld-musl* "$BUNDLE/lib"/libc.musl* "$BUNDLE/lib"/libc.so.6 \
    "$BUNDLE/lib"/ld-linux* 2>/dev/null | grep -q .; then
    echo "refusing to ship libc/ld-linux/musl in lib/:" >&2
    ls "$BUNDLE/lib"/ld-musl* "$BUNDLE/lib"/libc.musl* "$BUNDLE/lib"/libc.so.6 \
        "$BUNDLE/lib"/ld-linux* 2>/dev/null || true
    exit 1
fi

install -m 0755 "$BUNDLE/proxchunk-gui" "$OUT"
echo "wrote $OUT and $BUNDLE"
echo "lib files: $(find "$BUNDLE/lib" -type f | wc -l)"
(ldd "$BUNDLE/proxchunk-gui" || true) | head -20
# $ORIGIN/lib must resolve gtk from the bundle, not only the system.
if ! ldd "$BUNDLE/proxchunk-gui" | grep -q "$BUNDLE/lib/libgtk"; then
    echo "ldd did not resolve libgtk from $BUNDLE/lib" >&2
    ldd "$BUNDLE/proxchunk-gui" >&2
    exit 1
fi
# Transitive GTK deps must also come from lib/, not /usr/lib64.
if ldd "$BUNDLE/proxchunk-gui" | grep -E 'libcairo|libpango|libgdk-' | grep -q '/usr/lib'; then
    echo "some GTK deps still bind to /usr/lib — patchelf rpath failed" >&2
    ldd "$BUNDLE/proxchunk-gui" >&2
    exit 1
fi
