#!/bin/sh
# Manual VCM focus for the SC1320CS rear camera.
# VCM = Qualcomm qcom,actuator on CCI0-m1, slave 0x0c, 10-bit DAC.
# Position = reg 0x03 (MSB[1:0]) : 0x04 (LSB). Unlock = 0xed/0x06/0x07/0x08.
# The AF/IO rails are only powered while the sensor streams, so this only works
# with a live camera preview open (else every write NAKs "No such device").
# Needs rw on the CCI adapter (udev uaccess grants the active session user).
# The two CCI masters enumerate in probe order (i2c-10 vs i2c-11 swaps between
# boots), so NEVER hardcode the bus -- resolve it from the sensor client.
dev=$(for d in /sys/bus/i2c/devices/*-0*; do
	[ "$(cat "$d/name" 2>/dev/null)" = sc1320cs ] && { basename "$d"; break; }
done)
[ -n "$dev" ] || { echo "erhai-cam-focus: sc1320cs not probed" >&2; exit 1; }
BUS=${dev%%-*}
A=0x0c

unlock() {
	for w in "0xed 0xab" "0x06 0x84" "0x07 0x01" "0x08 0x6f"; do
		i2ctransfer -f -y $BUS w2@$A $w >/dev/null || return 1
	done
}

setpos() {
	c="$1"
	hi=$(printf 0x%02x $(( (c >> 8) & 3 )))
	lo=$(printf 0x%02x $(( c & 0xff )))
	i2ctransfer -f -y $BUS w2@$A 0x03 $hi >/dev/null || return 1
	i2ctransfer -f -y $BUS w2@$A 0x04 $lo >/dev/null || return 1
}

case "$1" in
	unlock) unlock ;;
	pos)    setpos "$2" ;;
	sweep)  unlock || exit 1
		for c in 0 100 200 300 400 500 600 700 800 900 1000; do
			echo "code=$c"; setpos $c; sleep 2
		done ;;
	"")     echo "usage: erhai-cam-focus.sh <0-1023> | pos <n> | unlock | sweep"; exit 2 ;;
	*)      unlock || exit 1; setpos "$1" ;;
esac
