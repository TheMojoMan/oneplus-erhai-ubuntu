#!/bin/sh
# erhai-backlight-fixed.sh [percent]
#
# Put the internal panel backlight at a FIXED percentage of max_brightness
# (default 50).  Called by:
#   * erhai-backlight.service      - at boot, before the greeter appears
#   * erhai-greeter-brightness.py  - every time a GDM greeter session starts
#     (i.e. every time the login screen comes up after a logout)
# so the boot state and the login screen are always at the same known level
# (owner request 2026-09-22), no matter what the previous session left behind
# or what gsd-power does.
#
# Never lights a panel that is currently BLANKED: the power-button toggle
# (erhai-panel-power.sh) and the folio-fold watcher blank the panel by writing
# brightness 0 and remember their own value, so touching it here would turn a
# deliberately dark panel back on.
set -u

BL=${BACKLIGHT:-/sys/class/backlight/ktz8868-backlight}
PCT=${1:-50}

log() { logger -t erhai-backlight "$*"; }

[ -d "$BL" ] || exit 0
max=$(cat "$BL/max_brightness" 2>/dev/null) || exit 0
case "$max" in ''|*[!0-9]*) exit 0 ;; esac
[ "$max" -gt 0 ] || exit 0

cur=$(cat "$BL/actual_brightness" 2>/dev/null)
case "$cur" in ''|*[!0-9]*) cur=-1 ;; esac
if [ "$cur" = 0 ]; then
	log "panel is blanked (actual_brightness 0) - not touching it"
	exit 0
fi
for m in /run/erhai-panel.state; do
	if [ -e "$m" ]; then
		log "blank state $m present ($(read -r o _ < "$m"; echo "${o:-unknown}")) - not touching the panel"
		exit 0
	fi
done

want=$(( (max * PCT + 50) / 100 ))
[ "$want" -lt 1 ] && want=1
[ "$want" -gt "$max" ] && want=$max

have=$(cat "$BL/brightness" 2>/dev/null)
[ "$have" = "$want" ] && exit 0

if echo "$want" > "$BL/brightness" 2>/dev/null; then
	log "backlight $have -> $want ($PCT% of $max)"
	exit 0
fi
log "could not write $want to $BL/brightness"
exit 1
