#!/bin/bash
# Build ALL erhai .debs; each builder publishes into the suite folder it lives
# in (one folder per suite: main/ = universal, resolute/ and stonking/ = those
# Ubuntu releases):
#   themojoman.github.io/ppa/oneplus-erhai/ubuntu/<suite>/
# Run a builder with NO_PPA=1 to keep the deb in its <series>/debs instead.
#
#   ./build-all-debs.sh            # everything
#   ./build-all-debs.sh wifi audio # only the named builders
#
# Builders invoked (each publishes its own deb unless noted):
#   build-wifi-deb.sh        oneplus-erhai-wifi
#   build-audio-deb.sh       oneplus-erhai-audio
#   build-bluetooth-deb.sh   oneplus-erhai-bluetooth
#   build-camera-deb.sh      oneplus-erhai-camera
#   build-cam-deb.sh         oneplus-erhai-erhaicam (ErhaiCam GTK4 camera app,
#                            STANDALONE — not part of the camera-drivers pkg)
#   build-sensors-deb.sh     oneplus-erhai-sensors
#   build-desktop-deb.sh     oneplus-erhai-desktop
#   build-thermal-deb.sh     oneplus-erhai-thermal
#   build-torch-deb.sh       oneplus-erhai-torch (rear-flash torch quick slider)
#   build-otg-deb.sh         oneplus-erhai-otg
#   build-firmware-deb.sh    oneplus-erhai-firmware
#   build-mesa-deb.sh        oneplus-erhai-mesa (from oneplus-erhai-mesa/;
#                            refresh the payload with ./update-mesa.sh first)
#   build-linux-deb.sh       oneplus-erhai-linux (repacks the committed 7.3
#                            payload snapshot -- NO kernel compile)
#   build-npu-deb.sh         oneplus-erhai-npu (NPU/HTP workload layer)
#   build-fastrpc-deb.sh     oneplus-erhai-fastrpc (FastRPC transport: libcdsprpc,
#                            cdsprpcd, the fastrpc group, the DSP payload trees)
#   build-llamacpp-deb.sh    oneplus-erhai-llamacpp (patched llama.cpp stack)
#   build-pen-deb.sh         oneplus-erhai-pen (stylus/pen support; local build
#                            unless PUBLISH=1)
#   build-video-deb.sh       oneplus-erhai-video (VA-API driver, rebuilt from
#                            the shipped source on every run)
# The kernel deb is NOT COMPILED here -- for a full compile use ./build-kernel.sh.
# steam is not in the list on purpose: its builder stages from a local Valve
# installation (STEAM_SRC) and is run by hand.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

declare -A BUILDERS=(
	[wifi]=main/build-wifi-deb.sh
	[audio]=main/build-audio-deb.sh
	[bluetooth]=main/build-bluetooth-deb.sh
	[camera]=main/build-camera-deb.sh
	[cam]=main/build-cam-deb.sh
	[sensors]=main/build-sensors-deb.sh
	[desktop]=main/build-desktop-deb.sh
	[thermal]=main/build-thermal-deb.sh
	[torch]=main/build-torch-deb.sh
	[otg]=main/build-otg-deb.sh
	[firmware]=main/build-firmware-deb.sh
	[mesa]=resolute/build-mesa-deb.sh
	[linux]=main/build-linux-deb.sh
	[npu]=main/build-npu-deb.sh
	[fastrpc]=main/build-fastrpc-deb.sh
	[llamacpp]=main/build-llamacpp-deb.sh
	[pen]=main/build-pen-deb.sh
	[video]=main/build-video-deb.sh
	[waydroid]=main/build-waydroid-installer-deb.sh
)

# waydroid LAST: its builder needs the patcher project in ../tmp/waydroid-patcher
# (source + the five stripped Mesa objects) and refuses to run without it.
ORDER=(wifi audio bluetooth camera cam sensors desktop thermal torch otg firmware mesa linux npu fastrpc llamacpp pen video waydroid)

if [ $# -gt 0 ]; then
	for want in "$@"; do
		[ -n "${BUILDERS[$want]:-}" ] || {
			echo "unknown builder '$want' (known: ${ORDER[*]})" >&2
			exit 1
		}
	done
	SELECTED=("$@")
else
	SELECTED=("${ORDER[@]}")
fi

for name in "${SELECTED[@]}"; do
	echo "=== $name: ${BUILDERS[$name]} (suite ${BUILDERS[$name]%%/*}) ==="
	"$HERE/${BUILDERS[$name]}"
done

echo
echo "all requested debs built; each landed in <suite>/debs (suite = the builder folder)"
echo "next: ../update-ppa.sh"
