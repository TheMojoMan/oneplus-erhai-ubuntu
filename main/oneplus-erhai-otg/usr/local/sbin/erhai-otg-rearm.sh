#!/bin/sh
# erhai (OnePlus Pad 3): revive Type-C attach detection after a USB-C host session ends.
#
# WHY: on this ADSP firmware the CC attach detection goes dead when a host session ends,
# so the next plug is invisible (no ADSP event, no VBUS, no role flip) until the pad is
# rebooted.  Since 2026-09-15 the kernel keeps the ADSP OTG session ARMED through detach
# (erhai_detach_disarm_enable=0), which switches off all of its self-heal paths -- and a
# plain re-arm (USB_OTG_AP_ENABLE=17 / USB_OTG_SWITCH=18 = 1) is a no-op while the session
# is still armed.  Measured 2026-10-28 (kernel #205): a disarm+arm restart does NOT revive
# it either; what does is the DATA ROLE: writing `host` to the dwc3 role switch makes the
# ADSP announce the partner (typec `port0-partner` appears, ucsi `partner_change role -> 1`)
# within a second.  VBUS is still off at that point, so a bus-powered disk stays dark until
# USB_OTG_VBUS_REGULATOR_ENABLE=24 is set -- the kernel's own vbus_apply() issues that write
# unchecked and it silently does not take (see the kernel memory entry).
#
# So the whole recovery is two userspace steps, both udev-driven (no polling):
#   host-kick  on the detach edge  : data role -> host, VBUS stays OFF (safe: no source
#                                    into a charger, nothing is powered while idle)
#   vbus       on a typec partner   : VBUS on + boost, but ONLY for a sink partner
#              add event             (power_role source) and with no charger online.
#
# MODES
#   check     -- run by the udev rule as PROGRAM=: CHEAP, prints one word, never acts.
#                present -> a host session is up (remember it in $MARKER)
#                edge    -> the host session just died AND one was seen
#                idle    -> nothing to do
#   host-kick -- the detach-edge action: data role -> host (no VBUS).
#   vbus      -- the attach action: VBUS on + boost for a sink partner only.
#   restart   -- manual recovery: disarm+arm the ADSP session, then host-kick.
#   status    -- human diagnosis; changes nothing.
#   (none)    -- old back-compatible auto path: edge-guarded `check` + `host-kick`.
#
# WHY THE GATE: this line's typec port0 emits a PHANTOM `change` uevent every ~4.03 s with
# no partner/role information in its payload (measured 2026-10-21: SEQNUM +1 every 4.03 s),
# so the original rule started the service ~15x/min and the mechanism was disabled on
# 2026-09-14.  The edge is therefore derived from STATE (the xHCI host platform device,
# which exists exactly while the pad owns a host session), never from the payload.  Do NOT
# gate on /sys/class/typec/port0-partner: it flaps, and in the broken state it is absent
# even with a device plugged in (2026-10-28) -- that is why the 1.3 gate never fired.
set -u
MARKER=/run/erhai-otg-was-present
STAMP=/run/erhai-otg-last-rearm
COUNT=/run/erhai-otg-rearm-count
MIN_INTERVAL=${OTG_REARM_MIN_INTERVAL:-30}
MAX_PER_BOOT=${OTG_REARM_MAX_PER_BOOT:-20}
SETTLE=${OTG_REARM_SETTLE:-3}
CMD=/sys/kernel/debug/erhai_otg/cmd
ROLE_SW=/sys/class/usb_role/a600000.usb-role-switch/role
TYPEC=/sys/class/typec/port0

# A host session needs BOTH: dwc3 in host mode (role) and the xhci-hcd platform device
# bound.  After alt-mode/charger churn the controller probe can fail with -110 and the
# platform device is then left UNBOUND, so a perfectly good attach stays dark even with
# VBUS on (2026-10-28 17:16: "can't setup: -110" -> probe failed -> drive plugged, no
# enumeration until `xhci-hcd.3.auto` was bound by hand).  Only ever touch
# xhci-hcd.*.auto: unbinding dwc3-qcom / qcom-qmp-combo-phy bricks the port.
xhci_host_dev() {
	ls -d /sys/bus/platform/devices/xhci-hcd.*.auto 2>/dev/null | head -1
}

xhci_rebind() {
	dev=$(xhci_host_dev)
	[ -n "$dev" ] || return 0
	name=${dev##*/}
	[ -e "/sys/bus/platform/drivers/xhci-hcd/$name" ] && return 0
	if echo "$name" > /sys/bus/platform/drivers/xhci-hcd/bind 2>/dev/null; then
		logger -t erhai-otg-rearm "xhci: $name was unbound, bound it"
	else
		logger -t erhai-otg-rearm "ERROR: could not bind $name"
		return 1
	fi
	return 0
}

host_present() {
	ls -d /sys/bus/platform/devices/xhci-hcd.*.auto >/dev/null 2>&1
}

role() { cat "$ROLE_SW" 2>/dev/null || echo unknown; }

# The ADSP charger psy is "online" only while a charger feeds the port.
charger_online() {
	for f in /sys/class/power_supply/ucsi-source-psy-*/online; do
		[ -e "$f" ] || continue
		[ "$(cat "$f" 2>/dev/null || echo 0)" != 0 ] && return 0
	done
	return 1
}

# power_role is "source [sink]"-style; the bracketed one is the CURRENT role.
power_role_current() {
	sed -n 's/.*\[\([a-z]*\)\].*/\1/p' "$TYPEC/power_role" 2>/dev/null
}

# One debugfs SET; the kernel driver returns the ADSP result, so a failing write must
# not be mistaken for success.
otg_set() {
	echo "$1 $2" > "$CMD"
}

do_host_kick() {
	# The udev hook fires the instant the xHCI host device goes away, but the kernel's
	# own detach path (erhai_otg_vbus_apply(false), VBUS off -> 500 ms -> role device)
	# finishes ~1.2 s LATER.  Acting immediately finds the role still at host and would
	# bail out; wait for the detach to settle first (measured 2026-10-28, kernel #205).
	sleep "${OTG_TOUCH_SETTLE:-2}"
	if host_present; then
		logger -t erhai-otg-rearm "host touch: session already up, nothing to do"
		return 0
	fi
	if [ ! -w "$ROLE_SW" ]; then
		logger -t erhai-otg-rearm "ERROR: $ROLE_SW not writable"
		return 1
	fi
	if echo host > "$ROLE_SW"; then
		rm -f "$MARKER"
		logger -t erhai-otg-rearm "host touch: data role -> host (VBUS off, power_role $(power_role_current), partner $([ -e "$TYPEC-partner" ] && echo present || echo none))"
	else
		logger -t erhai-otg-rearm "ERROR: could not set the data role to host"
		return 1
	fi
	return 0
}

do_vbus() {
	# Only ever power a SINK partner.  A charger makes the ADSP flip our typec power
	# role to sink, and then VBUS must stay off.  Do NOT gate on the charger psy:
	# when this pad sources VBUS itself the ADSP's charger detection sees that 5 V and
	# reports a charger (2026-10-28, drive attach: "vbus: skipped (charger online)"
	# while the partner was the disk), so power_role is the only trustworthy bit.
	sleep 0.3
	if [ "$(role)" != host ]; then
		logger -t erhai-otg-rearm "vbus: skipped (data role $(role))"
		return 0
	fi
	if [ "$(power_role_current)" = sink ]; then
		logger -t erhai-otg-rearm "vbus: skipped (port power role is sink, charger $(charger_online && echo online || echo none))"
		return 0
	fi
	if [ ! -w "$CMD" ]; then
		logger -t erhai-otg-rearm "ERROR: $CMD not writable (kernel debugfs missing?)"
		return 1
	fi
	if otg_set 24 1 && otg_set 64 2000; then
		logger -t erhai-otg-rearm "vbus: on (24 1, boost 2000 mA)"
	else
		logger -t erhai-otg-rearm "ERROR: VBUS enable failed"
		return 1
	fi
	xhci_rebind
	return 0
}

# The attach action: power a sink partner and make sure a host controller is really
# there.  Runs on the typec partner add AND on every usb_role change to host, because
# the ADSP can keep a stale partner object (no second `add` uevent) and because the
# role change is what the ADSP actually grants for a new device.
do_attach() {
	if [ "$(role)" != host ]; then
		logger -t erhai-otg-rearm "attach: skipped (data role $(role))"
		return 0
	fi
	xhci_rebind
	do_vbus
	return 0
}

do_restart() {
	now=$(date +%s)
	last=0
	[ -e "$STAMP" ] && last=$(cat "$STAMP" 2>/dev/null || echo 0)
	if [ "$((now - last))" -lt "$MIN_INTERVAL" ]; then
		logger -t erhai-otg-rearm "session restart suppressed (rate limit ${MIN_INTERVAL}s, last ${last})"
		return 0
	fi
	n=0
	[ -e "$COUNT" ] && n=$(cat "$COUNT" 2>/dev/null || echo 0)
	if [ "$n" -ge "$MAX_PER_BOOT" ]; then
		logger -t erhai-otg-rearm "session restart suppressed (boot cap ${MAX_PER_BOOT} reached)"
		return 0
	fi
	printf '%s\n' "$now" > "$STAMP" 2>/dev/null
	printf '%s\n' "$((n + 1))" > "$COUNT" 2>/dev/null
	sleep "$SETTLE"
	if [ -w "$CMD" ]; then
		otg_set 18 0 && otg_set 17 0 && sleep 1
		otg_set 17 1 && otg_set 18 1
	fi
	logger -t erhai-otg-rearm "ADSP OTG session restarted (disarm+arm)"
	do_host_kick
}

case "${1:-auto}" in
check)
	if host_present; then
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
host-kick|rearm)
	do_host_kick
	;;
vbus)
	do_vbus
	;;
attach)
	do_attach
	;;
restart)
	do_restart
	;;
auto)
	if host_present; then
		touch "$MARKER" 2>/dev/null
		exit 0
	fi
	[ -e "$MARKER" ] || exit 0
	do_host_kick
	;;
status)
	echo "host session  : $(host_present && echo up || echo GONE)"
	echo "role switch   : $(role)"
	echo "typec role    : data=$(cat "$TYPEC/data_role" 2>/dev/null) power=$(cat "$TYPEC/power_role" 2>/dev/null)"
	echo "partner       : $([ -e "$TYPEC-partner" ] && echo present || echo none)"
	echo "charger       : $(charger_online && echo online || echo none)"
	echo "xhci host     : $(ls -d /sys/bus/platform/devices/xhci-hcd.*.auto 2>/dev/null | head -1 || echo none)"
	echo "usb devices   : $(lsusb 2>/dev/null | grep -vc 'Linux Foundation')"
	echo "detach marker : $([ -e "$MARKER" ] && echo present || echo absent)"
	echo "last restart  : $([ -e "$STAMP" ] && cat "$STAMP" 2>/dev/null || echo never)  (count this boot: $([ -e "$COUNT" ] && cat "$COUNT" 2>/dev/null || echo 0)/$MAX_PER_BOOT)"
	echo "debugfs cmd   : $([ -w "$CMD" ] && echo writable || echo MISSING)"
	if ! host_present && [ "$(lsusb 2>/dev/null | grep -vc 'Linux Foundation')" = 0 ]; then
		echo "VERDICT       : no host session and no devices -> if a device is attached,"
		echo "                run 'sudo $0 restart' and replug it, else reboot."
	elif [ "$(lsusb 2>/dev/null | grep -vc 'Linux Foundation')" -gt 0 ]; then
		echo "VERDICT       : host session alive (devices enumerated)."
	fi
	;;
*)
	echo "usage: $0 check|host-kick|attach|vbus|restart|status" >&2
	exit 2
	;;
esac
