#!/bin/bash
# Build the erhai patched Mutter package set -- Ubuntu-named packages, like the
# mesa /usr replacement:
#
#   libmutter-18-0          Ubuntu's deb + the patched, stripped library
#   gir1.2-mutter-18        Ubuntu's deb, Version + exact pin re-versioned
#   libmutter-18-0-dbgsym   Ubuntu's dbgsym, Version + exact pin re-versioned
#
# all at <base>+erhai1 (default base 50.1-0ubuntu2.5).  See
# oneplus-erhai-mutter/README.md, and nautilus-MEMORY-2026-10-15.md §5-§7.
# The library carries three source patches: touch-drag-null-guard.patch,
# mutter-hw-cursor-external-monitor.patch (composited cursor while an external
# monitor is active, see mirror-MEMORY-2026-11-07.md §19) and
# mutter-primary-flag-reset.patch (GetCurrentState must report one primary, so
# Settings -> Displays -> Primary Display shows the real one).
#
#   ./build-mutter-deb.sh                       -> publish to the PPA suite
#   NO_PPA=1 ./build-mutter-deb.sh              -> build locally only
#   PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-mutter-deb.sh  -> to the public MAIN suite
#   BASE=50.1-0ubuntu2.5 ./build-mutter-deb.sh  -> after a new Ubuntu mutter update
#
# Unlike the other builders this one does NOT compile mutter: the patched library
# is a verified artifact in oneplus-erhai-mutter/ (a full mutter build cannot be
# packaged on this pad -- dh_shlibdeps refuses on the mesa replacement's libgbm.so.1).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
PROJ="$HERE/oneplus-erhai-mutter"
BASE=${BASE:-50.1-0ubuntu2.5}
SUFFIX=${ERHAI_SUFFIX:-+erhai1}
VERSION="$BASE$SUFFIX"
PORTS_MIRROR=${PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
EXPECT_LIB_MD5=${EXPECT_LIB_MD5:-df62d04160042122d2c7d17643c3fbb8}
LIBREL="/usr/lib/aarch64-linux-gnu"
WORK="$PROJ/build"
PATHS="$WORK/built-paths.txt"

die() { echo "ERROR: $*" >&2; exit 1; }
md5of() { md5sum "$1" | cut -d' ' -f1; }

# 0. the patched library artifact (the whole point of this project)
LIB="$PROJ/libmutter-18.so.0.0.0.patched"
[ -f "$LIB" ] || die "missing $LIB"
lib_md5=$(md5of "$LIB")
[ "$lib_md5" = "$EXPECT_LIB_MD5" ] || die "patched library md5 $lib_md5 != $EXPECT_LIB_MD5"
echo "patched library: md5 OK ($lib_md5)"
# The +erhai2 library must not carry this machine's build prefix: a -Dprefix
# default of /usr/local once pointed the X11 frames helper at
# /usr/local/libexec and every first X11 client took the compositor down
# (mirror-MEMORY-2026-11-07.md §12).  Fail closed.
if strings -a "$LIB" | grep -q '/usr/local'; then
  die "$LIB contains a /usr/local path -- rebuild with -Dprefix=/usr"
fi
echo "no /usr/local path baked in: OK"

mkdir -p "$WORK" "$PROJ/rollback"
: > "$PATHS"

# 1. pristine Ubuntu libmutter-18-0 for $BASE (sha256-checked against apt metadata)
apt_field() { apt-cache show "$1=$2" 2>/dev/null | sed -n "s/^$3: //p" | head -1; }
SRC="$WORK/libmutter-18-0_${BASE}_arm64.deb"
for c in "$PROJ/rollback/libmutter-18-0_${BASE}_arm64.deb" \
         "/var/cache/apt/archives/libmutter-18-0_${BASE}_arm64.deb"; do
  if [ ! -f "$SRC" ] && [ -f "$c" ]; then echo "using $c"; cp "$c" "$SRC"; fi
done
if [ ! -f "$SRC" ]; then
  fn=$(apt_field libmutter-18-0 "$BASE" Filename)
  [ -n "$fn" ] || die "no apt metadata for libmutter-18-0=$BASE (apt lists out of date?)"
  echo "downloading $PORTS_MIRROR/$fn"
  curl -fSL --retry 3 -o "$SRC.part" "$PORTS_MIRROR/$fn"
  mv "$SRC.part" "$SRC"
fi
want=$(apt_field libmutter-18-0 "$BASE" SHA256)
if [ -n "$want" ]; then
  got=$(sha256sum "$SRC" | cut -d' ' -f1)
  [ "$want" = "$got" ] || die "sha256 mismatch for $SRC (want $want, got $got)"
  echo "pristine libmutter-18-0: sha256 OK"
fi

# 2. swap the patched library in, bump the version, re-manifest, repack
TREE="$WORK/build-libmutter-18-0"
[ -n "$TREE" ] && [ "$TREE" != "/" ] || die "refusing: bad tree path"
rm -rf "$TREE"
mkdir -p "$TREE"
dpkg-deb -R "$SRC" "$TREE"
install -m 0755 "$LIB" "$TREE$LIBREL/libmutter-18.so.0.0.0"
python3 - "$TREE/DEBIAN/control" "$BASE" "$SUFFIX" <<'PY'
import re, sys
path, ver, suffix = sys.argv[1:4]
c = open(path).read()
assert re.search(r'^Version: %s$' % re.escape(ver), c, re.M), 'unexpected Version'
c = re.sub(r'^Version: %s$' % re.escape(ver),
           'Version: %s%s' % (ver, suffix), c, count=1, flags=re.M)
note = """Locally rebuilt package: Ubuntu's %s with the oneplus-erhai patched
libmutter-18.so.0.0.0 (compositor touch-drag NULL-deref guard, the composited
cursor while an external monitor is active, and one-primary reporting for the
Primary Display setting).  Depends, Breaks and Symbols are Ubuntu's,
unchanged.""" % ver
note = '\n'.join((' ' + l) if l.strip() else ' .' for l in note.splitlines())
open(path, 'w').write(c.rstrip('\n') + '\n .\n' + note + '\n')
PY
( cd "$TREE" && find . -path ./DEBIAN -prune -o -type f -print0 |
    sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
LIB_DEB="$DEB_DIR/libmutter-18-0_${VERSION}_arm64.deb"
rm -f "$LIB_DEB"
dpkg-deb --root-owner-group -Z zstd -b "$TREE" "$LIB_DEB" >/dev/null
printf '%s\n' "$LIB_DEB" >> "$PATHS"
echo "built $(basename "$LIB_DEB")"

# 3. verify the PACKED result: the library inside must be the patched one, the
#    payload must be root-owned, and its own md5sums manifest must verify
V="$WORK/verify"
rm -rf "$V"
mkdir -p "$V/data" "$V/ctrl"
dpkg-deb -x "$LIB_DEB" "$V/data"
dpkg-deb -e "$LIB_DEB" "$V/ctrl"
packed_md5=$(md5of "$V/data$LIBREL/libmutter-18.so.0.0.0")
[ "$packed_md5" = "$EXPECT_LIB_MD5" ] ||
  die "the library inside $(basename "$LIB_DEB") is $packed_md5, not the patched one"
( cd "$V/data" && md5sum -c --quiet "$V/ctrl/md5sums" ) ||
  die "md5sums mismatch inside $(basename "$LIB_DEB")"
if dpkg-deb -c "$LIB_DEB" | grep -v '[[:space:]]root/root[[:space:]]' >/dev/null; then
  die "not every payload file is root:root in $(basename "$LIB_DEB")"
fi
echo "verified: patched library inside the deb, root/root, md5sums clean"

# 4. the two exact-pin siblings (gir1.2-mutter-18 + libmutter-18-0-dbgsym)
BASE="$BASE" OUT_DIR="$DEB_DIR" PATHS_FILE="$PATHS" DBGSYM_AS_DEB=1 \
  "$PROJ/bump-mutter-siblings.sh"

# 5. keep the pristine Ubuntu debs for offline rollback
install -m644 "$SRC" "$PROJ/rollback/"
GIR_SRC_RO="$PROJ/sibling-bump/gir1.2-mutter-18_${BASE}_arm64.deb"
if [ -f "$GIR_SRC_RO" ]; then install -m644 "$GIR_SRC_RO" "$PROJ/rollback/"; fi
echo

# 6. summary + publish (PPA suite by default; PPA_DEB_DIR selects another one)
mapfile -t BUILT < "$PATHS"
echo "== built ${#BUILT[@]} package(s) =="
for p in "${BUILT[@]}"; do printf '  %s\n' "$(basename "$p")"; done
if [ "${NO_PPA:-0}" = 1 ]; then
  echo "NO_PPA=1: not published"
else
  ppa_copy "${BUILT[@]}"
fi

