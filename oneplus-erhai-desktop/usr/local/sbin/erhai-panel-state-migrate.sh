#!/bin/sh
# One-shot migration to the SINGLE blank state (2.20).
#
# Up to 2.19 each blank owner had its own marker file (/run/erhai-panel-off for
# the power button, /run/erhai-folio-fold-blanked for the folio) and every
# consumer had to test both.  Since 2.20 there is ONE owner-tagged file,
# <run>/erhai-panel.state, one line "<owner> <epoch>" — see the note in
# erhai-panel-power.sh (the format is pinned by tmp/folio-test/panel-state-test.sh).
#
# Run from the postinst: an upgrade in a live session must not lose a blank that
# is in force RIGHT NOW, so the old marker is translated before it is removed.
# Idempotent, and /run is volatile so a fresh boot has nothing to migrate.
#
# ERHAI_RUN_DIR overrides the directory (the test uses a temp dir).
RUN=${ERHAI_RUN_DIR:-/run}
STATE="$RUN/erhai-panel.state"
OLD_POWER="$RUN/erhai-panel-off"
OLD_FOLIO="$RUN/erhai-folio-fold-blanked"

if [ ! -e "$STATE" ]; then
	if [ -e "$OLD_FOLIO" ]; then
		printf 'folio %s\n' "$(date +%s)" > "$STATE"
	elif [ -e "$OLD_POWER" ]; then
		printf 'power %s\n' "$(date +%s)" > "$STATE"
	fi
fi
rm -f "$OLD_POWER" "$OLD_FOLIO"
exit 0
