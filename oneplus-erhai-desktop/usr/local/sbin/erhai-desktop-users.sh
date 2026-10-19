#!/bin/sh
# List the desktop (human) accounts on this system, one "user:uid:home" per line.
#
# The image can be built with a ready-made user (PRESET_USER=1) or with none at
# all, in which case gnome-initial-setup creates the account on first boot. Boot
# services therefore may not have a user name baked in — they source this and
# ask at the moment they run.
#
# Sourced, not executed:  . /usr/local/sbin/erhai-desktop-users.sh

erhai_desktop_users() {
	getent passwd | awk -F: '
		$3 >= 1000 && $3 < 65534 &&
		$7 !~ /(nologin|false)$/ &&
		$6 ~ /^\/home\// { print $1 ":" $3 ":" $6 }'
}

# The user of the currently active graphical session, if there is one. Empty
# otherwise — at boot there usually is not.
erhai_active_user() {
	[ -x /usr/bin/loginctl ] || return 0
	loginctl list-sessions --no-legend 2>/dev/null | awk '{ print $1 }' |
	while read -r s; do
		[ -n "$s" ] || continue
		state=$(loginctl show-session "$s" -p Active --value 2>/dev/null)
		[ "$state" = "yes" ] || continue
		u=$(loginctl show-session "$s" -p Name --value 2>/dev/null)
		id=$(loginctl show-session "$s" -p User --value 2>/dev/null)
		[ -n "$u" ] && [ "${id:-0}" -ge 1000 ] 2>/dev/null && echo "$u:$id"
		break
	done
}
