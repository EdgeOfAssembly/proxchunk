#!/bin/sh
# Optional Alpine/musl experiment. The public binary GUI is glibc:
#   scripts/build-glibc-gui.sh
# Runs inside the Alpine chroot. Output: /src/proxchunk/gui-out/
set -e
cd /src/proxchunk

apk add --no-cache gtk+3.0-dev vte3-dev g++ pkgconf file binutils \
    gdk-pixbuf 2>/dev/null || true

echo "gtk=$(pkg-config --modversion gtk+-3.0) vte=$(pkg-config --modversion vte-2.91)"

CFLAGS="-std=gnu++23 -O3 -DNDEBUG -ffunction-sections -fdata-sections -fno-pie -Wno-deprecated-declarations"
CPPFLAGS="-Iinclude -DPROXCHUNK_VERSION=\"1.1\" $(pkg-config --cflags vte-2.91 gtk+-3.0)"
LDFLAGS="-no-pie -Wl,--as-needed -Wl,--gc-sections -Wl,--build-id=none -Wl,-O1 -Wl,--hash-style=gnu -static-libgcc -static-libstdc++"
LIBS="$(pkg-config --libs vte-2.91 gtk+-3.0)"

rm -rf /src/proxchunk/gui-out
mkdir -p /src/proxchunk/gui-out/libexec /src/proxchunk/gui-out/lib

echo "==> try fully static GTK+VTE"
if g++ $CFLAGS $CPPFLAGS src/gui.cpp -o /tmp/proxchunk-gui-static-try \
    $LDFLAGS -static -Wl,-static $LIBS 2>/tmp/gui-static.err; then
    echo "STATIC_GUI=yes"
    strip --strip-all -R .note.gnu.build-id /tmp/proxchunk-gui-static-try
    cp /tmp/proxchunk-gui-static-try /src/proxchunk/gui-out/proxchunk-gui
    file /src/proxchunk/gui-out/proxchunk-gui
    exit 0
fi
echo "STATIC_GUI=no (GTK/GLib have no .a / use dlopen)"
tail -8 /tmp/gui-static.err

echo "==> dynamic musl GUI + bundled .so"
g++ $CFLAGS $CPPFLAGS src/gui.cpp -o /src/proxchunk/gui-out/libexec/proxchunk-gui.bin \
    $LDFLAGS -Wl,-rpath,'$ORIGIN/../lib' $LIBS
strip --strip-all -R .note.gnu.build-id /src/proxchunk/gui-out/libexec/proxchunk-gui.bin
file /src/proxchunk/gui-out/libexec/proxchunk-gui.bin

ldd_paths() {
    ldd "$1" 2>/dev/null | while read -r a b c d; do
        case "$a$b$c" in
            *vdso*) continue ;;
        esac
        if [ "$b" = "=>" ] && [ -f "$c" ]; then
            echo "$c"
        elif [ -f "$a" ]; then
            echo "$a"
        fi
    done
}

copy_one() {
    src=$1
    [ -f "$src" ] || return 1
    base=$(basename "$src")
    dest=/src/proxchunk/gui-out/lib/$base
    if [ ! -e "$dest" ]; then
        cp -a "$src" "$dest"
        if [ -L "$src" ]; then
            real=$(readlink -f "$src")
            if [ -n "$real" ] && [ -f "$real" ]; then
                cp -a "$real" /src/proxchunk/gui-out/lib/"$(basename "$real")"
            fi
        fi
        return 0
    fi
    return 1
}

: > /tmp/gui-deps.list
echo /src/proxchunk/gui-out/libexec/proxchunk-gui.bin >> /tmp/gui-deps.list
i=0
while [ "$i" -lt 40 ]; do
    i=$((i + 1))
    added=0
    : > /tmp/gui-deps.next
    while read -r bin; do
        [ -f "$bin" ] || continue
        ldd_paths "$bin" >> /tmp/gui-deps.next
    done < /tmp/gui-deps.list
    sort -u /tmp/gui-deps.next -o /tmp/gui-deps.next
    while read -r path; do
        [ -n "$path" ] && [ -f "$path" ] || continue
        if copy_one "$path"; then
            echo "$path" >> /tmp/gui-deps.list
            added=1
        fi
    done < /tmp/gui-deps.next
    [ "$added" -eq 0 ] && break
done

if [ -f /lib/ld-musl-x86_64.so.1 ]; then
    cp -a /lib/ld-musl-x86_64.so.1 /src/proxchunk/gui-out/lib/
    # musl libc soname is a symlink to the loader
    ln -sfn ld-musl-x86_64.so.1 /src/proxchunk/gui-out/lib/libc.musl-x86_64.so.1
fi

PBDIR=$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)
if [ -z "$PBDIR" ]; then
    PBDIR=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders
fi
if [ -d "$PBDIR" ]; then
    mkdir -p /src/proxchunk/gui-out/lib/gdk-pixbuf-2.0/2.10.0/loaders
    # Alpine gdk-pixbuf 2.44 ships classic XPM only; PNG/SVG go through glycin.
    cp -a "$PBDIR"/libpixbufloader-xpm.so* \
        /src/proxchunk/gui-out/lib/gdk-pixbuf-2.0/2.10.0/loaders/ 2>/dev/null || true
    cp -a "$PBDIR"/libpixbufloader-png.so* \
        /src/proxchunk/gui-out/lib/gdk-pixbuf-2.0/2.10.0/loaders/ 2>/dev/null || true
    for so in /src/proxchunk/gui-out/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.so*; do
        [ -f "$so" ] || continue
        echo "$so" >> /tmp/gui-deps.list
        ldd_paths "$so" | while read -r p; do
            copy_one "$p" || true
        done
    done
fi

gcc -std=gnu23 -O2 -DNDEBUG -static -fno-pie -no-pie \
    -Wl,--as-needed -Wl,--build-id=none \
    src/gui_launch.c -o /src/proxchunk/gui-out/proxchunk-gui
strip --strip-all -R .note.gnu.build-id /src/proxchunk/gui-out/proxchunk-gui
file /src/proxchunk/gui-out/proxchunk-gui
file /src/proxchunk/gui-out/libexec/proxchunk-gui.bin

echo "==> bundled lib/"
ls /src/proxchunk/gui-out/lib | wc -l
echo "NEEDED_GUI:"
readelf -d /src/proxchunk/gui-out/libexec/proxchunk-gui.bin | grep NEEDED || true
