#!/bin/sh
# Boot-time watcher: persist any WCN7880 firmware devcoredump + the crash dmesg
# to /root so a cold-boot-cal RDDM dump survives the next power cycle.
# Installed as a systemd service (erhai-wcn-devcd.service), runs once per boot.
OUT=/root/wcn-devcd
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo unknown)
# Poll for the first 200s of the boot (cold-boot cal RDDMs at ~13s; give margin).
i=0
while [ "$i" -lt 400 ]; do
	for d in /sys/class/devcoredump/devcd*; do
		[ -e "$d/data" ] || continue
		# Found a fresh coredump: copy it out before the 5-min devcd timeout.
		cp "$d/data" "$OUT/devcd-$STAMP.bin"
		dmesg | grep -iE 'ath12k|mhi|rddm|qmi|request memory|mem seg|host_cap|ddr_range|coredump|smmu|iova' > "$OUT/dmesg-$STAMP.txt"
		sync
		# Mark the marker file so the post-boot SSH session knows it fired.
		echo "$OUT/devcd-$STAMP.bin" > "$OUT/LATEST"
		exit 0
	done
	i=$((i + 1))
	sleep 0.1
done
exit 0
