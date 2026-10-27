#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo ./install-system.sh" >&2
    exit 1
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
driver_dir=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
[ -n "$driver_dir" ] || driver_dir=/usr/lib/aarch64-linux-gnu/dri

src=$repo_dir/build/iris_drv_video.so
dst=$driver_dir/iris_drv_video.so
rule=/etc/udev/rules.d/99-iris-dmaheap.rules
modprobe_conf=/etc/modprobe.d/99-iris-vaapi.conf

[ -f "$src" ] || { echo "missing $src"; exit 1; }

install -m 0755 "$src" "$dst"
echo "installed $dst"

install -m 0644 "$repo_dir/tools/99-iris-dmaheap.rules" "$rule"
chmod 0666 /dev/dma_heap/system 2>/dev/null || true
udevadm control --reload-rules 2>/dev/null || true
echo "installed $rule (dma_heap/system now writable)"

install -m 0644 "$repo_dir/tools/99-iris-vaapi.conf" "$modprobe_conf"
echo "installed $modprobe_conf (cacheable H.264/HEVC/VP9 CAPTURE enabled on next module load)"
echo "test: LIBVA_DRIVER_NAME=iris vainfo"
