#!/bin/bash
# Build oneplus-erhai-bluetooth_<version>_all.deb from the
# oneplus-erhai-bluetooth/ staging tree.
#
#   ./build-bluetooth-deb.sh -> debs/oneplus-erhai-bluetooth_<version>_all.deb
#
# Since 2.2 the package carries NO files outside DEBIAN/: per-device BT
# address machinery is retired (shared DTB address, stock bluetooth.service
# bring-up). The postinst only retires units/files left by <= 2.1 installs.
# Staged into root-setup-files/packages/ for build-root.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-bluetooth"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/DEBIAN/postrm
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
echo "built ${PKG}_${VER}_all.deb"
