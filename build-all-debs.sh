#!/bin/bash
# Build ALL erhai .debs and publish every one of them to the apt repo's
# testing suite folder:
#   themojoman.github.io/ppa/oneplus-erhai/ubuntu/testing/
# (private, opt-in suite -- promote to the public main/ with
#  ../update-ppa.sh --cptesting once the debs proved themselves)
#
#   ./build-all-debs.sh            # everything
#   ./build-all-debs.sh wifi audio # only the named builders
#
# Builders invoked (each publishes its own deb):
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
#   build-linux-deb.sh       oneplus-erhai-linux (packs the pre-staged kernel
#                            payload snapshot -- NO kernel compile)
#   build-npu-deb.sh         oneplus-erhai-npu (NPU/HTP workload layer)
#   build-fastrpc-deb.sh     oneplus-erhai-fastrpc (FastRPC transport: libcdsprpc,
#                            cdsprpcd, the fastrpc group, the DSP payload trees)
#   build-llamacpp-deb.sh    oneplus-erhai-llamacpp (patched llama.cpp stack)
# The kernel deb is NOT COMPILED here -- for a full compile use ./build-kernel.sh.
#
# After this run ../update-ppa.sh --cptesting to move tested debs from
# testing/ to main/ and republish (it rescans and pushes by itself).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

declare -A BUILDERS=(
	[wifi]=build-wifi-deb.sh
	[audio]=build-audio-deb.sh
	[bluetooth]=build-bluetooth-deb.sh
	[camera]=build-camera-deb.sh
	[cam]=build-cam-deb.sh
	[sensors]=build-sensors-deb.sh
	[desktop]=build-desktop-deb.sh
	[thermal]=build-thermal-deb.sh
	[torch]=build-torch-deb.sh
	[otg]=build-otg-deb.sh
	[firmware]=build-firmware-deb.sh
	[mesa]=build-mesa-deb.sh
	[linux]=build-linux-deb.sh
	[npu]=build-npu-deb.sh
	[fastrpc]=build-fastrpc-deb.sh
	[llamacpp]=build-llamacpp-deb.sh
	[waydroid]=build-waydroid-installer-deb.sh
)

# waydroid LAST: its builder needs the patcher project in ../tmp/waydroid-patcher
# (source + the five stripped Mesa objects) and refuses to run without it.
ORDER=(wifi audio bluetooth camera cam sensors desktop thermal torch otg firmware mesa linux npu fastrpc llamacpp waydroid)

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
	echo "=== $name: ${BUILDERS[$name]} ==="
	"$HERE/${BUILDERS[$name]}"
done

echo
echo "all requested debs built and published."
echo "next: ../update-ppa.sh"
