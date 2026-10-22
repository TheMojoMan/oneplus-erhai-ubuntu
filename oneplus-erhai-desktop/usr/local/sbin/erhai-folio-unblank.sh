#!/bin/sh
# Safety net for erhai-folio-fold.service (ExecStopPost).
#
# If the watcher is stopped or killed while it holds the panel blanked, undo
# the blank — a keyboard detach must never leave a black screen behind.
# The marker is written by the watcher only while IT blanked the panel, so a
# graceful stop (which already restored and removed it) makes this a no-op,
# and a power-button blank is never touched.
#
# The power-button blank carries its own marker (/run/erhai-panel-off) and is
# restored by the next power press, so it must NOT be undone here either — the
# watcher can only be holding a blank if a stale folio marker survived a
# SIGKILL in the middle of a power-button-owned blank (2026-10-22).
STATE=/run/erhai-folio-fold-blanked
POWER_MARKER=/run/erhai-panel-off
[ -e "$STATE" ] || exit 0
if [ -e "$POWER_MARKER" ]; then
    rm -f "$STATE"
    logger -t erhai-folio "watcher stopped while blanked, but the power button owns the panel blank -> left off"
    exit 0
fi
/usr/local/sbin/erhai-display-toggle.sh on || true
rm -f "$STATE"
logger -t erhai-folio "watcher stopped while blanked -> backlight restored"