#!/bin/bash
# Build oneplus-erhai-wifi_<version>_all.deb from the oneplus-erhai-wifi/
# staging tree.
#
#   ./build-wifi-deb.sh     -> debs/oneplus-erhai-wifi_<version>_all.deb
#
# The tree is a plain binary-package layout (DEBIAN/control + files as they
# land on the target), same as build-audio-deb.sh. It carries everything the
# internal WCN7880 WiFi needs at boot: modprobe.d policy, the three
# erhai-wcn-*.service units (+ their scripts and wl_drvcmd) and the qcacld
# config INI (scan_mode_6ghz=0). The deb is staged into
# root-setup-files/packages/ so build-root.sh installs it into the image.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-wifi"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

[ -x "$SRC/usr/local/sbin/erhai-wcn-wifi.sh" ] ||
	{ echo "no bring-up script in $SRC" >&2; exit 1; }

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/DEBIAN/prerm \
          "$SRC"/usr/local/sbin/*
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
echo "built ${PKG}_${VER}_all.deb"