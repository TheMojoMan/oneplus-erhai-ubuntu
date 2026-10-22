#!/bin/bash
# Build oneplus-erhai-desktop_<version>_all.deb from the
# oneplus-erhai-desktop/ staging tree.
#
#   ./build-desktop-deb.sh -> debs/oneplus-erhai-desktop_<version>_all.deb
#
# Plain binary-package layout like build-bluetooth-deb.sh: DEBIAN/control +
# DEBIAN/postinst, erhai-user-groups.service (+ script) that puts the first
# real user into the hardware groups (video/i2c/audio/...), touchscreen
# bring-up unit + rebind script, power-key REAL panel power cycle
# (erhai-power-watch.py -> erhai-panel-power.sh), pogo attach ->
# OSK unit, folio-fold watcher (+ its gate unit and the calibration probe),
# greeter helpers, and the FIXED gjs-osk on-screen keyboard as a system-wide
# GNOME Shell extension (see the gjs-osk block below).
# Staged into root-setup-files/packages/ for build-root.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-desktop"
SETUP="$HERE/root-setup-files"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")

for f in usr/local/sbin/erhai-user-groups.sh \
	 usr/local/sbin/erhai-touch-rebind.sh \
	 usr/local/sbin/erhai-display-toggle.sh \
	 usr/local/sbin/erhai-panel-power.sh \
	 usr/local/sbin/erhai-power-watch.py \
	 usr/local/sbin/erhai-suspend-gnome.sh \
	 usr/local/sbin/erhai-suspend-s2idle.sh \
	 usr/local/sbin/erhai-suspend-wlan-guard.sh \
	 etc/systemd/system/systemd-suspend.service.d/erhai-s2idle.conf \
	 usr/local/sbin/erhai-folio-fold.py \
	 usr/local/sbin/erhai-folio-gate.sh \
	 usr/local/sbin/erhai-folio-unblank.sh \
	 usr/local/sbin/erhai-gpu-external-pin.sh \
	 etc/udev/rules.d/72-erhai-gpu-external.rules \
	 usr/local/sbin/erhai-folio-calibrate \
	 usr/local/sbin/erhai-folio-probe.py \
	 etc/udev/rules.d/71-erhai-folio-udev.rules \
	 usr/local/sbin/erhai-backlight-fixed.sh \
	 usr/local/sbin/erhai-greeter-brightness.py \
	 etc/systemd/system/erhai-backlight.service \
	 etc/systemd/system/erhai-greeter-brightness.service \
	 usr/local/sbin/erhai-rtc-sync.py \
	 etc/systemd/system/erhai-rtc-sync.service \
	 etc/systemd/system/erhai-rtc-sync.timer \
	 usr/local/sbin/erhai-bypass-charge.sh \
	 etc/default/erhai-charge \
	 etc/systemd/system/erhai-bypass-charge.service \
	 etc/systemd/system/erhai-bypass-charge@.service \
	 etc/polkit-1/rules.d/50-erhai-bypass-charge.rules \
	 usr/share/gnome-shell/extensions/erhai-bypass@erhai.local/extension.js \
	 usr/share/gnome-shell/extensions/erhai-bypass@erhai.local/metadata.json \
	 usr/share/oneplus-erhai-desktop/README.md \
	 usr/share/glib-2.0/schemas/99-erhai-no-gsd-ambient.gschema.override \
	 usr/local/share/gdm/greeter/wayland-sessions/gnome-greeter.desktop \
	 usr/share/gdm/dconf/95-erhai-greeter \
	 etc/dconf/profile/gdm; do
	[ -f "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

# --------------------------------------------------------- gjs-osk payload ---
# The FIXED gjs-osk (folio gate, 2026-09-29) ships in THIS package as a
# system-wide GNOME Shell extension, the same way oneplus-erhai-sensors ships
# erhai-sensors@erhai.local.  gnome-shell loads
# /usr/share/gnome-shell/extensions/<uuid>/ for every user; what enables it is
# the user's `enabled-extensions` (the per-user dconf SEED stays image-only --
# /etc/skel/.config/dconf/user is not ours to ship).
#
# The payload is COPIED FROM THE IMAGE OVERLAY on every build, so the deb and a
# flashed image can never diverge: root-setup-files/overlay/usr/share/
# gnome-shell/extensions/gjsosk@vishram1123.com/ is the single source of truth
# (kept in sync with tmp/gjs-osk-erhai/, the mirror of the live
# ~/.local/share/gnome-shell/extensions/gjsosk@vishram1123.com/).
#
# PRECEDENCE: a USER-LEVEL copy of the same uuid WINS -- gnome-shell's
# FileUtils.collectFromDatadirs() unshifts the user data dir and the later
# system copy is skipped with "Extension <uuid> already installed in <path>".
# <path> will not be loaded".  So on the dev pad (which has a user copy) this
# payload changes nothing at runtime; it is the apt/reflash delivery of the fix.
GJSOSK_SRC="$SETUP/overlay/usr/share/gnome-shell/extensions/gjsosk@vishram1123.com"
GJSOSK_DST="$SRC/usr/share/gnome-shell/extensions/gjsosk@vishram1123.com"
[ -f "$GJSOSK_SRC/extension.js" ] || {
	echo "missing $GJSOSK_SRC/extension.js" >&2; exit 1; }
if [ -e "$GJSOSK_DST" ]; then
	case "$GJSOSK_DST" in
		"$SRC"/usr/share/gnome-shell/extensions/*) rm -rf -- "$GJSOSK_DST" ;;
		*) echo "refusing to remove $GJSOSK_DST" >&2; exit 1 ;;
	esac
fi
mkdir -p "$(dirname "$GJSOSK_DST")"
cp -a "$GJSOSK_SRC" "$GJSOSK_DST"
# gnome-shell runs as the DESKTOP USER, so a root-owned 0600 metadata.json
# under /usr/share makes it refuse the extension SILENTLY (it cannot read the
# uuid or shell-version).  Normalize the whole tree here too -- the same trap
# build-root.sh fixes for the overlay (2026-09-28).
find "$GJSOSK_DST" -type d -exec chmod 755 {} +
find "$GJSOSK_DST" -type f -exec chmod 644 {} +
for f in metadata.json extension.js pogoGate.js prefs.js physicalLayouts.json \
	 keycodes.tar.xz schemas/gschemas.compiled \
	 ui/icons/hicolor/scalable/actions/backspace.svg; do
	[ -f "$GJSOSK_DST/$f" ] || {
		echo "missing $f in the gjs-osk payload" >&2; exit 1; }
done
grep -q '"uuid": *"gjsosk@vishram1123.com"' "$GJSOSK_DST/metadata.json" || {
	echo "gjs-osk metadata.json does not declare the gjsosk uuid" >&2
	exit 1; }
echo "==> gjs-osk payload: $(find "$GJSOSK_SRC" -type f | wc -l) files," \
	"copied from $(realpath --relative-to="$HERE" "$GJSOSK_SRC")"

# ------------------------------------------- UBUNTU DOCK touch-scroll patch ---
# Finger-drag scrolling for the Ubuntu Dock (project dir oneplus-erhai-dock).
# ubuntu-dock@ubuntu.com is a SESSION-MODE extension (it is listed in
# /usr/share/gnome-shell/modes/ubuntu.json), so gnome-shell REFUSES a per-user
# copy of it: the patched dash.js has to live at
# /usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js, a path that
# belongs to gnome-shell-ubuntu-extensions.  A plain file at that path from this
# package would be a dpkg file conflict, so the deb ships the patched file under
# a PRIVATE name (usr/share/oneplus-erhai-desktop/ubuntu-dock-dash.js) and
# DEBIAN/postinst installs it with dpkg-divert: Ubuntu's file is kept as
# dash.js.erhai-orig and an upgrade of gnome-shell-ubuntu-extensions then writes
# to the diverted name, so it can never clobber the patch.  DEBIAN/postrm drops
# the diversion again (restoring Ubuntu's file) so removing this package cannot
# leave the dock without a dash.js.
# Source of truth is the oneplus-erhai-dock project dir, copied here on every
# build so the two can never diverge.
DOCK_DASH_SRC="$HERE/oneplus-erhai-dock/usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js"
DOCK_DASH_DST="$SRC/usr/share/oneplus-erhai-desktop/ubuntu-dock-dash.js"
[ -f "$DOCK_DASH_SRC" ] || {
	echo "missing $DOCK_DASH_SRC" >&2; exit 1; }
grep -q '_initTouchScroll' "$DOCK_DASH_SRC" || {
	echo "dock dash.js does not carry the touch-scroll patch" >&2; exit 1; }
install -D -m 644 "$DOCK_DASH_SRC" "$DOCK_DASH_DST"
echo "==> dock touch-scroll payload: $(md5sum "$DOCK_DASH_DST" | cut -d' ' -f1)," \
	"copied from $(realpath --relative-to="$HERE" "$DOCK_DASH_SRC")"

# ------------------------------- DESKTOP ICONS (DING) touch long-press patch ---
# Hold a finger on the desktop or an icon to open the context menu, which DING
# otherwise only opens on a secondary (mouse) button press (project dir
# oneplus-erhai-ding).  ding@rastersoft.com belongs to
# gnome-shell-ubuntu-extensions, so the patched files ship under a PRIVATE path
# and DEBIAN/postinst installs them with dpkg-divert (Ubuntu's files kept as
# *.erhai-orig) -- exactly like the dock patch above, because a plain file at
# the extension's own path would be a dpkg file conflict.
# Source of truth is the oneplus-erhai-ding project dir, copied here on every
# build so the two can never diverge.
DING_SRC="$HERE/oneplus-erhai-ding/usr/share/gnome-shell/extensions/ding@rastersoft.com"
DING_DST="$SRC/usr/share/oneplus-erhai-desktop/ding-touch"
for f in app/desktopGrid.js app/desktopManager.js app/desktopIconItem.js; do
	[ -f "$DING_SRC/$f" ] || { echo "missing $DING_SRC/$f" >&2; exit 1; }
	grep -q '_initTouchLongPress' "$DING_SRC/$f" || {
		echo "ding $f does not carry the touch long-press patch" >&2; exit 1; }
	install -D -m 644 "$DING_SRC/$f" "$DING_DST/$f"
done
echo "==> ding touch long-press payload: $(cd "$DING_DST" && md5sum app/*.js | awk '{print $1}' | tr '\n' ' ')," \
	"copied from $(realpath --relative-to="$HERE" "$DING_SRC")"

# Panel click sounds (deterministic, regenerated on every build — same pattern
# as build-sensors-deb.sh compiling its gschemas into the staging tree).
mkdir -p "$SRC/usr/share/sounds/erhai"
"$HERE/gen-panel-click.py" "$SRC/usr/share/sounds/erhai/panel-off.wav" off \
	>/dev/null
"$HERE/gen-panel-click.py" "$SRC/usr/share/sounds/erhai/panel-on.wav" on \
	>/dev/null
for f in usr/share/sounds/erhai/panel-off.wav usr/share/sounds/erhai/panel-on.wav; do
	[ -s "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

# Pre-pack guard: a Python bytecode cache is NOT payload.  A test that imports a
# script from this tree (importlib over usr/local/sbin/erhai-*.py) writes
# __pycache__ next to it, and a .pyc shipped in the deb would be a stale,
# root-owned copy of the script that dpkg -V cannot check against the source.
if find "$SRC" \( -name __pycache__ -o -name '*.pyc' \) -print -quit | grep -q .; then
	echo "refusing to pack: Python bytecode cache inside $SRC" >&2
	find "$SRC" \( -name __pycache__ -o -name '*.pyc' \) >&2
	exit 1
fi

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst
if [ -f "$SRC"/DEBIAN/postrm ]; then chmod 755 "$SRC"/DEBIAN/postrm; fi
# everything in sbin/ is executable — .sh AND .py (pogo-osk regression 2026-09-09)
find "$SRC/usr/local/sbin" -type f -exec chmod 755 {} \;
# the rest of the payload is plain data (units, udev rules, dconf, schemas,
# sounds, the gjs-osk tree): root:root 0644, never group-writable.
find "$SRC" -type f ! -path "$SRC/DEBIAN/*" ! -path "$SRC/usr/local/sbin/*" \
	-exec chmod 644 {} +

# md5sums for every payload file (DEBIAN/ excluded), sorted so the manifest is
# deterministic and `dpkg -V` has something to verify against.  The 2.1 build
# shipped no md5sums member at all.
rm -f "$SRC/DEBIAN/md5sums"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
chmod 644 "$SRC/DEBIAN/md5sums"
echo "==> md5sums: $(grep -c '' "$SRC/DEBIAN/md5sums") files"
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_all.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations;
# release policy: publish to the PPA only when the owner asks for it).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_all.deb"
fi
echo "built ${PKG}_${VER}_all.deb"
