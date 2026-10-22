#!/bin/sh
# erhai — keep the Adreno ACTIVE while an external display is connected.
#
# WHY (owner 2026-10-22: *"Make sure that GPU is NOT parked when external monitor
# is connected!"*): this pad pins the Adreno 830 always-active on purpose
# (erhai-gpu-pin.sh, the 2026-09-13 policy) — the runtime-suspended/unpinned GPU
# path is where the GMU resume hang and the display silent POR came from.  The
# power-button blank parks it deliberately (`echo auto` in erhai-panel-power.sh),
# but a DP monitor over USB-C keeps rendering while the internal panel is off, so
# a monitor attached while the panel is blanked must re-pin it.
#
# udev runs this from 72-erhai-gpu-external.rules on every card*-DP-* change
# uevent (attach, detach, modeset).  It writes only when a DP connector really
# reads `connected` AND the GPU is not already pinned, so repeated events cost a
# couple of reads and nothing else.  Idempotent by design (a stray extra run can
# never park anything).
#
# It also re-pins at BOOT (the rule matches the connector's add event), which is
# exactly what erhai-gpu-pin.service does — no conflict, same value.
#
# Test hooks: ERHAI_GPU (power dir), ERHAI_DRM_GLOB (status files).
GPU=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
DRM_GLOB=${ERHAI_DRM_GLOB:-/sys/class/drm/card*-DP-*/status}

[ -w "$GPU/control" ] || exit 0

for f in $DRM_GLOB; do
    [ -r "$f" ] || continue
    [ "$(cat "$f" 2>/dev/null)" = connected ] || continue
    state=$(cat "$GPU/control" 2>/dev/null)
    [ "$state" = on ] && exit 0
    if echo on > "$GPU/control" 2>/dev/null; then
        logger -t erhai-gpu "external display connected -> GPU pinned always-active (was ${state:-unknown})"
    else
        logger -t erhai-gpu "external display connected but $GPU/control is not writable"
    fi
    exit 0
done
exit 0
