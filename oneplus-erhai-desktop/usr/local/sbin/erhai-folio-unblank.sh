#!/bin/sh
# Safety net for erhai-folio-fold.service (ExecStopPost).
#
# If the watcher is stopped or killed while it holds the panel blanked, undo
# the blank — a keyboard detach must never leave a black screen behind.
# The marker is written by the watcher only while IT blanked the panel, so a
# graceful stop (which already restored and removed it) makes this a no-op,
# and a power-button blank is never touched.
STATE=/run/erhai-folio-fold-blanked
[ -e "$STATE" ] || exit 0
/usr/local/sbin/erhai-display-toggle.sh on || true
rm -f "$STATE"
logger -t erhai-folio "watcher stopped while blanked -> backlight restored"