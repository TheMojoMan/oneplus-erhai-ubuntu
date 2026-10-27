#!/bin/bash
# erhai-wifi-profile-normalize.sh — strip interface-name pins from Wi-Fi profiles.
#
# WHY: NetworkManager GUI-created profiles may pin connection.interface-name
# to whatever name the wifi interface had at creation time. On this device
# qcacld first registers as 'wlan0' and udev renames it to 'wlp1s0' seconds
# later. A profile created in that window is pinned to 'wlan0' and then NEVER
# matches again -> autoconnect and manual connect fail with
# "profile is not compatible with device (mismatching interface name)",
# and the only workaround is deleting and re-creating the profile.
#
# Clearing the pin makes the profile match ANY wifi device. Run once per boot
# after the interface is up (hooked into erhai-wifi-discover.service).

set -u

names=$(nmcli -t -f NAME,TYPE connection show 2>/dev/null |
	grep ':802-11-wireless$' | cut -d: -f1)
[ -n "$names" ] || exit 0

while IFS= read -r name; do
	pin=$(nmcli -g connection.interface-name connection show "$name" 2>/dev/null)
	if [ -n "$pin" ]; then
		nmcli connection modify "$name" connection.interface-name "" &&
			logger -t erhai-wifi-profile "cleared interface-name pin '$pin' on '$name'"
	fi
done <<< "$names"
exit 0
