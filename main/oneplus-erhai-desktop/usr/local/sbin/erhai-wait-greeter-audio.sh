#!/bin/sh
# Delay this session's PipeWire until the GDM greeter's audio stack is gone.
#
# GDM keeps the greeter running for a few seconds AFTER the user session
# starts, and the greeter's own PipeWire/WirePlumber still has the Qualcomm
# ADSP open.  If the new session's PipeWire opens the same ALSA device in that
# window, the DSP wedges: qcom-apm GPR command timeouts every 5 s, WirePlumber
# stuck in kernel D state and no audio for the whole session.
#
# WirePlumber starts right after pipewire.service, so gate pipewire.service on
# the greeter's WirePlumber disappearing.  Bounded to 15 s so a missing/odd
# greeter can never block audio.  Runs as the user; only /proc and loginctl
# are consulted, no root required.
set -u

me=$(id -u)

greeters=$(loginctl list-sessions --no-legend 2>/dev/null |
	awk '$3 == "gdm-greeter" { print $2 }' | sort -u)
[ -n "$greeters" ] || exit 0

i=0
while [ "$i" -lt 75 ]; do
	busy=0
	for p in $(pgrep -x wireplumber 2>/dev/null); do
		uid=$(stat -c %u "/proc/$p" 2>/dev/null) || continue
		[ "$uid" = "$me" ] && continue
		case " $greeters " in
			*" $uid "*) busy=1 ;;
		esac
	done
	[ "$busy" -eq 0 ] && exit 0
	sleep 0.2
	i=$((i + 1))
done
exit 0
