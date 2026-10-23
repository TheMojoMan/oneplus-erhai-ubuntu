#!/bin/bash
# erhai-panel-power.sh — power-button REAL panel power cycle (Android-style),
# NOT a suspend: the SoC stays fully awake (compiles, downloads, external apps).
#
#   erhai-panel-power.sh          toggle   (power button)
#   erhai-panel-power.sh off      real panel teardown (with GPU parked first)
#   erhai-panel-power.sh on       restore display + re-pin GPU
#   erhai-panel-power.sh reassert [--cycle]   put a blank that something ELSE
#                                 undid (kernel re-enabled the panel / the session
#                                 woke up) back where the power button left it
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
# erhai-display-toggle.sh (it plays the audio as the session user).
#
# THE BLANK STATE (2.20): ONE owner-tagged file, /run/erhai-panel.state, one line
# "<owner> <epoch>", owner = power | folio.  The file EXISTING means "the panel is
# wanted OFF"; its owner says who asked, and that is all the two watchers (the
# power key and the folio) need to keep from undoing each other:
#   * the power button blanks DEEPLY (lamp, GPU park, DPU/DSI teardown) and owns
#     the file while that is in force,
#   * the folio blanks LIGHTLY (lamp only — it must stay cheap and instant) and
#     owns the file while IT is in force,
#   * a press while the file is already there is a no-op (the owner keeps it), and
#     `on` clears the file, so the state machine can never end up "dark marker,
#     lit panel" — the stale-state trap that the old pair of markers had.
# A missing or garbled owner is treated as "blanked, owner unknown": never light a
# panel on state we cannot read.
# FORMAT is pinned by tmp/folio-test/panel-state-test.sh (bash <-> python).

set -u
ACTION=${1:-toggle}
PARK_WAIT=${PARK_WAIT:-1.5}         # best-effort fresh-park budget before teardown
ON_SETTLE_MS=${ON_SETTLE_MS:-0}     # settle between SetActive false and the lamp
RESYNC_GAP_MS=${RESYNC_GAP_MS:-200} # `reassert --cycle`: compositor round-trip gap
LOG=${LOG:-/media/data/develop/erhai/tmp/gpu-hang-capture/panel-power.log}
SURVLOG=${ERHAI_SURVLOG:-/usr/local/sbin/erhai-surv-log}
GPU=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
TOGGLE=${ERHAI_TOGGLE:-/usr/local/sbin/erhai-display-toggle.sh}
STATE=${ERHAI_PANEL_STATE:-/run/erhai-panel.state}
DP_STATUS=${ERHAI_DP_STATUS:-/sys/class/drm/card1-DP-1/status}
BL=${ERHAI_BL:-/sys/class/backlight/ktz8868-backlight}
# --- the desktop session user the compositor call must run as ---------------
# NEVER a baked-in name (the "session username hardcoded" bug, reported against
# 2.21): this package
# runs on other people's pads, where `themojoman` does not exist — `id -u` then
# failed and the runuser fallback below tried to switch to a user that is not
# there (the numeric fallback kept the setpriv path alive, but nothing should
# depend on the desktop user being uid 1000).
# `cheap`: this is the per-press hot path and it must blank at once (2.16), so
# the usual answer (uid 1000 = the first human account) is resolved with ONE
# getent lookup and only a different desktop uid pays the loginctl round trip.
# Order: $ERHAI_SESSION_USER / $ERHAI_USER -> uid 1000 -> the ACTIVE graphical
# logind session -> the first human account (erhai-desktop-users.sh).
USERS_HELPER=${ERHAI_USERS_HELPER:-/usr/local/sbin/erhai-desktop-users.sh}
[ -r "$USERS_HELPER" ] && . "$USERS_HELPER"
USER_NAME=''; UID_N=''; GID_N=''
if command -v erhai_session_user >/dev/null 2>&1; then
    IFS=: read -r USER_NAME UID_N GID_N <<<"$(erhai_session_user cheap 2>/dev/null)"
fi
[ -n "$UID_N" ] || UID_N=1000
[ -n "$GID_N" ] || GID_N=$UID_N
[ -n "$USER_NAME" ] || USER_NAME=$(id -nu "$UID_N" 2>/dev/null)
RT=/run/user/$UID_N

state_owner() {   # prints the owner (may be empty), non-zero when no blank
    [ -e "$STATE" ] || return 1
    local o=''
    read -r o _ < "$STATE" 2>/dev/null || true
    printf '%s' "$o"
}
state_set() {     # $1 = owner: claim the blank
    printf '%s %s\n' "$1" "$(date +%s)" > "$STATE" 2>/dev/null
}
state_clear() { rm -f "$STATE"; }
state_is_blanked() { [ -e "$STATE" ]; }
blank_is_dark() { [ "$(cat "$BL/actual_brightness" 2>/dev/null)" = 0 ]; }
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
# The folio-fold watcher (erhai-folio-fold.py) blanks the panel LIGHTLY while the
# pogo keyboard lies on the screen and OWNS that state until the folio is opened.
# A power press must not claim a second, power-owned blank on top of it (that used
# to leave two markers and made the next press a no-op): nothing to do while the
# state file names another owner.
state_owned_by_folio() {
    [ -e "$STATE" ] || return 1
    [ "$(state_owner)" = folio ] || return 1
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
    if state_is_blanked; then
        if blank_is_dark; then
            say "off: already blanked (state: $(state_owner))"
            return 0
        fi
        # The state says blanked but the panel is LIT: a stale state (a script that
        # was killed mid-blank, a hand-written file).  Drop it and take the blank
        # properly — the old pair of markers made this a permanent no-op.
        say "off: stale state ($(state_owner)) next to a lit panel -> taking over"
        state_clear
    fi
    if external_up; then
        say "off: DP-1 connected -> backlight-only fallback (GPU stays ACTIVE)"
        "$TOGGLE" off
        state_set power
        pin_gpu
        say "off: fallback done $(snap)"
        return 0
    fi
    say "off: blank begin $(snap)"
    "$TOGGLE" off                 # 1) VISIBLE: the panel goes dark NOW
    state_set power               # 2) the blank has an owner before anything slow
    park_gpu                      # 3) best-effort GPU park (bounded)
    say "off: SetActive true -> $(ss_active true)"   # 4) real DPU/DSI teardown
    say "off: done $(snap)"
    # 5) the forensic dmesg tail is 50-170 ms of forks: never on the user path
    ( dmesg 2>/dev/null | grep -aE 'nvt_ts_panel_power|mdss' | tail -3 \
        | while read -r l; do say "  dmesg| $l"; done ) &
}

do_on() {
    state_is_blanked || { say "on: already on (no blank state)"; return 0; }
    local was; was=$(state_owner)
    say "on: restoring (state was ${was:-unknown}) bl=$(cat "$BL/actual_brightness" 2>/dev/null)"
    # The blank stops being in force the moment we decide to restore, so the state
    # file goes FIRST: every other actor (the input guard's blanked(), the folio
    # watcher, erhai-power-watch.py's BLANK INTEGRITY monitor — which fires on the
    # panel-on record this call is about to produce, ~10-50 ms later) must never
    # see a LIT panel next to a state that says "blanked".  That window is exactly
    # what makes a re-assert fight a restore.
    state_clear
    if external_up; then
        say "on: DP-1 connected -> backlight-only restore (GPU re-pinned)"
        "$TOGGLE" on
        pin_gpu
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
    say "on: done $(snap)"
}

# REASSERT (2026-10-22, sensor-MEMORY-2026-10-22.md §9): the blank is OURS and
# something OUTSIDE this path undid it.  Measured on boot 866f5cf7b7464aa7: the
# DSI panel was re-enabled by the kernel 12 min into a 51-min blank (right at a
# folio fold, `nvt_ts_panel_power_on`), the session was awake behind the stale
# marker, and after that NO press produced a picture again until a reboot - the
# half-on state (panel enabled, nobody asked for it) is what a screen that never
# comes back looks like.
#
# The marker, the brightness MEMORY and the GPU park are left exactly as they
# are - they belong to the press that started the blank.  The only goal is that
# the panel ends where the power button put it: dark, with the DPU/DSI REALLY
# torn down.
#   reassert          the session woke up behind the blank (an input event from a
#                     device the blank does not grab) -> blank it again
#   reassert --cycle  the panel was enabled BEHIND the compositor's back -> give
#                     the compositor a real false->true round trip, because a
#                     plain SetActive true is a no-op while the compositor
#                     already believes it is blanked (and the CRTC would stay up)
reassert() {
    local cycle=no was
    [ "${1:-}" = "--cycle" ] && cycle=yes
    [ -e "$STATE" ] || { say "reassert: no blank to protect"; return 0; }
    if state_owned_by_folio && blank_is_dark; then
        say "reassert: the folio owns this blank -> nothing to do"
        return 0
    fi
    if external_up; then
        # With a monitor attached the blank is backlight-only ON PURPOSE (the
        # desktop keeps rendering on the DP output, 2.17), so an "awake session"
        # is the normal state there - blanking it would kill the user's monitor.
        say "reassert: DP-1 is connected (backlight-only blank) -> nothing to do"
        return 0
    fi
    was=$(cat "$BL/actual_brightness" 2>/dev/null)
    if [ "$cycle" = yes ]; then
        say "reassert: the panel came back behind the compositor -> round trip (bl=$was)"
        say "reassert: SetActive false -> $(ss_active false)"
        # Keep the lamp dark THROUGH the round trip: deactivating the screensaver
        # makes gsd-power restore its own brightness, and a flash of the desktop
        # is not part of "the panel is off".
        if [ "$(cat "$BL/actual_brightness" 2>/dev/null)" != 0 ]; then
            echo 0 > "$BL/brightness" 2>/dev/null \
                && say "reassert: lamp held at 0 through the round trip"
        fi
        sleep "$(awk -v m="$RESYNC_GAP_MS" 'BEGIN { printf "%.3f", m / 1000 }')"
        say "reassert: SetActive true -> $(ss_active true)"
    else
        say "reassert: the session woke up behind the blank -> re-blanking (bl=$was)"
        say "reassert: SetActive true -> $(ss_active true)"
    fi
    # The lamp must END dark.  Deactivating the screensaver makes gsd-power
    # restore its own brightness, so a plain re-blank can leave a LIT panel
    # behind.  erhai-display-toggle.sh must NOT be used here: it would store that
    # restored value as the brightness to come back to, and it plays a click.
    if [ "$(cat "$BL/actual_brightness" 2>/dev/null)" != 0 ]; then
        echo 0 > "$BL/brightness" 2>/dev/null \
            && say "reassert: the lamp was lit again -> forced back to 0"
    fi
    say "reassert: done $(snap)"
}

case "$ACTION" in
    off)      do_off ;;
    on)       do_on ;;
    reassert) reassert "${2:-}" ;;
    toggle)   if [ -e "$STATE" ]; then do_on; else do_off; fi ;;
    *) say "unknown action '$ACTION' (use on|off|toggle|reassert)"; exit 2 ;;
esac
