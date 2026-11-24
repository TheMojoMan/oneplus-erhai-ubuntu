#!/bin/bash
# Build vlc_<version>_arm64.deb: VLC 4 from the upstream git tree.
#
#   ./build-vlc-deb.sh        -> debs/vlc_<version>_arm64.deb
#
# The source tree (VLC_SRC, default /media/data/develop/vlc) must be configured
# for a system install and already built:
#
#   ./configure --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu \
#               --enable-vaapi --enable-avcodec --disable-kde
#   make -j$(nproc)
#
# The script re-runs `make -j` (a no-op when the tree is current) and installs
# into a scratch DESTDIR -- the running system is never touched.
#
# Why this package name: it is `vlc`, and its version carries a '~' before the
# git snapshot (4.0.0~git<date>.<commits>+<sha>-0erhai<N>), so any later
# 4.0.0/4.0.x from VideoLAN or Ubuntu sorts higher and upgrades over it, while
# it still outranks Ubuntu's 3.0.23.  It ships no wrapper: oneplus-erhai-av1fix
# owns the dpkg-diverted /usr/bin/vlc shim and reinstalls it from a file trigger
# after every VLC upgrade.
#
# Payload = the runtime tree only (no headers, .pc files, static libs, .la or
# unversioned .so symlinks), so it does not collide with Ubuntu's -dev package
# names.  The vlc 3 packages it replaces are listed in Breaks/Replaces/
# Conflicts in DEBIAN/control.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"

SRC="$HERE/vlc"				# packaging metadata (DEBIAN/)
VLC_SRC=${VLC_SRC:-/media/data/develop/vlc}
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")
DEB_DIR="$HERE/debs"
STAGE="$HERE/../../tmp/vlc-deb-stage"
LIBDIR=usr/lib/aarch64-linux-gnu

[ -x "$VLC_SRC/bin/vlc" ] || { echo "missing $VLC_SRC/bin/vlc" >&2; exit 1; }
[ -f "$VLC_SRC/Makefile" ] || { echo "missing $VLC_SRC/Makefile" >&2; exit 1; }
grep -qx 'prefix = /usr' "$VLC_SRC/Makefile" || {
	echo "$VLC_SRC is not configured with --prefix=/usr" >&2
	grep -m1 '^prefix' "$VLC_SRC/Makefile" >&2
	exit 1
}
grep -qx 'libdir = /usr/lib/aarch64-linux-gnu' "$VLC_SRC/Makefile" || {
	echo "$VLC_SRC is not configured with --libdir=/usr/lib/aarch64-linux-gnu" >&2
	grep -m1 '^libdir' "$VLC_SRC/Makefile" >&2
	exit 1
}

echo "==> make (tree: $VLC_SRC)"
make -C "$VLC_SRC" -j"$(nproc)"

# --------------------------------------------------------------- stage ------
chmod -R u+w "$STAGE" 2>/dev/null || true
rm -rf "$STAGE"
mkdir -p "$STAGE"
echo "==> make install DESTDIR=$STAGE"
make -C "$VLC_SRC" install DESTDIR="$STAGE" >/dev/null

# Runtime tree only.  .la/.a and the unversioned .so symlinks are libtool/dev
# artifacts (libvlc-dev owns those names on Ubuntu); headers and .pc files would
# only serve a vlc dev package we do not ship.
find "$STAGE" -name '*.la' -delete
find "$STAGE" -name '*.a' -delete
rm -f "$STAGE/$LIBDIR/libvlc.so" "$STAGE/$LIBDIR/libvlccore.so"
rm -rf "$STAGE/usr/include" "$STAGE/usr/lib/pkgconfig" \
	"$STAGE/$LIBDIR/pkgconfig"
# The tree's pre-built plugin cache records the build-time plugin state; a
# trimmed payload makes it stale, and VLC then rescans (and complains) on every
# start.  Ship none -- the user cache under ~/.cache/vlc is fine.
rm -f "$STAGE/$LIBDIR/vlc/plugins/plugins.dat"

for f in "$STAGE/usr/bin/vlc" "$STAGE/$LIBDIR/libvlc.so."*.*.* \
	"$STAGE/$LIBDIR/vlc/plugins/codec/libavcodec_plugin.so" \
	"$STAGE/$LIBDIR/vlc/plugins/codec/libvaapi_plugin.so" \
	"$STAGE/usr/share/applications/vlc.desktop"; do
	[ -e "$f" ] || { echo "payload sanity: missing $f" >&2; exit 1; }
done

install -d -m755 "$STAGE/DEBIAN"
cp -a "$SRC/DEBIAN/." "$STAGE/DEBIAN/"

# ------------------------------------------------- Depends / Installed-Size --
# ldd + dpkg -S over every ELF in the payload (same approach as the av1fix
# builder): anything that resolves inside the stage is this package, the rest
# maps to the Ubuntu package that owns the library.
elfs=$(mktemp)
find "$STAGE/usr/bin" "$STAGE/usr/lib" -type f -print0 |
while IFS= read -r -d '' f; do
	case $(file -b --mime-type "$f" 2>/dev/null) in
	application/x-sharedlib|application/x-executable|application/x-pie-executable)
		printf '%s\n' "$f" ;;
	esac
done > "$elfs"

deps=$(
	LD_LIBRARY_PATH="$STAGE/$LIBDIR:/usr/lib/aarch64-linux-gnu" \
	xargs -a "$elfs" -d '\n' ldd 2>/dev/null |
	awk '/=> \// {print $3}' | sort -u |
	while IFS= read -r lib; do
		case "$lib" in "$STAGE"*) continue ;; esac
		dpkg -S "$lib" 2>/dev/null | head -1 | cut -d: -f1
	done | sort -u | tr '\n' ' ')
rm -f "$elfs"
deps="${deps% }"
[ -n "$deps" ] || { echo "could not compute Depends" >&2; exit 1; }
awk -v d="Depends: ${deps// /, }" '
	/^Depends: / { print d; seen = 1; next }
	/^Description: / && !seen { print d; seen = 1 }
	{ print }
' "$STAGE/DEBIAN/control" > "$STAGE/DEBIAN/control.new"
mv "$STAGE/DEBIAN/control.new" "$STAGE/DEBIAN/control"
grep -q '^Depends: ' "$STAGE/DEBIAN/control" ||
	{ echo "Depends did not land in control" >&2; exit 1; }
echo "==> Depends: ${deps// /, }"

size=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
sed -i "s|^Installed-Size: .*|Installed-Size: $size|" "$STAGE/DEBIAN/control"

# owner rule: no debug info, no build path in the payload
ppa_scrub_debug "$STAGE"

( cd "$STAGE"
  find usr -type f -exec md5sum {} \; > DEBIAN/md5sums )
chmod 644 "$STAGE/DEBIAN/control" "$STAGE/DEBIAN/md5sums"

dpkg-deb --build --root-owner-group "$STAGE" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
echo "built ${PKG}_${VER}_${ARCH}.deb"
