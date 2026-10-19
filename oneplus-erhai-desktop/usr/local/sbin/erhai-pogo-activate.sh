#!/bin/sh
# Root helper: activate the OnePlus Pad 3 pogo keyboard.
# Re-binds the serdev driver, which re-runs probe and forces a clean gpio85
# power-cycle so the keyboard emits its power-up sync and starts streaming.
# Prints "OK" if the keyboard started streaming, "NODEV" otherwise.
DRV=/sys/bus/serial/drivers/oneplus-pogo-kbd
DEV=serial1-0

[ -d "$DRV" ] || { echo "NODRV"; exit 1; }

# Unbind if currently bound (forces a fresh probe on re-bind)
if [ -e "$DRV/$DEV" ]; then
	echo "$DEV" > "$DRV/unbind" 2>/dev/null
	sleep 1
fi
echo "$DEV" > "$DRV/bind" 2>/dev/null

# Wait for the power-up sync (driver power-cycles gpio85 then the keyboard syncs)
sleep 5

now=$(cut -d. -f1 /proc/uptime)
last=$(dmesg | grep "power-up sync" | tail -1 | sed -E 's/^\[ *([0-9]+).*/\1/')
if [ -n "$last" ] && [ $((now - last)) -le 8 ]; then
	echo OK
else
	echo NODEV
fi
exit 0
