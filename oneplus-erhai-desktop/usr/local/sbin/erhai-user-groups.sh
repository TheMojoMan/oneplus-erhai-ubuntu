#!/bin/sh
# Put every desktop account in the hardware groups.
#
# Only used by images built with PRESET_USER=0: there is no account at build
# time, and the one gnome-initial-setup creates lands in `sudo` (admin) but not
# in audio/video/input/dialout/render. Most device access on a seat comes from
# logind ACLs, but not all of it — the pogo keyboard's serdev node and the
# thermal/charge sysfs helpers go through group membership.
#
# Idempotent, runs every boot, so an account created after the first boot is
# picked up on the next one.

set -eu

. /usr/local/sbin/erhai-desktop-users.sh

# erhai-torch: the rear-flash LED attributes are raw sysfs files chgrp'd to that
# group by 71-erhai-torch.rules.  An LED class device has no device node, so
# TAG+="uaccess" cannot hand them to the session user -- without membership the
# torch quick-slider only answers EACCES ("Log out and back in so the
# erhai-torch group applies.") and the LED is dead for that account.
# fastrpc: /dev/fastrpc-adsp + /dev/fastrpc-cdsp are root:fastrpc 0640, which is
# what the Hexagon/NPU clients (npu-llm, GenieX, fastrpc_test) need.
GROUPS_WANTED="audio video input dialout render i2c erhai-torch fastrpc"

for entry in $(erhai_desktop_users); do
	user=${entry%%:*}
	add=""
	for g in $GROUPS_WANTED; do
		getent group "$g" > /dev/null 2>&1 || continue
		if id -nG "$user" 2>/dev/null | tr ' ' '\n' | grep -qx "$g"; then
			continue        # already a member
		fi
		add="${add:+$add,}$g"
	done
	[ -n "$add" ] || continue
	usermod -aG "$add" "$user"
	echo "erhai-user-groups: $user += $add"
done
exit 0
