#!/bin/bash
# erhai-gpu-pin-once.sh — pin the Adreno for the BOOT BRING-UP only, then hand
# the GPU back to runtime-PM once it is really up.
#
#   erhai-gpu-pin-once.sh pin      arm the GMU + pin the GPU (runtime-PM 'on')
#   erhai-gpu-pin-once.sh release  wait (bounded) for "the GPU is up", then 'auto'
#   erhai-gpu-pin-once.sh unpin [reason]
#                                  release NOW, without waiting (manual; also
#                                  called by erhai-gpu-external-pin.sh as
#                                  `unpin ext-detach` when a monitor goes away)
#   erhai-gpu-pin-once.sh status   one key=value line (for logs / support)
#
# WHY (2026-10-24, owner workaround): a boot with the Adreno NOT pinned could
# not bring the desktop up (no login was possible until the GPU was pinned by
# hand from tty3) — the very CM3/GMU-bring-up fragility the unconditional pin
# (erhai-gpu-pin.sh, 2026-09-13) exists for.  Pinning forever, though, is also
# wrong: 'on' is pm_runtime_forbid(), and a permanently pinned GPU used to make
# the DRM recovery a no-op (the CX domain could never collapse — see
# gpu-MEMORY-2026-10-23.md; kernel +b32-gp-01/02 make that recovery work, this
# policy keeps the GPU out of the fragile state in the first place).  So the pin
# is now a BOOT-ONLY hand-hold:
#
#   pin      BEFORE gdm (ExecStartPre of erhai-gpu-pin-once.service), so the GPU
#            is already up and active when the greeter/session first touches it.
#   release  runs as the service's main process (it does NOT block the boot: the
#            start job completes as soon as it is forked) and waits until the
#            GPU is demonstrably UP AND IN USE, then writes 'auto':
#              1. gdm.service is active            (the compositor is running)
#              2. the internal panel is LIT        (frames are on the glass)
#              3. the GPU runtime status is active (it really is powered)
#              4. the GPU clock has risen above its idle floor at least once
#                 (real submissions, not just probe)
#            and 1-4 have held for SETTLE seconds (default 15).
#            If they never hold, the GPU STAYS PINNED (the safe direction) and
#            the reason is logged — a timeout is never a release.
#   unpin    the manual form, for a tty / a test.
#
# An EXTERNAL display keeps the GPU pinned (owner 2026-10-22: a DP monitor must
# never find the Adreno parked) — the release refuses while card*-DP-* reads
# `connected`, and re-checks right before the write.
#
# MODE=always in /etc/erhai/gpu-pin.conf restores the pre-2.25 behaviour (the
# permanent pin, erhai-gpu-pin.service); this script then never releases.
#
# The state file /run/erhai-gpu-pin-once.state carries ONE line
# "<pinned|released|kept> <epoch> [reason]" — erhai-panel-power.sh reads it so a
# panel blank/restore does not silently take the permanent pin back.
#
# Test seams (defaults are the real paths; see tmp/gpu-pin-once-test/):
#   ERHAI_GPU, ERHAI_GPU_DEVFREQ, ERHAI_BL, ERHAI_DP_GLOB,
#   ERHAI_GPU_PIN_ONCE_STATE, ERHAI_GPU_PIN_CONF, ERHAI_GPU_PIN_HELPER,
#   ERHAI_SYSTEMCTL, ERHAI_GDM_UNIT, ERHAI_GPU_PIN_ONCE_MODE,
#   ERHAI_GPU_PIN_ONCE_SETTLE, ERHAI_GPU_PIN_ONCE_TIMEOUT,
#   ERHAI_GPU_PIN_ONCE_POLL

set -u

ACTION=${1:-status}

GPU=${ERHAI_GPU:-/sys/devices/platform/soc@0/3d00000.gpu/power}
DEVFREQ=${ERHAI_GPU_DEVFREQ:-/sys/class/devfreq/3d00000.gpu}
BL=${ERHAI_BL:-/sys/class/backlight/ktz8868-backlight}
DP_GLOB=${ERHAI_DP_GLOB:-/sys/class/drm/card*-DP-*/status}
STATE=${ERHAI_GPU_PIN_ONCE_STATE:-/run/erhai-gpu-pin-once.state}
CONF=${ERHAI_GPU_PIN_CONF:-/etc/erhai/gpu-pin.conf}
PIN_HELPER=${ERHAI_GPU_PIN_HELPER:-/usr/local/sbin/erhai-gpu-pin.sh}
SYSTEMCTL=${ERHAI_SYSTEMCTL:-systemctl}
GDM_UNIT=${ERHAI_GDM_UNIT:-gdm.service}
POLL=${ERHAI_GPU_PIN_ONCE_POLL:-0.25}

MODE_V=once
SETTLE=15
TIMEOUT=180

say() { printf 'erhai-gpu-pin-once: %s\n' "$*" >&2; }

# ---------------------------------------------------------------- config ------
conf_get() {   # $1 key, $2 default
    local v=''
    if [ -r "$CONF" ]; then
        v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" \
            2>/dev/null | tail -n1 | tr -d '\r' | sed 's/[[:space:]]*$//')
        case $v in \"*\") v=${v#\"}; v=${v%\"} ;; \'*\') v=${v#\'}; v=${v%\'} ;; esac
    fi
    [ -n "$v" ] || v=$2
    printf '%s' "$v"
}

is_uint() { case ${1:-} in ''|*[!0-9]*) return 1 ;; esac; return 0; }

read_config() {
    MODE_V=$(conf_get MODE once)
    SETTLE=$(conf_get SETTLE 15)
    TIMEOUT=$(conf_get TIMEOUT 180)
    [ -n "${ERHAI_GPU_PIN_ONCE_MODE:-}" ]   && MODE_V=$ERHAI_GPU_PIN_ONCE_MODE
    [ -n "${ERHAI_GPU_PIN_ONCE_SETTLE:-}" ] && SETTLE=$ERHAI_GPU_PIN_ONCE_SETTLE
    [ -n "${ERHAI_GPU_PIN_ONCE_TIMEOUT:-}" ] && TIMEOUT=$ERHAI_GPU_PIN_ONCE_TIMEOUT
    is_uint "$SETTLE"  || SETTLE=15
    is_uint "$TIMEOUT" || TIMEOUT=180
    case $MODE_V in once|always) ;; *) MODE_V=once ;; esac
}

# --------------------------------------------------------------- helpers ------
ctl() { cat "$GPU/control" 2>/dev/null; }
rst() { cat "$GPU/runtime_status" 2>/dev/null; }
state_get() { awk 'NR==1{print $1}' "$STATE" 2>/dev/null; }
state_set() {   # $1 state, $2 optional reason
    printf '%s %s %s\n' "$1" "$(date +%s)" "${2:-}" > "$STATE" 2>/dev/null || true
}

external_up() {
    local f
    for f in $DP_GLOB; do
        [ -r "$f" ] || continue
        [ "$(cat "$f" 2>/dev/null)" = connected ] && return 0
    done
    return 1
}

panel_lit() {
    local v
    v=$(cat "$BL/actual_brightness" 2>/dev/null)
    [ -n "$v" ] || return 1
    [ "$v" -gt 0 ] 2>/dev/null
}

gdm_up() { "$SYSTEMCTL" is-active --quiet "$GDM_UNIT" >/dev/null 2>&1; }

# Milliseconds, so SETTLE is honoured exactly instead of at 1-second granularity
# ($SECONDS is an integer).  EPOCHREALTIME is bash >= 5; the fallback keeps the
# script working elsewhere (1 s granularity, like the first version).
now_ms() {
    local t=${EPOCHREALTIME:-}
    if [ -n "$t" ]; then
        t=${t//[.,]/}
        printf '%s' "$(( t / 1000 ))"
    else
        printf '%s' "$(( SECONDS * 1000 ))"
    fi
}

# The GPU has done real work once its clock has been seen above the idle floor;
# the caller remembers the peak, because an idle compositor drops back to it.
clock_now() { cat "$DEVFREQ/cur_freq" 2>/dev/null; }
clock_floor() { cat "$DEVFREQ/min_freq" 2>/dev/null; }
clock_above_floor() {
    local c f
    c=$(clock_now); f=$(clock_floor)
    [ -n "$c" ] && [ -n "$f" ] || return 1
    [ "$c" -gt "$f" ] 2>/dev/null
}

# -------------------------------------------------------------------- pin -----
do_pin() {
    read_config
    local c
    # Same short-circuit as erhai-gpu-pin.sh: a kernel that boots without the
    # Adreno node (or with it disabled) must not spend time failing.
    local dt_status
    dt_status=$(cat ${ERHAI_GPU_DT_STATUS:-/proc/device-tree/soc@0/gpu@*/status} \
        2>/dev/null | tr -d '\0')
    case $dt_status in
        okay|ok) ;;
        *)
            say "gpu status '${dt_status:-absent}', nothing to pin"
            state_set absent "dt:${dt_status:-absent}"
            return 0
            ;;
    esac
    if [ -x "$PIN_HELPER" ]; then
        "$PIN_HELPER" || true          # arms the dev-kernel GMU gate + writes 'on'
    fi
    c=$(ctl)
    if [ "$c" != on ] && [ -w "$GPU/control" ]; then
        echo on > "$GPU/control" 2>/dev/null || true
        c=$(ctl)
    fi
    if [ "$c" = on ]; then
        state_set pinned
        say "pinned (runtime-PM 'on') for the boot bring-up"
        return 0
    fi
    say "no GPU to pin (control='${c:-absent}') — nothing to do"
    state_set absent
    return 0
}
# ---------------------------------------------------------------- release -----
# Keeps the pin and returns 0 when the release must not happen.
release_guards() {
    if external_up; then
        say "external display connected -> keeping the GPU PINNED"
        state_set kept external
        return 0
    fi
    return 1
}

do_release() {
    read_config
    local c v deadline since seen reason
    c=$(ctl)
    if [ -z "$c" ]; then
        say "no GPU power node — nothing to release"
        state_set absent
        return 0
    fi
    if [ "$MODE_V" = always ]; then
        say "MODE=always -> the permanent pin owns the GPU, staying pinned"
        state_set kept mode=always
        return 0
    fi
    if [ "$c" != on ]; then
        say "the pin is already gone (control=$c) — nothing to release"
        state_set released "control=$c"
        return 0
    fi
    release_guards && return 0

    deadline=$((SECONDS + TIMEOUT)); since=''; seen=''; reason=''
    while :; do
        release_guards && return 0
        reason=''
        if ! gdm_up; then
            reason="session not up ($GDM_UNIT)"
        elif ! panel_lit; then
            reason="panel dark"
        elif [ "$(rst)" != active ]; then
            reason="gpu $(rst)"
        else
            if clock_above_floor; then
                seen=$(clock_now)
            elif [ -z "$seen" ]; then
                reason="gpu clock never rose above its idle floor"
            fi
        fi
        if [ -z "$reason" ]; then
            [ -n "$since" ] || since=$(now_ms)
            if [ $(( $(now_ms) - since )) -ge $(( SETTLE * 1000 )) ]; then
                break
            fi
        else
            since=''
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            say "gave up after ${TIMEOUT}s (${reason:-conditions never settled}) -> keeping the GPU PINNED"
            state_set kept "timeout:${reason:-not-settled}"
            return 0
        fi
        sleep "$POLL"
    done

    # Last look right before the write: a monitor may have appeared while we
    # were settling, and the pin must survive for it.
    release_guards && return 0
    if ! echo auto > "$GPU/control" 2>/dev/null; then
        say "cannot write $GPU/control — keeping the GPU PINNED"
        state_set kept write-failed
        return 0
    fi
    v=$(clock_now)
    say "RELEASED (control=auto) — boot bring-up done ($GDM_UNIT up, panel lit, gpu clocked ${seen}Hz, settled ${SETTLE}s, now ${v}Hz)"
    state_set released "clock=$seen"
    return 0
}

# ------------------------------------------------------------- force unpin ----
# `unpin [reason]` — the manual form, and the SINGLE WRITER of a release for
# other shipped helpers: erhai-gpu-external-pin.sh calls it as
# `unpin ext-detach` when the monitor that justified the pin is gone, so the
# state file stays truthful (a stale `kept external` would let the next panel
# restore re-pin the GPU through erhai-panel-power.sh's pin_gpu()).
do_unpin() {
    read_config
    local c reason=${2:-manual}
    c=$(ctl)
    if [ -z "$c" ]; then
        say "no GPU power node — nothing to unpin"
        return 0
    fi
    if [ "$c" = auto ]; then
        state_set released "control=auto"
        say "already unpinned (control=auto)"
        return 0
    fi
    if echo auto > "$GPU/control" 2>/dev/null; then
        state_set released "$reason"
        say "released (control=auto, $reason)"
        return 0
    fi
    say "cannot write $GPU/control"
    return 1
}

# ------------------------------------------------------------------ status ----
do_status() {
    read_config
    local panel=no ext=no clock floor c r gdm
    panel_lit && panel=lit
    external_up && ext=yes
    clock=$(clock_now); floor=$(clock_floor)
    c=$(ctl); r=$(rst)
    if gdm_up; then gdm=active; else gdm=inactive; fi
    printf 'mode=%s state=%s control=%s runtime_status=%s gdm=%s panel=%s external=%s clock=%s floor=%s settle=%s timeout=%s\n' \
        "$MODE_V" "$(state_get)" "${c:-absent}" "${r:-absent}" "$gdm" \
        "$panel" "$ext" "${clock:-?}" "${floor:-?}" "$SETTLE" "$TIMEOUT"
    [ -r "$STATE" ] && printf 'state_file=%s: %s' "$STATE" "$(cat "$STATE" 2>/dev/null)"
    return 0
}

case $ACTION in
    pin)     do_pin ;;
    release) do_release ;;
    unpin)   do_unpin "$@" ;;
    status)  do_status ;;
    *)       printf 'usage: %s {pin|release|unpin [reason]|status}\n' "${0##*/}" >&2; exit 2 ;;
esac