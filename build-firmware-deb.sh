#!/bin/bash
# Build oneplus-erhai-firmware_<version>_all.deb from the oneplus-erhai-firmware/
# staging tree and publish it to the PPA folder.
#
#   ./build-firmware-deb.sh
#
# Plain binary-package layout (DEBIAN/control + files as they land on the
# target): WCN7880 "peach" firmware blobs under /lib/firmware, plus the other
# device firmware the kernel deb's postinst expects on the ESP/rootfs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-firmware"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

find "$SRC" -type d -exec chmod 755 {} \;
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
echo "built ${PKG}_${VER}_all.deb"
