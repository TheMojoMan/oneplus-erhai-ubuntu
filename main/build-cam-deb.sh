#!/bin/bash
# Build oneplus-erhai-erhaicam_<version>_arm64.deb from the
# oneplus-erhai-erhaicam/ staging tree.
#
#   ./build-cam-deb.sh -> debs/oneplus-erhai-erhaicam_<version>_arm64.deb
#
# ErhaiCam: STANDALONE GTK4/GStreamer camera app (preview, autofocus and
# auto-exposure measured in-process, photos, H.264+AAC video recording with an
# optional microphone soundtrack, camera switch, accelerometer orientation).
# NOT part of the camera-drivers package (oneplus-erhai-camera = kernel drivers
# + autofocus udev rules); this deb only ships the app binary + desktop file.
# The binary is compiled here from src/erhai-cam.cpp so the deb always
# matches its shipped source.
# The compile step needs libgtk-4-dev, libgstreamer1.0-dev and
# libgstreamer-plugins-base1.0-dev on the build host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-erhaicam"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

for f in src/erhai-cam.cpp \
	 usr/share/applications/com.github.themojoman.ErhaiCam.desktop DEBIAN/control DEBIAN/postinst; do
	[ -f "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

PKGS="gstreamer-app-1.0 gstreamer-video-1.0 gtk4"
pkg-config --exists $PKGS || {
	echo "missing pkg-config deps ($PKGS); install libgtk-4-dev," \
	     "libgstreamer1.0-dev, libgstreamer-plugins-base1.0-dev" >&2
	exit 1
}
mkdir -p "$SRC/usr/bin"
${CXX:-g++} -O2 -g -std=c++20 -Wall -Wextra -Wpedantic \
	$(pkg-config --cflags $PKGS) \
	-o "$SRC/usr/bin/erhai-cam" \
	"$SRC/src/erhai-cam.cpp" \
	$(pkg-config --libs $PKGS)

# The analysis is compiled into the app (erhai-cam.cpp): the deb ships no
# python helper, no ONNX model and no onnxruntime/QNN dependency.
find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/usr/bin/erhai-cam
# Deterministic manifest, so dpkg -V has something to verify (every erhai deb
# ships one; this builder was the exception until 1.6).
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
# personal data: no build path in the payload, no DWARF in the ELFs
ppa_scrub_debug "$SRC"
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations;
# the PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
echo "built ${PKG}_${VER}_${ARCH}.deb"
