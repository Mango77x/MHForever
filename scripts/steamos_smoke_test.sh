#!/bin/bash
# The launcher's window on SteamOS (the Steam Deck's system), without a Steam Deck: in the system of Valve's own
# recovery image, as the deck user, on a virtual display. Linux, as root (it uses chroot and mounts).
#
#   steamos_smoke_test.sh rootfs <dir>                      Valve's latest recovery image -> its system in <dir>
#   steamos_smoke_test.sh run <rootfs> <release> <png>      the launcher of <release> (an unpacked Linux archive) in it
#
# `run` passes when the window starts on the bundled WebKitGTK (lib/webkit), WebKit's web and network processes run
# from the folder the starter prepares, and the screen shows the launcher (it saves it to <png>). It needs Xvfb,
# ImageMagick and xwd; STEAMOS_EXTRA_ENV adds VAR=value words to its environment. `rootfs` needs curl, lbzip2 (or
# bzip2), btrfs-progs and python3.
set -euo pipefail

IMAGE=https://steamdeck-images.steamos.cloud/recovery/steamdeck-repair-latest.img.bz2
HELPERS=/tmp/.mhforever-webkit2gtk-4.1-helpers-1 # kRuntimeDir in launcher/native/starter_linux.cpp

rootfs() {
    local dir=$1 url
    url=$(curl -sSfI "$IMAGE" | awk 'tolower($1) == "location:" {print $2}' | tr -d '\r')
    [ -n "$url" ] || url=$IMAGE
    echo "image: $url"
    mkdir -p "$dir"
    local work
    work=$(mktemp -d)
    local unpack=bzcat
    command -v lbzip2 > /dev/null && unpack="lbzip2 -dc"
    # The image is a GPT disk; its system is the btrfs partition rootfs-A. Read in one pass, never stored whole: the
    # reader stops after that partition, so curl and the decompressor end on a broken pipe, which is expected.
    set +o pipefail
    curl -sSfL --retry 5 --retry-delay 15 "$url" | $unpack | python3 -c '
import struct, sys
src, out = sys.stdin.buffer, sys.argv[1]
head = src.read(34 * 512)
entries = struct.unpack_from("<Q", head, 512 + 72)[0]
count, size = struct.unpack_from("<II", head, 512 + 80)
parts = {}
for i in range(count):
    e = head[entries * 512 + i * size: entries * 512 + (i + 1) * size]
    first, last = struct.unpack_from("<QQ", e, 32)
    name = e[56:128].decode("utf-16-le").rstrip("\0")
    if first:
        parts[name] = (first, last)
first, last = parts["rootfs-A"]
pos = len(head)
skip = first * 512 - pos
while skip > 0:
    skip -= len(src.read(min(skip, 1 << 24)))
left = (last - first + 1) * 512
with open(out, "wb") as f:
    while left > 0:
        chunk = src.read(min(left, 1 << 24))
        if not chunk:
            sys.exit("the image ended early")
        f.write(chunk)
        left -= len(chunk)
' "$work/rootfs.btrfs"
    local status=("${PIPESTATUS[@]}")
    set -o pipefail
    [ "${status[2]}" = 0 ] || { echo "could not read the image (${status[*]})"; exit 1; }
    btrfs restore -x -m -S "$work/rootfs.btrfs" "$dir" > "$work/restore.log" 2>&1 || { tail "$work/restore.log"; exit 1; }
    rm -rf "$work"
    grep -E '^(PRETTY_NAME|VERSION_ID)=' "$dir/etc/os-release"
}

run() {
    local root=$1 release=$2 png=$3
    root=$(realpath "$root")
    grep -q '^deck:' "$root/etc/passwd" || echo 'deck:x:1000:1000:Steam Deck User:/home/deck:/bin/bash' >> "$root/etc/passwd"
    grep -q '^deck:' "$root/etc/group" || echo 'deck:x:1000:' >> "$root/etc/group"
    rm -rf "$root/home/deck"
    mkdir -p "$root/home/deck"
    cp -r "$release" "$root/home/deck/MHForever"
    chown -R 1000:1000 "$root/home/deck"
    chmod 1777 "$root/tmp"
    rm -rf "$root$HELPERS"
    local m
    for m in proc sys dev dev/shm dev/pts; do
        mkdir -p "$root/$m"
        mountpoint -q "$root/$m" || mount --bind "/$m" "$root/$m"
    done
    cp /etc/resolv.conf "$root/etc/resolv.conf" 2> /dev/null || true

    # A TCP display: the chroot sees the network, not the host's /tmp/.X11-unix.
    local display=127.0.0.1:95
    Xvfb :95 -screen 0 1280x800x24 -listen tcp -nolisten unix -ac > /tmp/steamos-xvfb.log 2>&1 &
    local xvfb=$!
    sleep 2
    local log=/tmp/steamos-launcher.log
    chroot --userspec=1000:1000 "$root" /usr/bin/env -i HOME=/home/deck USER=deck PATH=/usr/bin:/bin \
        DISPLAY=$display GDK_BACKEND=x11 XDG_RUNTIME_DIR=/tmp MHF_LAUNCHER_DEBUG=1 \
        MHF_DATA_DIR=/home/deck/data MHF_SAVES_DIR=/home/deck/saves ${STEAMOS_EXTRA_ENV:-} \
        /home/deck/MHForever/MHForever > "$log" 2>&1 &
    local launcher=$!
    sleep 25

    local failed=0 web network window
    web=$(pgrep -f "^$HELPERS/WebKitWebProcess" | head -1 || true)
    network=$(pgrep -f "^$HELPERS/WebKitNetworkProcess" | head -1 || true)
    window=""
    [ -n "$web" ] && window=$(ps -o ppid= -p "$web" | tr -d ' ')
    echo "== launcher output"
    grep -v -E '^(libEGL|MESA|$)' "$log" | head -40 || true
    echo "== processes"
    ps -eo pid,ppid,args | grep -E 'MHForever|WebKit' | grep -v -E 'grep|steamos_smoke' || true
    if [ -n "$window" ] && readlink "/proc/$window/exe" | grep -q 'MHForever/MHForever-window$'; then
        echo "window: $(readlink "/proc/$window/exe")"
        echo "  its WebKit: $(grep -oE '/[^ ]*libwebkit2gtk-4\.1\.so[^ ]*' "/proc/$window/maps" | sort -u | tr '\n' ' ')"
        grep -q 'home/deck/MHForever/lib/webkit/libwebkit2gtk-4.1.so.0' "/proc/$window/maps" \
            || { echo "FAIL: the window does not use the bundled WebKitGTK"; failed=1; }
    else
        echo "FAIL: no launcher window with a WebKit web process"
        failed=1
    fi
    [ -n "$web" ] && echo "web process: $(readlink "/proc/$web/exe")" || { echo "FAIL: no WebKitWebProcess"; failed=1; }
    [ -n "$network" ] && echo "network process: $(readlink "/proc/$network/exe")" \
        || { echo "FAIL: no WebKitNetworkProcess"; failed=1; }
    if [ -n "$network" ]; then
        echo "  its TLS: $(grep -oE '/[^ ]*(libgiognutls|libgnutls)[^ ]*' "/proc/$network/maps" | sort -u | tr '\n' ' ')"
    fi
    xwd -root -display $display -silent | convert xwd:- "$png"
    local spread
    spread=$(convert "$png" -format '%[fx:standard_deviation]' info:)
    echo "screen: $png (spread $spread)"
    awk -v s="$spread" 'BEGIN { exit !(s > 0.05) }' || { echo "FAIL: the screen is blank"; failed=1; }

    kill "$launcher" 2> /dev/null || true
    pkill -f '/home/deck/MHForever/' 2> /dev/null || true
    pkill -f "^$HELPERS/" 2> /dev/null || true
    sleep 2
    kill "$xvfb" 2> /dev/null || true
    for m in dev/pts dev/shm dev sys proc; do umount "$root/$m" 2> /dev/null || umount -l "$root/$m" || true; done
    [ "$failed" = 0 ] && echo "PASS: the launcher's window runs on SteamOS"
    return $failed
}

# (Not `[ ... ] && rootfs`: errexit does not apply inside a function called from a && list.)
if [ "${1:-}" = rootfs ] && [ $# -eq 2 ]; then
    rootfs "$2"
elif [ "${1:-}" = run ] && [ $# -eq 4 ]; then
    run "$2" "$3" "$4"
else
    sed -n '2,12p' "$0"
    exit 2
fi
