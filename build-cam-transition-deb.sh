#!/bin/bash
# Build the EMPTY transitional oneplus-erhai-cam_<version>_all.deb.
#
#   ./build-cam-transition-deb.sh            # build + publish to TESTING
#   NO_PPA=1 ./build-cam-transition-deb.sh    # build locally only
#
# The ErhaiCam app was renamed oneplus-erhai-cam (<= 1.2) ->
# oneplus-erhai-erhaicam (see build-cam-deb.sh, which builds the real app).
# A device that still has the OLD name installed would otherwise never migrate:
# apt only follows a rename if a package carrying the old name exists and pulls
# the new one.  This package provides exactly that:
#
#   oneplus-erhai-cam 1.3 (this, empty)  --Depends-->  oneplus-erhai-erhaicam (>= 1.4)
#   oneplus-erhai-erhaicam 1.4           --Replaces/Breaks/Provides-->  oneplus-erhai-cam
#
# The version pinning matters: Replaces/Breaks are `<< 1.3`, so the transitional
# package (1.3) can coexist with the successor while the FILE-carrying old
# versions (<= 1.2) are handed over.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-cam"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$PKG" = "oneplus-erhai-cam" ] || die "unexpected package name '$PKG'"
[ "$ARCH" = "all" ] || die "the transitional package must be Architecture: all"

# It must be EMPTY: a payload here would fight oneplus-erhai-erhaicam for the
# same paths (/usr/bin/erhai-cam, the desktop file).  Only DEBIAN/ is allowed.
extra="$(find "$SRC" -mindepth 1 -maxdepth 1 ! -name DEBIAN -print -quit)"
[ -z "$extra" ] || die "the transitional package tree must contain only DEBIAN/ (found $extra)"
grep -q '^Depends: oneplus-erhai-erhaicam' "$SRC/DEBIAN/control" \
	|| die "control must depend on oneplus-erhai-erhaicam"

find "$SRC" -type d -exec chmod 755 {} +
find "$SRC" -type f -exec chmod 644 {} +

OUT="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
dpkg-deb -f "$OUT" Package Version Architecture Depends Replaces Breaks Provides

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
