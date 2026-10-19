#!/bin/bash
#
# Install / revert the "hold a finger on the desktop or an icon to open the
# context menu" patch for the Desktop Icons NG (DING) GNOME Shell extension
# (ding@rastersoft.com).
#
# DING runs as a separate GTK3/gjs process (app/ding.js) and opens its context
# menus on a SECONDARY (mouse) button press (button == 3).  A touch tap or
# long tap is delivered to GTK as an emulated PRIMARY (button 1) press, so the
# menu never opened with a finger.  The patch adds a touch-ONLY long-press
# shim (500 ms, cancelled on movement) that opens the same menu.
#
# The files belong to gnome-shell-ubuntu-extensions, so to keep dpkg happy and
# survive package upgrades the originals are diverted with dpkg-divert:
#
#   apply   save Ubuntu's files as *.erhai-orig, register a dpkg-divert and put
#           our patched files in place
#   revert  drop the diversion and restore Ubuntu's originals
#   status  report which version is currently installed
#
# The diversion is registered WITHOUT --rename and Ubuntu's originals are kept
# by this script itself: `dpkg-divert --remove --rename` refuses to overwrite a
# different existing file, so the --rename round trip is not reliable here.
#
# Usage:  sudo ./install-ding-touch-longpress.sh [apply|revert|status]
#
set -euo pipefail

EXT_DIR="/usr/share/gnome-shell/extensions/ding@rastersoft.com"
# files that carry the patch, relative to the extension dir
FILES="app/desktopGrid.js app/desktopManager.js app/desktopIconItem.js"
MARKER="_initTouchLongPress"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/usr/share/gnome-shell/extensions/ding@rastersoft.com"

die() { echo "error: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
}

is_diverted() {
    dpkg-divert --list "$1" 2>/dev/null | grep -q "$1"
}

# Name of the package owning the diversion, or empty for a LOCAL diversion.
divert_owner() {
    dpkg-divert --list "$1" 2>/dev/null | sed -n 's/.* by \(.*\)$/\1/p'
}

# Keep Ubuntu's stock file as the diversion target, once.  Only when the
# current file still looks like the stock one (our patch carries the MARKER),
# so a re-run can never save our own file as "the original".
save_original() {
    local target="$1" orig="$2"
    if [ ! -f "$orig" ] && [ -f "$target" ] && ! grep -q "$MARKER" "$target"; then
        cp -a "$target" "$orig"
    fi
}

restore_original() {
    local target="$1" orig="$2"
    if [ -f "$orig" ]; then
        cp -f "$orig" "$target"
    elif [ ! -f "$target" ]; then
        echo "warning: no $target and no saved original -- reinstall" >&2
        echo "         gnome-shell-ubuntu-extensions" >&2
    fi
}

do_status() {
    local any=0
    for f in $FILES; do
        local target="${EXT_DIR}/${f}"
        if is_diverted "$target"; then
            any=1
            local owner
            owner="$(divert_owner "$target")"
            if [ -n "$owner" ]; then
                echo "$f: PATCHED (dpkg-divert owned by $owner)"
            else
                echo "$f: PATCHED (local dpkg-divert)"
            fi
        else
            echo "$f: STOCK (no diversion)"
        fi
        [ -f "$target" ] && echo "    md5:     $(md5sum "$target" | cut -d' ' -f1)"
        [ -f "${SRC}/${f}" ] && echo "    src md5: $(md5sum "${SRC}/${f}" | cut -d' ' -f1)"
    done
    [ "$any" -eq 1 ] && echo "state: PATCHED" || echo "state: STOCK"
}

do_apply() {
    need_root
    for f in $FILES; do
        [ -f "${SRC}/${f}" ] || die "patched ${f} not found at ${SRC}/${f}"
    done

    for f in $FILES; do
        local target="${EXT_DIR}/${f}"
        local orig="${target}.erhai-orig"
        [ -f "$target" ] || [ -f "$orig" ] ||
            die "no $target to patch and no saved original"
        # Register the diversion WITHOUT --rename and keep Ubuntu's original
        # ourselves (see the header note about --rename).
        save_original "$target" "$orig"
        dpkg-divert --add --no-rename --divert "$orig" "$target" \
            >/dev/null 2>&1 || true
        install -o root -g root -m 0644 "${SRC}/${f}" "$target"
        echo "installed patched ${f}: $(md5sum "$target" | cut -d' ' -f1)"
    done

    echo
    echo "NOTE: DING runs as its own process.  The patched code is picked up"
    echo "      when that process restarts -- log out and back in (or reboot)."
    echo "      To try it without a logout: 'pkill -f ding.js' and gnome-shell"
    echo "      respawns it with the new files."
}

do_revert() {
    need_root
    for f in $FILES; do
        local target="${EXT_DIR}/${f}"
        local orig="${target}.erhai-orig"
        if ! is_diverted "$target"; then
            echo "${f}: no diversion present, skipping"
            continue
        fi
        local owner
        owner="$(divert_owner "$target")"
        if [ -n "$owner" ]; then
            echo "error: the diversion of ${f} is owned by package '$owner';" >&2
            echo "       this patch ships in oneplus-erhai-desktop. Undo it" >&2
            echo "       there ('sudo apt purge $owner') instead of ripping" >&2
            echo "       the diversion out behind dpkg's back." >&2
            exit 1
        fi
        dpkg-divert --remove --no-rename --divert "$orig" "$target" \
            >/dev/null 2>&1 || true
        restore_original "$target" "$orig"
        echo "${f}: diversion removed; stock file restored"
    done
    echo
    echo "NOTE: log out and back in (or reboot) to reload the stock extension."
}

case "${1:-status}" in
    apply)  do_apply  ;;
    revert) do_revert ;;
    status) do_status ;;
    *) die "usage: $0 [apply|revert|status]" ;;
esac