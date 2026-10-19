#!/bin/bash
# Build oneplus-erhai-pen_<version>_all.deb from the oneplus-erhai-pen/
# staging tree.
#
#   ./build-pen-deb.sh           -> local deb in oneplus-erhai-ubuntu/
#   PUBLISH=1 ./build-pen-deb.sh -> also ppa_copy to the testing suite
#
# Development policy: local builds only until the owner asks for a publish,
# so ppa_copy is behind PUBLISH=1.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh" >/dev/null 2>&1 || true
DEB_DIR="${DEB_DIR:-$HERE/debs}"        # every built .deb lands in <repo>/debs
mkdir -p "$DEB_DIR"
SRC="$HERE/oneplus-erhai-pen"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

for f in DEBIAN/postinst \
	DEBIAN/postrm \
	etc/erhai/pen.conf \
	etc/systemd/system/erhai-pen.service \
	etc/systemd/system/erhai-pen-notify.service \
	usr/local/sbin/erhai-pen-monitor.py \
	usr/local/sbin/erhai-pen-notify.py \
	usr/local/sbin/erhai-pen \
	usr/share/oneplus-erhai-pen/README.md; do
	[ -e "$SRC/$f" ] || { echo "missing $SRC/$f" >&2; exit 1; }
done

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/DEBIAN/postrm \
	"$SRC"/usr/local/sbin/erhai-pen-monitor.py \
	"$SRC"/usr/local/sbin/erhai-pen-notify.py \
	"$SRC"/usr/local/sbin/erhai-pen

# Syntax gate before packaging.
python3 -m py_compile "$SRC/usr/local/sbin/erhai-pen-monitor.py" || exit 1
python3 -m py_compile "$SRC/usr/local/sbin/erhai-pen-notify.py" || exit 1
bash -n "$SRC/usr/local/sbin/erhai-pen" || exit 1
# py_compile just regenerated __pycache__ — do not ship it.
rm -f "$SRC"/usr/local/sbin/__pycache__/*.pyc 2>/dev/null || true
rmdir "$SRC"/usr/local/sbin/__pycache__ 2>/dev/null || true

# Every erhai deb ships a dpkg md5sums manifest (deterministic, fastrpc-style).
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )

DEB="$DEB_DIR/${PKG}_${VER}_all.deb"
rm -f "$DEB"
dpkg-deb --build --root-owner-group "$SRC" "$DEB"

if [ "${PUBLISH:-0}" = "1" ]; then
	ppa_copy "$DEB"
else
	echo "built ${DEB##*/} (local; PUBLISH=1 to push to the testing suite)"
fi