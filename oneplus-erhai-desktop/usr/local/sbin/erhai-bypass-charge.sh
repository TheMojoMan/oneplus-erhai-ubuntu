#!/bin/bash
#
# erhai-bypass-charge — "bypass charging" for the OnePlus Pad 3.
#
# WHAT IT DOES.  Bypass charging stops the pack from charging while a charger
# is attached: the tablet runs from the brick, the battery just sits there
# (measured live 2026-09-30: pack current 3.93 A -> 0.117 A = 0.49 W, i.e.
# system-only, and back to 1.74 A the moment the switch goes off).  It is the
# laptop-style "powered by the adapter" mode: no heat and no wear from charging
# while you are plugged in for hours, and the pack is left at whatever level it
# was when you flipped the switch.
#
# HOW.  On erhai the charger is owned by the OnePlus ADSP charger firmware; the
# AP-side lever is the battery-psy "charging enable" property
# (BATT_CHG_EN = battery_property_id 24) written over pmic_glink:
#
#     echo "24 0 0x31" > /sys/kernel/debug/erhai_otg/cmd   # 0 = stop charging
#     echo "24 1 0x31" > /sys/kernel/debug/erhai_otg/cmd   # 1 = allow charging
#
# (0x31 = BC_BATTERY_PROPERTY_SET; the property ids/opcodes are the ADSP V2
# charger ABI, see drivers/power/supply/erhai_otg_glink.c).  The legacy
# USB_CHARGE_OUTPUT_SUSPEND (usb prop 19) is accepted but IGNORED by this
# firmware — that is the property that silently rotted the max_soc cap away
# (charging-MEMORY-2026-09-23.md), so never use it here.
#
# The inhibit is re-asserted on every poll: the ADSP clears it on its own
# charge events (attach, curve changes), and unlike the SoC cap a standing
# inhibit is exactly what a fresh attach must inherit.
#
# WHY NOT THE KERNEL max_soc PARAM.  erhai_otg_glink's max_soc limiter drives
# the SAME property, and its poller releases any standing inhibit as soon as
# the pack is below its cap ("limit disabled -> release any standing inhibit" /
# "soc <= max - hyst -> resume").  With the cap at its default 80 % and the
# pack at 40 % the kernel would release our bypass every 10 s while we
# re-assert it in the same tick — a charge/no-charge oscillation the pack would
# really notice.  So while bypass is on this helper PARKS the kernel limiter
# (max_soc = 100, its documented "limit disabled" value) and remembers the
# value it parked so `off` can restore it.  One charging policy at a time.
#
# WHY THE PPS STALL WATCHDOG IS PARKED TOO.  The inhibit is a SOFT TARGET: the
# ADSP tapers the charge current down rather than cutting it (measured
# 2026-09-30: a pure exponential, ratio 0.709 per 5 s => tau ~= 15 s, i.e. ~1 min
# from 4 A to ~0; the reverse edge resumes in 5-15 s).  Expect that ramp — no
# AP-side lever makes it instant (FCC/prop-10 clamping is inert, ICL lowering
# only makes the pack cover the load, the legacy prop 19 is ignored).  What CAN
# be fixed is the churn a HELD pack causes in erhai_pps: its pack-stall watchdog
# (`pps_stall_polls`, default 15 polls ~= 30 s of `ibat < 100 mA` at
# `VBUS > 7 V`) reads "held" as "stalled", exits the session, re-negotiates and
# ramps its input target 1250 -> 3000 mA — re-injecting charge current every
# 60-90 s, and every exit is an abort the ADSP's own charger_pd remembers
# (measured 2026-09-30: 7 exits in one session, after which the engine could no
# longer re-arm at all — stuck idle from 09:00, PPS not deployable, the system
# left on the 5 V fallback lane until a physical replug).  So while bypass holds
# this helper PARKS `pps_stall_polls` at 0 ("never") and restores the saved (or
# default) value on `off`: the engine stays in `charging`, keeps the ~8.4-9.4 V
# / 3 A contract and the pack just sits at 0.
#
# WHY THE SC8547A CHARGE PUMP IS PARKED TOO.  The ADSP charging-enable inhibit
# only reaches the ADSP's OWN charger.  A genuine SuperVOOC brick is charged by
# the SC8547A charge pump, and that pump is a DIRECT 2:1 path to the cell driven
# by THIS kernel (drivers/power/supply/sc8547a.c) — an inhibiting ADSP does not
# stop it.  Measured live 2026-10-03 on the owner's pad: with bypass ON the pack
# kept CHARGING, and faster (4.55 -> 4.72 A) because the CP simply expanded to
# carry the whole pack current once the ADSP path was held (CP bus jumped 1.1 ->
# 2.4 A).  Writing its `fastchg_deliver=0` (CP off) dropped the pack to ~0 A in
# seconds and it stayed held.  So while bypass holds we also park the CP master.
# The master's debugfs is the `*-006f` dir that carries `fastchg_state` (the
# slave dir has none) — never hardcode the i2c bus.  A PD/PPS-only source owns no
# SC8547A session, so the park is a clean no-op there (the ADSP inhibit already
# holds the pack on that path).
#
# Restoring the CP is NOT just the inverse write: a stopped CP lets the SVOOC
# adapter fall back to 5 V, and the ladder only re-identifies from a fresh PHY
# start, so `off` also restarts the VOOC PHY (`vooc` 0 -> 1) — proven live
# 2026-10-03, after which the pad returns to full dual-CP SuperVOOC.
#
# THE CHARGE LIMIT IS A BYPASS TOO.  erhai_otg_glink has an in-kernel charge
# limit (module parameter max_soc, default 80 %) that stops the ADSP's charger
# at the cap.  Stopping ALONE does not hold the pack on this hardware -- and
# that is the "charging limit is reached and then the pad runs by battery" the
# owner reported: the erhai_pps engine reads the deliberately-held pack as a
# *stall* and exits/re-negotiates (re-injecting charge current), and a genuine
# SuperVOOC brick keeps charging straight through the ADSP inhibit via the
# SC8547A charge pump.  So while the pack is AT the in-kernel limit the holder
# ALSO takes the two actions manual bypass takes and the kernel cannot -- it
# parks the PPS stall watchdog AND the SC8547A charge pump -- releasing both
# below the limit.  The result is the laptop-style "the pad runs from the
# adapter and the pack just sits" instead of a charge/no-charge sawtooth that
# ends with the pack on battery.
#
# The kernel limiter keeps owning the BATT_CHG_EN property itself, so there is
# still exactly ONE writer of it: the policy split is "the kernel says WHEN to
# stop", "this helper supplies the extra levers AT the limit".  The limit is
# read live from max_soc, so changing it (or disabling it with 0/100) needs no
# helper change, and if this service is ever not running the kernel limiter is
# still there as the last-resort fallback.  Set ERHAI_BYPASS_AUTO_LIMIT=0 to
# turn the augmentation off (then a limit only stops charging, as before).
#
# SAFETY FLOOR.  Bypass holds the pack wherever it is, so a source that cannot
# carry the load (a weak hub, a heavy compile) would slowly drain the tablet.
# Below ERHAI_BYPASS_MIN_SOC (10 %) the helper lets the pack charge again and
# only re-inhibits at ERHAI_BYPASS_RESTORE_SOC (15 %), so the switch can never
# strand a flat battery.
#
# Usage:
#   erhai-bypass-charge.sh on       enable bypass now AND persist the choice
#   erhai-bypass-charge.sh off      normal charging now AND persist the choice
#   erhai-bypass-charge.sh toggle   flip the persisted choice
#   erhai-bypass-charge.sh apply    re-apply the persisted choice (boot)
#   erhai-bypass-charge.sh run      hold the persisted state (the service)
#   erhai-bypass-charge.sh status   key=value report (also for diagnostics)
#
# The GNOME Quick Settings switch starts this through
# erhai-bypass-charge@<on|off>.service, which polkit lets the local active user
# start without a prompt (50-erhai-bypass-charge.rules);
# erhai-bypass-charge.service is the boot-time holder.  Both are just this
# script as root — no shell has to write sysfs itself.

set -u

KNOB=${ERHAI_BYPASS_KNOB:-/sys/kernel/debug/erhai_otg/cmd}
STATE=${ERHAI_BYPASS_STATE:-/etc/default/erhai-charge}
GAUGE=${ERHAI_BYPASS_GAUGE:-/sys/class/power_supply/bq27z561-0}
UCSI_GLOB=${ERHAI_BYPASS_UCSI_GLOB:-/sys/class/power_supply/ucsi-source-psy-*}
MAXSOC=${ERHAI_BYPASS_MAXSOC:-/sys/module/erhai_otg_glink/parameters/max_soc}
PPS_STALL=${ERHAI_BYPASS_PPS_STALL:-/sys/module/erhai_pps/parameters/pps_stall_polls}
SC8547_GLOB=${ERHAI_BYPASS_SC8547_GLOB:-/sys/kernel/debug/*-006f}
# Record of a REAL charge-limit CP park.  "fastchg_deliver reads 0" cannot tell a
# park WE made from the driver's own idle state (a PHY stop also leaves it 0),
# and confusing the two made the release path re-arm and bounce the VOOC PHY on
# every poll -- see limit_release.  Runtime state (/run), so a reboot clears it;
# a park stranded by an unclean exit is handed back by `apply`.
LIMIT_PARK_FLAG=${ERHAI_BYPASS_LIMIT_FLAG:-/run/erhai-charge-limit-parked}

POLL=${ERHAI_BYPASS_POLL:-10}		# re-assert period (s)
MIN_SOC=${ERHAI_BYPASS_MIN_SOC:-10}	# floor: allow charging below this
RESTORE_SOC=${ERHAI_BYPASS_RESTORE_SOC:-15}
HYST=${ERHAI_BYPASS_HYST:-3}		# release this many % below the charge limit
AUTO=${ERHAI_BYPASS_AUTO_LIMIT:-1}	# 1 = full bypass at the in-kernel limit
FALLBACK_MAXSOC=80			# erhai_otg_glink's own default
FALLBACK_STALL=15			# erhai_pps' own default (15 polls ~= 30 s)
P_CHGEN=24				# battery_property_id: charging enable
OP_BATT=0x31				# BC_BATTERY_PROPERTY_SET

log() { logger -t erhai-bypass-charge "$*" 2>/dev/null; printf '%s\n' "$*"; }

# ---- ADSP / sysfs access ----------------------------------------------------

# Send one property SET to the ADSP.  Returns non-zero when the write is
# refused (knob missing, no permission, ADSP service down => -ENODEV), which is
# what the caller reports to the UI instead of pretending the switch worked.
adsp_set() {	# property value opcode
	printf '%s %s %s\n' "$1" "$2" "$3" > "$KNOB" 2>/dev/null
}

hold_charging() { adsp_set "$P_CHGEN" 0 "$OP_BATT"; }
allow_charging() { adsp_set "$P_CHGEN" 1 "$OP_BATT"; }

gauge_soc() {
	local soc
	soc=$(cat "$GAUGE/capacity" 2>/dev/null) || return 1
	case "$soc" in ''|*[!0-9]*) return 1 ;; esac
	printf '%s\n' "$soc"
}

# Is a USB-C source (charger / powered hub) attached, as the ADSP sees it?
adapter_online() {
	local f
	for f in $UCSI_GLOB/online; do
		[ -r "$f" ] || continue
		[ "$(cat "$f" 2>/dev/null)" = "1" ] && return 0
	done
	return 1
}

# Park / restore the in-kernel SoC limiter (see the header).  A kernel without
# the parameter (or without write permission) is not an error: the helper then
# simply owns the property alone.
maxsoc_value() { cat "$MAXSOC" 2>/dev/null; }
maxsoc_parked() { [ -w "$MAXSOC" ] || return 0; printf '100\n' > "$MAXSOC" 2>/dev/null; }
maxsoc_restore() { # value
	[ -w "$MAXSOC" ] || return 0
	printf '%s\n' "$1" > "$MAXSOC" 2>/dev/null
}

# Park / restore erhai_pps' pack-stall watchdog while we own the pack (see the
# header).  Same best-effort contract as max_soc: a kernel without the parameter
# or without write permission is not an error.
stall_value()   { cat "$PPS_STALL" 2>/dev/null; }
stall_parked()  { [ -w "$PPS_STALL" ] || return 0; printf '0\n' > "$PPS_STALL" 2>/dev/null; }
stall_restore() { # value
	[ -w "$PPS_STALL" ] || return 0
	printf '%s\n' "$1" > "$PPS_STALL" 2>/dev/null
}

# Park / restore the SC8547A SuperVOOC charge pump while we own the pack (see the
# header).  The master debugfs dir is the `*-006f` sibling that carries
# `fastchg_state`; a source without a SuperVOOC session has none, which is a
# clean no-op.  Same best-effort contract as max_soc: missing/unwritable knobs
# are not an error.
sc8547_dir() {
	local d
	for d in $SC8547_GLOB; do
		[ -e "$d/fastchg_state" ] && { printf '%s' "$d"; return 0; }
	done
	return 1
}
cp_deliver_value() { # the live CP delivery flag ("" when no SC8547A / unreadable)
	local d; d=$(sc8547_dir) || return 0
	cat "$d/fastchg_deliver" 2>/dev/null
}
cp_park() {
	local d; d=$(sc8547_dir) || return 0
	[ -w "$d/fastchg_deliver" ] || return 0
	# Remember what we are about to park, but never re-save a repeated `on`.
	if [ -z "$SAVED_CP" ]; then
		SAVED_CP=$(cat "$d/fastchg_deliver" 2>/dev/null)
		SAVED_CP_AUTO=$(cat "$d/fastchg_auto" 2>/dev/null)
		case "$SAVED_CP" in ''|*[!0-9]*) SAVED_CP=1 ;; esac
		case "$SAVED_CP_AUTO" in ''|*[!0-9]*) SAVED_CP_AUTO=1 ;; esac
	fi
	printf '0\n' > "$d/fastchg_deliver" 2>/dev/null
	[ -w "$d/fastchg_auto" ] && printf '0\n' > "$d/fastchg_auto" 2>/dev/null
	return 0
}
cp_restore() { # [re-arm the VOOC PHY: 1 default, 0 = don't]
	local d; d=$(sc8547_dir) || return 0
	[ -w "$d/fastchg_deliver" ] || return 0
	[ -w "$d/fastchg_auto" ] && \
		printf '%s\n' "${SAVED_CP_AUTO:-1}" > "$d/fastchg_auto" 2>/dev/null
	printf '%s\n' "${SAVED_CP:-1}" > "$d/fastchg_deliver" 2>/dev/null
	# A stopped CP collapsed the SVOOC contract to 5 V: restart the PHY so the
	# ladder re-identifies (the adapter only ramps from a fresh PHY start).
	if [ "${1:-1}" = 1 ] && [ -w "$d/vooc" ]; then
		printf '0\n' > "$d/vooc" 2>/dev/null
		sleep 1
		printf '1\n' > "$d/vooc" 2>/dev/null
	fi
	return 0
}

# ---- the charge limit (see the header) --------------------------------------

# The in-kernel charge limit (erhai_otg_glink's max_soc): a real limit is
# 1..99; 0/100 means "charge to full".  Printed on stdout when there is one.
chg_limit() {
	local v
	v=$(cat "$MAXSOC" 2>/dev/null) || return 1
	case "$v" in ''|*[!0-9]*) return 1 ;; esac
	[ "$v" -ge 1 ] && [ "$v" -lt 100 ] || return 1
	printf '%s\n' "$v"
	return 0
}

# Park the two levers the kernel limiter cannot reach while the pack sits at the
# charge limit: the PPS stall watchdog (a held pack is not a stall) and the
# SC8547A charge pump (a direct 2:1 path the ADSP inhibit cannot close).  This
# is the same pair manual bypass parks, and it is what turns "stopped charging,
# then the pack ran down" into "tablet on adapter power, pack held".  The CP
# knobs are written directly (NOT via cp_park) so this never touches the manual
# branch's SAVED_CP/SAVED_CP_AUTO; the writes stay idempotent (only while
# something is on), so a fresh attach that re-arms the pump is re-parked.  Every
# REAL park is recorded in $LIMIT_PARK_FLAG so the release path can tell it from
# the driver's own idle state (see limit_release).
limit_hold() {
	local d
	[ -n "$(stall_value)" ] && [ "$(stall_value)" != 0 ] && stall_parked
	d=$(sc8547_dir) || return 0
	[ -w "$d/fastchg_deliver" ] || return 0
	# Record the park BEFORE writing it, so a park can never exist without its
	# record (the record is what limit_release acts on).
	: > "$LIMIT_PARK_FLAG" 2>/dev/null
	if [ "$(cat "$d/fastchg_deliver" 2>/dev/null)" != 0 ] ||
	   [ "$(cat "$d/fastchg_auto" 2>/dev/null)" != 0 ]; then
		printf '0\n' > "$d/fastchg_deliver" 2>/dev/null
		[ -w "$d/fastchg_auto" ] && printf '0\n' > "$d/fastchg_auto" 2>/dev/null
	fi
}

# Give those two levers back (the release edge, and stale-park hygiene while no
# limit is in force).  A stopped CP dropped the adapter to 5 V, so restart the
# VOOC PHY for the ladder to re-identify; the stalls go back to the saved/default
# value.
#
# ONLY acts when the limit hold really parked the pump ($LIMIT_PARK_FLAG).  The
# CP being off is the NORMAL idle state of a pad on no (S)VOOC brick, and it is
# also exactly what a PHY stop leaves behind -- so releasing on "deliver reads 0"
# made EVERY poll re-arm the pump and bounce the VOOC PHY, and a PHY stop flips
# the USB D+/D- crossbar (sc8547_vooc_stop ->
# wcd939x_usbss_dpdm_switch_fast_charge(false)): every attached USB device was
# dropped and re-enumerated once per POLL+1 s, forever (measured 2026-10-18: a
# Kingston DataTraveler cycled every 11 s with no charger even attached).
limit_release() {
	local d
	[ "$(stall_value)" = 0 ] && stall_restore "${SAVED_STALL:-$FALLBACK_STALL}"
	[ -e "$LIMIT_PARK_FLAG" ] || return 0
	d=$(sc8547_dir) || return 0
	[ -w "$d/fastchg_deliver" ] || return 0
	printf '1\n' > "$d/fastchg_deliver" 2>/dev/null
	[ -w "$d/fastchg_auto" ] && printf '1\n' > "$d/fastchg_auto" 2>/dev/null
	if [ -w "$d/vooc" ]; then
		printf '0\n' > "$d/vooc" 2>/dev/null
		sleep 1
		printf '1\n' > "$d/vooc" 2>/dev/null
	fi
	rm -f "$LIMIT_PARK_FLAG" 2>/dev/null
}

# ---- persisted state --------------------------------------------------------

# /etc/default/erhai-charge, one key per line:
#   ERHAI_BYPASS_CHARGE=0|1     the user's choice
#   ERHAI_MAX_SOC_SAVED=<n>     the max_soc the helper parked (1..99)
#   ERHAI_PPS_STALL_SAVED=<n>   the pps_stall_polls the helper parked (>=1)
#   ERHAI_SC8547_SAVED=<n>      the sc8547a fastchg_deliver the helper parked
#   ERHAI_SC8547_AUTO_SAVED=<n> the sc8547a fastchg_auto the helper parked
BYPASS=0
SAVED=
SAVED_STALL=
SAVED_CP=
SAVED_CP_AUTO=

read_state() {
	BYPASS=$(sed -n 's/^ERHAI_BYPASS_CHARGE=//p' "$STATE" 2>/dev/null | tail -1)
	SAVED=$(sed -n 's/^ERHAI_MAX_SOC_SAVED=//p' "$STATE" 2>/dev/null | tail -1)
	SAVED_STALL=$(sed -n 's/^ERHAI_PPS_STALL_SAVED=//p' "$STATE" 2>/dev/null | tail -1)
	SAVED_CP=$(sed -n 's/^ERHAI_SC8547_SAVED=//p' "$STATE" 2>/dev/null | tail -1)
	SAVED_CP_AUTO=$(sed -n 's/^ERHAI_SC8547_AUTO_SAVED=//p' "$STATE" 2>/dev/null | tail -1)
	case "$BYPASS" in 0|1) ;; *) BYPASS=0 ;; esac
	case "$SAVED" in ''|*[!0-9]*) SAVED="" ;; esac
	case "$SAVED_STALL" in ''|*[!0-9]*) SAVED_STALL="" ;; esac
	case "$SAVED_CP" in ''|*[!0-9]*) SAVED_CP="" ;; esac
	case "$SAVED_CP_AUTO" in ''|*[!0-9]*) SAVED_CP_AUTO="" ;; esac
}

write_state() {	# bypass [saved_maxsoc] [saved_stall] [saved_cp] [saved_cp_auto]
	local tmp="$STATE.tmp.$$"
	{
		printf '# erhai bypass charging; set with: erhai-bypass-charge.sh on|off\n'
		printf 'ERHAI_BYPASS_CHARGE=%s\n' "$1"
		if [ -n "${2:-}" ]; then
			printf 'ERHAI_MAX_SOC_SAVED=%s\n' "$2"
		fi
		if [ -n "${3:-}" ]; then
			printf 'ERHAI_PPS_STALL_SAVED=%s\n' "$3"
		fi
		if [ -n "${4:-}" ]; then
			printf 'ERHAI_SC8547_SAVED=%s\n' "$4"
		fi
		if [ -n "${5:-}" ]; then
			printf 'ERHAI_SC8547_AUTO_SAVED=%s\n' "$5"
		fi
	} > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 1; }
	chmod 644 "$tmp" 2>/dev/null
	mv -f "$tmp" "$STATE" || { rm -f "$tmp"; return 1; }
}

# ---- the two edges ----------------------------------------------------------

# Enable: hold the pack now, remember the choice, park the kernel limiter and the
# PPS stall watchdog.
apply_on() {
	read_state
	local saved="$SAVED" saved_stall="$SAVED_STALL" cur

	# Remember the limiter value we are about to park, but never trip over a
	# repeated `on`: 100 is the parked value itself and must not be saved.
	if [ -z "$saved" ]; then
		cur=$(maxsoc_value)
		case "$cur" in ''|*[!0-9]*) cur="" ;; esac
		if [ -n "$cur" ] && [ "$cur" -ge 1 ] && [ "$cur" -lt 100 ]; then
			saved="$cur"
		fi
	fi

	# Same for the PPS stall watchdog: 0 is the value we park (and means
	# "never exit"), so only a real count is worth restoring later.
	if [ -z "$saved_stall" ]; then
		cur=$(stall_value)
		case "$cur" in ''|*[!0-9]*) cur="" ;; esac
		if [ -n "$cur" ] && [ "$cur" -ge 1 ]; then
			saved_stall="$cur"
		fi
	fi

	if ! hold_charging; then
		log "bypass ON FAILED: the ADSP refused BATT_CHG_EN=0 (knob $KNOB)"
		return 1
	fi

	maxsoc_parked
	stall_parked
	cp_park
	write_state 1 "$saved" "$saved_stall" "$SAVED_CP" "$SAVED_CP_AUTO" || true

	if adapter_online; then
		log "bypass ON: charging inhibited, tablet on adapter power (soc $(gauge_soc)%, max_soc parked${saved:+ from $saved}${saved_stall:+, pps stall watchdog parked from $saved_stall}${SAVED_CP:+, SC8547A CP parked from $SAVED_CP})"
	else
		log "bypass ON: charging inhibited; takes effect when a charger is attached (soc $(gauge_soc)%)"
	fi
	return 0
}

# Disable: allow charging now, drop the choice, give the limiter, the watchdog and
# the SC8547A charge pump back.
apply_off() {
	read_state
	local saved="${SAVED:-$FALLBACK_MAXSOC}"
	local saved_stall="${SAVED_STALL:-$FALLBACK_STALL}"

	allow_charging || log "bypass OFF: the ADSP refused BATT_CHG_EN=1 (knob $KNOB)"
	# Give the charge pump back BEFORE dropping the state so the saved values
	# are still available (the file is then rewritten without them).
	cp_restore
	write_state 0 "" "" "" "" || true
	maxsoc_restore "$saved"
	# Only restore a watchdog we (or a stale state file) parked, so an operator's
	# own value is never clobbered by a plain `off`.
	[ "$(stall_value)" = 0 ] && stall_restore "$saved_stall"
	log "bypass OFF: charging allowed (max_soc=$saved, pps stall watchdog=$saved_stall, SC8547A CP restored, soc $(gauge_soc)%)"
	return 0
}

# ---- the holder loop --------------------------------------------------------

# Hold the persisted state: re-assert the inhibit every poll and apply the
# low-SoC floor.  This is the long-running service.  `on`/`off` apply their edge
# immediately, so the switch never waits for a tick — this loop only keeps the
# state from rotting when the ADSP clears it on its own charge events.
run() {
	log "holder started (poll ${POLL}s, floor ${MIN_SOC}%, restore ${RESTORE_SOC}%, limit-bypass ${AUTO})"
	local last="" soc low limit

	while :; do
		read_state

		if [ "$BYPASS" != 1 ]; then
			# A manual-bypass park must not outlive the manual bypass: if
			# max_soc is still 100 while a saved value exists, give the
			# limiter back its number, else the charge limit is silently
			# disabled and nothing below can work.
			if [ -n "$SAVED" ] && [ "$(maxsoc_value)" = 100 ]; then
				maxsoc_restore "$SAVED"
			fi

			limit=$(chg_limit) || limit=""

			# CHARGE LIMIT -> full bypass (see the header).  The kernel
			# limiter owns BATT_CHG_EN; while the pack sits at the cap we
			# supply the two levers it cannot reach (the PPS stall watchdog
			# and the SC8547A charge pump), so the pad runs from the adapter
			# instead of the pack running down.  Release below the cap.
			if [ "$AUTO" = 1 ] && [ -n "$limit" ]; then
				soc=$(gauge_soc) || { sleep "$POLL"; continue; }

				if [ "$soc" -ge "$limit" ]; then
					if [ "$last" != lim-hold ]; then
						log "charge limit reached (soc ${soc}% >= ${limit}%): bypassing - pack held, tablet on adapter"
						last=lim-hold
					fi
					limit_hold
				elif [ "$soc" -le "$((limit - HYST))" ]; then
					if [ "$last" = lim-hold ]; then
						log "charge limit released (soc ${soc}% <= $((limit - HYST))%): charging resumed"
					fi
					last=lim-rel
					limit_release
				elif [ "$last" = lim-hold ]; then
					limit_hold	# hysteresis band: hold what we had
				else
					limit_release
				fi

				sleep "$POLL"
				continue
			fi

			# No limit (or the augmentation is off): plain charging, and
			# hand back anything a previous session left parked.
			if [ "$last" != off ]; then
				allow_charging && last=off
				limit_release
			fi

			sleep "$POLL"
			continue
		fi

		# ---- manual bypass (the Quick Settings switch) ---------------------

		# The kernel limiter must stay parked while we own the property: if
		# something restores max_soc (a module reload, a manual write), the
		# kernel would fight this loop over the same BATT_CHG_EN property.
		if [ -n "$(maxsoc_value)" ] && [ "$(maxsoc_value)" != 100 ]; then
			maxsoc_parked
		fi

		# And the PPS stall watchdog must stay parked for the same reason: with
		# the pack deliberately held, "no current at high VBUS" is not a stall.
		if [ -n "$(stall_value)" ] && [ "$(stall_value)" != 0 ]; then
			stall_parked
		fi

		# The SC8547A charge pump is a SECOND, AP-driven owner of the pack and
		# the ADSP inhibit cannot reach it: keep it parked while bypass holds
		# (the driver re-arms on a fresh attach, which would resume real
		# charging).  No-op without a SuperVOOC session.
		if [ -n "$(cp_deliver_value)" ] && [ "$(cp_deliver_value)" != 0 ]; then
			cp_park
		fi

		soc=$(gauge_soc) || { sleep "$POLL"; continue; }

		if [ "$soc" -lt "$MIN_SOC" ]; then
			low=1
		elif [ "$soc" -ge "$RESTORE_SOC" ]; then
			low=0
		fi

		if [ "$low" = 1 ]; then
			if [ "$last" != floor ]; then
				log "bypass ON but soc ${soc}% < ${MIN_SOC}%: charging allowed until ${RESTORE_SOC}%"
				last=floor
			fi
			allow_charging
		else
			if [ "$last" != hold ]; then
				log "bypass ON: charging inhibited (soc ${soc}%)"
				last=hold
			fi
			hold_charging || sleep 2
		fi

		sleep "$POLL"
	done
}

# ---- report / dispatch ------------------------------------------------------

status() {
	read_state
	local online=no soc
	adapter_online && online=yes
	soc=$(gauge_soc) || soc=?
	printf 'bypass=%s\n' "$([ "$BYPASS" = 1 ] && echo on || echo off)"
	printf 'adapter=%s\n' "$online"
	printf 'soc=%s\n' "${soc}"
	printf 'pack_current_ma=%s\n' "$(cat "$GAUGE/current_now" 2>/dev/null || echo '?')"
	printf 'pack_status=%s\n' "$(cat "$GAUGE/status" 2>/dev/null || echo '?')"
	printf 'max_soc=%s\n' "$(maxsoc_value)"
	printf 'max_soc_saved=%s\n' "${SAVED:-none}"
	printf 'charge_limit=%s\n' "$(chg_limit || echo none)"
	printf 'limit_bypass=%s\n' "$([ "$AUTO" = 1 ] && echo on || echo off)"
	printf 'pps_stall_polls=%s\n' "$(stall_value)"
	printf 'pps_stall_saved=%s\n' "${SAVED_STALL:-none}"
	printf 'sc8547_dir=%s\n' "$(sc8547_dir || echo none)"
	printf 'sc8547_deliver=%s\n' "$(cp_deliver_value)"
	printf 'sc8547_saved=%s\n' "${SAVED_CP:-none}"
	printf 'knob_writable=%s\n' "$([ -w "$KNOB" ] && echo yes || echo no)"
}

case "${1:-status}" in
on) apply_on ;;
off) apply_off ;;
toggle)
	read_state
	if [ "$BYPASS" = 1 ]; then apply_off; else apply_on; fi
	;;
apply)
	read_state
	if [ "$BYPASS" = 1 ]; then
		apply_on
	else
		# Boot with the choice "off": do NOT write the property.  The ADSP
		# already allows charging, and a spurious BATT_CHG_EN=1 would clear an
		# inhibit the KERNEL limiter legitimately holds at its cap.
		maxsoc_restore "${SAVED:-$FALLBACK_MAXSOC}"
		if [ "$(stall_value)" = 0 ]; then
			stall_restore "${SAVED_STALL:-$FALLBACK_STALL}"
		fi
		# Undo a stale CP park from a session that ended uncleanly.  A
		# stopped pump is the signal (there is no attribution to make here:
		# this runs once at start, not in the poll loop), and the limit-park
		# flag goes with it.
		if [ -e "$LIMIT_PARK_FLAG" ] || [ "$(cp_deliver_value)" = 0 ]; then
			cp_restore
			rm -f "$LIMIT_PARK_FLAG" 2>/dev/null
		fi
		log "bypass OFF (persisted): charging allowed"
	fi
	;;
run) run ;;
status) status ;;
limit)
	# Show or set the in-kernel charge limit (erhai_otg_glink's max_soc).
	# With no argument it prints the limit now in force.  A runtime write is
	# a MODULE PARAMETER, so it does NOT survive a reboot (the module is
	# built in): make it permanent with erhai_otg_glink.max_soc=<n> on the
	# kernel command line, or re-apply it after each boot.
	if [ -z "${2:-}" ]; then
		chg_limit || echo none
		exit 0
	fi
	case "$2" in
	*[!0-9]*|'') echo "usage: $0 limit <1..99|0|100>" >&2; exit 1 ;;
	esac
	if [ "$2" -lt 0 ] || [ "$2" -gt 100 ]; then
		echo "usage: $0 limit <1..99|0|100>" >&2; exit 1
	fi
	printf '%s\n' "$2" > "$MAXSOC" || {
		echo "cannot write $MAXSOC (need root)" >&2; exit 1; }
	if [ "$2" = 0 ] || [ "$2" = 100 ]; then
		echo "charge limit disabled (charge to full)"
	else
		echo "charge limit set to $2% (bypass engages at $2%)"
	fi
	;;
*)
	echo "usage: $0 {on|off|toggle|apply|run|status|limit [n]}" >&2
	exit 1
	;;
esac
