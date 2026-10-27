#!/usr/bin/env bash
#
# build-nautilus-erhai.sh -- build a patched GNOME Files (Nautilus) with
# touch-friendly additions (erhai):
#   * long-tap on the empty view background opens the context menu
#   * "Show Select Boxes" preference: a selection checkbox beside each item,
#     with finger-swipe painting over the checkboxes.
#
# It extracts the Ubuntu source package for the INSTALLED nautilus version,
# applies nautilus-select-boxes.patch, builds it with meson and stages the
# result into ./build/ (binary + gschema).
#
# The result is meant to be installed with ./install-nautilus-erhai.sh
# (dpkg-divert based), NOT published to the PPA while in development.
#
# Requires (build deps, not auto-installed): blueprint-compiler, meson, ninja,
# libadwaita-1-dev, libgnome-autoar-0-dev, libgnome-desktop-4-dev,
# libportal-gtk4-dev, libtinysparql-dev, libicu-dev, libglycin-gtk4-2-dev,
# libgexiv2-dev, libgstreamer1.0-dev, libgstreamer-plugins-base1.0-dev,
# libcloudproviders-dev, libgbm-dev.
#
# NOTE: libgbm-dev depends on `libgbm1 (= <ubuntu version>)`, but this pad
# ships the erhai mesa replacement (a HIGHER libgbm1), so the plain Ubuntu
# libgbm-dev cannot be installed. A locally relaxed libgbm-dev (Depends:
# libgbm1 (>= ...)) is what is used on the owner's pad.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# per-line patch: the plain name is the 26.04 (50.x) one, the -51 variant the 26.10 one
PATCH="$HERE/nautilus-select-boxes.patch"
WORK="${ERHAI_NAUTILUS_WORK:-/media/data/develop/erhai/tmp/nautilus-erhai-build}"
BUILD_DIR="$HERE/build"
JOBS="$(nproc)"

command -v meson    >/dev/null || { echo "meson missing"    >&2; exit 1; }
command -v ninja    >/dev/null || { echo "ninja missing"    >&2; exit 1; }
command -v dpkg-source >/dev/null || { echo "dpkg-source missing (dpkg-dev)" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "patch not found: $PATCH" >&2; exit 1; }

VER="$(dpkg-query -W -f='${Version}' nautilus 2>/dev/null || true)"
[ -n "$VER" ] || { echo "nautilus is not installed" >&2; exit 1; }
# debian version "1:50.2.2-0ubuntu0.2" -> upstream "50.2.2"
UP="$(printf '%s' "$VER" | sed 's/^[0-9]*://; s/-.*$//')"
echo "== installed nautilus $VER (upstream $UP)"
MAJOR="${UP%%.*}"
if [ -f "$HERE/nautilus-select-boxes-$MAJOR.patch" ]; then
    PATCH="$HERE/nautilus-select-boxes-$MAJOR.patch"
fi
echo "== patch: $(basename "$PATCH")"

mkdir -p "$WORK"
cd "$WORK"

mkdir -p src
DSC="$(ls src/nautilus_"$UP"*.dsc 2>/dev/null | head -1 || true)"
if [ -z "$DSC" ]; then
    echo "== fetching source (nautilus $UP) =="
    ( cd src && apt-get source --download-only nautilus )
    DSC="$(ls src/nautilus_"$UP"*.dsc 2>/dev/null | head -1 || true)"
fi
[ -n "$DSC" ] || { echo "no .dsc for $UP" >&2; exit 1; }
echo "== extracting a clean source tree =="
rm -rf "src/nautilus-$UP"
( cd src && dpkg-source -x "$(basename "$DSC")" )
SRC="$WORK/src/nautilus-$UP"
[ -d "$SRC" ] || { echo "source dir missing: $SRC" >&2; exit 1; }

echo "== applying patch =="
( cd "$SRC" && patch -p1 --forward < "$PATCH" )

echo "== configuring =="
# A stale PKG_CONFIG_PATH (from other in-tree builds) can hide system .pc files.
unset PKG_CONFIG_PATH || true
rm -rf "$WORK/build"
meson setup "$WORK/build" "$SRC" --prefix=/usr --buildtype=release \
      -Dtests=none -Ddocs=false -Dintrospection=false -Dextensions=true \
      -Dxdg-terminal-exec=true -Dunity-launcher=true \
      -Dcloudproviders=false -Dselinux=true

echo "== building ($JOBS jobs) =="
ninja -C "$WORK/build" -j "$JOBS"

echo "== staging into $BUILD_DIR =="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/bin" "$BUILD_DIR/schemas"
install -m 0755 "$WORK/build/src/nautilus" "$BUILD_DIR/bin/nautilus"
install -m 0644 "$SRC/data/org.gnome.nautilus.gschema.xml" "$BUILD_DIR/schemas/org.gnome.nautilus.gschema.xml"

echo "== done =="
echo "  binary   : $BUILD_DIR/bin/nautilus"
echo "  gschema  : $BUILD_DIR/schemas/org.gnome.nautilus.gschema.xml"
echo "  install  : $HERE/install-nautilus-erhai.sh apply"
