#!/bin/bash
# Build the kernel .deb from the PRE-STAGED payload snapshot in
# oneplus-erhai-linux/ -- WITHOUT compiling anything.
#
#   ./build-linux-deb.sh
#
# The snapshot tree must already contain the staged kernel image and modules
# (DEBIAN/ + boot/vmlinuz-<ver> + boot/dtb-<ver> + usr/lib/modules/<ver>/).
# Use the kernel tree's own build-kernel.sh to (re)stage those, then run this
# script to pack + publish. Useful when only the packaging (DEBIAN/control,
# postinst, postrm) changed and no recompile is wanted.
#
# Output: debs/oneplus-erhai-linux-<maj.min>_<release>_arm64.deb, published
# to themojoman.github.io/ppa/oneplus-erhai/ubuntu/main/ (see ppa-common.sh).
# NOTE: `dpkg -i` reruns the BAKED-IN postinst -- rebuild this deb after
# editing postinst behaviour that must run at install time.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-linux"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

# Sanity: staged payload must match the control version exactly.
[ -f "$SRC/boot/vmlinuz-$VER" ] || {
	echo "no boot/vmlinuz-$VER in $SRC (staging tree out of sync with control)" >&2
	exit 1
}
[ -d "$SRC/usr/lib/modules/$VER" ] || {
	echo "no usr/lib/modules/$VER in $SRC (staging tree out of sync with control)" >&2
	exit 1
}

find "$SRC" -type d -exec chmod 755 {} +
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
echo "built ${PKG}_${VER}_${ARCH}.deb (no kernel compile)"