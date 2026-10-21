#!/bin/bash
# erhai-wifi-recover.sh — bring the INTERNAL WCN7880 station back after a failed
# suspend wedged it, WITHOUT a reboot.
#
# WHY THIS EXISTS (2026-10-21): the suspend's WLAN teardown can Oops in
# `hif_exec_napi_kill` (see MEMORY-2026-10-21.md §suspend).  The guard now
# refuses to suspend in that state, so the pad survives — but the qcacld driver
# is left broken: every open fails with
#     wlan: [N:E:HDD] hdd_open: VDEV SYNC TRANS start fails -16
# (NetworkManager shows the device but no networks; wpa_supplicant logs
# "Failed to initialize driver interface").  Neither of the obvious things works:
#   * `nmcli radio wifi off/on`      -> the radio comes up, the driver stays broken
#   * `systemctl restart erhai-wcn-wifi.service`
#                                    -> NO-OP: the script skips the load while the
#                                       station netdev exists ("internal station ...
#                                       already present -- skipping load")
# The only in-place cure is to RELOAD wlan.ko (the driver's own init rebuilds the
# vdev/firmware state) — sequentially, with a verification gate between steps.
#
# HARD RULES (learned the hard way, MEMORY-20260823 §CRITICAL GOTCHAS):
#   * NEVER race two reloads: a half-unloaded wlan.ko leaves `lsmod` refcount -1
#     and then the module can neither be rmmod'ed nor modprobe'd (EBUSY forever)
#     -> only a REBOOT recovers.  One step at a time, always verify.
#   * NEVER `rmmod cnss2`: its exit path hangs in D-state
#     (MEMORY-2026-09-14 §"rmmod cnss2 then HANGS").
#   * **NEVER `rmmod wlan` WHILE THE DRIVER IS WEDGED** (2026-10-21, this cost a
#     hard reset): with the crashed IDLE_SHUTDOWN state machine stuck, the
#     driver's own exit path waits on it, so `rmmod wlan` hangs in D-state right
#     after `wlan: Hard Unloading driver v5.2.1.119O` — the module then stays
#     GOING (refs -1) forever AND the stuck task/unit makes a PLAIN reboot hang
#     too.  So `recover` refuses to unload a station that cannot scan and tells
#     you to reboot with `systemctl reboot --force`; only `--force recover`
#     (experiments) still unloads, bounded by UNLOAD_TIMEOUT.
#
# USAGE
#   erhai-wcn-recover.sh status          # what state is the internal station in?
#   erhai-wcn-recover.sh recover         # SAFE: radio/NM fixes; reload only if sane
#   erhai-wcn-recover.sh --force recover # also unload a wedged driver (risky)
#   DRY=1 erhai-wcn-recover.sh recover   # print the steps, change nothing
set -u

EP=/sys/bus/pci/devices/0000:01:00.0
DRY=${DRY:-0}
FORCE=0
[ "${1:-}" = "--force" ] && { FORCE=1; shift; }
MODE=${1:-status}
SETTLE=${SETTLE:-20}
DISCOVER=erhai-wifi-discover.service

say() { echo "wcn-recover: $*"; }
die() { say "$*"; exit 1; }
run() { if [ "$DRY" = 1 ]; then say "DRY would run: $*"; else "$@"; fi; }

find_sta() {
	local d n t
	for d in "$EP"/net/*; do
		[ -e "$d" ] || continue
		n=$(basename "$d")
		t=$(iw dev "$n" info 2>/dev/null | awk '/type/{print $2}')
		[ "$t" = managed ] && { echo "$n"; return 0; }
	done
	return 1
}

refcount() { awk '$1=="wlan"{print $3}' /proc/modules 2>/dev/null; }

wedge_evidence() {
	local o v
	o=$(dmesg 2>/dev/null | grep -c 'hif_exec_napi_kill')
	v=$(dmesg 2>/dev/null | grep -c 'VDEV SYNC TRANS start fails')
	echo "$o $v"
}

can_scan() {
	local n
	n=$(timeout 20 nmcli -t -f SSID dev wifi list ifname "$1" 2>/dev/null | grep -c .)
	[ "${n:-0}" -gt 0 ]
}

status() {
	local sta rc o v
	sta=$(find_sta) || sta=""
	rc=$(refcount)
	read -r o v <<EOF
$(wedge_evidence)
EOF
	say "station:      ${sta:-ABSENT}"
	say "wlan.ko refs: ${rc:-not loaded}"
	say "kernel log:   ${o} x hif_exec_napi_kill Oops, ${v} x 'VDEV SYNC TRANS start fails' this boot"
	if [ -n "$sta" ]; then
		say "nm state:     $(nmcli -t -f DEVICE,STATE,CONNECTION dev 2>/dev/null | awk -F: -v d="$sta" '$1==d{print $2"/"$3}')"
		if can_scan "$sta"; then
			say "verdict:      station is USABLE (a scan returned networks)"
		else
			say "verdict:      station present but NOT scanning -> WEDGED"
			say "verdict:      in this state the driver's unload HANGS, so 'recover' refuses;"
			say "verdict:      REBOOT with: sudo systemctl reboot --force"
		fi
	elif [ -n "$rc" ] && [ "$rc" -lt 0 ] 2>/dev/null; then
		say "verdict:      module is GOING (half-unloaded) -> REBOOT with: sudo systemctl reboot --force"
	else
		say "verdict:      no internal station -> run: $0 recover"
	fi
}

recover() {
	[ "$(id -u)" = 0 ] || die "needs root (sudo $0 $MODE)"
	local sta rc i
	sta=$(find_sta) || sta=""
	rc=$(refcount)

	# 0. refs -1 = the module is HALF-UNLOADED (GOING): nothing here can touch it.
	if [ -n "$rc" ] && [ "$rc" -lt 0 ] 2>/dev/null; then
		die "wlan.ko refcount is $rc (half-unloaded/GOING) — neither rmmod nor modprobe can reach it now.  REBOOT: 'sudo systemctl reboot --force' (a PLAIN reboot can hang on the stuck unit/task — measured 2026-10-21 — then it is a hard reset)"
	fi

	# 1. HEALTHY?  then do nothing at all (never churn NM for a working station).
	if [ -n "$sta" ] && [ "$FORCE" != 1 ] && can_scan "$sta"; then
		say "station $sta is healthy and scanning — nothing to do (--force reloads anyway)"
		return 0
	fi

	# 2. cheap, safe, non-module steps (a confused NM/radio is not a wedge).
	if ! nmcli radio wifi 2>/dev/null | grep -q enabled; then
		run nmcli radio wifi on && say "turned the wifi radio on"
	fi
	if [ -n "$sta" ]; then
		run nmcli dev set "$sta" managed yes 2>/dev/null || true
		run systemctl restart NetworkManager 2>/dev/null || true
		say "re-managed $sta and restarted NetworkManager (no module touched)"
	fi

	if [ -n "$sta" ] && [ "$FORCE" != 1 ]; then
		if can_scan "$sta"; then
			say "station $sta is healthy and scanning — nothing to do (--force reloads anyway)"
			return 0
		fi
		# WEDGED — and this is where the 2026-10-21 lesson lives: in this state
		# the driver's own exit path WAITS on the stuck cnss state machine, so
		# `rmmod wlan` HANGS in D-state right after "wlan: Hard Unloading driver"
		# and the module stays GOING (refs -1) until a reboot — and the hung unit
		# then makes a plain reboot hang too.  Never do that again.
		say "WEDGED: $sta is present but cannot scan — the crash left the driver/cnss state"
		say "WEDGED:   stuck, and in THIS state the driver's unload HANGS (measured:"
		say "WEDGED:   'Hard Unloading driver' then nothing; module left GOING, refs -1)."
		say "WEDGED:   So this script deliberately does NOT touch the module here."
		say "REBOOT: sudo systemctl reboot --force     (a PLAIN reboot can hang on the"
		say "REBOOT: stuck task/unit — if even --force hangs, hard reset with the keys)"
		say "PREVENT: tmp/suspend-psoc-idle-off.sh apply (gInterfaceChangeWait=0) removes"
		say "PREVENT: the crashing teardown; the suspend guard already stops the hang."
		return 3
	fi

	if [ -n "$sta" ] && [ "$FORCE" = 1 ]; then
		say "FORCE: about to unload a possibly-wedged driver — if this hangs, the module"
		say "FORCE: stays GOING (refs -1) and only a reboot (--force) recovers."
	fi

	# 2. nothing may race the unload: the discover sweeper and NM both poke the
	#    station, and a half-unloaded module is unrecoverable without a reboot.
	if systemctl is-active --quiet "$DISCOVER"; then
		if [ "$DRY" = 1 ]; then
			say "DRY would run: systemctl stop $DISCOVER"
		else
			systemctl stop "$DISCOVER" && say "stopped $DISCOVER (so nothing races the reload)"
		fi
	fi
	if [ -n "$sta" ]; then
		run nmcli dev disconnect "$sta" 2>/dev/null || true
		run nmcli dev set "$sta" managed no 2>/dev/null || true
		say "unmanaged $sta"
	fi

	# 3. unload — BOUNDED.  A hang here is the failure mode described above, so
	#    never wait forever: report the GOING state and tell the user to reboot.
	if [ -n "$rc" ]; then
		say "unloading wlan.ko (rmmod wlan, timeout ${UNLOAD_TIMEOUT:-25}s)..."
		if [ "$DRY" = 1 ]; then
			say "DRY would run: timeout ${UNLOAD_TIMEOUT:-25} rmmod wlan"
		else
			timeout "${UNLOAD_TIMEOUT:-25}" rmmod wlan; rmu=$?
			for i in $(seq 1 15); do
				[ -z "$(refcount)" ] && break
				sleep 1
			done
			rc=$(refcount)
			if [ -n "$rc" ]; then
				die "wlan.ko is stuck in the unload (rmmod rc=$rmu, refs now ${rc}) — the module is GOING and cannot be reloaded.  REBOOT: 'sudo systemctl reboot --force'; do NOT retry the reload in a loop"
			fi
			say "wlan.ko unloaded"
		fi
	fi

	# 3. load again — the driver's own init is what rebuilds the vdev state.
	say "loading wlan.ko (modprobe wlan)..."
	run modprobe wlan || true
	if [ "$DRY" != 1 ]; then
		sta=""
		for i in $(seq 1 "$SETTLE"); do
			sta=$(find_sta) && break
			sleep 1
		done
		[ -n "$sta" ] || die "no station netdev ${SETTLE}s after modprobe — the driver init failed (cnss2 state stuck): REBOOT"
		say "station back: $sta"
		run nmcli dev set "$sta" managed yes 2>/dev/null || true
		sleep 2
		if can_scan "$sta"; then
			say "SUCCESS: $sta scans again — NetworkManager will reconnect the last-used network"
		else
			die "$sta is back but cannot scan — REBOOT (firmware/cnss state still wedged)"
		fi
	fi

	if systemctl is-active --quiet "$DISCOVER"; then :; else
		if [ "$DRY" = 1 ]; then
			say "DRY would run: systemctl start $DISCOVER"
		else
			systemctl start "$DISCOVER" 2>/dev/null && say "restarted $DISCOVER"
		fi
	fi
}

case "$MODE" in
status) status ;;
recover) recover ;;
*) echo "usage: $0 [--force] status|recover   (DRY=1 for a dry run)" >&2; exit 2 ;;
esac

