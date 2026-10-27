#!/bin/bash
# Build steam_<version>_arm64.deb -- the erhai-packaged native arm64 Steam
# client: Valve's BOOTSTRAP installed to /usr/lib/steam plus a /usr/bin/steam
# launcher that seeds it into the user's own writable Steam root on the first
# start, where the client then downloads the rest of itself.
#
#   ./build-steam-deb.sh                 -> local deb in oneplus-erhai-ubuntu/
#   PUBLISH=1 ./build-steam-deb.sh       -> also ppa_copy to the public MAIN
#                                           suite (universal: no Ubuntu ABI, so
#                                           every release can see it)
#   SKIP_STAGE=1 ./build-steam-deb.sh    -> repack the already staged payload
#
#   STEAM_SRC=<dir>      source installation (default /media/data/PC-Games/Steam)
#   DEB_COMPRESS='-Zzstd -z3'   dpkg-deb compression flags
#
# WHY THE VERSION IS 1:2.0-1erhai1 (owner request, 2026-10-20: "bump the version
# to 2.0 so that a Ubuntu update e.g. to 1.0.0.52 does not overwrite our steam
# version"):
#  * The EPOCH 1 is what actually protects the package.  Ubuntu's steam FAMILY
#    (steam-devices / steam-installer in resolute = 1:1.0.0.85~ds-2build1, the
#    same source package their `steam` binary would come from) carries epoch 1,
#    and an epoch beats ANY epoch-less version: measured with dpkg
#    --compare-versions, a plain "2.0-1erhai1" is NOT greater than
#    1:1.0.0.85~ds-2build1, while "1:2.0-1erhai1" is greater than it AND than
#    1.0.0.52.  If Ubuntu ever raises its epoch, ours has to be raised above it.
#  * The upstream part is 2.0 so that a later steam 1.0.0.52 (or any 1.0.0.x)
#    cannot sort above ours either.
#  * The revision is this project's build counter (-<n>erhai1); the next build
#    of the same upstream version is -2erhai1, -3erhai1, ...
#  * NEVER go back to a bare 1.0.0.x: Ubuntu's steam-devices declares
#    "Breaks: steam (<< 1.0.0.51-1~)", so anything below that floor makes dpkg
#    REFUSE to configure this package wherever steam-devices is installed
#    (measured: "dependency problems prevent configuration of steam").
#    1:2.0-1erhai1 satisfies it (epoch 1 > 0), so both constraints hold at once.
#
# WHAT IS SHIPPED: the BOOTSTRAP ONLY -- steamrtarm64/steam (a 10 MB aarch64
# binary), the steamrt3c sentinel, the beta selector and an empty Steam-root
# skeleton.  The client downloads the rest of itself on the first start:
# measured from exactly this bootstrap on a scratch HOME (2026-10-17) the
# client pulled 669 522 KB = 654 MB and unpacked a 3.1 GB client, steamrt3c
# container runtime included.  Shipping that tree would duplicate what Valve
# already delivers to every user, so stage() copies an explicit file list and
# guards() enforces an ALLOWLIST over the payload -- the payload cannot grow
# into a client tree by accident (and no account data can be in a bag this
# small: the 13 250-file / 2.3 GB tree build is gone).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"

SRC="$HERE/steam"
STEAM_SRC="${STEAM_SRC:-/media/data/PC-Games/Steam}"
PAYLOAD="$SRC/usr/lib/steam"
CLIENT_MANIFEST="package/steam_client_publicbeta_linuxarm64.manifest"

[ -f "$SRC/DEBIAN/control" ] || { echo "missing $SRC/DEBIAN/control" >&2; exit 1; }
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

# --------------------------------------------------------------- staging -----
# The bootstrap: the one file Valve's steam_steamrt_linuxarm64 group ships,
# plus what the client cannot fetch for itself.
stage() {
	[ -s "$STEAM_SRC/steamrtarm64/steam" ] ||
		{ echo "no arm64 Steam bootstrap in $STEAM_SRC" >&2; exit 1; }
	echo "staging the bootstrap from $STEAM_SRC"
	# Only the payload directory is cleaned; the static package files
	# (usr/bin, usr/share, etc) are hand-maintained and stay.
	rm -rf "$PAYLOAD"
	install -d "$PAYLOAD/steamrtarm64"
	install -m0755 "$STEAM_SRC/steamrtarm64/steam" "$PAYLOAD/steamrtarm64/steam"

	# Selects Valve's steamrt3c (container) client; the launcher re-touches it
	# on every start, and the client reads it before it downloads anything.
	install -m0644 /dev/null "$PAYLOAD/.steam-enable-steamrt64-client"

	# A Steam root always has these; ship them empty so the client finds its
	# usual skeleton instead of having to create it on the very first start.
	local d
	for d in config steamapps userdata logs appcache depotcache friends \
		 steam/cached package; do
		install -d "$PAYLOAD/$d"
	done

	# Diagnostic stamp: our version + the client build this bootstrap came from
	# (read from the source install's own manifest).
	local cver=unknown
	if [ -f "$STEAM_SRC/$CLIENT_MANIFEST" ]; then
		cver=$(awk -F'"' '/^[[:space:]]*"version"/ {print $4; exit}' \
			"$STEAM_SRC/$CLIENT_MANIFEST")
		[ -n "$cver" ] || cver=unknown
	fi
	printf '%s %s\n' "$VER" "$cver" >"$PAYLOAD/VERSION"
	echo "payload stamp: $(cat "$PAYLOAD/VERSION")"
}

if [ "${SKIP_STAGE:-0}" != 1 ]; then
	stage
fi

# ------------------------------------------------------------------- icon ----
# public/steam_tray_posix.png (128x128) is Valve's own client icon, taken from
# the SOURCE install -- the payload is a bootstrap and does not carry it.
make_icon() {
	local src="$STEAM_SRC/public/steam_tray_posix.png" size dir
	[ -f "$src" ] ||
		{ echo "no icon source ($src); keeping the icons in the package tree" >&2; return 0; }
	command -v convert >/dev/null 2>&1 ||
		{ echo "ImageMagick convert missing; keeping existing icon" >&2; return 0; }
	for size in 48 128 256; do
		dir="$SRC/usr/share/icons/hicolor/${size}x${size}/apps"
		install -d "$dir"
		convert "$src" -resize "${size}x${size}" -strip "PNG32:$dir/steam.png"
	done
	echo "icon: $(identify -format '%wx%h' \
		"$SRC/usr/share/icons/hicolor/256x256/apps/steam.png")"
}
make_icon

# ------------------------------------------------------------------- perms ---
# Files are handed to dpkg in a canonical shape: 0644, or 0755 when the
# executable bit is set.  --root-owner-group then makes them root:root, so the
# staging tree's own ownership never reaches the package.
find "$SRC/usr/lib/steam" -type d -exec chmod 0755 {} +
find "$SRC/usr/lib/steam" -type f ! -perm -u+x -exec chmod 0644 {} +
find "$SRC/usr/lib/steam" -type f -perm -u+x -exec chmod 0755 {} +
find "$SRC" -type d -exec chmod 0755 {} +
chmod 0755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm" "$SRC/usr/bin/steam"
chmod 0644 "$SRC/DEBIAN/control" "$SRC/DEBIAN/conffiles" \
	"$SRC/etc/steam/launcher.conf"
chmod -R u=rwX,go=rX "$SRC/usr/share"

# ------------------------------------------------------------------ guards ---
guards() {
	local fail=0 p

	# 1. ALLOWLIST.  A bootstrap is small enough to describe exactly, so this
	#    guard is exact: whatever the client downloads for itself must NOT be
	#    here, the skeleton dirs must be EMPTY, and a staging mistake (or a
	#    future edit that starts copying a whole client tree) cannot pass.
	python3 - "$PAYLOAD" <<'PY' || fail=1
import os, sys
root = sys.argv[1]
files = {'.steam-enable-steamrt64-client', 'VERSION', 'steamrtarm64/steam'}
dirs = {'config', 'steamapps', 'userdata', 'logs', 'appcache', 'depotcache',
        'friends', 'package', 'steam', 'steam/cached', 'steamrtarm64'}
# dirs that legitimately hold something; the rest must be empty skeletons
filled = {'steamrtarm64', 'steam'}
bad = []
for dirpath, _dirnames, filenames in os.walk(root):
    rel = os.path.relpath(dirpath, root)
    rel = '' if rel == '.' else rel
    if rel not in dirs and rel != '':
        bad.append('unexpected directory: ' + rel + '/')
    for f in filenames:
        r = os.path.join(rel, f) if rel else f
        if r not in files:
            bad.append('unexpected file: ' + r)
for d in sorted(dirs):
    p = os.path.join(root, d)
    if not os.path.isdir(p):
        bad.append('missing directory: ' + d + '/')
    elif d not in filled and os.listdir(p):
        bad.append('skeleton directory is not empty: ' + d + '/')
for f in sorted(files):
    if not os.path.isfile(os.path.join(root, f)):
        bad.append('missing file: ' + f)
if bad:
    print('GUARD FAIL: the payload is not the bootstrap:', file=sys.stderr)
    for b in bad:
        print('  ' + b, file=sys.stderr)
    sys.exit(1)
print('guards: payload is exactly the bootstrap (%d entries)' % (len(files) + len(dirs)))
PY

	# 1b. It must also stay bootstrap-sized: a client tree here means the
	#     staging step copied the wrong thing.
	local psize
	psize=$(du -sm "$PAYLOAD" | cut -f1)
	if [ "$psize" -gt 50 ]; then
		echo "GUARD FAIL: payload is ${psize} MB, expected a ~10 MB bootstrap" >&2
		fail=1
	fi

	# 2. No account reference can survive in a bootstrap, but check anyway.
	while IFS= read -r -d '' f; do
		if grep -aqE 'themagicmojoman|76561198327511705|ConnectCache' "$f"; then
			echo "GUARD FAIL: personal marker in ${f#"$SRC"/}" >&2
			fail=1
		fi
	done < <(find "$PAYLOAD" -type f -size -1M -print0)

	# 3. Everything the launcher and the client need must be there.
	for p in usr/bin/steam usr/lib/steam/VERSION \
		 usr/lib/steam/.steam-enable-steamrt64-client \
		 usr/lib/steam/steamrtarm64/steam \
		 usr/share/applications/steam.desktop \
		 usr/share/icons/hicolor/128x128/apps/steam.png \
		 etc/steam/launcher.conf; do
		if [ ! -e "$SRC/$p" ]; then
			echo "GUARD FAIL: missing $p" >&2
			fail=1
		fi
	done

	# 4. The client that will RUN must really be aarch64 (the whole point).
	if [ -e "$SRC/usr/lib/steam/steamrtarm64/steam" ]; then
		file -b "$SRC/usr/lib/steam/steamrtarm64/steam" | grep -q 'ARM aarch64' ||
			{ echo "GUARD FAIL: steamrtarm64/steam is not aarch64" >&2; fail=1; }
	fi

	# 5. Maintainer scripts and the launcher must be executable + shell-clean.
	for p in DEBIAN/postinst DEBIAN/postrm usr/bin/steam; do
		bash -n "$SRC/$p" ||
			{ echo "GUARD FAIL: $p is not valid shell" >&2; fail=1; }
	done
	for p in DEBIAN/postinst DEBIAN/postrm usr/bin/steam; do
		[ -x "$SRC/$p" ] ||
			{ echo "GUARD FAIL: $p not executable" >&2; fail=1; }
	done

	# 6. The erhai-forget-location block is SHARED between postinst and postrm:
	#    dpkg runs the ALREADY-INSTALLED package's postrm on a removal, so the
	#    install path has to clean the same state, and the two copies must not
	#    drift apart.
	local blk fi_blk fr_blk
	blk=$(mktemp -d "${HERE}/../../tmp/steam-guard.XXXXXX")
	fi_blk="$blk/postinst"
	fr_blk="$blk/postrm"
	sed -n '/^# >>> erhai-forget-location$/,/^# <<< erhai-forget-location$/p' \
		"$SRC/DEBIAN/postinst" >"$fi_blk"
	sed -n '/^# >>> erhai-forget-location$/,/^# <<< erhai-forget-location$/p' \
		"$SRC/DEBIAN/postrm" >"$fr_blk"
	if [ ! -s "$fi_blk" ] || [ ! -s "$fr_blk" ]; then
		echo "GUARD FAIL: erhai-forget-location block missing from postinst or postrm" >&2
		fail=1
	elif ! diff -q "$fi_blk" "$fr_blk" >/dev/null; then
		echo "GUARD FAIL: postinst and postrm erhai-forget-location blocks differ" >&2
		fail=1
	else
		echo "guards: forget-location block identical in postinst/postrm ($(wc -l <"$fi_blk") lines)"
	fi
	rm -f "$fi_blk" "$fr_blk"
	rmdir "$blk" 2>/dev/null || true

	[ "$fail" = 0 ] || { echo "refusing to build" >&2; exit 1; }
	echo "guards: clean"
}
guards


# ------------------------------------------------------- manifest, build -----
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )

# The .deb FILE is named WITHOUT the epoch -- the archive convention, and the
# reason the epoch stays in the control field only: Ubuntu files their own
# epoch-1 package as steam-devices_1.0.0.85~ds-2build1_all.deb while its
# Version: reads "1:1.0.0.85~ds-2build1" (see /var/lib/apt/lists/*_Packages).
# A literal ':' in the name would travel into the PPA's download URL, and apt
# compares the VERSION FIELD, so the epoch still wins every comparison.
DEB_VER="${VER#*:}"
DEB="$DEB_DIR/${PKG}_${DEB_VER}_${ARCH}.deb"
rm -f "$DEB"
echo "packing $(find "$SRC" -type f | wc -l) files, $(du -sh "$SRC" | cut -f1) staged..."
dpkg-deb --build --root-owner-group ${DEB_COMPRESS:--Zzstd -z3} "$SRC" "$DEB"

SIZE=$(stat -c %s "$DEB")
printf 'built %s (%s bytes, %s)\n' "${DEB##*/}" "$SIZE" \
	"$(numfmt --to=iec "$SIZE")"
if [ "$SIZE" -gt 104857600 ]; then
	echo "NOTE: this deb is larger than 100 MB, so it CANNOT be published to" >&2
	echo "the GitHub-Pages-based PPA (per-file limit). Install it locally with" >&2
	echo "'sudo apt install ./${DEB##*/}' (or dpkg -i); publishing needs a" >&2
	echo "different host." >&2
fi

if [ "${PUBLISH:-0}" = 1 ]; then
	ppa_copy "$DEB"
else
	echo "(local; PUBLISH=1 puts it into the main suite, update-ppa.sh --mv does it too)"
fi

