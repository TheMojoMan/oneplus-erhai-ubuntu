#!/bin/bash
# erhai Waydroid installer steps -- shipped by oneplus-erhai-waydroid-installer.
#
# Installs Waydroid from the repository the package carries, and makes its stock
# vendor.img carry the erhai Mesa driver payload:
#   1. `setup`: apt-get update + apt-get install waydroid (the package's postinst
#      cannot do this itself -- dpkg holds the lock while a postinst runs -- so
#      it schedules this action a few seconds later);
#   2. hooks waydroid's own image downloader (tools/helpers/images.py), so
#      `waydroid init`, `waydroid upgrade` and an OTA image replace patch the
#      image they just downloaded;
#   3. patches the vendor.img that is on disk right now, if any.
# Without the payload the stock fake "a830" Mesa driver wedges the Adreno a few
# seconds into a session.
#
#   sudo waydroid-erhai-installer.sh setup        # install waydroid + hook + patch
#   sudo waydroid-erhai-installer.sh              # hook + patch what is there
#   sudo waydroid-erhai-installer.sh status       # payload, hook, image state
#   sudo waydroid-erhai-installer.sh patch-now    # patch now, no hook changes
#   sudo waydroid-erhai-installer.sh reapply      # hook only if it went missing
#   sudo waydroid-erhai-installer.sh uninstall    # give waydroid its file back
#
# reapply is what /etc/apt/apt.conf.d/99erhai-waydroid-mesa runs after every
# dpkg run: `apt upgrade waydroid` overwrites images.py and would otherwise
# silently drop the hook.  It is a no-op while the hook is in place.
set -u -o pipefail

# Env seams exist for the tests (tmp/waydroid-patcher/deb-test.sh) and mirror the
# names the patcher itself already honours.
PATCHER=${ERHAI_WAYDROID_PATCHER:-/usr/lib/waydroid/tools/erhai_vendor_patch.py}
IMAGES_PY=${ERHAI_WAYDROID_IMAGES_PY:-/usr/lib/waydroid/tools/helpers/images.py}
STATE_DIR=${ERHAI_WAYDROID_IMAGES_PATH:-/var/lib/waydroid/images}
OFFMARK=${ERHAI_WAYDROID_OFFMARK:-/etc/erhai/waydroid-mesa.off}
APT_CONF=${ERHAI_WAYDROID_APT_CONF:-/etc/apt/apt.conf.d/99erhai-waydroid-mesa}
SOURCES=${ERHAI_WAYDROID_SOURCES:-/etc/apt/sources.list.d/waydroid.sources}
LEGACY_LIST=${ERHAI_WAYDROID_LEGACY_LIST:-/etc/apt/sources.list.d/waydroid.list}
SETUP_LOG=${ERHAI_WAYDROID_SETUP_LOG:-/var/log/erhai-waydroid-setup.log}
APT_GET=${ERHAI_WAYDROID_APT_GET:-apt-get}
SYSTEMCTL=${ERHAI_WAYDROID_SYSTEMCTL:-systemctl}
LOG=/var/log/erhai-waydroid-vendor-patch.log
ACTION=install
IMAGE=

die() { echo "ERROR: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        setup|install|status|patch-now|reapply|uninstall) ACTION=$1; shift ;;
        --image) [ $# -ge 2 ] || die "--image needs a path"; IMAGE=$2; shift 2 ;;
        -h|--help)
            # the usage block above: every leading comment line but this one
            awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
            exit 0
            ;;
        *) die "unknown argument '$1' (setup|install|status|patch-now|reapply|uninstall|--image PATH)" ;;
    esac
done

say() { echo "erhai-waydroid: $*"; }

patched_ok() { [ -f "$PATCHER" ] || die "$PATCHER is missing (broken package?)"; }

# Waydroid's own tools package, i.e. is the hook target even there?
have_waydroid() { [ -f "$IMAGES_PY" ]; }

image_mounted() {
    # The image is loop-mounted while a session runs (the patcher refuses it).
    # Ask the mount table, NOT a waydroid process: a stray/unrelated process
    # must not block a patch that is perfectly safe.
    local img=$1
    [ -r /proc/mounts ] || return 1
    awk -v want="$(readlink -f "$img")" '
        { m = $2; gsub(/\\040/, " ", m) }
        m == want { found = 1; exit }
        END { exit !found }' /proc/mounts
}

patch_image() {
    local img=${IMAGE:-$STATE_DIR/vendor.img}
    if [ ! -f "$img" ]; then
        say "no vendor image at $img -- 'waydroid init' downloads it (and patches it)"
        return 0
    fi
    if image_mounted "$img"; then
        say "$img is mounted (a waydroid session is running); stop it with"
        say "  waydroid session stop"
        say "and run:  sudo $0 patch-now"
        return 3
    fi
    say "patching $img"
    python3 "$PATCHER" --image "$img"
    local rc=$?
    case "$rc" in
        0) say "image carries the erhai Mesa payload" ;;
        2) say "the image could not be patched right now (log: $LOG)" ;;
        *) say "PATCHING FAILED (rc=$rc), log: $LOG" ;;
    esac
    return "$rc"
}

# The 2026-10-24 hand install (install-hook.sh in tmp/waydroid-patcher) put a
# SECOND patcher + apt hook under /usr/local.  This package owns the job now, so
# remove that tree explicitly (never a glob, never a recursive delete).
cleanup_legacy() {
    [ "${ERHAI_WAYDROID_LEGACY_CLEANUP:-1}" = 1 ] || return 0
    local old_lib=/usr/local/lib/erhai-waydroid-mesa
    if [ -f /etc/apt/apt.conf.d/99erhai-waydroid-hook ]; then
        rm -f /etc/apt/apt.conf.d/99erhai-waydroid-hook
        say "removed the hand-installed apt hook (99erhai-waydroid-hook)"
    fi
    for f in /usr/local/sbin/erhai-waydroid-hook-reapply \
             /usr/local/sbin/erhai-waydroid-vendor-patch; do
        [ -e "$f" ] && rm -f "$f"
    done
    if [ -d "$old_lib" ]; then
        rm -f "$old_lib/patch.py" "$old_lib/verify.py" "$old_lib/apply_hook.py"
        [ -d "$old_lib/stripped" ] && {
            for f in "$old_lib"/stripped/*.so; do [ -e "$f" ] && rm -f "$f"; done
            rmdir "$old_lib/stripped" 2>/dev/null || true
        }
        rmdir "$old_lib" 2>/dev/null || true
        say "removed the hand-installed patcher copy in $old_lib"
    fi
}

install_hook() {
    patched_ok
    if ! have_waydroid; then
        say "waydroid is not installed yet ($IMAGES_PY missing)"
        say "the hook is installed automatically once it is (apt post-invoke)"
        return 0
    fi
    rm -f "$OFFMARK"
    python3 "$PATCHER" --install-hook
}

# The hand-added repo file (waydroid's docs script) and the one this package
# ships point at the same URI; keeping both makes apt warn "Target Packages ...
# is configured multiple times".  Only remove it when no package owns it.
remove_legacy_list() {
    [ -f "$LEGACY_LIST" ] || return 0
    local owner
    owner=$(dpkg -S "$LEGACY_LIST" 2>/dev/null) || owner=
    if [ -n "$owner" ]; then
        say "keeping $LEGACY_LIST ($owner owns it)"
        return 0
    fi
    rm -f "$LEGACY_LIST"
    say "removed the hand-added $LEGACY_LIST ($SOURCES replaces it)"
}

# apt part of the job.  Runs from the deferred unit, never from a postinst:
# there dpkg holds /var/lib/dpkg/lock-frontend.  DPkg::Lock::Timeout makes it
# wait for a still-running apt instead of failing.
apt_install_waydroid() {
    local try
    for try in 1 2 3; do
        if "$APT_GET" update -q \
                -o DPkg::Lock::Timeout=600 -o Acquire::Retries=3; then
            if DEBIAN_FRONTEND=noninteractive "$APT_GET" install -y \
                    -o DPkg::Lock::Timeout=600 -o Acquire::Retries=3 waydroid; then
                return 0
            fi
            say "apt-get install waydroid failed (try $try/3)"
        else
            say "apt-get update failed (try $try/3)"
        fi
        [ "$try" -lt 3 ] && sleep 15
    done
    return 1
}

do_setup() {
    local rc=0
    if [ ! -f "$SOURCES" ]; then
        say "missing $SOURCES -- the waydroid repository is not configured"
        say "reinstall the package:  sudo apt install --reinstall ./oneplus-erhai-waydroid-installer_*.deb"
        return 1
    fi
    remove_legacy_list
    if apt_install_waydroid; then
        say "waydroid $(dpkg-query -W -f='${Version}' waydroid 2>/dev/null || echo '?') installed"
    else
        say "could not install waydroid from https://repo.waydro.id/"
        say "check the network, then run:  sudo $0 setup"
        rc=1
    fi
    # waydroid-container.service exists now: re-arm the shutdown watcher.
    if command -v "$SYSTEMCTL" >/dev/null 2>&1; then
        "$SYSTEMCTL" daemon-reload >/dev/null 2>&1 || true
        "$SYSTEMCTL" enable erhai-waydroid-container-guard.service >/dev/null 2>&1 || true
    fi
    if have_waydroid; then
        install_hook || say "could not hook $IMAGES_PY (run: sudo $0 reapply)"
        patch_image || true
    else
        say "waydroid is not present yet; the apt post-invoke hook puts the"
        say "images.py hook in place as soon as it is, and 'sudo $0 setup' retries"
    fi
    return "$rc"
}

case "$ACTION" in
    setup)
        [ "$(id -u)" -eq 0 ] || die "run with sudo"
        # The postinst runs this from a transient systemd unit, whose output
        # only reaches the journal -- keep a plain log the user can read.
        install -d -m755 "$(dirname "$SETUP_LOG")"
        do_setup 2>&1 | tee -a "$SETUP_LOG"
        exit "${PIPESTATUS[0]}"
        ;;
    install)
        [ "$(id -u)" -eq 0 ] || die "run with sudo"
        cleanup_legacy
        install_hook || die "could not hook waydroid's image downloader"
        patch_image || true    # never fail the package/install for this
        echo
        python3 "$PATCHER" --status --image "${IMAGE:-$STATE_DIR/vendor.img}"
        ;;
    patch-now)
        [ "$(id -u)" -eq 0 ] || die "run with sudo"
        patch_image
        ;;
    status)
        patched_ok
        python3 "$PATCHER" --status --image "${IMAGE:-$STATE_DIR/vendor.img}"
        echo
        say "installer  $0"
        say "repo       $SOURCES: $( [ -f "$SOURCES" ] && echo present || echo absent)"
        say "waydroid   $(dpkg-query -W -f='${Version}' waydroid 2>/dev/null || echo 'not installed')"
        say "apt hook   $APT_CONF: $( [ -f "$APT_CONF" ] && echo present || echo absent)"
        say "guard      $("$SYSTEMCTL" is-enabled erhai-waydroid-container-guard.service 2>/dev/null || echo not-enabled), $("$SYSTEMCTL" is-active erhai-waydroid-container-guard.service 2>/dev/null || echo inactive)"
        say "disabled   $( [ -f "$OFFMARK" ] && echo 'yes (uninstall was run)' || echo no)"
        ;;
    reapply)
        # Called by the apt post-invoke hook: silent, always exit 0.  Always ask
        # the patcher -- it is idempotent and ALSO replaces a hook block from an
        # older version, which a shell grep could not tell apart.
        have_waydroid || exit 0
        [ -f "$OFFMARK" ] && exit 0
        [ -f "$PATCHER" ] || exit 0
        python3 "$PATCHER" --install-hook -q || true
        exit 0
        ;;
    uninstall)
        [ "$(id -u)" -eq 0 ] || die "run with sudo"
        install -d -m755 "$(dirname "$OFFMARK")"
        : >"$OFFMARK"
        if have_waydroid; then
            python3 "$PATCHER" --remove-hook || true
        fi
        say "hook removed from $IMAGES_PY (apt will not put it back)"
        say "already patched images are NOT reverted -- remove $OFFMARK and run"
        say "  sudo $0 install   to enable it again"
        ;;
esac
