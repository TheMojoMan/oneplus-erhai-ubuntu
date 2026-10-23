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
	command -v loginctl >/dev/null 2>&1 || return 0
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

# "name:uid:gid" for a user NAME or a numeric UID, empty when there is no such
# HUMAN account (uid 1000..65533 — the same bound erhai_desktop_users() uses, so
# a system account is never picked).
erhai_pw_fmt() {
	[ -n "${1:-}" ] || return 0
	getent passwd "$1" 2>/dev/null |
		awk -F: 'NR == 1 && $3 >= 1000 && $3 < 65534 { print $1 ":" $3 ":" $4 }'
}

# The user the DESKTOP SESSION belongs to, as "name:uid:gid".
#
# WHY it is resolved and never baked in: this package runs on other people's
# pads (where the user is not called themojoman) and a boot unit may run before
# the account even EXISTS (gnome-initial-setup creates it on first boot).  A
# name that is not there makes `id -u` return nothing, so /run/user/<empty>
# becomes a wrong XDG_RUNTIME_DIR and the display steps that address the session
# (gnome-shell SetActive in erhai-suspend-s2idle.sh / erhai-panel-power.sh)
# cannot reach it — the "session username hardcoded" bug reported against 2.21
# (fixed in 2.23).
#
# Order:
#   1. $ERHAI_SESSION_USER / $ERHAI_USER — explicit override (must resolve)
#   2. the user of the ACTIVE graphical logind session
#   3. uid 1000 — the first human account by construction
#   4. the first human account (erhai_desktop_users, the rule at the top)
# The optional 1st argument `cheap` swaps 2 and 3: the per-press panel path pays
# ONE getent lookup instead of a loginctl round trip when the usual answer
# (uid 1000) is the right one.
#
# Empty output + non-zero = nothing found; callers keep their numeric fallback.
erhai_session_user() {
	local cheap=${1:-} want line

	# 1) explicit override(s)
	for want in "${ERHAI_SESSION_USER:-}" "${ERHAI_USER:-}"; do
		[ -n "$want" ] || continue
		line=$(erhai_pw_fmt "$want")
		[ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
	done

	# 3) uid 1000 first when the caller is latency-sensitive
	if [ "$cheap" = cheap ]; then
		line=$(erhai_pw_fmt 1000)
		[ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
	fi

	# 2) the active graphical session
	want=$(erhai_active_user 2>/dev/null | head -1)
	if [ -n "$want" ]; then
		line=$(erhai_pw_fmt "${want%%:*}")
		[ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
	fi

	# 3) uid 1000 (when 2 ran first)
	if [ "$cheap" != cheap ]; then
		line=$(erhai_pw_fmt 1000)
		[ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
	fi

	# 4) any human account at all
	want=$(erhai_desktop_users 2>/dev/null | head -1)
	if [ -n "$want" ]; then
		line=$(erhai_pw_fmt "${want%%:*}")
		[ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
	fi

	return 1
}
