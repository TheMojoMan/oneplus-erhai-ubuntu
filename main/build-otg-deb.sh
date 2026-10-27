#!/bin/bash
# Build oneplus-erhai-otg_<version>_all.deb from the
# oneplus-erhai-otg/ staging tree.
#
#   ./build-otg-deb.sh -> debs/oneplus-erhai-otg_<version>_all.deb
#
# Plain binary-package layout like build-thermal-deb.sh. The deb is published
# to the PPA folder; build-root.sh picks it up from /opt/erhai-debs/.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-otg"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

[ -d "$SRC/usr" ] || [ -d "$SRC/etc" ] ||
	{ echo "no payload in $SRC" >&2; exit 1; }

find "$SRC" -type d -exec chmod 755 {} \;
[ -f "$SRC/DEBIAN/postinst" ] && chmod 755 "$SRC/DEBIAN/postinst"
[ -f "$SRC/usr/local/sbin/erhai-otg-rearm.sh" ] && chmod 755 "$SRC/usr/local/sbin/erhai-otg-rearm.sh"
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

# NO_PPA=1 keeps the deb local (debs/) instead of copying it into the testing suite --
# the same guard the other erhai builders have (dpkg-deb generates DEBIAN/md5sums
# automatically since dpkg 1.19, so the manifest is always shipped).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
	echo "built ${PKG}_${VER}_all.deb"
else
	echo "NO_PPA=1: built ${PKG}_${VER}_all.deb (NOT published; stays in $DEB_DIR)"
fi
