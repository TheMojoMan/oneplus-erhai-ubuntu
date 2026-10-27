#!/bin/bash
# Build oneplus-erhai-nautilus_<version>_arm64.deb — the erhai-patched GNOME
# Files (Nautilus) delivered as a dpkg-divert override of Ubuntu's files.
#
#   ./build-nautilus-deb.sh              -> local deb in oneplus-erhai-ubuntu/
#   PUBLISH=1 ./build-nautilus-deb.sh    -> also ppa_copy to the PPA suite
#   PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" PUBLISH=1 ./build-nautilus-deb.sh
#                                        -> straight to the public MAIN suite
#   SKIP_BUILD=1 ./build-nautilus-deb.sh -> reuse the existing build/ binary
#
# The payload (patched /usr/bin/nautilus + gschema) comes from
# oneplus-erhai-nautilus/build/ (see build-nautilus-erhai.sh).  The deb ships
# them under /usr/share/oneplus-erhai-nautilus/ and DEBIAN/postinst diverts
# Ubuntu's own files (originals kept as *.erhai-orig).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
PROJ="$HERE/oneplus-erhai-nautilus"
SRC="$PROJ/pkg"

[ -f "$SRC/DEBIAN/control" ] || { echo "missing $SRC/DEBIAN/control" >&2; exit 1; }
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

# 1. make sure the patched binary is current
if [ "${SKIP_BUILD:-0}" != "1" ]; then
	"$PROJ/build-nautilus-erhai.sh"
fi
[ -f "$PROJ/build/bin/nautilus" ] || {
	echo "no built binary; run $PROJ/build-nautilus-erhai.sh" >&2; exit 1; }

# 2. stage the payload
PAYLOAD="$SRC/usr/share/oneplus-erhai-nautilus"
rm -rf "$SRC/usr"
mkdir -p "$PAYLOAD"
install -m 0755 "$PROJ/build/bin/nautilus" "$PAYLOAD/nautilus"
install -m 0644 "$PROJ/build/schemas/org.gnome.nautilus.gschema.xml" \
	"$PAYLOAD/org.gnome.nautilus.gschema.xml"
install -m 0644 "$PROJ/README.md" "$PAYLOAD/README.md"

# 3. guards: the binary and schema must actually carry our changes
# (grep without -q so `strings` is not SIGPIPE-d under pipefail)
strings "$PAYLOAD/nautilus" | grep 'xdg-terminal-exec' >/dev/null ||
	{ echo "binary lacks xdg-terminal-exec (wrong build options)" >&2; exit 1; }
grep -q 'show-select-boxes' "$PAYLOAD/org.gnome.nautilus.gschema.xml" ||
	{ echo "gschema lacks show-select-boxes" >&2; exit 1; }

# 4. perms + maintainer scripts + md5sums manifest
find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )

DEB="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
rm -f "$DEB"
dpkg-deb --build --root-owner-group "$SRC" "$DEB"

echo "built ${DEB##*/}"
if [ "${PUBLISH:-0}" = "1" ]; then
	ppa_copy "$DEB"
else
	echo "(local; PUBLISH=1 to push to the PPA suite, or set PPA_DEB_DIR to main)"
fi
