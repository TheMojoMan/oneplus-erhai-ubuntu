#!/bin/bash
# Build oneplus-erhai-camera_<version>_arm64.deb from the
# oneplus-erhai-camera/ staging tree.
#
#   ./build-camera-deb.sh -> debs/oneplus-erhai-camera_<version>_arm64.deb
#
# Plain binary-package layout like build-bluetooth-deb.sh: DEBIAN/control +
# DEBIAN/postinst, erhai-cam-arm.service (+ its arm script) that arms both
# sensors at boot, 70-erhai-cam-i2c.rules uaccess rules (session-user rw on
# the CCI adapter AND the cn3927 lens subdev), the manual erhai-cam-focus /
# erhai-cam-exposure scripts (runtime-resolved CCI bus, VCM slave 0x0c /
# sensor slave 0x36) and the erhai-autofocus contrast-AF prototype, compiled
# from src/erhai-autofocus.cpp (port of CFM880/nabu-camera's nabu-autofocus;
# needs the kernel cn3927 module).  The erhai-cam-focus@erhai.local shell
# extension was REMOVED in 1.3: ErhaiCam (oneplus-erhai-erhaicam >= 1.5) owns
# focus/exposure natively (NPU-driven AE/AF + manual sliders).
# Depends on i2c-tools + gtk4/gstreamer runtime libs. The compile step needs
# libgtk-4-dev, libgstreamer1.0-dev and libgstreamer-plugins-base1.0-dev.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-camera"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

for f in usr/local/sbin/erhai-cam-arm.sh \
	 etc/systemd/system/erhai-cam-arm.service \
	 etc/udev/rules.d/70-erhai-cam-i2c.rules \
	 src/erhai-autofocus.cpp; do
	[ -f "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

# erhai-autofocus: GTK4 + GStreamer contrast autofocus driving the kernel
# lens subdev. Compiled here so the deb always matches its shipped source.
PKGS="gstreamer-app-1.0 gstreamer-video-1.0 gtk4"
pkg-config --exists $PKGS || {
	echo "missing pkg-config deps ($PKGS); install libgtk-4-dev," \
	     "libgstreamer1.0-dev, libgstreamer-plugins-base1.0-dev" >&2
	exit 1
}
mkdir -p "$SRC/usr/local/bin"
${CXX:-g++} -O2 -g -std=c++20 -Wall -Wextra -Wpedantic \
	$(pkg-config --cflags $PKGS) \
	-o "$SRC/usr/local/bin/erhai-autofocus" \
	"$SRC/src/erhai-autofocus.cpp" \
	$(pkg-config --libs $PKGS)

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/usr/local/sbin/*.sh \
	  "$SRC"/usr/local/bin/erhai-autofocus
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
echo "built ${PKG}_${VER}_${ARCH}.deb"
