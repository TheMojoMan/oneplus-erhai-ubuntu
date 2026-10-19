#!/bin/bash
# Build oneplus-erhai-thermal_<version>_all.deb from the
# oneplus-erhai-thermal/ staging tree.
#
#   ./build-thermal-deb.sh -> debs/oneplus-erhai-thermal_<version>_all.deb
#
# Plain binary-package layout like build-bluetooth-deb.sh.
# Staged into root-setup-files/packages/ for build-root.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-thermal"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

[ -d "$SRC/usr" ] || [ -d "$SRC/etc" ] ||
	{ echo "no payload in $SRC" >&2; exit 1; }

find "$SRC" -type d -exec chmod 755 {} \;
[ -f "$SRC/DEBIAN/postinst" ] && chmod 755 "$SRC/DEBIAN/postinst"

# md5sums for every payload file (DEBIAN/ excluded), sorted so the manifest is
# deterministic and `dpkg -V` has something to verify against.
rm -f "$SRC/DEBIAN/md5sums"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
# conffiles are EXCLUDED from the manifest (dpkg-deb does the same): they are
# modifiable by design - /etc/default/erhai-thermal is rewritten by the governor
# on every profile switch - so listing them would make `dpkg -V` cry wolf.
if [ -s "$SRC/DEBIAN/conffiles" ]; then
	awk 'NR == FNR { p = $0; sub(/^\//, "", p); skip[p] = 1; next }
	     !($2 in skip)' "$SRC/DEBIAN/conffiles" "$SRC/DEBIAN/md5sums" > "$SRC/DEBIAN/md5sums.new"
	mv "$SRC/DEBIAN/md5sums.new" "$SRC/DEBIAN/md5sums"
fi
echo "==> md5sums: $(grep -c '' "$SRC/DEBIAN/md5sums") files (conffiles excluded)"
OUT="$DEB_DIR/${PKG}_${VER}_all.deb"
dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $(basename "$OUT")"

# NO_PPA=1 builds locally without publishing (in-development iterations; the PPA
# is only updated when the owner asks for it).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
