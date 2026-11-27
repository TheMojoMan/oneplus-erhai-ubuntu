#!/bin/bash
# erhai-wifi-rescan.sh - keep the radio scanning across NetworkManager's
# post-resume wake so NM's first full-band scan fails (-EBUSY) and NM falls
# back to the BSS cache the harness warms first (STEP5b).
#
# WHY: on ath12k a full-band scan costs ~8.3 s (measured 2026-11-27) and NM
# waits for its own scan before autoconnecting after a resume -- that scan is
# the whole remaining wifi delay. When a scan is already running, wpa_supplicant
# rejects NM's request immediately and NM auto-activates from the cached BSS
# list in ~0.5 s (measured 2026-11-27 18:17:27 on the old path).
#
# The harness starts this as an independent unit (`systemctl start --no-block`),
# because systemd-suspend.service exiting is what makes logind wake NM: a scan
# owned by that unit would be killed at exactly the wrong moment.
#
# SHAPE (learned the hard way): only SHORT directed scans are used. One full
# 8.3 s scan (tried in 2.39-test-13) blocks the scan engine, so if the cache
# was not warm yet NM has nothing to activate from until that scan finishes --
# the 19:38 resume took 10 s exactly that way. Short scans (~0.7 s) keep
# results flowing every scan and make the radio busy ~95 % of the time, so
# NM's request almost always collides while the cache is already populated.
set -u

RESCAN_SECS=${ERHAI_WIFI_RESCAN_SECS:-12}
CHANNELS="2412 2437 2462 5180 5200 5240 5745"

station() {
	local n t
	for n in /sys/class/net/*; do
		n=${n##*/}
		t=$(iw dev "$n" info 2>/dev/null | awk '/type/{print $2}')
		[ "$t" = managed ] && echo "$n"
	done
}

connected() {
	local n
	n=$(station | head -1)
	[ -n "$n" ] || return 1
	iw dev "$n" link 2>/dev/null | grep -q '^Connected'
}

end=$((SECONDS + RESCAN_SECS))
scans=0
connected && { echo "erhai-wifi-rescan: already connected, nothing to do"; exit 0; }

# Warm the BSS cache: repeated short directed scans until the station iface
# exists and a scan returns at least one BSS. Right after the resume the first
# scan can run before the firmware is ready and come back empty.
warm_tries=0
while [ "$warm_tries" -lt 5 ]; do
	warm_tries=$((warm_tries + 1))
	n=$(station | head -1)
	[ -n "$n" ] || { sleep 0.2; continue; }
	ip link set "$n" up 2>/dev/null || true
	iw dev "$n" scan freq $CHANNELS >/dev/null 2>&1
	scans=$((scans + 1))
	bss=$(iw dev "$n" scan dump 2>/dev/null | grep -c '^BSS')
	echo "erhai-wifi-rescan: warm attempt ${warm_tries}: bss=${bss}"
	[ "${bss:-0}" -gt 0 ] && break
	connected && break
	sleep 0.2
done
connected && { echo "erhai-wifi-rescan: connected during warm-up"; exit 0; }

# Hold the radio busy with back-to-back short scans until the station
# associates. `iw` is used for the state check too; an nmcli spawn every
# iteration would leave a 50-100 ms hole for NM's scan to slip through.
while [ "$SECONDS" -lt "$end" ]; do
	n=$(station | head -1)
	[ -n "$n" ] || break
	iw dev "$n" scan trigger freq $CHANNELS >/dev/null 2>&1
	scans=$((scans + 1))
	connected && break
	sleep 0.05
done
echo "erhai-wifi-rescan: done after ${scans} scan(s), $((SECONDS))s, connected=$(connected && echo yes || echo no)"
