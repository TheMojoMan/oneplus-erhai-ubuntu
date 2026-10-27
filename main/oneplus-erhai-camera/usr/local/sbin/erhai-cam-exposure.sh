#!/bin/sh
# Manual exposure for the SC1320CS rear camera.
# Sensor = on the CCI bus, slave 0x36. Integration-time register 0x3e00 is a
# 3-byte big-endian burst (auto-increment 0x3e00/01/02) holding lines << 5
# (decoded from an Android AE rwmmio trace; each frame ran VTS - 5 lines).
#
# The kernel s_ctrl no-ops V4L2_CID_EXPOSURE (libcamera's soft-ISP AE is thereby
# defeated — it writes the control but the driver ignores it), so this direct
# write is the *sole* owner of the exposure register and sticks frame to frame.
# The sensor rails are only powered while it streams, so this only does anything
# with a live preview (Snapshot) open — otherwise the write NAKs.
#
# Frame length VTS is fixed at 6404 lines; exposure must stay under it, so the
# range is 0 .. VTS-8. Near the top the raw can saturate and the soft-ISP may
# invert ("negative") preview frames — that's the ceiling, back off if seen.
# Needs rw on the CCI adapter (udev uaccess grants the active session user).
# The two CCI masters enumerate in probe order (i2c-10 vs i2c-11 swaps between
# boots), so NEVER hardcode the bus -- resolve it from the sensor client.
dev=$(for d in /sys/bus/i2c/devices/*-0*; do
	[ "$(cat "$d/name" 2>/dev/null)" = sc1320cs ] && { basename "$d"; break; }
done)
[ -n "$dev" ] || { echo "erhai-cam-exposure: sc1320cs not probed" >&2; exit 1; }
BUS=${dev%%-*}
A=0x36
LINES_MIN=0
LINES_MAX=6396		# VTS(6404) - 8; exposure must stay under the frame length

setexp() {
	l="$1"
	[ "$l" -lt "$LINES_MIN" ] && l=$LINES_MIN
	[ "$l" -gt "$LINES_MAX" ] && l=$LINES_MAX
	v=$(( l << 5 ))
	b0=$(printf 0x%02x $(( (v >> 16) & 0xff )))
	b1=$(printf 0x%02x $(( (v >> 8)  & 0xff )))
	b2=$(printf 0x%02x $((  v         & 0xff )))
	i2ctransfer -f -y $BUS w5@$A 0x3e 0x00 $b0 $b1 $b2 >/dev/null || return 1
}

case "$1" in
	"")  echo "usage: erhai-cam-exposure.sh <lines $LINES_MIN-$LINES_MAX>"; exit 2 ;;
	pos) setexp "$2" ;;
	*)   setexp "$1" ;;
esac
