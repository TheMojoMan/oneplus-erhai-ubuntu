#!/usr/bin/env bash
#
# build-ptyxis-erhai.sh -- build a patched Ptyxis (GNOME terminal) with
# touch-friendly additions (erhai):
#
#   * one-finger drag on the terminal scrolls the contents (with inertia);
#   * a long press starts a text selection (the word under the finger), dragging
#     extends it; on release the marked region is colour-inverted, copied to the
#     clipboard with a "Copied to clipboard" toast, and two draggable handles are
#     shown to adjust the region (releasing a handle re-copies); the marking is
#     dismissed by the next input you send (typing, on-screen keyboard, paste) or
#     by a tap outside it;
#   * a plain tap is forwarded to the application as a left mouse click when it
#     wants mouse events (micro, vim, less, ...);
#   * the header bar carries a Paste button next to the new-tab button.
#
# It extracts the Ubuntu source package for the INSTALLED ptyxis version,
# applies ptyxis-erhai-touch.patch, builds it with meson and stages the result
# into ./build/ (the `ptyxis` binary).
#
# The result is meant to be installed with ./install-ptyxis-erhai.sh
# (dpkg-divert based), NOT published to the PPA while in development.
#
# Build deps (not auto-installed): meson, ninja, libglib2.0-dev, libgtk-4-dev,
# libadwaita-1-dev, libjson-glib-dev, libportal-gtk4-dev, libvte-2.91-gtk4-dev.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="$HERE/ptyxis-erhai-touch.patch"
WORK="${ERHAI_PTYXIS_WORK:-/media/data/develop/erhai/tmp/ptyxis-erhai-build}"
BUILD_DIR="$HERE/build"
JOBS="$(nproc)"

command -v meson       >/dev/null || { echo "meson missing"    >&2; exit 1; }
command -v ninja       >/dev/null || { echo "ninja missing"    >&2; exit 1; }
command -v dpkg-source >/dev/null || { echo "dpkg-source missing (dpkg-dev)" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "patch not found: $PATCH" >&2; exit 1; }

VER="$(dpkg-query -W -f='${Version}' ptyxis 2>/dev/null || true)"
[ -n "$VER" ] || { echo "ptyxis is not installed" >&2; exit 1; }
# debian version "50.1-1ubuntu2" -> upstream "50.1"
UP="$(printf '%s' "$VER" | sed 's/^[0-9]*://; s/-.*$//')"
echo "== installed ptyxis $VER (upstream $UP)"

mkdir -p "$WORK"
cd "$WORK"

mkdir -p src
DSC="$(ls src/ptyxis_"$UP"*.dsc 2>/dev/null | head -1)"
if [ -z "$DSC" ]; then
    echo "== fetching source (ptyxis $UP) =="
    ( cd src && apt-get source --download-only ptyxis )
    DSC="$(ls src/ptyxis_"$UP"*.dsc 2>/dev/null | head -1)"
fi
[ -n "$DSC" ] || { echo "no .dsc for $UP" >&2; exit 1; }

echo "== extracting a clean source tree =="
rm -rf "src/ptyxis-$UP"
( cd src && dpkg-source -x "$(basename "$DSC")" )
SRC="$WORK/src/ptyxis-$UP"
[ -d "$SRC" ] || { echo "source dir missing: $SRC" >&2; exit 1; }

echo "== applying patch =="
( cd "$SRC" && patch -p1 --forward < "$PATCH" )

echo "== configuring =="
# A stale PKG_CONFIG_PATH (from other in-tree builds) can hide system .pc files.
unset PKG_CONFIG_PATH || true
rm -rf "$WORK/build-erhai"
meson setup "$WORK/build-erhai" "$SRC" --prefix=/usr --buildtype=release

echo "== building ($JOBS jobs) =="
ninja -C "$WORK/build-erhai" -j "$JOBS"

echo "== staging into $BUILD_DIR =="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
install -m 0755 "$WORK/build-erhai/src/ptyxis" "$BUILD_DIR/ptyxis"

echo "== done =="
echo "  binary   : $BUILD_DIR/ptyxis"
echo "  install  : $HERE/install-ptyxis-erhai.sh apply"
