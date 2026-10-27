#!/bin/bash
# erhai-suspend-wlan-guard.sh — refuse to enter s2idle on a crashed / half-dead
# WLAN.  Called by erhai-suspend-s2idle.sh (STEP2e) on EVERY suspend path.
#
# WHY (2026-10-21): four GNOME "Suspend" tests were run in a row.  Three of them
# could not be woken with the power key and needed a hard reset; the fourth slept
# 20.2 s and woke normally.  The journal shows the difference is NOT the suspend
# itself but the WLAN teardown that NetworkManager does on logind's
# PrepareForSleep (radio off -> last vdev down -> the 250 ms "psoc idle timer"
# (`gInterfaceChangeWait`, hdd_psoc_idle_timeout_callback) fires ->
# `cnss: Doing idle shutdown` -> hdd_psoc_idle_shutdown -> cds_post_disable ->
# htc_stop -> hif_ce_stop -> hif_disable_isr -> hif_exec_kill ->
# hif_exec_napi_kill -> mutex_lock(<garbage>)):
#
#   * crashed boots: "Unable to handle kernel paging request" +
#     "kworker/u33:N exited with preempt_count 1" 0.1-0.2 s after
#     "htc_stop: stopping hif layer", the cnss event NEVER completes
#     ("Completed event: IDLE_SHUTDOWN" count = 0) and `echo mem` 2.7-3.0 s later
#     never returns -> the SoC must be hard reset.
#   * the surviving boot: the very same shutdown COMPLETED
#     ("cnss: Completed event: IDLE_SHUTDOWN(14) ... ret: 0/0") before the
#     suspend, which then slept 20.2 s and woke with a short power press.
#
# So the guard's job is to make the FAILURE MODE SAFE: if the WLAN teardown has
# already Oopsed (the driver's state machine is stuck, so any later WLAN
# operation blocks forever) or is still in flight, DO NOT suspend — abort the
# suspend and restore the panel.  A pad that does not sleep is a nuisance; a pad
# that needs a hard reset loses the session.
#
# It is a GUARD, not a fix: the underlying race (a WLAN idle-shutdown racing the
# interface teardown in the vendor driver) is still there.
#
# EXIT CODES
#   0  safe to suspend (teardown complete / none pending, WLAN settled)
#   3  ABORT — do NOT write /sys/power/state
#
# KNOBS (env)
#   GUARD_ENABLE=1           0 = always exit 0 (escape hatch)
#   GUARD_SECS=20            budget for an in-flight teardown to complete
#   GUARD_REQUIRE_SETTLED=1  1 = require wlp1s0 to be settled too
#                            (unavailable|unmanaged|disconnected|absent)
#   WLAN_IF=wlp1s0           interface to watch
#   DMESG_CMD=dmesg          overridable for tests
#   NMCLI_CMD=nmcli          overridable for tests
set -u

GUARD_ENABLE=${GUARD_ENABLE:-1}
GUARD_SECS=${GUARD_SECS:-20}
GUARD_REQUIRE_SETTLED=${GUARD_REQUIRE_SETTLED:-1}
WLAN_IF=${WLAN_IF:-wlp1s0}
DMESG_CMD=${DMESG_CMD:-dmesg}
NMCLI_CMD=${NMCLI_CMD:-nmcli}

note() { printf 'wlan-guard: %s\n' "$*"; }

if [ "$GUARD_ENABLE" != 1 ]; then
	note "DISABLED (GUARD_ENABLE=$GUARD_ENABLE) — suspend allowed without a WLAN check"
	exit 0
fi

# The harness runs as root (systemd-suspend.service), so dmesg works there; if it
# does NOT (a hand-run as a user, kernel.dmesg_restrict=1), fall back to the
# journal — a guard that cannot read the log would PASS blindly, which is the one
# failure mode that still costs a hard reset.
if ! $DMESG_CMD >/dev/null 2>&1; then
	if command -v journalctl >/dev/null 2>&1; then
		note "dmesg unreadable ($DMESG_CMD) — reading the kernel log from journalctl"
		DMESG_CMD="journalctl -b -k -o cat --no-pager"
	else
		note "WARNING: dmesg unreadable and no journalctl — the guard cannot see the kernel log"
	fi
fi

dcount() { $DMESG_CMD 2>/dev/null | grep -c -e "$1"; }

# "Posted event: IDLE_SHUTDOWN" starts a teardown, "Completed event:
# IDLE_SHUTDOWN" ends it; a POSTED-but-never-COMPLETED pair is exactly the
# crashed/hung state (the cnss driver-event worker died inside it).
posted=$(dcount 'Posting event: IDLE_SHUTDOWN')
completed=$(dcount 'Completed event: IDLE_SHUTDOWN')
oops=$(dcount 'hif_exec_napi_kill')
note "baseline: idleshutdown posted=$posted completed=$completed wlan-teardown-oops=$oops"

if [ "$oops" -gt 0 ]; then
	note "ABORT: the WLAN teardown has ALREADY crashed this boot"
	note "ABORT:   'hif_exec_napi_kill' Oops in dmesg -> the cnss worker died"
	note "ABORT:   holding a lock; entering s2idle now HANGS the SoC (hard reset)."
	note "ABORT:   the WLAN is wedged too — a reboot is the only recovery."
	exit 3
fi

state() {
	$NMCLI_CMD -t -f DEVICE,STATE dev 2>/dev/null |
		awk -F: -v i="$WLAN_IF" '$1 == i { print $2; exit }'
}

settled() {
	case "$(state)" in
	""|unavailable|unmanaged|disconnected) return 0 ;;
	*) return 1 ;;
	esac
}

i=0
while :; do
	st=$(state)
	posted=$(dcount 'Posting event: IDLE_SHUTDOWN')
	completed=$(dcount 'Completed event: IDLE_SHUTDOWN')
	oops=$(dcount 'hif_exec_napi_kill')
	in_flight=$((posted - completed))

	if [ "$oops" -gt 0 ]; then
		note "ABORT t=${i}s: the WLAN teardown crashed DURING the wait"
		note "ABORT t=${i}s:   (hif_exec_napi_kill) — suspending would hang the SoC."
		exit 3
	fi

	if [ "$in_flight" -le 0 ]; then
		if [ "$GUARD_REQUIRE_SETTLED" != 1 ] || settled; then
			note "PASS t=${i}s: no teardown in flight (posted=$posted completed=$completed), ${WLAN_IF}=${st:-absent}"
			exit 0
		fi
	fi

	if [ "$i" -ge "$GUARD_SECS" ]; then
		if [ "$in_flight" -gt 0 ]; then
			note "ABORT: the WLAN idle shutdown never completed within ${GUARD_SECS}s"
			note "ABORT:   posted=$posted completed=$completed (in flight=$in_flight) —"
			note "ABORT:   this is the state that hangs s2idle (hard reset)."
		else
			note "ABORT: ${WLAN_IF} still not settled after ${GUARD_SECS}s (state=${st:-absent})"
			note "ABORT:   never carry a WLAN mid-teardown into s2idle."
		fi
		exit 3
	fi

	if [ "$i" = 0 ]; then
		note "waiting for the WLAN teardown (${WLAN_IF}=${st:-absent} posted=$posted completed=$completed)..."
	fi
	sleep 1
	i=$((i + 1))
done
