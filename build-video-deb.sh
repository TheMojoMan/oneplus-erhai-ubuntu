#!/bin/bash
# Build oneplus-erhai-video_<version>_arm64.deb from the
# oneplus-erhai-video/ staging tree.
#
#   ./build-video-deb.sh -> debs/oneplus-erhai-video_<version>_arm64.deb
#
# The staging tree carries the full payload (VA-API driver, browser config,
# udev rule) AND the iris-vaapi source under usr/src/. The driver .so is
# REBUILT from that source on every run so the shipped binary always
# matches the shipped source. arm64: the .so is arch-specific.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-video"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

SRCDIR="$SRC/usr/src/oneplus-erhai-video/iris-vaapi"
[ -f "$SRCDIR/Makefile" ] || { echo "missing $SRCDIR" >&2; exit 1; }

# Rebuild the driver into a scratch dir (never leave build/ in the tree).
BUILD="$SRC/DEBIAN/build-scratch"
rm -rf "$BUILD"
make -C "$SRCDIR" BUILD="$(readlink -f "$BUILD")" -j"$(nproc)" \
	"$(readlink -f "$BUILD")/iris_drv_video.so"
install -m755 "$BUILD/iris_drv_video.so" \
	"$SRC/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so"
rm -rf "$BUILD"

find "$SRC" -type d -exec chmod 755 {} \;
chmod 644 "$SRC/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so"
chmod 644 "$SRC"/etc/udev/rules.d/*.rules "$SRC"/etc/environment.d/*.conf \
	"$SRC"/etc/firefox/policies/policies.json "$SRC"/etc/chromium.d/* \
	"$SRC"/etc/brave/policies/managed/*.json
[ -f "$SRC/DEBIAN/postinst" ] && chmod 755 "$SRC/DEBIAN/postinst"

# md5sums for every payload file (DEBIAN/ excluded).
cd "$SRC"
find etc usr -type f -exec md5sum {} \; > DEBIAN/md5sums
cd "$HERE"

dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_arm64.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations;
# the PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_arm64.deb"
fi
echo "built ${PKG}_${VER}_arm64.deb"