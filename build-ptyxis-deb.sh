#!/bin/bash
# Build oneplus-erhai-ptyxis_<version>_arm64.deb -- the erhai-patched Ptyxis
# (GNOME terminal) delivered as a dpkg-divert override of Ubuntu's
# /usr/bin/ptyxis.
#
#   ./build-ptyxis-deb.sh              -> local deb in oneplus-erhai-ubuntu/
#   PUBLISH=1 ./build-ptyxis-deb.sh    -> also ppa_copy to the TESTING suite
#   PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" PUBLISH=1 ./build-ptyxis-deb.sh
#                                      -> straight to the public MAIN suite
#   SKIP_BUILD=1 ./build-ptyxis-deb.sh -> reuse the existing build/ binary
#
# The payload (patched ptyxis + README + patch) comes from
# oneplus-erhai-ptyxis/build/ (see build-ptyxis-erhai.sh).  The deb ships it
# under /usr/share/oneplus-erhai-ptyxis/ and DEBIAN/postinst diverts Ubuntu's
# binary (original kept as /usr/bin/ptyxis.erhai-orig).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
PROJ="$HERE/oneplus-erhai-ptyxis"
SRC="$PROJ/pkg"

[ -f "$SRC/DEBIAN/control" ] || { echo "missing $SRC/DEBIAN/control" >&2; exit 1; }
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

# 1. make sure the patched binary is current
if [ "${SKIP_BUILD:-0}" != "1" ]; then
	"$PROJ/build-ptyxis-erhai.sh"
fi
[ -f "$PROJ/build/ptyxis" ] || {
	echo "no built binary; run $PROJ/build-ptyxis-erhai.sh" >&2; exit 1; }

# 2. stage the payload
PAYLOAD="$SRC/usr/share/oneplus-erhai-ptyxis"
rm -rf "$SRC/usr"
mkdir -p "$PAYLOAD"
install -m 0755 "$PROJ/build/ptyxis" "$PAYLOAD/ptyxis"
install -m 0644 "$PROJ/README.md" "$PAYLOAD/README.md"
install -m 0644 "$PROJ/ptyxis-erhai-touch.patch" "$PAYLOAD/ptyxis-erhai-touch.patch"

# 3. guards: the binary must actually carry our code, and the shipped patch
# must be the touch patch.  (Keep the nm output in a file: `nm ... | grep -q`
# would SIGPIPE nm and, under pipefail, report a false failure.)
SYMS="$(mktemp)"
trap 'rm -f "$SYMS"' EXIT
nm "$PAYLOAD/ptyxis" >"$SYMS" 2>/dev/null || true
for sym in ptyxis_terminal_autoscroll_cb ptyxis_terminal_fling_cb \
           ptyxis_terminal_touch_event_cb; do
	grep "[[:space:]]$sym\$" "$SYMS" >/dev/null ||
		{ echo "binary lacks $sym (erhai touch patch not applied?)" >&2; exit 1; }
done
grep -q 'GSK_BLEND_MODE_DIFFERENCE' "$PAYLOAD/ptyxis-erhai-touch.patch" ||
	{ echo "shipped patch is not the touch patch" >&2; exit 1; }

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
	echo "(local; PUBLISH=1 to push to the testing suite, or set PPA_DEB_DIR to main)"
fi
