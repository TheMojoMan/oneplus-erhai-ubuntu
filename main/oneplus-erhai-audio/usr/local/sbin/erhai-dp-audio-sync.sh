#!/bin/bash
# erhai (OnePlus Pad 3) — keep DP audio in sync with USB-C monitor hotplug.
#
# Why: the DisplayPort UCM device (hw:0,1, MM2 -> DISPLAY_PORT_RX_0) only
# probes successfully while a DP alt-mode sink is attached. When WirePlumber
# builds the card at boot WITHOUT a monitor, that probe fails and the
# "DisplayPort" output node never gets created; plugging a monitor later does
# not trigger a re-probe by itself. This script runs on DRM connector change
# uevents (udev rule 70-erhai-dp-audio.rules -> erhai-dp-audio-sync.service),
# remembers the last 'DP0 Jack' state in /run/erhai-dp-audio.state and only
# acts when it CHANGED:
#   jack on  -> force the MM2->DP route on, then bounce WirePlumber in every
#               active graphical session so ACP re-probes and creates the
#               DP sink.
#   jack off -> just update the cache (ACP hides the port on its own via the
#               JackControl; internal audio is unaffected).
# Idempotent and cheap: most uevents exit after one amixer read.

set -u

STATE=/run/erhai-dp-audio.state
log() { echo "erhai-dp-audio-sync: $*"; }

# NOTE: amixer cannot resolve 'DP0 Jack' by name on this card (snd_ctl
# returns ENOENT for name-only lookups of CARD-interface elements), so parse
# the full control dump instead. The state row looks like "  : values=on"
# and follows an info row that also contains "values=<n>".
jack() {
	amixer -c0 contents 2>/dev/null | awk '
		/DP0 Jack/ {found=1}
		found && /^[ \t]*:[ \t]*values=/ {sub(/^.*values=/, ""); print $1; exit}'
}

JACK=$(jack)
[ -n "$JACK" ] || { log "no card0 / no DP0 Jack control yet"; exit 0; }

PREV=
[ -r "$STATE" ] && PREV=$(cat "$STATE")
if [ "$PREV" = "$JACK" ]; then
	exit 0   # nothing changed — typical case for unrelated DRM change events
fi

if [ "$JACK" = on ] || [ "$PREV" = on ]; then
	:
else
	echo "$JACK" > "$STATE"
	exit 0
fi

echo "$JACK" > "$STATE"
log "DP0 Jack changed: $PREV -> $JACK"

if [ "$JACK" != on ]; then
	log "monitor detached; ACP will hide the port itself"
	exit 0
fi

# Make sure the FE->BE routing exists before ACP re-probes the PCM.
amixer -c0 cset name='DISPLAY_PORT_RX_0 Audio Mixer MultiMedia2' 1 >/dev/null 2>&1 \
	|| log "warning: could not set MM2->DP route"

# Give the DP alt-mode handshake / modeset a moment to settle, then re-check
# so we do not restart wireplumber for a transient plug event.
sleep 2
NOW=$(jack)
if [ "$NOW" != on ]; then
	log "DP0 Jack no longer on; skipping restart"
	exit 0
fi

RESTARTED=0
for UID_N in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $2}' | sort -u); do
	if systemctl --user -M "${UID_N}@.host" is-active wireplumber >/dev/null 2>&1; then
		if systemctl --user -M "${UID_N}@.host" restart wireplumber 2>&1; then
			log "restarted wireplumber for uid $UID_N"
			RESTARTED=1
		else
			log "failed to restart wireplumber for uid $UID_N"
		fi
	fi
done

[ "$RESTARTED" = 0 ] && log "no active user session with wireplumber found"
exit 0