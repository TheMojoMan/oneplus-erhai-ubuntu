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
# never park anything on attach).
#
# It also re-pins at BOOT (the rule matches the connector's add event), which is
# exactly what the GPU pin unit does — no conflict, same value.
#
# 2.26 — THE OTHER HALF: a pin taken for a monitor must not OUTLIVE the monitor.
# The boot-only pin policy (erhai-gpu-pin-once.service, 2.25) hands the GPU back
# to runtime-PM after the boot bring-up, because a permanently pinned GPU is
# pm_runtime_forbid() and that is what used to make the DRM recovery a no-op
# (the CX domain can never collapse — gpu-MEMORY-2026-10-23.md).  Pinning on a
# monitor attach without ever releasing again silently re-created that state for
# the rest of the boot, so on a `card*-DP-*` uevent with NOTHING connected the
# helper now handles the pin back — and it does that through
# `erhai-gpu-pin-once.sh unpin ext-detach`, so the state file records the release
# instead of going stale (a stale `kept external` would let the next panel
# restore re-pin the GPU).  Only the once-pin policy's own states are released
# (`released`, or `kept … external` = the boot release was declined FOR a
# monitor); `pinned` (the boot hand-hold), `kept mode=always` (MODE=always),
# `absent` and a missing state file — an older image — keep their permanent pin.
#
# Test hooks: ERHAI_GPU (power dir), ERHAI_DRM_GLOB (status files),
#             ERHAI_GPU_PIN_ONCE_STATE (once-pin state file),
#             ERHAI_GPU_PIN_ONCE_HELPER (the policy script to call).
GPU=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
DRM_GLOB=${ERHAI_DRM_GLOB:-/sys/class/drm/card*-DP-*/status}
PIN_ONCE_STATE=${ERHAI_GPU_PIN_ONCE_STATE:-/run/erhai-gpu-pin-once.state}
ONCE_HELPER=${ERHAI_GPU_PIN_ONCE_HELPER:-/usr/local/sbin/erhai-gpu-pin-once.sh}

[ -w "$GPU/control" ] || exit 0

# ---- a DP monitor is attached: the Adreno must never be parked for it --------
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

# ---- nothing is attached: hand a MONITOR-ONLY pin back to runtime-PM ---------
# Two states are ours to undo, and nothing else is:
#   released            — the boot policy already handed the GPU back; a pin on
#                         top of it can only have come from a monitor;
#   kept ... external   — the boot release was DECLINED because a monitor was
#                         attached; that blocker is gone now.
# `pinned` (the boot hand-hold), `kept mode=always` (MODE=always), `absent` and
# a missing state file all keep the pin — that is their intent.
once_state=$(awk 'NR==1{print $1}' "$PIN_ONCE_STATE" 2>/dev/null)
once_reason=$(awk 'NR==1{print $3}' "$PIN_ONCE_STATE" 2>/dev/null)
case $once_state in
    released) ;;
    kept) [ "$once_reason" = external ] || exit 0 ;;
    *)    exit 0 ;;
esac
state=$(cat "$GPU/control" 2>/dev/null)
[ "$state" = on ] || exit 0

# The RELEASE goes through the policy script so the state file tells the truth
# (`released ... ext-detach`).  A stale `kept external` would let the next panel
# restore re-pin the GPU: erhai-panel-power.sh's pin_gpu() skips only when the
# state file says `released`.
if [ -x "$ONCE_HELPER" ] && "$ONCE_HELPER" unpin ext-detach >/dev/null 2>&1; then
    logger -t erhai-gpu "external display gone -> GPU back to runtime-PM (control=auto, once-pin policy)"
    exit 0
fi
if echo auto > "$GPU/control" 2>/dev/null; then
    logger -t erhai-gpu "external display gone -> GPU back to runtime-PM (control=auto, once-pin policy)"
else
    logger -t erhai-gpu "external display gone but $GPU/control is not writable"
fi
exit 0
