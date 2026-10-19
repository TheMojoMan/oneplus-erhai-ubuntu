#!/bin/bash
# Build oneplus-erhai-mesa_<version>_arm64.deb from the
# oneplus-erhai-mesa/ staging tree.
#
#   ./update-mesa.sh           # refresh the staging payload from mesa git
#   ./build-mesa-deb.sh        # package it -> debs/oneplus-erhai-mesa_...deb
#
# The tree is a plain binary-package layout: DEBIAN/control plus the files as
# they land on the target (DESTDIR of the mesa build with prefix=/usr
# -- update-mesa.sh keeps it refreshed). md5sums is regenerated on every build
# so dpkg -V and debsums have something to check.
set -euo pipefail

# NOTE: HERE must be resolved against the ABSOLUTE script path BEFORE the
# 'cd $MESA_DIR' below -- a relative $0 breaks once CWD changed, which made
# 'source ppa-common.sh' (the ppa_copy publish step) fail whenever this
# builder was invoked as ./build-mesa-deb.sh from its own directory.
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

MESA_DIR="${MESA_DIR:-$HERE/oneplus-erhai-mesa}"
cd "$MESA_DIR"
PKG=$(awk '/^Package:/ {print $2}' DEBIAN/control)
VER=$(awk '/^Version:/ {print $2}' DEBIAN/control)
ARCH=$(awk '/^Architecture:/ {print $2}' DEBIAN/control)
# build output: <repo>/debs (see ppa-common.sh) -- resolved here because the
# publish step sources ppa-common.sh only further down.
DEB_DIR="${DEB_DIR:-$HERE/debs}"
mkdir -p "$DEB_DIR"
OUT="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

# Refresh Installed-Size (KiB) so apt's disk-space estimate is honest.
SIZE=$(du -sk --exclude=DEBIAN . | cut -f1)
sed -i "s/^Installed-Size: .*/Installed-Size: $SIZE/" DEBIAN/control

rm -f DEBIAN/md5sums
find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' |
	sort -z |
	xargs -0 -r md5sum > DEBIAN/md5sums

chmod 644 DEBIAN/control DEBIAN/md5sums
fakeroot dpkg-deb --build --root-owner-group . "$OUT"
echo "built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,20p'

. "$HERE/ppa-common.sh"
ppa_copy "$OUT"
