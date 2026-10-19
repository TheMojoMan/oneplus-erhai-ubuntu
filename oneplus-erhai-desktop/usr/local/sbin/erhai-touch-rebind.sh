#!/bin/sh
# erhai: bring up the Novatek NT36536 (in-cell) SPI touch controller, wait until it
# is fully ready, THEN start the display manager.
#
# Why this shape:
#   * At cold boot the touch driver (NVT-ts-spi) probes while panel power/reset is
#     still settling, so the firmware download fails/retries and the input device
#     registers in a non-responsive state. Rebinding once the panel is up forces a
#     clean firmware reload.
#   * The panel is IN-CELL: the touch sensing layer only syncs to the display during
#     a display-manager bring-up. So gdm is DISABLED (systemctl disable gdm) and this
#     service starts it ONLY after the touch event device is confirmed ready -- making
#     the very first gdm bring-up happen post-touch, which is what actually makes
#     sensing work. A live rebind after the session is up does NOT help (libinput has
#     already grabbed the bad device).
#
# Set ERHAI_NO_GDM=1 to do everything except start gdm (for testing on a live system).
set -u

DRV=/sys/bus/spi/drivers/NVT-ts-spi
DEV=spi0.0
BUSDEV=/sys/bus/spi/devices/$DEV
TS_NAME=NVTCapacitiveTouchScreen
READY_RE='boot firmware ready'
MAX_ATTEMPTS=6
READY_WAIT=80          # per attempt: 80 * 0.1s = 8s to see "boot firmware ready"
EVENT_WAIT=50          # 50 * 0.1s = 5s for the event node to appear
SPI_WAIT=150           # 150 * 0.1s = 15s for the SPI device to enumerate

log() {
	echo "erhai-touch-rebind: $*"
	if [ -w /dev/kmsg ]; then echo "erhai-touch-rebind: $*" > /dev/kmsg; fi
}

start_gdm() {
	if [ "${ERHAI_NO_GDM:-0}" = 1 ]; then
		log "ERHAI_NO_GDM set -> NOT starting gdm"
		return 0
	fi
	log "starting gdm"
	systemctl --no-block start gdm.service
}

# Echo the /dev/input/eventN belonging to the touchscreen, or return non-zero.
ts_event_node() {
	for nm in /sys/class/input/input*/name; do
		[ -r "$nm" ] || continue
		[ "$(cat "$nm")" = "$TS_NAME" ] || continue
		inp=$(dirname "$nm")
		for ev in "$inp"/event*; do
			[ -e "$ev" ] || continue
			base=$(basename "$ev")
			if [ -e "/dev/input/$base" ]; then
				echo "/dev/input/$base"
				return 0
			fi
		done
	done
	return 1
}

# --- 1. wait for the SPI touch device to enumerate (kernel/driver init) ----------
i=0
while [ ! -e "$BUSDEV" ] && [ "$i" -lt "$SPI_WAIT" ]; do
	sleep 0.1; i=$((i + 1))
done
if [ ! -e "$BUSDEV" ]; then
	log "$BUSDEV never appeared; starting gdm without touch"
	start_gdm
	exit 0
fi

# --- 2. rebind until the firmware boots cleanly ---------------------------------
# The firmware boot is asynchronous (nvt_boot_fw_work_func), so after the bind write
# returns we poll dmesg -- scoped to lines after a unique /dev/kmsg marker -- for the
# "boot firmware ready" line. A bad load logs "Download Firmware failed" and never
# prints it, so we retry the rebind.
ready=0
attempt=0
while [ "$attempt" -lt "$MAX_ATTEMPTS" ]; do
	attempt=$((attempt + 1))
	MARK="erhai-touch-rebind-mark-$$-$attempt"
	if [ -w /dev/kmsg ]; then echo "$MARK" > /dev/kmsg; fi

	if [ -e "$DRV/$DEV" ]; then
		echo "$DEV" > "$DRV/unbind" || true
		sleep 0.5
	fi
	echo "$DEV" > "$DRV/bind" || true

	j=0
	while [ "$j" -lt "$READY_WAIT" ]; do
		if dmesg | awk -v m="$MARK" 'f; index($0, m) {f=1}' | grep -q "$READY_RE"; then
			ready=1
			break
		fi
		sleep 0.1; j=$((j + 1))
	done
	if [ "$ready" = 1 ]; then
		log "touch firmware ready on attempt $attempt"
		break
	fi
	log "attempt $attempt: firmware not ready, retrying"
done
if [ "$ready" != 1 ]; then
	log "touch never reported firmware-ready after $MAX_ATTEMPTS attempts; starting gdm anyway"
fi

# --- 3. wait until the touch input event node is mounted ------------------------
ev=""
k=0
while [ "$k" -lt "$EVENT_WAIT" ]; do
	ev=$(ts_event_node) && break
	sleep 0.1; k=$((k + 1))
done
if [ -n "$ev" ]; then
	log "touch event node ready: $ev"
else
	log "touch event node never appeared; starting gdm anyway"
fi

# --- 4. now bring up the display manager ----------------------------------------
start_gdm
exit 0
