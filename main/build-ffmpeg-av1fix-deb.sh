#!/bin/bash
# Build ffmpeg-av1fix_<version>_arm64.deb: upstream ffmpeg with the AV1
# VA-API refresh-frame fixes (see av1fix-patches/README.md).
#
#   ./build-ffmpeg-av1fix-deb.sh -> debs/ffmpeg-av1fix_<version>_arm64.deb
#
# A /usr/local drop-in next to libva-av1fix: /usr/local/lib/<multiarch> comes
# before /usr/lib/<multiarch>, so mpv and VLC pick these libav* up while
# Ubuntu's own packages stay installed and keep their security updates.  The
# build is the full native codec set (no --disable-everything experiment
# config), because this library is what every player on the pad actually uses.
#
# Requires libva-av1fix installed (or LIBVA_PREFIX pointing at a tree that was
# built from it): ffmpeg's VA-API AV1 code does not compile without the
# refresh_frame_flags fields in <va/va_dec_av1.h>.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/ffmpeg-av1fix"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")
UPSTREAM=${VER%%+*}			# 8.0.1
DEB_DIR="$HERE/debs"
LIBVA_PREFIX=${LIBVA_PREFIX:-/usr/local}

TARBALL_SHA=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
PATCHES="$HERE/av1fix-patches"
for p in 0002-ffmpeg-vaapi-av1-carry-refresh-frame-flags.patch \
	 0006-ffmpeg-vaapi-av1-fix-loop-restoration-type.patch; do
	[ -f "$PATCHES/$p" ] || { echo "missing $PATCHES/$p" >&2; exit 1; }
done

grep -q refresh_frame_flags_valid "$LIBVA_PREFIX/include/va/va_dec_av1.h" ||
	{ echo "no patched libva headers under $LIBVA_PREFIX/include/va" >&2
	  echo "  install libva-av1fix first, or set LIBVA_PREFIX" >&2; exit 1; }

tarball=""
for c in "${FFMPEG_TARBALL:-}" \
	 "$HERE/../../build/av1-refresh-fix/src/ffmpeg-$UPSTREAM.tar.xz" \
	 "$HERE/../../tmp/av1fix-src/ffmpeg-$UPSTREAM.tar.xz"; do
	[ -n "$c" ] && [ -f "$c" ] && { tarball=$c; break; }
done
if [ -z "$tarball" ]; then
	echo "==> downloading ffmpeg $UPSTREAM"
	mkdir -p "$HERE/../../tmp/av1fix-src"
	tarball="$HERE/../../tmp/av1fix-src/ffmpeg-$UPSTREAM.tar.xz"
	curl -fsSL -o "$tarball" \
		"https://ffmpeg.org/releases/ffmpeg-$UPSTREAM.tar.xz"
fi
echo "==> ffmpeg tarball: $tarball"
got=$(sha256sum "$tarball" | cut -d" " -f1)
[ "$got" = "$TARBALL_SHA" ] || { echo "unexpected ffmpeg tarball ($got)" >&2; exit 1; }

WORK="$HERE/../../tmp/av1fix-build/ffmpeg"
rm -rf "$WORK"
mkdir -p "$WORK"
STAGE="$WORK/root"
LIBDIR="$LIBVA_PREFIX/lib/aarch64-linux-gnu"

tar -C "$WORK" -xJf "$tarball"
cd "$WORK/ffmpeg-$UPSTREAM"
for p in "$PATCHES"/0002-*.patch "$PATCHES"/0006-*.patch; do
	patch -p1 --batch --forward <"$p"
done
grep -q refresh_frame_flags_valid libavcodec/vaapi_av1.c || {
	echo "patch 0002 did not reach libavcodec/vaapi_av1.c" >&2; exit 1; }

./configure \
	--prefix=/usr/local \
	--libdir=/usr/local/lib/aarch64-linux-gnu \
	--enable-shared --disable-static \
	--disable-doc --disable-debug \
	--enable-vaapi \
	--enable-libdrm \
	--extra-cflags="-I$LIBVA_PREFIX/include" \
	--extra-ldflags="-L$LIBDIR -Wl,-rpath,/usr/local/lib/aarch64-linux-gnu"
make -j"$(nproc)"
make DESTDIR="$STAGE" install
# a stray build tree must never end up in the payload
rm -rf "$STAGE/usr/local/share/doc" "$STAGE/usr/local/share/man"

# /usr/local/lib itself comes AFTER /usr/lib in ld.so.conf, only the multiarch
# directory really shadows Ubuntu's libavcodec - hence the explicit --libdir.
[ -e "$STAGE/usr/local/lib/aarch64-linux-gnu/libavcodec.so.62" ] || {
	echo "libavcodec was not installed in the stage" >&2; exit 1; }
"$STAGE/usr/local/bin/ffmpeg" -hide_banner -hwaccels 2>/dev/null |
	grep -q vaapi || { echo "stage ffmpeg has no vaapi hwaccel" >&2; exit 1; }

install -d -m755 "$STAGE/DEBIAN"
cp -a "$SRC/DEBIAN/." "$STAGE/DEBIAN/"

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
