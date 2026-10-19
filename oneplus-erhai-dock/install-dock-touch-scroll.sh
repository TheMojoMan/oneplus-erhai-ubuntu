#!/bin/bash
#
# Install / revert the "drag with a finger to scroll the app icons" patch for
# the Ubuntu Dock GNOME Shell extension (ubuntu-dock@ubuntu.com).
#
# The dock is a SESSION-MODE extension, so gnome-shell refuses to load a
# per-user copy from ~/.local/share/gnome-shell/extensions/ (it would only load
# it for a non-mode extension).  The modification therefore has to live at the
# system path.  To keep dpkg happy and survive package upgrades, the original
# file is diverted with dpkg-divert:
#
#   apply   save Ubuntu's dash.js as dash.js.erhai-orig, register a dpkg-divert
#           and put our patched dash.js in place
#   revert  drop the diversion and restore Ubuntu's original dash.js
#   status  report which version is currently installed
#
# The diversion is registered WITHOUT --rename and Ubuntu's original is kept by
# this script itself: `dpkg-divert --remove --rename` refuses to overwrite a
# different existing file, so the --rename round trip is not reliable here.
#
# Usage:  sudo ./install-dock-touch-scroll.sh [apply|revert|status]
#
set -euo pipefail

EXT_DIR="/usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com"
TARGET="${EXT_DIR}/dash.js"
DIVERT="${EXT_DIR}/dash.js.erhai-orig"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js"

die() { echo "error: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
}

is_diverted() {
    dpkg-divert --list "$TARGET" 2>/dev/null | grep -q "$TARGET"
}

# Name of the package owning the diversion, or empty for a LOCAL diversion.
divert_owner() {
    dpkg-divert --list "$TARGET" 2>/dev/null | sed -n 's/.* by \(.*\)$/\1/p'
}

# Keep Ubuntu's stock dash.js as the diversion target, once.  Only when the
# current file still looks like the stock one (our patch carries
# _initTouchScroll), so a re-run can never save our own file as "the original".
save_original() {
    if [ ! -f "$DIVERT" ] && [ -f "$TARGET" ] &&
       ! grep -q '_initTouchScroll' "$TARGET"; then
        cp -a "$TARGET" "$DIVERT"
    fi
}

# Put Ubuntu's stock dash.js back over our copy.
restore_original() {
    if [ -f "$DIVERT" ]; then
        cp -f "$DIVERT" "$TARGET"
    elif [ ! -f "$TARGET" ]; then
        echo "warning: no dash.js and no saved original -- run 'dpkg -i" >&2
        echo "         --force-confmiss gnome-shell-ubuntu-extensions' or" >&2
        echo "         reinstall that package" >&2
    fi
}

do_status() {
    echo "target:  $TARGET"
    if is_diverted; then
        local owner
        owner="$(divert_owner)"
        if [ -n "$owner" ]; then
            echo "state:   PATCHED (dpkg-divert owned by $owner)"
        else
            echo "state:   PATCHED (local dpkg-divert)"
        fi
        echo "orig:    $DIVERT"
    else
        echo "state:   STOCK (no diversion)"
    fi
    [ -f "$TARGET" ] && echo "md5:     $(md5sum "$TARGET" | cut -d' ' -f1)"
    [ -f "$SRC" ] && echo "src md5: $(md5sum "$SRC" | cut -d' ' -f1)"
}

do_apply() {
    need_root
    [ -f "$SRC" ] || die "patched dash.js not found at $SRC"
    [ -f "$TARGET" ] || [ -f "$DIVERT" ] ||
        die "no $TARGET to patch and no saved original"

    if [ -n "$(divert_owner)" ]; then
        echo "note: the diversion is owned by package '$(divert_owner)'; this"
        echo "      patch ships in oneplus-erhai-desktop (>= 2.8). Refreshing"
        echo "      $TARGET from this source tree anyway."
    fi

    # Register the diversion WITHOUT --rename and keep Ubuntu's original
    # ourselves.  The --rename form is asymmetric: `dpkg-divert --remove
    # --rename` REFUSES to overwrite a different existing file, so a later
    # revert/purge could fail and leave the dock with NO dash.js at all
    # (measured 2026-10-04).
    save_original
    dpkg-divert --add --no-rename --divert "$DIVERT" "$TARGET" \
        >/dev/null 2>&1 || true
    install -o root -g root -m 0644 "$SRC" "$TARGET"

    echo "installed patched dash.js:"
    md5sum "$TARGET"
    echo
    echo "NOTE: extension code is only reloaded at LOGIN. Log out and back in"
    echo "      (or reboot) for the finger-drag scrolling to take effect."
}

do_revert() {
    need_root
    if ! is_diverted; then
        echo "no diversion present, nothing to revert"
        do_status
        exit 0
    fi

    local owner
    owner="$(divert_owner)"
    if [ -n "$owner" ]; then
        echo "error: the diversion is owned by package '$owner'; this patch is" >&2
        echo "       now shipped by oneplus-erhai-desktop. Undo it there" >&2
        echo "       (e.g. 'sudo apt purge $owner') instead of ripping the" >&2
        echo "       diversion out behind dpkg's back." >&2
        exit 1
    fi

    dpkg-divert --remove --no-rename --divert "$DIVERT" "$TARGET" \
        >/dev/null 2>&1 || true
    restore_original
    echo "diversion removed; stock dash.js restored"
    do_status
    echo
    echo "NOTE: log out and back in (or reboot) to reload the stock extension."
}

case "${1:-status}" in
    apply)  do_apply  ;;
    revert) do_revert ;;
    status) do_status ;;
    *) die "usage: $0 [apply|revert|status]" ;;
esac
