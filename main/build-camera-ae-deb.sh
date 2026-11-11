#!/bin/bash
# Build oneplus-erhai-camera-ae_<version>_all.deb from the vendored
# oneplus-erhai-camera-ae/ source tree.
#
#   ./build-camera-ae-deb.sh -> debs/oneplus-erhai-camera-ae_<version>_all.deb
#
# Maintainers: nadelacruz-00 (engine, vendored from
#              https://github.com/nadelacruz-00/erhai-camera-ae),
#              TheMojoMan (signs and publishes the .deb).
#
# Unlike the plain staging trees (build-thermal-deb.sh), the package dir holds
# the SOURCE layout (camera_ae/, tuning/, systemd/, tests/, see PROVENANCE.md);
# this builder assembles the payload in a temporary root:
#   /usr/lib/oneplus-erhai-camera-ae/{camera-ae.py,camera_ae/}
#   /usr/share/oneplus-erhai-camera-ae/ipa/simple/{sc820cs,sc1320cs,uncalibrated}.yaml
#   /usr/lib/systemd/user/erhai-camera-ae.service
#   /usr/lib/systemd/user/{pipewire,wireplumber}.service.d/oneplus-erhai-camera-ae.conf
#   /usr/share/doc/oneplus-erhai-camera-ae/{copyright,PROVENANCE.md}
# tests/ is run here (SKIP_TESTS=1 skips it) and never packed.
# NO_PPA=1 builds locally without publishing.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-camera-ae"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")
NAME=oneplus-erhai-camera-ae

for f in camera-ae.py camera_ae/main.py tuning/simple.yaml LICENSE PROVENANCE.md \
	 systemd/erhai-camera-ae.service systemd/pipewire-camera-ae.conf \
	 systemd/wireplumber-camera-ae.conf DEBIAN/postinst DEBIAN/postrm; do
	[ -f "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

if [ "${SKIP_TESTS:-0}" != 1 ]; then
	( cd "$SRC" && PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -q -s tests -t . )
fi

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
chmod 755 "$ROOT"

install -d -m755 "$ROOT/DEBIAN"
install -m644 "$SRC/DEBIAN/control" "$ROOT/DEBIAN/control"
install -m755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm" "$ROOT/DEBIAN/"

LIB="$ROOT/usr/lib/$NAME"
install -d -m755 "$LIB/camera_ae"
install -m755 "$SRC/camera-ae.py" "$LIB/camera-ae.py"
install -m644 "$SRC"/camera_ae/*.py "$LIB/camera_ae/"

IPA="$ROOT/usr/share/$NAME/ipa/simple"
install -d -m755 "$IPA"
for f in sc820cs.yaml sc1320cs.yaml uncalibrated.yaml; do
	install -m644 "$SRC/tuning/simple.yaml" "$IPA/$f"
done

UNITS="$ROOT/usr/lib/systemd/user"
install -d -m755 "$UNITS/pipewire.service.d" "$UNITS/wireplumber.service.d"
install -m644 "$SRC/systemd/erhai-camera-ae.service" "$UNITS/"
install -m644 "$SRC/systemd/pipewire-camera-ae.conf" "$UNITS/pipewire.service.d/$NAME.conf"
install -m644 "$SRC/systemd/wireplumber-camera-ae.conf" "$UNITS/wireplumber.service.d/$NAME.conf"

DOC="$ROOT/usr/share/doc/$NAME"
install -d -m755 "$DOC"
install -m644 "$SRC/LICENSE" "$DOC/copyright"
install -m644 "$SRC/PROVENANCE.md" "$DOC/PROVENANCE.md"

# Nothing from tests/ and no bytecode may reach the payload.
if find "$ROOT" \( -name tests -o -name 'test_*' -o -name __pycache__ -o -name '*.pyc' \) | grep -q .; then
	echo "refusing to pack: test files or bytecode in the payload" >&2
	exit 1
fi
find "$ROOT" -type d -exec chmod 755 {} +
ppa_scrub_debug "$ROOT"

rm -f "$ROOT/DEBIAN/md5sums"
( cd "$ROOT" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
chmod 644 "$ROOT/DEBIAN/md5sums"

OUT="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$ROOT" "$OUT"
echo "built $(basename "$OUT")"

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
