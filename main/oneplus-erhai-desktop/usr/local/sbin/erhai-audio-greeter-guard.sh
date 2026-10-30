#!/bin/sh
# Exit 1 (skip the unit) when this user manager belongs to the GDM greeter.
#
# The greeter does not need audio, but its PipeWire/WirePlumber grabs the
# Qualcomm ADSP right after logout and can wedge it (qcom-apm command
# timeouts, WirePlumber in D state).  The next session then starts without
# audio, and stopping the wedged greeter daemon blocks for ~23 s.
#
# The greeter is a dynamic user (no /etc/passwd entry), so resolve it through
# logind, which knows the session name.  A user unit cannot use ConditionUser
# reliably.
set -u

me=$(id -u)

if loginctl list-sessions --no-legend 2>/dev/null |
	awk -v me="$me" '$2 == me && $3 == "gdm-greeter" { found = 1 }
			 END { exit !found }'; then
	exit 1
fi
exit 0
