#!/bin/bash
# Build the release-independent mesa-devel meta .deb.
#
#   ./build-mesa-devel-deb.sh            -> debs/mesa-devel_<gen>_all.deb
#   NO_PPA=1 ./build-mesa-devel-deb.sh   -> keep it local (no ppa_copy)
#
# WHAT IT IS: mesa-devel carries no payload.  It is the single name that pulls
# in the erhai Mesa /usr replacement -- the six Ubuntu-named packages built per
# release by resolute|stonking/build-mesa-usr-debs.sh -- with a version floor,
# not an exact "=", so the SAME meta package is installable on 26.04 and 26.10
# and apt still refuses it on a release that has no erhai Mesa generation.
#
# VERSION: mirrors the Mesa generation (e.g. 27.0.0~devel.git234733.g8df830aa844-1erhai1)
# taken from the per-suite staging controls, so a mesa refresh always rebuilds
# the meta.  The dependency floor is the OLDEST generation found: that keeps the
# meta installable on both releases when the suites are built at different
# commits (an unpublished, newer set must never raise the floor above what the
# other release has).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/mesa-devel"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")

GEN=""
for stage in "$HERE/../resolute/oneplus-erhai-mesa" "$HERE/../stonking/oneplus-erhai-mesa"; do
	[ -f "$stage/DEBIAN/control" ] || continue
	# a staging tree only counts once it carries a built /usr payload -- the
	# placeholder control of a suite that has not been built yet must not set
	# the version/floor (it would drag both down to Ubuntu's mesa version)
	[ -d "$stage/usr/lib/aarch64-linux-gnu" ] || continue
	v=$(awk '/^Version:/ {print $2}' "$stage/DEBIAN/control")
	[ -n "$v" ] || continue
	if [ -z "$GEN" ] || dpkg --compare-versions "$v" lt "$GEN"; then
		GEN="$v"
	fi
done
GEN="${MESA_GEN:-$GEN}"
[ -n "$GEN" ] && [ "$GEN" != 0.0 ] || {
	echo "FATAL: no Mesa generation found in resolute|stonking/oneplus-erhai-mesa" >&2
	exit 1
}

sed -i "s/^Version: .*/Version: $GEN/" "$SRC/DEBIAN/control"
sed -i "s/(>= [^)]*)/(>= $GEN)/g" "$SRC/DEBIAN/control"
rm -f "$SRC/DEBIAN/md5sums"
: > "$SRC/DEBIAN/md5sums"
chmod 644 "$SRC/DEBIAN/control" "$SRC/DEBIAN/md5sums"

OUT="$DEB_DIR/${PKG}_${GEN}_all.deb"
fakeroot dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published; stays in $DEB_DIR"
fi
