#!/bin/sh
# Pick the desktop GL environment to match the kernel we actually booted.
#
# The public kernel ships the Adreno node disabled, the dev kernel enables it.
# A session that forces GALLIUM_DRIVER=freedreno against a disabled GPU dies with
# "get-param failed! -6" -> "Failed to setup: No GPUs found", and gdm bounces back
# to the greeter -- so the env has to follow the kernel, not the other way round.
#
# environment.d is read once when the user manager starts, so this has to run
# before the display manager does.
#
# The image may ship with no user account (gnome-initial-setup creates it on
# first boot), so this walks every existing home *and* /etc/skel: whoever is
# created later inherits the right template.

set -eu

. /usr/local/sbin/erhai-desktop-users.sh

status=$(cat /proc/device-tree/soc@0/gpu@*/status 2>/dev/null | tr -d '\0' || true)

case "$status" in
okay|ok) want_name=10-desktop.conf.gpu ;;
*)       want_name=10-desktop.conf.llvmpipe ;;
esac

apply() {
	# apply <home> [owner]
	home=$1
	owner=${2:-}
	envd="$home/.config/environment.d"
	want="$envd/$want_name"
	active="$envd/10-desktop.conf"

	[ -f "$want" ] || return 0
	if cmp -s "$want" "$active"; then
		return 0
	fi

	cp "$want" "$active"
	if [ -n "$owner" ]; then
		chown "$owner:$owner" "$active"
	fi
	echo "erhai-desktop-gl: gpu status '$status', installed $want_name for $home"
}

# /etc/skel first, so an account created after this boot starts out correct.
apply /etc/skel

found=0
for entry in $(erhai_desktop_users); do
	user=${entry%%:*}
	home=${entry##*:}
	found=1
	apply "$home" "$user"
done

[ "$found" = 1 ] || echo "erhai-desktop-gl: no desktop user yet, only /etc/skel updated"
exit 0
