#!/usr/bin/env bash
#
# install-nautilus-erhai.sh apply|revert|status
#
# Installs the erhai-patched Nautilus built by build-nautilus-erhai.sh by
# diverting Ubuntu's files (dpkg-divert --no-rename + our own backup copy),
# exactly like the dock/DING overrides.  apply needs root.
#
# Overridden files:
#   /usr/bin/nautilus
#   /usr/share/glib-2.0/schemas/org.gnome.nautilus.gschema.xml
#
# Reverts are exact; `revert` restores Ubuntu's files and drops the diversions.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"

BIN=/usr/bin/nautilus
SCHEMA=/usr/share/glib-2.0/schemas/org.gnome.nautilus.gschema.xml
SCHEMA_DIR=/usr/share/glib-2.0/schemas

need_root() { [ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }; }

# NOTE: `dpkg-divert --list <path>` exits 0 even when nothing matches, so we
# must test for actual output naming the path.
diverted() { dpkg-divert --list "$1" 2>/dev/null | grep -qF "$1"; }

# We divert the file TO its own ".erhai-orig" name (--divert) with --no-rename,
# and place a copy of the original there ourselves.  This keeps `dpkg -V` clean
# (the package's file list then expects the original at that path, where it
# exists) while avoiding the `--remove --rename` overwrite refusal.
install_one() {
    # $1 = system path, $2 = payload path, $3 = mode
    local sys="$1" payload="$2" mode="$3"
    local bak="$sys.erhai-orig"
    [ -f "$payload" ] || { echo "payload missing: $payload" >&2; exit 1; }
    if diverted "$sys"; then
        echo "  already diverted: $sys"
    else
        [ -f "$sys" ] || { echo "missing system file: $sys" >&2; exit 1; }
        cp -a "$sys" "$bak"
        dpkg-divert --local --add --no-rename --divert "$bak" "$sys" >/dev/null
    fi
    install -m "$mode" "$payload" "$sys"
    echo "  installed: $sys"
}

restore_one() {
    local sys="$1"
    local bak="$sys.erhai-orig"
    if [ -f "$bak" ]; then
        cp -a "$bak" "$sys"
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
        [ -f "$BUILD/bin/nautilus" ] || { echo "build first: $HERE/build-nautilus-erhai.sh" >&2; exit 1; }
        echo "== stopping Nautilus =="
        pkill -x nautilus 2>/dev/null || true
        sleep 1
        echo "== installing patched nautilus =="
        install_one "$BIN"    "$BUILD/bin/nautilus" 0755
        install_one "$SCHEMA" "$BUILD/schemas/org.gnome.nautilus.gschema.xml" 0644
        echo "== recompiling schemas =="
        glib-compile-schemas "$SCHEMA_DIR"
        echo "done. Nautilus restarts on next 'Files' launch."
        echo "Enable the feature in Files > Preferences > 'Show Select Boxes'."
        ;;
    revert)
        need_root
        echo "== reverting =="
        pkill -x nautilus 2>/dev/null || true
        sleep 1
        restore_one "$SCHEMA"
        restore_one "$BIN"
        glib-compile-schemas "$SCHEMA_DIR"
        echo "done."
        ;;
    status)
        echo "diversions:"
        dpkg-divert --list /usr/bin/nautilus || true
        dpkg-divert --list "$SCHEMA" || true
        echo "files:"
        for f in "$BIN" "$SCHEMA"; do
            printf '  %s ' "$f"
            if [ -f "$f.erhai-orig" ]; then echo "(patched; backup present)"; else echo "(original / no backup)"; fi
        done
        echo "md5:"
        md5sum "$BIN" 2>/dev/null || true
        grep -c show-select-boxes "$SCHEMA" 2>/dev/null | sed 's/^/  schema key count: /'
        ;;
    *)
        echo "usage: $0 apply|revert|status" >&2
        exit 2
        ;;
esac
