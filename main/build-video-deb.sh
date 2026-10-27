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
. "$HERE/../ppa-common.sh"
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

# Pre-pack guard: a nested .git in the payload is dev cruft, never payload.  The
# 1.2 build (2026-09-29) SHIPPED the vendored iris-vaapi's .git -- 44 entries: the
# object store, the hooks and commit signatures carrying the author's identity --
# because the tree still had its clone metadata.  The tree is clean since, and this
# makes a recurrence impossible (the same reason build-desktop-deb.sh refuses a
# Python bytecode cache: anything a build must never ship is a hard error here).
if find "$SRC" -name .git -print -quit | grep -q .; then
	echo "refusing to pack: a nested .git inside $SRC" >&2
	find "$SRC" -name .git >&2
	echo "  remove it -- the payload must not carry git metadata" >&2
	exit 1
fi

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

# personal data: no build path in the payload, no DWARF in the ELFs
ppa_scrub_debug "$SRC"
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_arm64.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations;
# the PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_arm64.deb"
fi
echo "built ${PKG}_${VER}_arm64.deb"