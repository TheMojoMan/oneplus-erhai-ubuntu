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
# SEQUENCE: one short directed scan warms the cache (~0.7 s), then an ASYNC
# full-band scan is triggered and re-triggered until a station is connected.
# A full scan runs ~8.3 s, so it is still in flight when NM wakes (2-3 s after
# the resume) and its scan request is guaranteed to collide. The earlier
# synchronous directed loop left ~40 % idle gaps and NM won the race 2 of 4
# times (2026-11-27 19:28 / 19:33 resumes took the full 8.5 s).
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
	nmcli -t -f DEVICE,STATE device 2>/dev/null | grep -q ':connected$'
}

end=$((SECONDS + RESCAN_SECS))
scans=0
connected && { echo "erhai-wifi-rescan: already connected, nothing to do"; exit 0; }

# Warm the BSS cache: short, directed, synchronous. Its results are what NM
# activates from once its own scan is rejected.
for n in $(station); do
	ip link set "$n" up 2>/dev/null || true
	iw dev "$n" scan freq $CHANNELS >/dev/null 2>&1
	scans=$((scans + 1))
done
connected && { echo "erhai-wifi-rescan: connected after warm scan"; exit 0; }

# Hold the radio busy: trigger a full-band scan asynchronously and keep
# re-triggering (a trigger while one is running returns -EBUSY). The first one
# spans NM's wake; do not wait for its results.
while [ "$SECONDS" -lt "$end" ]; do
	found=0
	for n in $(station); do
		iw dev "$n" scan trigger >/dev/null 2>&1
		scans=$((scans + 1))
		found=1
	done
	[ "$found" = 1 ] || break
	connected && break
	sleep 0.2
done
echo "erhai-wifi-rescan: done after ${scans} scan(s)/trigger(s), $((SECONDS))s, connected=$(connected && echo yes || echo no)"
