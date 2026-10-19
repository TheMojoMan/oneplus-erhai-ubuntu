#!/bin/bash
# erhai-panel-power.sh — power-button REAL panel power cycle (Android-style),
# NOT a suspend: the SoC stays fully awake (compiles, downloads, external apps).
#
#   erhai-panel-power.sh          toggle   (power button)
#   erhai-panel-power.sh off      real panel teardown (with GPU parked first)
#   erhai-panel-power.sh on       restore display + re-pin GPU
#
# WHY THIS SEQUENCE (b29, gpu-MEMORY-2026-09-12.md):
#   * The display-off silent POR is a race between display teardown and the
#     GPU/MMCX power collapse.  Serializing them — unpin (echo auto), wait for a
#     FRESH runtime park, THEN tear the DPU/DSI down (gnome-shell ScreenSaver
#     SetActive true) — is what made display OFF->ON survivable at 08:02-08:15
#     on this kernel, and it never touches /sys/power/state (suspend itself is
#     a 3/6 silent-POR lottery on b29 and stays OFF).
#   * Backlight off alone keeps the DPU/DSI rails up (gpu stays on) -> not a
#     real power-down.  SetActive true drops mdss_gdsc/mdss_int2_gdsc AND stops
#     mutter, which is the only way the GPU can actually runtime-suspend.
# * EXTERNAL MONITOR: if DP-1 is connected we fall back to the backlight-only
#   toggle (erhai-display-toggle.sh). A real blank would take the DP output
#   down too, but the whole point of the external case is that the app keeps
#   rendering — so leave the pipeline fully up and just blank the panel lamp.
#
# The click sounds, brightness memory and alsd guard come from
# erhai-display-toggle.sh (it plays the audio as the session user). State is
# /run/erhai-panel-off.

set -u
ACTION=${1:-toggle}
PARK_WAIT=${PARK_WAIT:-10}          # best-effort fresh-park budget before blank
LOG=/media/data/develop/erhai/tmp/gpu-hang-capture/panel-power.log
SURVLOG=/usr/local/sbin/erhai-surv-log
GPU=/sys/devices/platform/soc@0/3d00000.gpu/power
TOGGLE=/usr/local/sbin/erhai-display-toggle.sh
MARKER=/run/erhai-panel-off
DP_STATUS=/sys/class/drm/card1-DP-1/status
BL=/sys/class/backlight/ktz8868-backlight
USER_NAME=${ERHAI_SESSION_USER:-themojoman}
UID_N=$(id -u "$USER_NAME" 2>/dev/null || echo 1000)
RT=/run/user/$UID_N
GENPD=/sys/kernel/debug/pm_genpd/pm_genpd_summary

say() {
    echo "[$(date +%T) up=$(cut -d' ' -f1 /proc/uptime)] $*" | tee -a "$LOG"
    [ -x "$SURVLOG" ] && "$SURVLOG" "panel-power: $*" 2>/dev/null
}
snap() {
    printf 'bl=%s gpu=%s/%s gdsc[%s]\n' \
        "$(cat "$BL/actual_brightness" 2>/dev/null)" \
        "$(cat "$GPU/control" 2>/dev/null)" "$(cat "$GPU/runtime_status" 2>/dev/null)" \
        "$(grep -E '^(mdss_gdsc|mdss_int2_gdsc)' "$GENPD" 2>/dev/null | awk '{printf "%s=%s ",$1,$2}')"
}
ss_active() {   # gnome-shell ScreenSaver DPMS via the real session bus
    local val="$1"
    [ -S "$RT/bus" ] || return 1
    runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="$RT" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus" timeout 10 gdbus call --session \
        --dest org.gnome.Shell --object-path /org/gnome/ScreenSaver \
        --method org.gnome.ScreenSaver.SetActive "$val" 2>&1
}
external_up() { [ "$(cat "$DP_STATUS" 2>/dev/null)" = connected ] && return 0 || return 1; }
park_gpu() {    # unpin + wait for a FRESH runtime park (best-effort)
    echo auto > "$GPU/control" 2>/dev/null || { say "park: cannot echo auto"; return 1; }
    local i=0
    while [ "$i" -lt "$PARK_WAIT" ]; do
        [ "$(cat "$GPU/runtime_status" 2>/dev/null)" = suspended ] && { say "park: parked after ${i}s"; return 0; }
        sleep 1; i=$((i+1))
    done
    say "park: NOT parked within ${PARK_WAIT}s (proceeding, status=$(cat "$GPU/runtime_status" 2>/dev/null))"
    return 1
}

do_off() {
    [ -e "$MARKER" ] && { say "off: already off (marker present)"; return 0; }
    if external_up; then
        say "off: DP-1 connected -> backlight-only fallback"
        "$TOGGLE" off; touch "$MARKER"; say "off: fallback done $(snap)"; return 0
    fi
    say "off: park-then-blank begin $(snap)"
    park_gpu
    say "off: SetActive true -> $(ss_active true)"
    sleep 2
    "$TOGGLE" off
    touch "$MARKER"
    say "off: done $(snap)"
    dmesg 2>/dev/null | grep -aE 'nvt_ts_panel_power|mdss' | tail -3 | while read -r l; do say "  dmesg| $l"; done
}

do_on() {
    [ -e "$MARKER" ] || { say "on: already on (no marker)"; return 0; }
    if external_up; then
        say "on: DP-1 connected -> backlight-only restore"
        "$TOGGLE" on; rm -f "$MARKER"; say "on: fallback done $(snap)"; return 0
    fi
    say "on: SetActive false -> $(ss_active false)"
    sleep 3
    "$TOGGLE" on
    echo on > "$GPU/control" 2>/dev/null
    sleep 1
    rm -f "$MARKER"
    say "on: done $(snap)"
}

case "$ACTION" in
    off)    do_off ;;
    on)     do_on ;;
    toggle) if [ -e "$MARKER" ]; then do_on; else do_off; fi ;;
    *) say "unknown action '$ACTION' (use on|off|toggle)"; exit 2 ;;
esac
