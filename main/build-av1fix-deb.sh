#!/bin/bash
# Build oneplus-erhai-av1fix_<version>_arm64.deb: the private AV1 client stack.
#
#   ./build-av1fix-deb.sh        build (reuses a finished build of the same
#                               versions, FRESH=1 forces a rebuild)
#
# Payload: /opt/oneplus-erhai-av1fix/{lib,include} -- libva 2.23.0 with the
# refresh_frame_flags pass-through plus FFmpeg 8.0.1 with the VA-API AV1
# refresh-set and loop-restoration fixes.  Nothing goes into the system
# library path; Ubuntu keeps owning /usr/lib.  The package's wrappers
# (/usr/local/bin/mpv, /usr/local/bin/ffplay, the diverted /usr/bin/vlc)
# prepend /opt/oneplus-erhai-av1fix/lib to LD_LIBRARY_PATH.
#
# FFmpeg is configured like a distribution build for the parts a player needs:
# vdpau (VLC's configure probes av_vdpau_bind_context and fails to link that
# plugin when the local libavcodec lacks it -- the 2026-11-23 VLC 4 link
# error), libdav1d, gnutls (https in libavformat) and VA-API.
#
# The driver (oneplus-erhai-video) is built against the patched headers;
# build-video-deb.sh points PKG_CONFIG_PATH here on its own.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-av1fix"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")
DEB_DIR="$HERE/debs"

PREFIX=/opt/oneplus-erhai-av1fix
LIBVA_VER=2.23.0
LIBVA_SHA=b10aceb30e93ddf13b2030eb70079574ba437be9b3b76065caf28a72c07e23e7
FFMPEG_VER=8.0.1
FFMPEG_SHA=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
PATCHES="$HERE/av1fix-patches"
DRIVERDIR=${DRIVERDIR:-/usr/lib/aarch64-linux-gnu/dri}

WORK="$HERE/../../tmp/av1fix-opt-build"
STAGE="$WORK/root"
SRCS="$HERE/../../tmp/av1fix-src"
STAMP="$WORK/.built-$LIBVA_VER-$FFMPEG_VER"

mkdir -p "$SRCS" "$DEB_DIR"

fetch() {	# fetch <url> <file> <sha256>
	local url=$1 file=$2 want=$3
	if [ ! -f "$file" ]; then
		echo "==> downloading $(basename "$file")"
		curl -fsSL -o "$file" "$url"
	fi
	[ "$(sha256sum "$file" | cut -d' ' -f1)" = "$want" ] || {
		echo "unexpected checksum for $file" >&2; exit 1; }
}

# The 2026-11-15 AV1 work left both upstream tarballs in build/av1-refresh-fix;
# reuse them instead of downloading (the libva release asset is not always
# reachable).
seed_local() {	# seed_local <dst> <candidate...>
	local dst=$1 c
	shift
	[ -f "$dst" ] && return 0
	for c in "$@"; do
		[ -f "$c" ] && { cp "$c" "$dst"; return 0; }
	done
	return 0
}
seed_local "$SRCS/libva-$LIBVA_VER.tar.gz" \
	"$HERE/../../build/av1-refresh-fix/src/libva-$LIBVA_VER.tar.gz"
seed_local "$SRCS/ffmpeg-$FFMPEG_VER.tar.xz" \
	"$HERE/../../build/av1-refresh-fix/src/ffmpeg-$FFMPEG_VER.tar.xz"

if [ "${FRESH:-0}" = 1 ] || [ ! -f "$STAMP" ]; then
	fetch "https://github.com/intel/libva/releases/download/$LIBVA_VER/libva-$LIBVA_VER.tar.gz" \
	      "$SRCS/libva-$LIBVA_VER.tar.gz" "$LIBVA_SHA"
	fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" \
	      "$SRCS/ffmpeg-$FFMPEG_VER.tar.xz" "$FFMPEG_SHA"

	rm -rf "$WORK"
	mkdir -p "$STAGE"

	# ------------------------------------------------------------ libva ------
	tar -C "$WORK" -xzf "$SRCS/libva-$LIBVA_VER.tar.gz"
	( cd "$WORK/libva-$LIBVA_VER"
	  patch -p1 --batch --forward < "$PATCHES/0001-libva-av1-carry-refresh-frame-flags.patch"
	  grep -q refresh_frame_flags_valid va/va_dec_av1.h
	  meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release \
		-Ddriverdir="$DRIVERDIR" \
		-Dwith_x11=no -Dwith_glx=no -Dwith_wayland=no -Denable_docs=false
	  ninja -C build -j"$(nproc)"
	  DESTDIR="$STAGE" ninja -C build install )

	# ----------------------------------------------------------- ffmpeg ------
	tar -C "$WORK" -xJf "$SRCS/ffmpeg-$FFMPEG_VER.tar.xz"
	( cd "$WORK/ffmpeg-$FFMPEG_VER"
	  patch -p1 --batch --forward < "$PATCHES/0002-ffmpeg-vaapi-av1-carry-refresh-frame-flags.patch"
	  patch -p1 --batch --forward < "$PATCHES/0006-ffmpeg-vaapi-av1-fix-loop-restoration-type.patch"
	  grep -q refresh_frame_flags_valid libavcodec/vaapi_av1.c
	  # The stage paths must stay OUT of the configure command line: FFmpeg
	  # records it verbatim in FFMPEG_CONFIGURATION, which every library
	  # embeds (ppa_scrub_debug then refuses the workspace path).
	  PKG_CONFIG_PATH="$STAGE$PREFIX/lib/pkgconfig" \
	  CPPFLAGS="-I$STAGE$PREFIX/include" \
	  LDFLAGS="-L$STAGE$PREFIX/lib -Wl,-rpath,$PREFIX/lib" \
	  ./configure \
		--prefix="$PREFIX" --libdir="$PREFIX/lib" --incdir="$PREFIX/include" \
		--enable-shared --disable-static --disable-doc --disable-debug \
		--disable-stripping --enable-gpl \
		--enable-vaapi --enable-vdpau --enable-libdrm \
		--enable-libdav1d --enable-gnutls
	  make -j"$(nproc)"
	  make DESTDIR="$STAGE" install )

	# Only the runtime libraries, the headers and the pkg-config files: the
	# players use their own binaries and the ffmpeg tools stay Ubuntu's.
	rm -rf "$STAGE$PREFIX/bin" "$STAGE$PREFIX/share"
	touch "$STAMP"
else
	echo "==> reusing the finished build in $WORK (FRESH=1 rebuilds)"
fi

# --------------------------------------------------------------- package -----
install -d -m755 "$STAGE/DEBIAN"
cp -a "$SRC/DEBIAN/." "$STAGE/DEBIAN/"
install -d -m755 "$STAGE/usr/local/bin" "$STAGE/usr/share/oneplus-erhai-av1fix"
install -m755 "$SRC/usr/local/bin/mpv" "$SRC/usr/local/bin/ffplay" \
	"$STAGE/usr/local/bin/"
install -m755 "$SRC/usr/share/oneplus-erhai-av1fix/vlc" \
	"$STAGE/usr/share/oneplus-erhai-av1fix/vlc"
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

# Payload sanity: the patched header, libva, libavcodec with the symbols VLC's
# configure probes for.
[ -f "$STAGE$PREFIX/lib/libva.so.2" ] || { echo "no libva.so.2" >&2; exit 1; }
[ -f "$STAGE$PREFIX/lib/libavcodec.so.62" ] || { echo "no libavcodec" >&2; exit 1; }
grep -q refresh_frame_flags_valid "$STAGE$PREFIX/include/va/va_dec_av1.h" ||
	{ echo "no patched va_dec_av1.h" >&2; exit 1; }
for sym in av_vdpau_bind_context av_vdpau_get_surface_parameters; do
	nm -D --defined-only "$STAGE$PREFIX/lib/libavcodec.so.62" | grep -q "$sym" ||
		{ echo "libavcodec has no $sym (vdpau missing)" >&2; exit 1; }
done

# Depends from the payload itself (the private libs resolve into the stage, so
# they are skipped; everything else maps to its owning Ubuntu package).
deps=$(
	LD_LIBRARY_PATH="$STAGE$PREFIX/lib" ldd "$STAGE$PREFIX"/lib/*.so* 2>/dev/null |
	awk '/=> \// {print $3}' | sort -u |
	while IFS= read -r lib; do
		case "$lib" in "$STAGE"*) continue ;; esac
		dpkg -S "$lib" 2>/dev/null | head -1 | cut -d: -f1
	done | sort -u | tr '\n' ' ')
deps="${deps% }"
[ -n "$deps" ] || { echo "could not compute Depends" >&2; exit 1; }
sed -i "s|^Depends: .*|Depends: ${deps// /, }|" "$STAGE/DEBIAN/control"
echo "==> Depends: ${deps// /, }"

# owner rule: no debug info, no build path in the payload
ppa_scrub_debug "$STAGE$PREFIX"

( cd "$STAGE"
  find opt usr -type f -exec md5sum {} \; > DEBIAN/md5sums )

dpkg-deb --build --root-owner-group "$STAGE" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
echo "built ${PKG}_${VER}_${ARCH}.deb"
