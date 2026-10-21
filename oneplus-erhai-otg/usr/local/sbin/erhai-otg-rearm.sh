#!/bin/sh
# erhai (OnePlus Pad 3): revive ADSP CC attach detection after a USB-C detach.
#
# WHY: on every USB-C detach the kernel driver erhai_otg_glink disarms the ADSP OTG
# session; on this ADSP firmware a session disarmed after a DP-altmode-capable partner
# leaves the Type-C CC attach detection DEAD -- replugs are invisible until the session
# is re-armed (2026-08-28) or the pad is rebooted.  Re-arm = the two debugfs writes
# USB_OTG_AP_ENABLE=17 / USB_OTG_SWITCH=18.
#
# MODES
#   check   -- run by the udev rule as PROGRAM=: CHEAP, prints one word, NEVER re-arms
#              (a service spawn per event is what made the old rule noisy).
#              present -> a partner is attached (remember it in $MARKER)
#              edge    -> the partner just disappeared AND one was seen  => re-arm
#              idle    -> nothing to do
#   rearm   -- run by erhai-otg-rearm.service (ExecStart): rate-guarded re-arm.
#   status  -- human diagnosis; changes nothing.
#   (none)  -- old back-compatible auto path: edge-guarded like `check` + `rearm`.
#
# WHY THE GATE: this line's typec port0 emits a PHANTOM `change` uevent every ~4.03 s
# with no partner/role information in its payload (measured 2026-10-21: SEQNUM +1 every
# 4.03 s), so the original rule started this service ~15x/min and the mechanism was
# disabled on 2026-09-14.  With `PROGRAM=... check, RESULT=="edge"` in the rule the
# service only ever starts on a REAL detach edge.
#
# The guards below are belt-and-braces against any future edge storm: at most one
# re-arm per OTG_REARM_MIN_INTERVAL seconds and OTG_REARM_MAX_PER_BOOT per boot.
set -u
MARKER=/run/erhai-otg-was-present
STAMP=/run/erhai-otg-last-rearm
COUNT=/run/erhai-otg-rearm-count
MIN_INTERVAL=${OTG_REARM_MIN_INTERVAL:-30}
MAX_PER_BOOT=${OTG_REARM_MAX_PER_BOOT:-20}
SETTLE=${OTG_REARM_SETTLE:-3}
CMD=/sys/kernel/debug/erhai_otg/cmd

partner_present() { [ -e /sys/class/typec/port0-partner ]; }

do_rearm() {
	now=$(date +%s)
	last=0
	[ -e "$STAMP" ] && last=$(cat "$STAMP" 2>/dev/null || echo 0)
	if [ "$((now - last))" -lt "$MIN_INTERVAL" ]; then
		logger -t erhai-otg-rearm "re-arm suppressed (rate limit ${MIN_INTERVAL}s, last ${last})"
		return 0
	fi
	n=0
	[ -e "$COUNT" ] && n=$(cat "$COUNT" 2>/dev/null || echo 0)
	if [ "$n" -ge "$MAX_PER_BOOT" ]; then
		logger -t erhai-otg-rearm "re-arm suppressed (boot cap ${MAX_PER_BOOT} reached)"
		return 0
	fi
	printf '%s\n' "$now" > "$STAMP" 2>/dev/null
	printf '%s\n' "$((n + 1))" > "$COUNT" 2>/dev/null
	# let the detach settle before poking the ADSP session
	sleep "$SETTLE"
	if [ -w "$CMD" ] && echo "17 1" > "$CMD" && echo "18 1" > "$CMD"; then
		rm -f "$MARKER"
		logger -t erhai-otg-rearm "ADSP OTG session re-armed (detach edge)"
	else
		logger -t erhai-otg-rearm "ERROR: failed to write $CMD (kernel debugfs missing?)"
		return 1
	fi
	return 0
}

case "${1:-auto}" in
check)
	# NOTE: the verdict is rate-gated too, so a phantom partner oscillation cannot
	# spawn the unit 15x/min (it only spawns when a re-arm is actually due).
	if partner_present; then
		touch "$MARKER" 2>/dev/null
		echo present
	elif [ -e "$MARKER" ]; then
		now=$(date +%s); last=0
		[ -e "$STAMP" ] && last=$(cat "$STAMP" 2>/dev/null || echo 0)
		if [ "$((now - last))" -ge "$MIN_INTERVAL" ]; then
			echo edge
		else
			echo edge-suppressed
		fi
	else
		echo idle
	fi
	;;
rearm)
	do_rearm
	;;
auto)
	if partner_present; then
		touch "$MARKER" 2>/dev/null
		exit 0
	fi
	[ -e "$MARKER" ] || exit 0
	do_rearm
	;;
status)
	echo "partner      : $(partner_present && echo present || echo none)"
	echo "typec role   : data=$(cat /sys/class/typec/port0/data_role 2>/dev/null) power=$(cat /sys/class/typec/port0/power_role 2>/dev/null)"
	echo "xhci host    : $(ls -d /sys/bus/platform/devices/xhci-hcd.*.auto 2>/dev/null | head -1 || echo none)"
	echo "usb devices  : $(lsusb 2>/dev/null | grep -vc 'Linux Foundation')"
	echo "detach marker: $([ -e "$MARKER" ] && echo present || echo absent)"
	echo "last re-arm  : $([ -e "$STAMP" ] && cat "$STAMP" 2>/dev/null || echo never)  (count this boot: $([ -e "$COUNT" ] && cat "$COUNT" 2>/dev/null || echo 0)/$MAX_PER_BOOT)"
	echo "debugfs cmd  : $([ -w "$CMD" ] && echo writable || echo MISSING)"
	if [ "$(ls -d /sys/bus/platform/devices/xhci-hcd.*.auto 2>/dev/null | wc -l)" = 0 ] && [ "$(lsusb 2>/dev/null | grep -vc 'Linux Foundation')" = 0 ]; then
		echo "VERDICT      : no xHCI host and no devices -> if a device is attached, run"
		echo "               'sudo $0 rearm' and replug it (unplug/replug), else reboot."
	elif [ "$(lsusb 2>/dev/null | grep -vc 'Linux Foundation')" -gt 0 ]; then
		echo "VERDICT      : CC detection works (devices enumerated)."
	fi
	;;
*)
	echo "usage: $0 check|rearm|status" >&2
	exit 2
	;;
esac
