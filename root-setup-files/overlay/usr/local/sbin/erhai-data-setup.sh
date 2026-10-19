#!/bin/sh
# One-shot hand-over of the "data" partition (PARTLABEL=data, sda19) to the
# first desktop account.
#
# build-root.sh can bake an account into the image (PRESET_USER=1) or ship with
# none at all, in which case GDM stays at the greeter and gnome-initial-setup
# creates the user on first boot. This service waits until that has finished,
# then:
#
#   - checks /dev/disk/by-partlabel/data exists -- if not (installs may
#     deliberately ship without a data partition) it removes this service and
#     itself right away, permanently,
#   - creates the /media/data mount point,
#   - appends an fstab entry so the partition mounts on every boot,
#   - mounts it right away,
#   - chowns the FILESYSTEM ROOT to the new user,
#   - removes this service and itself.
#
# The wait runs in a transient worker unit (erhai-data-setup-worker) spawned
# with systemd-run: this oneshot exits immediately, so it never holds the boot
# transaction open while the wizard is up. If the wait or any step fails the
# service stays enabled and the whole thing is retried on the next boot.
#
# SAFETY: sda19 usually survives a reinstall (only the linux partition is
# reflashed) and still holds the previous install's data. This script NEVER
# formats, NEVER deletes and NEVER recurses into the data: it only chowns the
# top-level directory of the mounted filesystem. First accounts are uid 1000 on
# every erhai boot, so files from the previous install stay owned by — and
# accessible to — the new user.

set -eu

UNIT="erhai-data-setup.service"
SELF="/usr/local/sbin/erhai-data-setup.sh"
LOG="/var/log/erhai-data-setup.log"
DEV="/dev/disk/by-partlabel/data"
MNT="/media/data"
FSTAB="/etc/fstab"
WAIT_USER=7200	# give gnome-initial-setup up to 2 h per boot

# Success or permanent give-up: disable + delete the unit, delete this script.
remove_self() {
	systemctl disable "$UNIT" 2>/dev/null || true
	rm -f "/etc/systemd/system/$UNIT"
	rm -f "$SELF"
	systemctl daemon-reload || true
}

# First invocation (from the oneshot unit): detach into a transient worker so
# this unit finishes at once and the service cgroup is not killed around the
# wait loop.
if [ "${1:-}" != "worker" ]; then
	exec systemd-run --collect --unit=erhai-data-setup-worker \
		--description="erhai data partition hand-over" \
		"$SELF" worker
fi

exec >>"$LOG" 2>&1
echo "erhai-data-setup: worker started $(date -Is)"

# The data partition must exist. /dev/disk/by-partlabel/* is populated by the
# same udev coldplug pass as the root device, and this runs After=
# multi-user.target -- so by now it is either there or it does not exist at
# all (installs that deliberately ship without a data partition). No retrying:
# take the service out for good.
if [ ! -e "$DEV" ]; then
	echo "erhai-data-setup: $DEV does not exist (no data partition), removing the service"
	remove_self
	exit 0
fi

# Sanity guard: never point at the running root filesystem, whatever went
# wrong with partition labels.
rootsrc="$(findmnt -nro SOURCE / 2>/dev/null || true)"
if [ -n "$rootsrc" ] &&
	[ "$(readlink -f "$DEV")" = "$(readlink -f "$rootsrc")" ]; then
	echo "erhai-data-setup: $DEV resolves to the running root filesystem, refusing"
	exit 1
fi

# Desktop user. Same runtime resolution as every other erhai boot service (see
# erhai-desktop-users.sh, oneplus-erhai-desktop) -- nothing may bake a user in
# at build time. The fallback is a copy of that helper for robustness.
if [ -r /usr/local/sbin/erhai-desktop-users.sh ]; then
	. /usr/local/sbin/erhai-desktop-users.sh
fi
erhai_first_desktop_user() {
	if command -v erhai_desktop_users >/dev/null 2>&1; then
		erhai_desktop_users
	else
		getent passwd | awk -F: '
			$3 >= 1000 && $3 < 65534 &&
			$7 !~ /(nologin|false)$/ &&
			$6 ~ /^\/home\// { print $1 ":" $3 ":" $6 }'
	fi | head -n 1 | cut -d: -f1
}

# Wait for: a desktop account exists AND gnome-initial-setup has finished
# (it exits once the wizard is done; the copy-worker that briefly runs in the
# first session matches the same pattern and only costs a few seconds).
n=0
user=""
while :; do
	user="$(erhai_first_desktop_user)"
	if [ -n "$user" ] && ! pgrep -f '/gnome-initial-setup' >/dev/null 2>&1; then
		break
	fi
	[ "$n" -lt "$WAIT_USER" ] || {
		echo "erhai-data-setup: no desktop user after ${WAIT_USER}s" \
			"(gnome-initial-setup still running?) -- retrying next boot"
		exit 1
	}
	sleep 5
	n=$((n + 5))
done
echo "erhai-data-setup: desktop user is '$user'"

# Mount point + fstab. Idempotent: an existing fstab entry for the mount point
# is left alone, whatever it says.
if [ -e "$MNT" ] && [ ! -d "$MNT" ]; then
	echo "erhai-data-setup: $MNT exists and is not a directory, refusing"
	exit 1
fi
mkdir -p "$MNT"

entry="PARTLABEL=data $MNT ext4 defaults,nofail 0 2"
# No doubling: skip if fstab already references PARTLABEL=data (under ANY
# mount point) or already mounts $MNT (from ANY device).
if awk -v d="PARTLABEL=data" -v m="$MNT" '
	$1 !~ /^#/ && ($1 == d || $2 == m) { found = 1 }
	END { exit found ? 0 : 1 }' "$FSTAB"; then
	echo "erhai-data-setup: fstab already has an entry for PARTLABEL=data or $MNT, not adding another"
else
	# keep a missing trailing newline from gluing the entry onto the last line
	[ -n "$(tail -c 1 "$FSTAB")" ] && echo >> "$FSTAB"
	printf '%s\n' "$entry" >> "$FSTAB"
	echo "erhai-data-setup: fstab entry added: $entry"
fi

# Make it available right now, this boot.
systemctl daemon-reload || true
if ! mountpoint -q "$MNT"; then
	mntunit="$(systemd-escape --path --suffix=mount "$MNT")"
	systemctl start "$mntunit" 2>/dev/null || mount "$MNT" || {
		echo "erhai-data-setup: mounting $MNT failed -- keeping the service for the next boot"
		exit 1
	}
fi

# Hand-over: chown the top level of the mounted filesystem ONLY. No -R, no
# mkfs, no deletion -- existing content keeps its ownership (uid 1000 across
# installs), the new user gets read/write on the volume root.
uid="$(id -u "$user")"
gid="$(id -g "$user")"
chown "$uid:$gid" "$MNT"
echo "erhai-data-setup: $MNT handed over to $user ($uid:$gid)"

# Done: remove ourselves (the transient worker dies with this script).
remove_self
echo "erhai-data-setup: complete, service removed"
