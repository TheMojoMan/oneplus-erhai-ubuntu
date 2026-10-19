#!/bin/bash
# Build oneplus-erhai-sensors_<version>_arm64.deb from the
# oneplus-erhai-sensors/ staging tree.
#
#   ./build-sensors-deb.sh -> debs/oneplus-erhai-sensors_<version>_arm64.deb
#
# Ships the whole SSC sensor stack: patched iio-sensor-proxy 3.9 (SSC support
# + startup-claim-race fix) + libssc 0.4.4 in /usr/local, patched
# hexagonrpcd-erhai with an /etc/systemd/system/hexagonrpcd.service OVERRIDE
# (same name replaces the distro unit, which is After=multi-user.target and thus
# ~75 s late on erhai because multi-user waits for the WiFi bring-up) and the
# complete /var/lib/hexagonfs-erhai remotefs tree (MUST be complete before
# hexagonrpcd's first attach -- the ADSP sensor PD loads its configs once),
# the 90-erhai-ssc-sensors.rules udev rule (SSC types + ACCEL_MOUNT_MATRIX
# -90deg + SYSTEMD_WANTS), an iio-sensor-proxy.service ExecStart drop-in and
# the erhai-sensors@erhai.local quick-settings extension (auto-rotate saved
# and turned OFF on pogo-kbd attach, recovered on detach, always-enabled
# toggle + built-in-toggle suppression, auto-brightness).
# Real ELF binaries => Architecture: arm64. Depends on the distro
# iio-sensor-proxy/hexagonrpcd/tqftpserv packages (unit files, base daemons).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-sensors"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

for f in DEBIAN/postinst \
	 etc/systemd/system/hexagonrpcd.service \
	 etc/systemd/system/iio-sensor-proxy.service.d/erhai.conf \
	 etc/systemd/system/erhai-tablet-mode.service \
	 etc/systemd/user/erhai-alsd.service \
	 etc/udev/rules.d/90-erhai-ssc-sensors.rules \
	 usr/local/bin/hexagonrpcd-erhai \
	 usr/local/bin/monitor-sensor \
	 usr/local/bin/ssccli \
	 usr/local/libexec/iio-sensor-proxy \
	 usr/local/lib/aarch64-linux-gnu/libssc.so.2 \
	 usr/local/sbin/erhai-tablet-mode.py \
	 usr/local/sbin/erhai-alsd.py \
	 usr/share/gnome-shell/extensions/erhai-sensors@erhai.local/extension.js \
	 usr/share/gnome-shell/extensions/erhai-sensors@erhai.local/metadata.json \
	 usr/share/gnome-shell/extensions/erhai-sensors@erhai.local/schemas/gschemas.compiled \
	 usr/share/gnome-shell/extensions/erhai-sensors@erhai.local/schemas/org.erhai.sensors.gschema.xml \
	 var/lib/hexagonfs-erhai/odm/sensor/config/json_list \
	 var/lib/hexagonfs-erhai/dsp/fastrpc_shell_0 \
	 var/lib/hexagonfs-erhai/socinfo; do
	[ -e "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst \
	"$SRC"/usr/local/bin/hexagonrpcd-erhai \
	"$SRC"/usr/local/bin/monitor-sensor \
	"$SRC"/usr/local/bin/ssccli \
	"$SRC"/usr/local/libexec/iio-sensor-proxy \
	"$SRC"/usr/local/sbin/erhai-tablet-mode.py \
	"$SRC"/usr/local/sbin/erhai-alsd.py

# Compile the extension's GSettings schema (org.erhai.sensors).
glib-compile-schemas \
	"$SRC"/usr/share/gnome-shell/extensions/erhai-sensors@erhai.local/schemas/
chmod -R u=rwX,go=rX "$SRC"/etc "$SRC"/var "$SRC"/usr/share

# md5sums for every payload file (DEBIAN/ excluded), sorted so the manifest is
# deterministic -- without it `dpkg -V` has nothing to verify.  Same recipe as
# build-npu-deb.sh / the fastrpc-style builders.
rm -f "$SRC/DEBIAN/md5sums"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
echo "==> md5sums: $(grep -c '' "$SRC/DEBIAN/md5sums") files"

dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations;
# the PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
echo "built ${PKG}_${VER}_${ARCH}.deb"
