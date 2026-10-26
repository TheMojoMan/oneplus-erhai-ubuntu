#!/bin/bash
# Build an Ubuntu rootfs image for the OnePlus Pad 3 (erhai / SM8750).
#
# ONE SCRIPT PER UBUNTU RELEASE, because their package sets differ on purpose:
#
#   build-root/resolute/build-root.sh   Ubuntu 26.04 LTS (resolute) -- the
#                                       shipped image, WITH the erhai patches
#                                       of the Ubuntu desktop stack (mutter
#                                       touch-drag guard, GTK4 guard, Mesa
#                                       /usr repack, nautilus/ptyxis)
#   build-root/stonking/build-root.sh   Ubuntu 26.10 (stonking, GNOME 51) --
#                                       first test image, STOCK desktop stack,
#                                       only the erhai-owned packages
#
# Usage:  sudo ./build-root.sh                      -> the HOST's release
#         sudo ERHAI_SERIES=stonking ./build-root.sh -> the 26.10 image
#         sudo PRESET_USER=1 ./build-root.sh         -> ready-made user + autologin
#
# Result: ../rootfs.img (resolute) or ../rootfs-stonking.img (stonking), ready
# to dd to the "linux" partition (PARTLABEL=linux).  Run it from this directory
# (the series scripts resolve ../ as the workspace root).  Everything else --
# stages, overlay, apt sources, verification -- lives in the series script.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERIES="${ERHAI_SERIES:-$(sed -n 's/^VERSION_CODENAME=//p' /etc/os-release 2>/dev/null | tr -d '"')}"
[ -n "$SERIES" ] || SERIES=resolute

if [ ! -x "$HERE/build-root/$SERIES/build-root.sh" ]; then
	echo "no rootfs script for series '$SERIES' (have: $(cd "$HERE/build-root" && ls -d */ | tr -d /))" >&2
	exit 2
fi

cd "$HERE"
exec "$HERE/build-root/$SERIES/build-root.sh" "$@"
