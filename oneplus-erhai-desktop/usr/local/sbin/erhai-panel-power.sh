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
# LATENCY (2026-10-22, owner: *"the display off/on task needs high priority —
# currently it can take several seconds before the screen turns off when a
# process with high CPU load is running"*):
#   * ORDER — the VISIBLE blank is the FIRST thing this script does.  The old
#     order was park GPU -> SetActive true -> sleep 2 -> backlight off: the park
#     waits for the GPU to runtime-suspend, which never happens while the desktop
#     renders (probe: >5.5 s idle, 1.7 s under load, budget was 10 s), and the
#     fixed sleeps added 2 s / 3 s on top, so the panel stayed LIT for 2-12 s
#     after the press.  Now: backlight off (~17 ms idle, ~71 ms under 8 threads),
#     marker, bounded best-effort park (PARK_WAIT, default 1.5 s), then the
#     DPU/DSI teardown — the park-before-teardown ordering that fixed the POR is
#     kept, only its wait is bounded and it no longer delays the dark screen.
#   * WAITS — do_on dropped its sleep 3 + sleep 1: the s2idle harness has always
#     done `SetActive false` -> backlight on back-to-back (STEP6) and that is the
#     proven path; while the DSI rail is down the lit lamp is invisible anyway.
#     ON_SETTLE_MS is kept as a knob (0 = immediate) in case a flash is ever
#     reported.  Measured after the change: ~0.1 s off, ~0.15 s on.
#   * PRIORITY — the whole path renices itself to -20 and takes realtime I/O for
#     its few hundred ms (only root can), and the compositor call uses `setpriv`
#     instead of `runuser`: no PAM round trip, measured 26 ms vs 101 ms under
#     load.  erhai-power-toggle.service carries the same priority for the daemon.
#   * The post-run `dmesg` tail (53-168 ms of forks) moved off the user path.
#
# The click sounds, brightness memory and alsd guard come from
# erhai-display-toggle.sh (it plays the audio as the session user). State is
# /run/erhai-panel-off.

set -u
ACTION=${1:-toggle}
PARK_WAIT=${PARK_WAIT:-1.5}         # best-effort fresh-park budget before teardown
ON_SETTLE_MS=${ON_SETTLE_MS:-0}     # settle between SetActive false and the lamp
LOG=${LOG:-/media/data/develop/erhai/tmp/gpu-hang-capture/panel-power.log}
SURVLOG=${ERHAI_SURVLOG:-/usr/local/sbin/erhai-surv-log}
GPU=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
TOGGLE=${ERHAI_TOGGLE:-/usr/local/sbin/erhai-display-toggle.sh}
MARKER=${ERHAI_MARKER:-/run/erhai-panel-off}
FOLIO_MARKER=${ERHAI_FOLIO_MARKER:-/run/erhai-folio-fold-blanked}
DP_STATUS=${ERHAI_DP_STATUS:-/sys/class/drm/card1-DP-1/status}
BL=${ERHAI_BL:-/sys/class/backlight/ktz8868-backlight}
USER_NAME=${ERHAI_SESSION_USER:-themojoman}
UID_N=$(id -u "$USER_NAME" 2>/dev/null || echo 1000)
GID_N=$(id -g "$USER_NAME" 2>/dev/null || echo 1000)
RT=/run/user/$UID_N
GENPD=/sys/kernel/debug/pm_genpd/pm_genpd_summary

# Ahead of everything else: a few hundred ms of work that must not queue behind
# a compile/encode (owner 2026-10-22).  Children (the toggle, the click player)
# inherit it; it dies with this script.  Best-effort: only root may renice.
# I/O stays BEST-EFFORT class at priority 0 on purpose (top of the queue, but an
# RT-class process could starve the display/disk I/O for its lifetime).
if [ "$(id -u)" = 0 ]; then
    renice -n -20 -p $$ >/dev/null 2>&1 || true
    ionice -c2 -n0 -p $$ >/dev/null 2>&1 || true
fi


# say/snap run on the critical path, so they must not fork: bash's %(%T)T and
# $(<file) are builtins, and the log append is a plain redirect (the old version
# spawned date+cut+tee — 3 processes per line, ~4-6 lines per press, which is
# exactly what got slow under load).  The survivability line is still written
# SYNCHRONOUSLY through erhai-surv-log when it exists (one fork + fsync, and it
# must survive a hard reset) — that script is a dev-pad tool, not shipped.
say() {
    local up line
    read -r up _ < /proc/uptime 2>/dev/null || up=?
    printf -v line '[%(%T)T up=%s] %s\n' -1 "$up" "$*"
    printf '%s' "$line"
    printf '%s' "$line" >> "$LOG" 2>/dev/null
    [ -x "$SURVLOG" ] && "$SURVLOG" "panel-power: $*" 2>/dev/null
    return 0
}
snap() {
    local bl gpu_c gpu_r name state gdsc='' rest blf
    [ -r "$BL/actual_brightness" ] && bl=$(<"$BL/actual_brightness")
    [ -r "$GPU/control" ] && gpu_c=$(<"$GPU/control")
    [ -r "$GPU/runtime_status" ] && gpu_r=$(<"$GPU/runtime_status")
    if [ -r "$GENPD" ]; then
        while read -r name state rest; do
            case "$name" in
                mdss_gdsc|mdss_int2_gdsc) gdsc+="$name=$state " ;;
            esac
        done < "$GENPD"
    fi
    printf 'bl=%s gpu=%s/%s gdsc[%s]\n' "$bl" "$gpu_c" "$gpu_r" "$gdsc"
}
ss_active() {   # gnome-shell ScreenSaver DPMS via the real session bus
    local val="$1"
    if [ -n "${ERHAI_SS_STUB:-}" ]; then "$ERHAI_SS_STUB" "$val"; return $?; fi
    [ -S "$RT/bus" ] || return 1
    # setpriv (no PAM round trip) when available: measured 26 ms vs runuser's
    # 101 ms under 8 CPU threads — this call is on the critical path.
    if command -v setpriv >/dev/null 2>&1; then
        setpriv --reuid="$UID_N" --regid="$GID_N" --init-groups \
            env XDG_RUNTIME_DIR="$RT" DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus" \
            timeout 10 gdbus call --session \
            --dest org.gnome.Shell --object-path /org/gnome/ScreenSaver \
            --method org.gnome.ScreenSaver.SetActive "$val" 2>&1
    else
        runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="$RT" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus" timeout 10 gdbus call --session \
            --dest org.gnome.Shell --object-path /org/gnome/ScreenSaver \
            --method org.gnome.ScreenSaver.SetActive "$val" 2>&1
    fi
}
external_up() { [ "$(cat "$DP_STATUS" 2>/dev/null)" = connected ] && return 0 || return 1; }
# The Adreno must NEVER be runtime-parked while an external display is driven
# (owner 2026-10-22: *"Make sure that GPU is NOT parked when external monitor is
# connected!"*).  This pad pins it always-active on purpose (erhai-gpu-pin.sh, the
# 2026-09-13 policy — the unpinned path is where the GMU resume hang and the
# display silent POR came from), and a DP monitor over USB-C keeps rendering
# while the internal panel is off.  park_gpu() therefore refuses with DP-1
# connected, every restore re-pins, and udev re-pins it on attach
# (72-erhai-gpu-external.rules -> erhai-gpu-external-pin.sh).
pin_gpu() {
    [ -w "$GPU/control" ] || { say "pin: $GPU/control is not writable"; return 1; }
    [ "$(cat "$GPU/control" 2>/dev/null)" = on ] && return 0
    if echo on > "$GPU/control" 2>/dev/null; then
        say "pin: GPU re-pinned always-active"
        return 0
    fi
    say "pin: cannot write $GPU/control"
    return 1
}
# The folio-fold watcher (erhai-folio-fold.py) blanks the panel while the pogo
# keyboard lies on the screen and OWNS that blank until the folio is opened.
# A power press must not create a second, power-button-owned blank on top of it
# (that used to leave two markers and made the next press a no-op): nothing to
# do while the folio holds it.  The backlight value is part of the test so a
# STALE folio marker next to a lit panel can never block the power button.
folio_holds_blank() {
    [ -e "$FOLIO_MARKER" ] || return 1
    [ "$(cat "$BL/actual_brightness" 2>/dev/null)" = 0 ] || return 1
    return 0
}
park_gpu() {    # unpin + wait for a FRESH runtime park (best-effort, BOUNDED)
    # The panel is already dark when this runs, so a long wait costs nothing the
    # user can see; but it used to cost up to 10 s of script time on every press
    # (the GPU never parks while the desktop renders).  50 ms ticks, PARK_WAIT
    # seconds, PARK_WAIT=0 skips the wait entirely.
    if external_up; then
        say "park: SKIPPED - DP-1 is connected, the GPU must stay active for it"
        pin_gpu
        return 1
    fi
    echo auto > "$GPU/control" 2>/dev/null || { say "park: cannot echo auto"; return 1; }
    local ticks i=0
    ticks=$(awk -v s="$PARK_WAIT" 'BEGIN { printf "%d", (s * 20) + 0.5 }')
    while [ "$i" -lt "$ticks" ]; do
        [ "$(cat "$GPU/runtime_status" 2>/dev/null)" = suspended ] && {
            say "park: parked after $(( i * 50 ))ms"; return 0; }
        sleep 0.05; i=$((i+1))
    done
    say "park: NOT parked within ${PARK_WAIT}s (proceeding, status=$(cat "$GPU/runtime_status" 2>/dev/null))"
    return 1
}

do_off() {
    [ -e "$MARKER" ] && { say "off: already off (marker present)"; return 0; }
    if folio_holds_blank; then
        say "off: folio-fold watcher holds the panel blanked -> nothing to do"
        return 0
    fi
    if external_up; then
        say "off: DP-1 connected -> backlight-only fallback (GPU stays ACTIVE)"
        "$TOGGLE" off
        touch "$MARKER"
        pin_gpu
        say "off: fallback done $(snap)"
        return 0
    fi
    say "off: blank begin $(snap)"
    "$TOGGLE" off                 # 1) VISIBLE: the panel goes dark NOW
    touch "$MARKER"               # 2) the blank has an owner before anything slow
    park_gpu                      # 3) best-effort GPU park (bounded)
    say "off: SetActive true -> $(ss_active true)"   # 4) real DPU/DSI teardown
    say "off: done $(snap)"
    # 5) the forensic dmesg tail is 50-170 ms of forks: never on the user path
    ( dmesg 2>/dev/null | grep -aE 'nvt_ts_panel_power|mdss' | tail -3 \
        | while read -r l; do say "  dmesg| $l"; done ) &
}

do_on() {
    [ -e "$MARKER" ] || { say "on: already on (no marker)"; return 0; }
    if external_up; then
        say "on: DP-1 connected -> backlight-only restore (GPU re-pinned)"
        "$TOGGLE" on
        pin_gpu
        rm -f "$MARKER"
        say "on: fallback done $(snap)"
        return 0
    fi
    say "on: SetActive false -> $(ss_active false)"
    # No fixed settle: the DSI rail is down while the panel is dark, so lighting
    # the lamp before the CRTC is back shows nothing — the s2idle harness restores
    # the backlight back-to-back with SetActive false (STEP6) for exactly this
    # reason.  ON_SETTLE_MS>0 is available if a flash is ever reported.
    if [ "$ON_SETTLE_MS" != 0 ]; then
        sleep "$(awk -v m="$ON_SETTLE_MS" 'BEGIN { printf "%.3f", m / 1000 }')"
    fi
    "$TOGGLE" on
    pin_gpu
    rm -f "$MARKER"
    say "on: done $(snap)"
}

case "$ACTION" in
    off)    do_off ;;
    on)     do_on ;;
    toggle) if [ -e "$MARKER" ]; then do_on; else do_off; fi ;;
    *) say "unknown action '$ACTION' (use on|off|toggle)"; exit 2 ;;
esac
