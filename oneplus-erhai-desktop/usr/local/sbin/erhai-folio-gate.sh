#!/bin/sh
# erhai — start/stop the folio-fold watcher with the pogo keyboard state.
#
# erhai-folio-fold.service is deliberately NOT enabled in systemd: its
# lifecycle belongs to the keyboard.  The oneplus-pogo-kbd driver exports
# /sys/bus/serial/drivers/oneplus-pogo-kbd/*/attached but emits NO uevent when
# it flips (the driver only logs), so this gate polls it — the same pattern
# erhai-pogo-osk.py uses.  1 Hz costs nothing and reacts well under a second.
#
#   attached  -> systemctl start erhai-folio-fold.service
#   detached  -> systemctl stop  erhai-folio-fold.service
#   unknown   -> stop (no driver/attribute on this kernel => nothing to watch)
#
# Test hooks (all optional): ERHAI_POGO_ATTACH_GLOB, ERHAI_FOLIO_UNIT,
# ERHAI_SYSTEMCTL.
ATTACH_GLOB=${ERHAI_POGO_ATTACH_GLOB:-/sys/bus/serial/drivers/oneplus-pogo-kbd/*/attached}
FOLD_UNIT=${ERHAI_FOLIO_UNIT:-erhai-folio-fold.service}
SYSTEMCTL=${ERHAI_SYSTEMCTL:-systemctl}
POLL_S=${ERHAI_FOLIO_POLL_S:-1}

attached_file() {
    for f in $ATTACH_GLOB; do
        [ -r "$f" ] && { printf '%s\n' "$f"; return 0; }
    done
    return 1
}

last=
while :; do
    state=unknown
    if f=$(attached_file); then
        v=$(cat "$f" 2>/dev/null)
        case "$v" in
            1) state=attached ;;
            0) state=detached ;;
        esac
    fi

    if [ "$state" != "$last" ]; then
        last=$state
        logger -t erhai-folio "pogo keyboard $state"
        case "$state" in
            attached) "$SYSTEMCTL" start "$FOLD_UNIT" ;;
            *)        "$SYSTEMCTL" stop  "$FOLD_UNIT" ;;
        esac
    fi
    sleep "$POLL_S"
done