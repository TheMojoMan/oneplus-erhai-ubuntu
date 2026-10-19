#!/usr/bin/env bash
#
# install-ptyxis-erhai.sh apply|revert|status
#
# Installs the erhai-patched Ptyxis built by build-ptyxis-erhai.sh by diverting
# Ubuntu's /usr/bin/ptyxis (dpkg-divert --no-rename + our own backup copy),
# exactly like the dock/DING/nautilus overrides.  apply needs root.
#
# Overridden file:
#   /usr/bin/ptyxis
#
# Reverts are exact; `revert` restores Ubuntu's binary and drops the diversion.
# The already-running ptyxis processes keep the old (mapped) binary; new windows
# and new launches use the patched one, so no logout is required.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"

BIN=/usr/bin/ptyxis

need_root() { [ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }; }

# NOTE: `dpkg-divert --list <path>` exits 0 even when nothing matches, so we
# must test for actual output naming the path.
diverted() { dpkg-divert --list "$1" 2>/dev/null | grep -qF "$1"; }

# Keep Ubuntu's original as *.erhai-orig and divert the path to that name
# (--no-rename), so `dpkg -V` stays clean.
install_one() {
    local sys="$1" payload="$2" mode="$3"
    local bak="$sys.erhai-orig"
    local tmp="$sys.erhai-new.$$"
    [ -f "$payload" ] || { echo "payload missing: $payload" >&2; exit 1; }
    if diverted "$sys"; then
        echo "  already diverted: $sys"
    else
        [ -f "$sys" ] || { echo "missing system file: $sys" >&2; exit 1; }
        cp -a "$sys" "$bak"
        dpkg-divert --local --add --no-rename --divert "$bak" "$sys" >/dev/null
    fi
    # Write beside the target and rename it into place: overwriting a *running*
    # executable with cp/install fails with ETXTBSY ("Text file busy").
    install -m "$mode" "$payload" "$tmp"
    mv -f "$tmp" "$sys"
    echo "  installed: $sys"
}

restore_one() {
    local sys="$1"
    local bak="$sys.erhai-orig"
    local tmp="$sys.erhai-restore.$$"
    if [ -f "$bak" ]; then
        cp -a "$bak" "$tmp"
        mv -f "$tmp" "$sys"
        echo "  restored: $sys"
    else
        echo "  no backup for $sys (skipped)"
    fi
    if diverted "$sys"; then
        dpkg-divert --local --remove --no-rename "$sys" >/dev/null 2>&1 || true
        echo "  diversion removed: $sys"
    fi
    rm -f "$bak"
}

case "${1:-}" in
    apply)
        need_root
        [ -f "$BUILD/ptyxis" ] || { echo "build first: $HERE/build-ptyxis-erhai.sh" >&2; exit 1; }
        echo "== installing patched ptyxis =="
        install_one "$BIN" "$BUILD/ptyxis" 0755
        echo "done. Open a new Ptyxis window/tab to use the touch gestures."
        ;;
    revert)
        need_root
        echo "== reverting =="
        restore_one "$BIN"
        echo "done."
        ;;
    status)
        echo "diversions:"
        dpkg-divert --list "$BIN" || true
        echo "files:"
        printf '  %s ' "$BIN"
        if [ -f "$BIN.erhai-orig" ]; then echo "(patched; backup present)"; else echo "(original / no backup)"; fi
        echo "md5:"
        md5sum "$BIN" 2>/dev/null || true
        if [ -f "$BIN.erhai-orig" ]; then
            md5sum "$BIN.erhai-orig" 2>/dev/null | sed 's/^/  orig: /'
        fi
        ;;
    *)
        echo "usage: $0 apply|revert|status" >&2
        exit 2
        ;;
esac
