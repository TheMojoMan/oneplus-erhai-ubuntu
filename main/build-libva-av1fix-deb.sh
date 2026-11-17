#!/bin/bash
# Build libva-av1fix_<version>_arm64.deb: upstream libva with the AV1
# refresh-frame flag pass-through (see av1fix-patches/README.md).
#
#   ./build-libva-av1fix-deb.sh -> debs/libva-av1fix_<version>_arm64.deb
#
# The payload is a /usr/local drop-in for Ubuntu's libva.so.2 (Debian's
# /usr/local/lib/<multiarch> sits BEFORE /usr/lib/<multiarch> in the loader
# path), so the distro package keeps owning /usr/lib and libva-x11/libva-drm
# stay Ubuntu's own.  Built exactly like the tree that was validated on
# hardware: no X11/GLX/Wayland backends, release build.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/libva-av1fix"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")
UPSTREAM=${VER%%+*}			# 2.23.0
DEB_DIR="$HERE/debs"

TARBALL_SHA=b10aceb30e93ddf13b2030eb70079574ba437be9b3b76065caf28a72c07e23e7
PATCH="$HERE/av1fix-patches/0001-libva-av1-carry-refresh-frame-flags.patch"
[ -f "$PATCH" ] || { echo "missing $PATCH" >&2; exit 1; }

# The tarball: the one the AV1 work used first, else whatever the caller has,
# else download the release.
tarball=""
for c in "${LIBVA_TARBALL:-}" \
	 "$HERE/../../build/av1-refresh-fix/src/libva-$UPSTREAM.tar.gz" \
	 "$HERE/../../tmp/av1fix-src/libva-$UPSTREAM.tar.gz"; do
	[ -n "$c" ] && [ -f "$c" ] && { tarball=$c; break; }
done
if [ -z "$tarball" ]; then
	echo "==> downloading libva $UPSTREAM"
	mkdir -p "$HERE/../../tmp/av1fix-src"
	tarball="$HERE/../../tmp/av1fix-src/libva-$UPSTREAM.tar.gz"
	curl -fsSL -o "$tarball" \
		"https://github.com/intel/libva/releases/download/$UPSTREAM/libva-$UPSTREAM.tar.gz"
fi
echo "==> libva tarball: $tarball"
got=$(sha256sum "$tarball" | cut -d" " -f1)
[ "$got" = "$TARBALL_SHA" ] || {
	echo "unexpected libva tarball ($got)" >&2; exit 1; }

WORK="$HERE/../../tmp/av1fix-build/libva"
rm -rf "$WORK"
mkdir -p "$WORK"
STAGE="$WORK/root"

tar -C "$WORK" -xzf "$tarball"
cd "$WORK/libva-$UPSTREAM"
patch -p1 --batch --forward <"$PATCH"
grep -q refresh_frame_flags_valid va/va_dec_av1.h || {
	echo "patch did not apply to va/va_dec_av1.h" >&2; exit 1; }

# prefix is the RUNTIME path: the package must not carry this machine's build
# path (ppa_scrub_debug refuses it), so the install goes through DESTDIR.
meson setup build --prefix=/usr/local --libdir=lib/aarch64-linux-gnu \
	--buildtype=release \
	-Ddriverdir="${DRIVERDIR:-/usr/lib/aarch64-linux-gnu/dri}" \
	-Dwith_x11=no -Dwith_glx=no -Dwith_wayland=no -Denable_docs=false
ninja -C build -j"$(nproc)"
DESTDIR="$STAGE" ninja -C build install

# Payload sanity: exactly one soname, the AV1 struct must carry the fields.
[ -e "$STAGE/usr/local/lib/aarch64-linux-gnu/libva.so.2" ] || {
	echo "libva.so.2 was not installed in the stage" >&2; exit 1; }

install -d -m755 "$STAGE/DEBIAN"
cp -a "$SRC/DEBIAN/." "$STAGE/DEBIAN/"
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

# Compiled artifacts: no debug info, no build path in the payload (owner rule).
ppa_scrub_debug "$STAGE/usr/local"

mkdir -p "$DEB_DIR"
dpkg-deb --build --root-owner-group "$STAGE" \
	"$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
cd "$HERE"
rm -rf "$WORK"
echo "built $(basename "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb")"
# NO_PPA=1 builds locally without publishing (in-development iterations;
# the PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
