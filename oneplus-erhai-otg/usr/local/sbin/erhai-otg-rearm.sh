#!/bin/sh
# erhai (OnePlus Pad 3): revive ADSP CC attach detection after a USB-C detach.
#
# Triggered by udev on port0 "change" events, but RE-ARMS ONLY ON THE
# DETACH EDGE (partner present -> absent), tracked via a marker file:
#  - partner present: remember it, exit (attach re-arms its session anyway)
#  - partner absent + marker exists: this is the detach edge -> re-arm once,
#    clear the marker
#  - partner absent, no marker: the re-arm write itself emits port0 change
#    uevents; without the edge guard this loops forever (2026-08-28: ~4 s
#    storm for hours, broke SVOOC handshakes)
#
# NOTE: ACTION=="remove" rules with SYSTEMD_WANTS never fire (systemd
# drops the request because the device is gone by the time it processes
# the uevent) - that is why the 1.1 remove-only rule never triggered.

MARKER=/run/erhai-otg-was-present

if [ -e /sys/class/typec/port0-partner ]; then
	touch "$MARKER" 2>/dev/null
	exit 0
fi

[ -e "$MARKER" ] || exit 0
rm -f "$MARKER"

sleep 3
if echo "17 1" > /sys/kernel/debug/erhai_otg/cmd &&
   echo "18 1" > /sys/kernel/debug/erhai_otg/cmd; then
	logger -t erhai-otg-rearm "ADSP OTG session re-armed (detach edge)"
else
	logger -t erhai-otg-rearm "ERROR: failed to write erhai_otg/cmd"
fi
