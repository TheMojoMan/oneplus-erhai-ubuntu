#!/bin/sh
# Seed per-session defaults into the GDM greeter's fresh DYNAMIC-user HOME.
# The greeter user is allocated per login (gdm-greeter, uid 605xx, home under
# /run/gdm3/home/) so NOTHING persists between sessions; anything we want the
# login screen to honor must be copied in at shell start.
#
# Seeds (into BOTH $HOME/.config and $XDG_CONFIG_HOME -- gdm points the
# greeter's XDG_CONFIG_HOME at /var/lib/gdm3/seat0/config, which may differ
# from HOME):
#   monitors.xml      3392x2400 @ scale 2 (login UI not at scale 1)
#   dconf/user        pre-compiled GVDB: on-screen keyboard ON (touch login),
#                     interface scaling-factor=2
#
# NOTE: do NOT use /etc/dconf/profile/gdm or system-db:gdm for this -- that
# breaks gnome-session's session selection and the greeter comes up as a full
# 'ubuntu' desktop. A plain user-db seed is safe.
#
# All actions are logged to /dev/kmsg so they show up in journalctl.
log() { echo "erhai-greeter-seed: $*" > /dev/kmsg 2>/dev/null; }

h="${HOME:-}"
[ -n "$h" ] || h="$(getent passwd "$(id -u)" 2>/dev/null | cut -d: -f6)"
if [ -z "$h" ] || [ ! -d "$h" ]; then
    log "no home directory resolved, nothing seeded"
    exit 0
fi
log "home=$h"

# --- debug trail for scale regressions --------------------------------------
dbg="/var/lib/gdm3/seat0/config/greeter-seed.log"
{
    echo "=== $(date -Is) uid=$(id -u) home=$h XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-unset}"
    echo "drm-dsi1-modes: $(cat /sys/class/drm/card*-DSI-1/modes 2>/dev/null | tr '\n' ' ')"
    [ -f "$h/.config/monitors.xml" ] && echo "home-monitors-md5: $(md5sum < "$h/.config/monitors.xml")"
    x="${XDG_CONFIG_HOME:-$h/.config}/monitors.xml"
    [ -f "$x" ] && echo "xdg-monitors-md5: $(md5sum < "$x")"
} >> "$dbg" 2>/dev/null

seed_one() { # $1 = base dir
    cfg="$1/.config"
    mkdir -p "$cfg" 2>/dev/null || { log "cannot create $cfg"; return 0; }
    m="$cfg/monitors.xml"
    if [ ! -f "$m" ] && [ -f /usr/share/erhai/greeter-monitors.xml ]; then
        if cp /usr/share/erhai/greeter-monitors.xml "$m" 2>/dev/null; then
            log "seeded $m"
        else
            log "FAILED to copy monitors.xml to $m"
        fi
    fi
    d="$cfg/dconf/user"
    if [ ! -f "$d" ] && [ -f /usr/share/erhai/greeter-dconf-user ]; then
        mkdir -p "${d%/*}" 2>/dev/null
        if cp /usr/share/erhai/greeter-dconf-user "$d" 2>/dev/null; then
            log "seeded $d"
        else
            log "FAILED to copy dconf user db to $d"
        fi
    fi
}

seed_one "$h"
[ -n "${XDG_CONFIG_HOME:-}" ] && [ "$XDG_CONFIG_HOME" != "$h/.config" ] && \
    mkdir -p "$XDG_CONFIG_HOME" 2>/dev/null && seed_one "${XDG_CONFIG_HOME%/}"
exit 0
