#!/usr/bin/env bash
# Build proxchunk-gui on host glibc.
#
# Link policy (linker-static skill):
#   1. -static-libgcc -static-libstdc++
#   2. -Wl,-Bstatic + --start-group for every library that has a .a
#   3. -Wl,-Bdynamic for libc/libm/libdl/libpthread and any -l that has no .a
#   4. Remaining .so (dlopen / no .a) go in lib/ with DT_RPATH $ORIGIN/lib
#
# Do NOT ship libc, ld-linux, or musl.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUNDLE=${BUNDLE:-"$ROOT/gui-bundle"}
OUT=${OUT:-"$ROOT/proxchunk-gui"}

need() { command -v "$1" >/dev/null || { echo "missing $1" >&2; exit 1; }; }
need g++
need pkg-config
need patchelf
pkg-config --exists vte-2.91 gtk+-3.0 || { echo "need gtk+-3.0 and vte-2.91" >&2; exit 1; }

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/lib"

CFLAGS="-std=gnu++23 -O3 -DNDEBUG -ffunction-sections -fdata-sections -fno-pie -Wall -Wextra"
CPPFLAGS="-I$ROOT/include -DPROXCHUNK_VERSION=\"1.0\" $(pkg-config --cflags vte-2.91 gtk+-3.0)"
# Driver always sees as-needed (linker-dynamic). Full -static is NOT used:
# glibc NSS + GTK dlopen need a dynamic libc.
LDFLAGS="-no-pie -static-libgcc -static-libstdc++ -Wl,--as-needed -Wl,--gc-sections -Wl,--build-id=none -Wl,-O1 -Wl,--hash-style=gnu"
RPATH='-Wl,-rpath,$ORIGIN/lib'

# pkg-config --static pulls .a-aware Libs.private.
PC_LIBS="$(pkg-config --static --libs vte-2.91 gtk+-3.0 2>/dev/null || pkg-config --libs vte-2.91 gtk+-3.0)"

has_static() {
    local name=$1
    [[ -f /usr/lib64/lib${name}.a || -f /usr/lib/lib${name}.a ]]
}

# Split -lfoo into static vs dynamic according to whether libfoo.a exists.
# Always dynamic: c m dl pthread rt resolv gcc_s (glibc / unwinder).
STATIC_L=()
DYN_L=()
OTHER=()
skip_dyn='^(c|m|dl|pthread|rt|resolv|gcc_s|gcc|stdc\+\+)$'

eval "set -- $PC_LIBS"
for tok in "$@"; do
    case "$tok" in
        -l*)
            n=${tok#-l}
            if [[ $n =~ $skip_dyn ]]; then
                DYN_L+=("$tok")
            elif has_static "$n"; then
                STATIC_L+=("$tok")
            else
                DYN_L+=("$tok")
            fi
            ;;
        *)
            OTHER+=("$tok")
            ;;
    esac
done

echo "==> static .a: ${STATIC_L[*]:-none}"
echo "==> dynamic .so: ${DYN_L[*]:-none}"
echo "==> other: ${OTHER[*]:-none}"

link_try() {
    local err=$1
    shift
    g++ $CFLAGS $CPPFLAGS "$ROOT/src/gui.cpp" -o "$BUNDLE/proxchunk-gui" \
        $LDFLAGS "$RPATH" "$@" 2>"$err"
}

ERR=/tmp/gui-glibc-link.err
echo "==> hybrid: -Bstatic (start-group) then -Bdynamic"
if ! link_try "$ERR" \
    "${OTHER[@]}" \
    -Wl,-Bstatic -Wl,--start-group "${STATIC_L[@]}" -Wl,--end-group \
    -Wl,-Bdynamic "${DYN_L[@]}" -ldl -lm -lpthread -lrt; then
    echo "hybrid link failed; log tail:"
    tail -30 "$ERR"
    echo "==> fallback: all dynamic (previous behaviour)"
    link_try "$ERR" $(pkg-config --libs vte-2.91 gtk+-3.0)
fi

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

# Only bundle libstdc++/libgcc_s if the hybrid link still DT_NEEDED them.
if ldd "$BUNDLE/proxchunk-gui" | grep -q 'libstdc++\.so'; then
    extra=$(g++ -print-file-name=libstdc++.so.6)
    copy_one "$extra" || true
    real=$(readlink -f "$extra")
    [[ -n $real && $real != "$extra" ]] && copy_one "$real" || true
fi
if ldd "$BUNDLE/proxchunk-gui" | grep -q 'libgcc_s\.so'; then
    extra=$(g++ -print-file-name=libgcc_s.so.1)
    copy_one "$extra" || true
    real=$(readlink -f "$extra")
    [[ -n $real && $real != "$extra" ]] && copy_one "$real" || true
fi

while IFS= read -r -d '' so; do
    file -bL "$so" | grep -q ELF || continue
    patchelf --force-rpath --set-rpath '$ORIGIN' "$so"
done < <(find "$BUNDLE/lib" -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$BUNDLE/proxchunk-gui"

if ls "$BUNDLE/lib"/ld-musl* "$BUNDLE/lib"/libc.musl* "$BUNDLE/lib"/libc.so.6 \
    "$BUNDLE/lib"/ld-linux* 2>/dev/null | grep -q .; then
    echo "refusing to ship libc/ld-linux/musl in lib/" >&2
    exit 1
fi

install -m 0755 "$BUNDLE/proxchunk-gui" "$OUT"
echo "wrote $OUT and $BUNDLE"
echo "lib files: $(find "$BUNDLE/lib" -type f | wc -l)"
echo "ldd:"
(ldd "$BUNDLE/proxchunk-gui" || true)
echo "NEEDED again:"
readelf -d "$BUNDLE/proxchunk-gui" | grep NEEDED || true
