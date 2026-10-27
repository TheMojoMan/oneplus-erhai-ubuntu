#!/bin/sh
# erhai-host-charge — charge the OnePlus Pad 3 from a powered USB-C source (a
# powered hub/dock, or a charger) while it is acting as the USB *data host*.
#
# On erhai, USB charging is owned by the OnePlus ADSP charger firmware; the HLOS
# UCSI/typec layer is a thin shell (no real PD source-capabilities are exposed).
# When the tablet is a USB host (plugged into a powered hub/dock), no BC1.2
# charger detection runs, so the ADSP parks the input current limit (ICL) at the
# 100mA SDP default and the battery effectively does not charge — even though
# VBUS (~5V) is present from the source. Nothing is *blocking* charge
# (USB_SUSPEND_PMIC=0); the ICL is just pinned low.
#
# This service raises the ADSP ICL (USB_INPUT_CURR_LIMIT, V2 usb_property_id #5)
# via the erhai_otg_glink debugfs knob whenever we are a data-host with USB-in
# present. The current then tracks the ICL 1:1 up to whatever the source can
# deliver at 5V (verified ceilings: ~1.3A from a powered hub, ~3.1A from a
# SuperVOOC brick forced to fixed-5V/3A; VBUS stays healthy, no collapse).
#
# RAMP MODE (default): instead of a fixed ICL, BINARY-SEARCH the highest ICL the
# source sustains. Probe START_MA (3400) first; if it holds, use it (we're at or
# above the source ceiling). If it breaks (VBUS collapse / charger offline), the
# search halves downward — the first midpoint of [0,START] is START/2 (= "if it
# breaks, halve it"), then it refines (a break lowers the top bound, a hold
# raises the floor) to the highest value that holds. This auto-discovers the right
# limit for *any* source (weak hub or strong brick) in only ~log2(START/GRAN)
# probes — much faster than a +100mA linear ramp.
#
# That settled value is the input-current CEILING — a MAXIMUM, not a target. The
# ADSP charger's own CC/CV loop draws *up to* it and tapers below it on its own as
# the battery fills, so in steady state we do NOT re-pin the ICL every poll (that
# would glue the input current to the ceiling = "forcing"). We only (a) back OFF by
# half if VBUS later collapses under load, or (b) re-assert the ceiling if the ADSP
# has reset the ICL out from under us (readback << ceiling while still online).
# Set RAMP=0 to fall back to the old fixed ICL_UA write.
#
# It deliberately does NOTHING in device mode (data_role=device): there the
# ADSP's own BC1.2 detection already sets a correct ICL for a directly-attached
# charger, so we must not clobber a possibly-higher limit. (For the offline,
# directly-attached-charger case use charging/quickcharge_ramp.sh, which also
# force-sinks the port out of OTG first.)
#
# Requires erhai_otg_glink.bind=1 (the debugfs knob's owner). Safe no-op if the
# knob is absent (e.g. when the power-supply slot is yielded to qcom_battmgr).

set -u

KNOB=/sys/kernel/debug/erhai_otg/cmd
DATA_ROLE=/sys/class/typec/port0/data_role
SET_OP=0x33			# BC_USB_PROPERTY_SET
GET_OP=0x32			# BC_USB_PROPERTY_GET

# V2 usb_property_id (oplus_hal_adsp.h), 0-based
P_ONLINE=0; P_VOLT=1; P_CURR=3; P_ICL=5

# ---- tunables (env) -------------------------------------------------------------
RAMP="${RAMP:-1}"		# 1 = binary-search the max sustainable ICL; 0 = fixed ICL_UA
ICL_UA="${ICL_UA:-1500000}"	# fixed ICL (µA) used only when RAMP=0
POLL="${POLL:-30}"		# hold/re-assert period (s)

START_MA="${START_MA:-3400}"	# top of the search = first probe (& hard ceiling)
MIN_MA="${MIN_MA:-200}"		# floor; below this the source can't usefully charge
GRAN_MA="${GRAN_MA:-100}"	# stop searching once the [lo,hi] interval <= this
SETTLE_S="${SETTLE_S:-2}"	# wait after setting an ICL before measuring
GET_WAIT="${GET_WAIT:-0.4}"	# wait for the async ADSP GET reply to reach dmesg

# breakdown thresholds (validated on the SuperVOOC brick; a gradual IR droop is
# NOT breakdown — 0.5->3.1A sags VBUS ~0.3V over ~0.12Ω cable, well within SAG_MV)
VBUS_FLOOR_MV="${VBUS_FLOOR_MV:-3800}"	# absolute VBUS floor => collapse
SAG_MV="${SAG_MV:-700}"			# VBUS drop below running-peak => collapse
IBUS_COLLAPSE_MA="${IBUS_COLLAPSE_MA:-60}"  # ~0 input after offline => collapse

# ---- helpers --------------------------------------------------------------------
log() { echo "host-charge: $*"; }

set_prop() { echo "$1 $2 $SET_OP" > "$KNOB"; }
set_icl()  { set_prop "$P_ICL" "$(( $1 * 1000 ))"; }	# arg in mA

# GET a USB property -> echo raw value (micro-units / flag); empty on miss.
# The knob's GET is async: the driver logs "resp: op 0x32 property N value V ..."
adsp_get() {
	echo "$1 0 $GET_OP" > "$KNOB" || true
	sleep "$GET_WAIT"
	dmesg | grep -E "resp: op 0x32 property $1 value -?[0-9]+" | tail -1 \
		| grep -oE "value -?[0-9]+" | tail -1 | awk '{print $2}'
}

is_online() {
	for f in /sys/class/power_supply/ucsi-source-psy-*/online; do
		[ -r "$f" ] && [ "$(cat "$f")" = 1 ] && return 0
	done
	return 1
}

# one measurement -> globals VBUS_MV IBUS_MA ONLINE
measure() {
	v=$(adsp_get "$P_VOLT"); i=$(adsp_get "$P_CURR"); o=$(adsp_get "$P_ONLINE")
	[ -z "$v" ] && v=0; [ -z "$i" ] && i=0; [ -z "$o" ] && o=-1
	VBUS_MV=$(( v / 1000 )); IBUS_MA=$(( i / 1000 )); ONLINE=$o
}

# broken? uses current VBUS_MV/IBUS_MA/ONLINE + running VBUS_PEAK; sets BREAK_REASON
is_broken() {
	if [ "$ONLINE" = "0" ] && [ "$IBUS_MA" -lt "$IBUS_COLLAPSE_MA" ]; then
		BREAK_REASON="offline(${IBUS_MA}mA)"; return 0
	fi
	if [ "$VBUS_MV" -lt "$VBUS_FLOOR_MV" ]; then
		BREAK_REASON="vbus<floor(${VBUS_MV}mV)"; return 0
	fi
	if [ "$VBUS_MV" -lt "$(( VBUS_PEAK - SAG_MV ))" ]; then
		BREAK_REASON="vbus-sag(${VBUS_MV}<${VBUS_PEAK}-${SAG_MV})"; return 0
	fi
	return 1
}

# probe an ICL (mA): set it, settle, measure, update peak -> 0 holds, 1 broke
probe_icl() {
	set_icl "$1" || { BREAK_REASON="adsp-reject"; return 1; }
	sleep "$SETTLE_S"; measure
	[ "$VBUS_MV" -gt "$VBUS_PEAK" ] && VBUS_PEAK="$VBUS_MV"
	if is_broken; then return 1; fi
	return 0
}

# binary-search the highest sustainable ICL -> sets BEST_MA (0 = none found)
binary_search() {
	BEST_MA=0
	hi="$START_MA"; lo=0
	log "ramp: probing ceiling ${hi}mA (VBUS peak ${VBUS_PEAK}mV)"
	if probe_icl "$hi"; then
		BEST_MA="$hi"
		log "ramp: ${hi}mA holds (VBUS=${VBUS_MV}mV ibus=${IBUS_MA}mA) -> at/above source ceiling"
		return 0
	fi
	log "ramp: ${hi}mA broke (${BREAK_REASON}); halving downward"
	while [ $(( hi - lo )) -gt "$GRAN_MA" ]; do
		mid=$(( (lo + hi) / 2 ))
		if probe_icl "$mid"; then
			BEST_MA="$mid"; lo="$mid"
			log "ramp: ${mid}mA holds (VBUS=${VBUS_MV}mV ibus=${IBUS_MA}mA) best=${BEST_MA}"
		else
			hi="$mid"
			log "ramp: ${mid}mA broke (${BREAK_REASON})"
		fi
	done
	return 0
}

# ---- main loop ------------------------------------------------------------------
log "starting: RAMP=$RAMP START=${START_MA}mA min=${MIN_MA}mA gran=${GRAN_MA}mA poll=${POLL}s (fixed ICL_UA=${ICL_UA} when RAMP=0)"
RAMPED=0
APPLIED=0
VBUS_PEAK=0

while :; do
	if [ -w "$KNOB" ] && grep -q '\[host\]' "$DATA_ROLE" && is_online; then
		measure
		if [ "$VBUS_MV" = 0 ]; then
			# ADSP charger domain reports 0 VBUS while a source is online:
			# a DP alt-mode session is routed and the charger path is blind.
			# Probing ICL into a blind charger domain made the ADSP drop
			# the whole USB session (20:26:43: 8 probes in 23 s -> detach
			# notify 10 s in). Stand down; re-discover when VBUS is sane.
			if [ "$RAMPED" != 0 ]; then
				log "vbus reads 0 while online -> standing down (DP session routed?)"
				RAMPED=0
			fi
		elif [ "$RAMP" != "1" ]; then
			# legacy: fixed input current limit
			set_icl "$(( ICL_UA / 1000 ))" || true
		elif [ "$RAMPED" = "0" ]; then
			# new source attached -> discover its sustainable ICL once
			VBUS_PEAK=0
			set_icl "$MIN_MA" || true; sleep "$SETTLE_S"; measure
			VBUS_PEAK="$VBUS_MV"
			binary_search
			if [ "$BEST_MA" -ge "$MIN_MA" ]; then
				APPLIED="$BEST_MA"
			else
				log "ramp: no sustainable ICL >= ${MIN_MA}mA (source too weak); pinning ${MIN_MA}mA"
				APPLIED="$MIN_MA"
			fi
			set_icl "$APPLIED" || true
			RAMPED=1
			log "ramp: settled at ${APPLIED}mA"
		else
			# hold: APPLIED is the input-current CEILING (<= START_MA), a MAXIMUM
			# the charger draws up to and tapers below near full. Do NOT re-pin it
			# every poll (that would force the current to the ceiling). Just watch:
			#  (a) VBUS collapsed under load -> back off by half;
			#  (b) the ADSP reset the ICL well below our ceiling while still online
			#      -> restore the ceiling (otherwise leave the charger alone so it
			#      can taper).
			measure
			if is_broken; then
				half=$(( APPLIED / 2 ))
				log "hold: collapse at ${APPLIED}mA ceiling (${BREAK_REASON}) -> halving to ${half}mA"
				APPLIED="$half"
				if [ "$APPLIED" -lt "$MIN_MA" ]; then
					APPLIED="$MIN_MA"; RAMPED=0	# re-discover from scratch next poll
				fi
				set_icl "$APPLIED" || true
			else
				icl_now=$(adsp_get "$P_ICL"); [ -z "$icl_now" ] && icl_now=0
				if [ "$icl_now" -lt "$(( APPLIED * 1000 - 200000 ))" ]; then
					log "hold: ADSP reset ICL to $(( icl_now / 1000 ))mA -> restoring ${APPLIED}mA ceiling"
					set_icl "$APPLIED" || true
				fi
			fi
		fi
	else
		# source gone or not data-host -> forget, re-discover on next attach
		RAMPED=0
	fi
	sleep "$POLL"
done
