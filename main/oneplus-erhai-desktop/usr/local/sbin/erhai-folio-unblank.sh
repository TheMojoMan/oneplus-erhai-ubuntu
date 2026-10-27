#!/bin/sh
# Safety net for erhai-folio-fold.service (ExecStopPost).
#
# If the watcher is stopped or killed while it holds the panel blanked, undo the
# blank — a keyboard detach must never leave a black screen behind.
#
# OWNERSHIP (2.20): the SHARED state file /run/erhai-panel.state names the owner
# ("<owner> <epoch>", owner = power | folio).  The folio watcher writes "folio"
# only while IT blanked the panel, so a graceful stop (which already restored and
# cleared it) makes this a no-op.  A state owned by the POWER button is never
# touched here: that blank outlives this service and only the next power press
# (or a cover open) may undo it.  An unreadable/garbled owner IS restored — a
# black screen behind state we cannot parse is the worse failure.
STATE=/run/erhai-panel.state
[ -e "$STATE" ] || exit 0
read -r owner _ < "$STATE" 2>/dev/null || owner=''
if [ "$owner" = power ]; then
    logger -t erhai-folio "watcher stopped while blanked, but the power button owns the panel blank -> left off"
    exit 0
fi
/usr/local/sbin/erhai-display-toggle.sh on || true
rm -f "$STATE"
logger -t erhai-folio "watcher stopped while blanked (owner '${owner:-unknown}') -> backlight restored"
