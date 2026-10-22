#!/bin/sh
# Panel backlight off/on (ktz8868). Android-style power-button blank.
#
#   erhai-display-toggle.sh          toggle  (power button — legacy behaviour)
#   erhai-display-toggle.sh off      blank, remembering the current brightness
#   erhai-display-toggle.sh on       restore, but only while blanked
#
# The explicit on/off forms are idempotent so that a repeated event can never
# flip the panel the wrong way; the folio-fold watcher (erhai-folio-fold.py)
# uses them, the power button keeps using the toggle form.
#
# A short click is played through the ACTIVE GRAPHICAL SESSION whenever the
# panel is switched off, so the user hears that the blank really happened (the
# screen is often covered by the folio at that moment).  This script normally
# runs as root from a system service and root has no audio session of its own,
# so the player is launched as the session user with its XDG_RUNTIME_DIR and
# session bus address — the same trick erhai-pogo-osk.py uses for gsettings.
#
# Optional /etc/erhai/display.conf — sourced if present, same variable names:
#   CLICK=/usr/share/sounds/erhai/panel-off.wav   click on blank (default)
#   CLICK=off                                     no click at all
#   CLICK_ON=/usr/share/sounds/erhai/panel-on.wav click on restore (default);
#                                                 CLICK_ON=off mutes it
#   SOUND_PLAYER=/usr/bin/pw-play                 override the player
BL=/sys/class/backlight/ktz8868-backlight
STATE=/run/erhai-display-last-brightness
ACTION=${1:-toggle}

# The blank/unblank must not queue behind a compile/encode (owner 2026-10-22):
# a fold or a power press needs the panel dark NOW, and this script is what both
# paths use.  Only root may renice; the value dies with the script.
if [ "$(id -u)" = 0 ]; then
    renice -n -20 -p $$ >/dev/null 2>&1 || true
    ionice -c2 -n0 -p $$ >/dev/null 2>&1 || true
fi

CLICK=/usr/share/sounds/erhai/panel-off.wav
CLICK_ON=/usr/share/sounds/erhai/panel-on.wav
SOUND_PLAYER=$(command -v pw-play 2>/dev/null \
    || command -v paplay 2>/dev/null || true)
CONF=${ERHAI_DISPLAY_CONF:-/etc/erhai/display.conf}
[ -r "$CONF" ] && . "$CONF"

cur=$(cat "$BL/actual_brightness" 2>/dev/null)
[ -z "$cur" ] && exit 1

# user name of the active wayland/X11 session of a real (uid >= 1000) user
session_user() {
    loginctl list-sessions --no-legend 2>/dev/null | while read -r sid rest; do
        props=$(loginctl show-session "$sid" -p Active -p Type -p Name -p User \
            2>/dev/null) || continue
        case "$props" in *Active=yes*) ;; *) continue ;; esac
        case "$props" in *Type=wayland*|*Type=x11*) ;; *) continue ;; esac
        uid=$(printf '%s\n' "$props" | sed -n 's/^User=//p')
        name=$(printf '%s\n' "$props" | sed -n 's/^Name=//p')
        [ -n "$name" ] || continue
        case "$uid" in ''|*[!0-9]*) continue ;; esac
        [ "$uid" -ge 1000 ] 2>/dev/null || continue
        printf '%s\n' "$name"
        break
    done
}

# Run a command as the session user.  setpriv is preferred over runuser: no PAM
# round trip, measured 4x faster under CPU load (the click must not sit behind a
# compile either).
run_as_user() {   # $1 user, $2 uid, $3 gid, rest = command
    local u=$1 uid=$2 gid=$3
    shift 3
    if command -v setpriv >/dev/null 2>&1; then
        setpriv --reuid="$uid" --regid="$gid" --init-groups "$@"
    else
        runuser -u "$u" -- "$@"
    fi
}

# play a sound file through the session audio (never blocking the toggle)
play_file() {
    case "${1:-}" in ''|off|0|no) return 0 ;; esac
    [ -r "$1" ] || return 0
    if [ -z "$SOUND_PLAYER" ]; then
        command -v canberra-gtk-play >/dev/null 2>&1 || return 0
    fi
    if [ "$(id -u)" = 0 ]; then
        u=$(session_user)
        [ -n "$u" ] || return 0
        uid=$(id -u "$u" 2>/dev/null) || return 0
        gid=$(id -g "$u" 2>/dev/null) || gid=$uid
        rt=/run/user/$uid
        [ -d "$rt" ] || return 0
        if [ -n "$SOUND_PLAYER" ]; then
            run_as_user "$u" "$uid" "$gid" env XDG_RUNTIME_DIR="$rt" \
                DBUS_SESSION_BUS_ADDRESS="unix:path=$rt/bus" \
                "$SOUND_PLAYER" "$1" >/dev/null 2>&1 &
        else
            run_as_user "$u" "$uid" "$gid" env XDG_RUNTIME_DIR="$rt" \
                DBUS_SESSION_BUS_ADDRESS="unix:path=$rt/bus" \
                canberra-gtk-play --id=bell >/dev/null 2>&1 &
        fi
    elif [ -n "$SOUND_PLAYER" ]; then
        "$SOUND_PLAYER" "$1" >/dev/null 2>&1 &
    fi
    return 0
}

restore_value() {
    saved=$(cat "$STATE" 2>/dev/null)
    case "$saved" in ''|*[!0-9]*) saved=$(cat "$BL/max_brightness") ;; esac
    [ "$saved" -gt 0 ] 2>/dev/null || saved=$(cat "$BL/max_brightness")
    printf '%s\n' "$saved"
}

blank() {
    if [ "$cur" -gt 0 ]; then
        echo "$cur" > "$STATE"
        echo 0 > "$BL/brightness"
        logger -t erhai-display "backlight OFF (was $cur)"
        play_file "$CLICK"
    fi
}

unblank() {
    if [ "$cur" -eq 0 ]; then
        saved=$(restore_value)
        echo "$saved" > "$BL/brightness"
        logger -t erhai-display "backlight ON ($saved)"
        play_file "$CLICK_ON"
    fi
}

case "$ACTION" in
    off)    blank ;;
    on)     unblank ;;
    toggle) [ "$cur" -gt 0 ] && blank || unblank ;;
    *)
        logger -t erhai-display "unknown action '$ACTION' (use on|off|toggle)"
        exit 2
        ;;
esac