#!/bin/bash
# Bring up the Adreno 830 GMU before the GNOME session starts.
#
# Since 2026-09-13 the GPU is PINNED UNCONDITIONALLY (runtime-PM 'on') on every
# kernel. The 09-12/09-13 unpinned ('auto' + IFPC-disabled) experiment ended in
# a spontaneous workload-path silent POR on 7.2 #89 (HFI H2F_MSG_GX_BW_PERF_VOTE
# msg 3418 timeout, see gpu-MEMORY-2026-09-12.md) and modeset-era freezes on the
# 7.3.0-rc2 line, so the pre-09-12 stable behavior (always pinned) is back for
# good. Pinning means the GMU never runtime-suspends: the intermittent CM3
# resume hang (GMU firmware init timeout -> OOB storm -> hangcheck freeze) can
# never fire, and holding gpu_cc_cx_gdsc keeps MMCX up across display power
# cycles. Cost: the Adreno idles at its 160 MHz floor instead of suspending.
#
# A kernel may boot with the Adreno node disabled, and this unit sits in front of
# basic.target, so it must not spend time failing when there is no GPU: the wait
# loop below would burn its full 5s, the final write would fail, and the service
# would exit non-zero. Measured cost of that was ~6.2s of extra boot -- basic.target
# at 14.0s instead of 7.8s, and since erhai-touchscreen.service (which starts gdm)
# is ordered after it, the desktop appeared 9.5s later.
#
# So check the same thing erhai-desktop-gl.sh checks, and do nothing if the
# kernel has no GPU.

# Test seams (defaults are the real paths; tmp/gpu-pin-once-test/ drives this
# script through them — erhai-gpu-pin-once.sh `pin` calls it):
#   ERHAI_GPU_DT_STATUS  the device-tree status file of the GPU node
#   ERHAI_GMU_GATE       the dev kernel's GMU arming parameter
#   ERHAI_GPU            the GPU's sysfs power directory
DT_STATUS=${ERHAI_GPU_DT_STATUS:-/proc/device-tree/soc@0/gpu@*/status}
status=$(cat $DT_STATUS 2>/dev/null | tr -d '\0')
case "$status" in
okay|ok)
	;;
*)
	echo "erhai-gpu-pin: gpu status '${status:-absent}', nothing to arm"
	exit 0
	;;
esac

# The dev kernel gates the GMU behind a module parameter and needs it armed here.
# The public kernel carries no such gate -- its a8xx driver brings the GMU up at
# probe -- so the parameter is simply absent there and nothing needs arming.
GATE=${ERHAI_GMU_GATE:-/sys/module/msm/parameters/erhai_gmu_enable}
if [ -w "$GATE" ]; then
	echo 1 > "$GATE"
fi

G=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
for i in $(seq 1 50); do [ -e "$G/control" ] && break; sleep 0.1; done
if [ ! -e "$G/control" ]; then
	echo "erhai-gpu-pin: $G/control never appeared, leaving the GPU unpinned" >&2
	exit 0
fi
sleep 1
# PIN the GPU always-active. Never runtime-suspend -> the intermittent GMU
# resume hang can never trigger (see header). Pre-09-12 stable daily behavior,
# restored unconditionally after the 2026-09-13 crash.
echo on > "$G/control"
echo "erhai-gpu-pin: GPU PINNED always-active (echo on)" >&2
