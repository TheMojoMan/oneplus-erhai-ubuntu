#!/bin/sh
# erhai-cam-arm.sh — arm CCI CSR + both camera sensors at boot.
#
# Cold boot leaves erhai_cci_csr=0 (CSR bring-up DEFERRED) so the rear sensor
# never enters the media graph. Setting csr=8 and rebinding the CCI driver runs
# CSR bring-up and re-probes sc1320cs on the CCI i2c bus.
#
# The front sensor sc820cs needs the same treatment but is NOT on the CCI bus --
# it hangs off GENI i2c9 -- so the CCI rebind does not touch it. Its boot probe
# runs before the CSR is armed, the MCLK4 branch is still gated, and it dies:
#
#   cam_bist_mclk_cc_mclk4_clk status stuck at 'off'
#   sc820cs 6-0010: failed to enable mclk: -16
#
# That failure is not just a missing front camera. camss has an async connection
# per sensor port (csiphy4 -> sc1320cs, csiphy5 -> sc820cs), and its notifier
# only completes once *both* are bound. Until then camss_subdev_notifier_complete
# never runs, so no sensor-to-csiphy links are created and libcamera reports
# "No sensor found for /dev/media0" -- even though the rear sensor bound fine and
# is sitting in the graph with zero links. So bind sc820cs here too.
P=/sys/module/i2c_qcom_cci/parameters/erhai_cci_csr
DRV=/sys/bus/platform/drivers/i2c-qcom-cci
DEV=ac7b000.cci
POW=/sys/bus/platform/devices/$DEV/power/control

# Wait until the CCI driver is loaded + bound (modules come up async at boot).
i=0
while [ $i -lt 60 ]; do
	[ -e "$P" ] && [ -e "$DRV/$DEV" ] && break
	i=$((i + 1)); sleep 1
done
[ -e "$P" ] || { echo "erhai-cam-arm: cci param never appeared"; exit 0; }

echo 8 > "$P"
# Rebind so probe re-runs with CSR bring-up (it deferred at boot with csr=0).
echo "$DEV" > "$DRV/unbind" 2>/dev/null
sleep 1
echo "$DEV" > "$DRV/bind"
sleep 2
[ -e "$POW" ] && echo on > "$POW" 2>/dev/null

# Front sensor: bind whichever i2c device the sc820cs driver owns, by name, so
# this does not hard-code a bus number that can move.
SDRV=/sys/bus/i2c/drivers/sc820cs
if [ -d "$SDRV" ]; then
	i=0
	while [ $i -lt 20 ]; do
		for d in /sys/bus/i2c/devices/*; do
			[ -e "$d/name" ] || continue
			[ "$(cat "$d/name")" = "sc820cs" ] || continue
			id=$(basename "$d")
			[ -e "$SDRV/$id" ] && break 2      # already bound
			echo "$id" > "$SDRV/bind" 2>/dev/null
			[ -e "$SDRV/$id" ] && break 2
		done
		i=$((i + 1)); sleep 1
	done
fi
sleep 2

# Read the ring buffer once. Piping it into `grep -q` twice makes grep exit on
# the first match and dmesg die of SIGPIPE, which systemd logs as
# "dmesg: write error" and looks like a failure.
dm=$(dmesg)
rear=no; front=no
case "$dm" in *"SC1320CS detected"*) rear=yes ;; esac
case "$dm" in *"SC820CS detected"*) front=yes ;; esac
echo "erhai-cam-arm: rear(sc1320cs)=$rear front(sc820cs)=$front"
if [ "$rear" = no ] || [ "$front" = no ]; then
	echo "erhai-cam-arm: WARNING a sensor is missing -- camss will not link either" \
	     "of them, and libcamera will list no camera"
fi

# If a desktop session is already up, nudge pipewire so its libcamera plugin
# re-enumerates the now-armed cameras. If not up yet, pipewire will enumerate
# them at login and this is skipped. The user is resolved here rather than baked
# in, because the image may ship with no account at all (gnome-initial-setup
# creates one on first boot).
for rt in /run/user/*; do
	[ -d "$rt" ] || continue
	uid=${rt##*/}
	[ "$uid" -ge 1000 ] 2>/dev/null || continue
	user=$(getent passwd "$uid" | cut -d: -f1)
	[ -n "$user" ] || continue
	# su+systemctl --user fails here (Operation not permitted); -M works.
	systemctl --user -M "$user@.host" restart wireplumber pipewire pipewire-pulse 2>/dev/null || pkill -KILL -u "$user" -x wireplumber 2>/dev/null
done
exit 0
