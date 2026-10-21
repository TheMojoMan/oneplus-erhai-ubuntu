#!/bin/bash
# Build oneplus-erhai-audio_<version>_all.deb from the oneplus-erhai-audio/
# staging tree.
#
#   ./build-audio-deb.sh     -> debs/oneplus-erhai-audio_<version>_all.deb
#
# The tree is a plain binary-package layout: DEBIAN/control plus the files as
# they land on the target (UCM profile + conf.d selection symlinks). The deb is
# then staged into root-setup-files/packages/ so build-root.sh picks it up.
#
# Modeled on the xiaomi-nabu-alsa deb (usr/share/alsa/ucm2/<profile> + a
# conf.d/<driver>/ selection file), except erhai needs the HiFi verb to also
# un-mute the AW88261 amps, which lives in the profile itself.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-audio"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

[ -f "$SRC/usr/share/alsa/ucm2/Qualcomm/sm8750/OnePlusPad3/HiFi.conf" ] ||
	{ echo "no UCM profile in $SRC" >&2; exit 1; }

find "$SRC" -type d -exec chmod 755 {} \;
# the helper scripts must be executable in the payload
chmod 755 "$SRC"/usr/local/sbin/*.sh
chmod 644 "$SRC"/etc/systemd/system/*.service
# Deterministic manifest, so dpkg -V has something to verify (every erhai deb
# ships one).
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
fi
echo "built ${PKG}_${VER}_all.deb"