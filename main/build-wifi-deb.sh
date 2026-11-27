#!/bin/bash
# Build oneplus-erhai-wifi_<version>_all.deb from the oneplus-erhai-wifi/
# staging tree.
#
#   ./build-wifi-deb.sh     -> debs/oneplus-erhai-wifi_<version>_all.deb
#
# The tree is a plain binary-package layout (DEBIAN/control + files as they
# land on the target), same as build-audio-deb.sh. It carries only the ath12k
# policy: the modprobe.d backend selection (blacklist cnss2/wlan) and the
# README. No service, no script -- the vendor cnss2/qcacld bring-up that 1.5
# shipped was removed in 1.7. The deb is staged into
# root-setup-files/packages/ so build-root.sh installs it into the image.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-wifi"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

[ -f "$SRC/etc/modprobe.d/erhai-wifi-backend.conf" ] ||
	{ echo "no modprobe backend policy in $SRC" >&2; exit 1; }

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/DEBIAN/prerm
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

# NO_PPA=1 keeps the deb local (debs/) instead of copying it into the PPA suite --
# the same guard the other erhai builders have (dpkg-deb generates DEBIAN/md5sums
# automatically since dpkg 1.19, so the manifest is always shipped).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
	echo "built ${PKG}_${VER}_all.deb"
else
	echo "NO_PPA=1: built ${PKG}_${VER}_all.deb (NOT published; stays in $DEB_DIR)"
fi