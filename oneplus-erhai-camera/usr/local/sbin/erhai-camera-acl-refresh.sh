#!/bin/sh
# Re-apply uaccess ACLs to camera devices once a user session exists.
#
# Race: erhai-cam-arm.service rebinds CCI/camss early at boot, which recreates
# /dev/media*, /dev/video*, /dev/v4l-subdev* and /dev/i2c-* BEFORE any user
# session is active. The udev uaccess builtin therefore grants no ACL, and
# systemd-logind does not revisit devices that appeared after it started --
# so wireplumber/libcamera get EACCES ("No camera found") even though the
# sensors are armed.
#
# uaccess only evaluates on ADD uevents (a 'change' trigger is ignored), so
# once the first desktop session exists we replay add-events for those
# subsystems; logind's active-session user then gets rw ACLs everywhere.
log() { echo "erhai-cam-acl: $*" > /dev/kmsg 2>/dev/null; }

# Wait for an ACTIVE seat0 session. The user manager's bus appears BEFORE
# logind marks the session Active, and uaccess only grants for ACTIVE
# sessions -- triggering earlier silently does nothing.
active_session() {
	local line sid
	while read -r line; do
		sid=${line%% *}
		[ -n "$sid" ] || continue
		case "$line" in (*seat0*) ;; (*) continue ;; esac
		[ "$(loginctl show-session "$sid" -p Active 2>/dev/null)" = "Active=yes" ] && return 0
	done <<EOF
$(loginctl list-sessions --no-legend 2>/dev/null)
EOF
	return 1
}

i=0
while [ $i -lt 180 ]; do
	if active_session; then
		ready=1
		break
	fi
	sleep 2
	i=$((i + 1))
done

if [ "${ready:-0}" != 1 ]; then
	log "no active seat0 session within timeout, nothing to refresh"
	exit 0
fi

log "active session present, replaying add uevents"
udevadm trigger --action=add \
	--subsystem-match=media \
	--subsystem-match=video4linux \
	--subsystem-match=i2c-dev 2>/dev/null
log "uaccess ACLs refreshed"

# libcamera's udev enumerator does NOT retry after an early EACCES -- if
# wireplumber started before the ACLs existed it stays camera-less forever.
# Bounce the session's pipewire/wireplumber so they re-enumerate with access.
for rt in /run/user/*; do
	uid=${rt##*/}
	case "$uid" in (*[!0-9]*|'') continue ;; esac
	[ "$uid" -ge 1000 ] 2>/dev/null || continue
	[ -e "$rt/bus" ] || continue
	systemctl --user -M "$uid@.host" try-restart wireplumber pipewire >/dev/null 2>&1 || true
	log "restarted pipewire/wireplumber for uid=$uid"
done

# The pipewire bounce KILLS xdg-desktop-portal's own pipewire connection
# ("Caught PipeWire error: connection error") and the portal never recovers --
# every app that asks it for the camera then gets nothing ("No camera found").
# Restart the portals AFTER pipewire/wireplumber are back so they reconnect.
sleep 2
for rt in /run/user/*; do
	uid=${rt##*/}
	case "$uid" in (*[!0-9]*|'') continue ;; esac
	[ "$uid" -ge 1000 ] 2>/dev/null || continue
	[ -e "$rt/bus" ] || continue
	systemctl --user -M "$uid@.host" try-restart \
		xdg-desktop-portal xdg-desktop-portal-gnome xdg-desktop-portal-gtk \
		>/dev/null 2>&1 || true
	log "restarted xdg-desktop-portal for uid=$uid"
done
exit 0
