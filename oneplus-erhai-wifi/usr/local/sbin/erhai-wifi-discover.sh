#!/bin/bash
# erhai-wifi-discover.sh — PNO-style discovery sweeper for the WCN7880 station.
#
# WHY THIS EXISTS (see ubuntu-erhai-wifi.txt "UNII-1 scan quirk"):
# In DISCONNECTED full-band scans the qcacld firmware never sends probe
# requests on UNII-1 (chans 36-48) once the 5GHz channel list grows past ~5
# channels — those channels are effectively scanned passively only, so an AP
# on ch36 is discovered by beacon luck alone. Small channel lists DO actively
# probe UNII-1 and always find such APs. Connected scans also work.
# Android on this same device discovers these APs because its supplicant uses
# PNO (prefered-network-offload) directed scans instead of plain full scans.
#
# This sweeper emulates that: while the internal station is disconnected,
# fire a SMALL-list active scan (<=4 5GHz channels + three common 2.4GHz
# channels) every SCAN_INTERVAL seconds. The results land in wpa_supplicant's
# BSS table, from where NetworkManager autoconnects saved networks as usual.
#
# Runs as a simple systemd service started after erhai-wcn-wifi.service.
# The station interface disappearing (module unload/reload cycle, driver
# crash) is NOT fatal: we wait and resume sweeping once it reappears, so
# discovery survives erhai-wcn-wifi.sh re-runs without a service restart.

SCAN_INTERVAL=${ERHAI_WIFI_DISCOVER_INTERVAL:-30}
WAIT_INTERVAL=${ERHAI_WIFI_DISCOVER_WAIT:-10}

find_sta() {
	nmcli -t -f DEVICE,TYPE dev status 2>/dev/null |
	awk -F: '$2=="wifi" && $1!~/^(wlan0|p2p)/ {print $1; exit}'
	return 0
}

sta_connected() {
	# NM GENERAL.STATE codes: 100=connected(assembled/activated), 50=activating
	local st
	st=$(nmcli -t -g GENERAL.STATE device show "$1" 2>/dev/null)
	case "${st%% *}" in
		100|50) return 0 ;;
	esac
	return 1
}

# Find saved NM connections whose SSID matches a FRESHLY-seen (<8s) 5GHz BSS
# from our small-list scan, not pinned to a DIFFERENT interface, and activate
# it. NM does not reliably autoconnect off externally-triggered scans.
try_connect() {
	local sta="$1"
	local bsses ssids cname cssid cifn fr
	# fresh 5GHz frequencies seen in this sweep.
	# NOTE two "last seen" formats from iw: "<N> ms ago" = AGE, and
	# "<X>s [boottime]" = ABSOLUTE boot time (compare against /proc/uptime).
	local up
	up=$(cut -d' ' -f1 /proc/uptime)
	bsses=$(iw dev "$sta" scan dump 2>/dev/null | awk -v up="$up" '
		/^BSS / { b=$2 }
		/last seen: .* ago/ { gsub(/[^0-9]/,"",$3); age=$3 }
		/last seen: .*boottime/ { age=(up-$3)*1000 }
		/freq: / { if (b!="" && age>=0 && age<8000 && $2 ~ /^5/) printf "%d\n", $2 }
	' | sort -u)
	[ -n "$bsses" ] || return 0
	local names cname cssid cifn
	names=$(nmcli -t -f NAME con show 2>/dev/null)
	while IFS= read -r cname; do
		[ -n "$cname" ] || continue
		cssid=$(nmcli -t -g 802-11-wireless.ssid con show "$cname" 2>/dev/null)
		[ -n "$cssid" ] || continue
		cifn=$(nmcli -t -g connection.interface-name con show "$cname" 2>/dev/null)
		case "$cifn" in ""|"$sta") ;; *) continue ;; esac
		for fr in $bsses; do
			if iw dev "$sta" scan dump 2>/dev/null |
			   awk -v fr="$fr" -v s="$cssid" '
				/^[[:space:]]*freq: / && ($NF + 0) == fr { hit = 20 }
				hit > 0 { hit--; if (/^[[:space:]]*SSID: / && substr($0, index($0, ":") + 2) == s) found = 1 }
				END { exit !found }'; then
				echo "erhai-discover: activating '$cname' on $sta (fresh ${fr}MHz hit)"
				if nmcli con up "$cname" ifname "$sta" >/dev/null 2>&1; then
					echo "erhai-discover: connected"
					return 0
				fi
			fi
		done
	done < <(printf '%s\n' "$names")
	return 0
}

while true; do
	STA=$(find_sta)
	if [ -z "$STA" ]; then
		# station gone (module unloaded / reloading): wait, don't die
		echo "erhai-discover: no station iface, waiting ${WAIT_INTERVAL}s"
		sleep "$WAIT_INTERVAL"
		continue
	fi

	if ! sta_connected "$STA"; then
		# Small list => firmware probes actively on UNII-1.
		if iw dev "$STA" scan trigger freq 2412 2437 2462 5180 5200 >/dev/null 2>&1; then
			sleep 3
			try_connect "$STA"
		fi
	fi

	sleep "$SCAN_INTERVAL"
done
