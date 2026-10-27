#!/bin/bash
# build-gtk4-deb.sh -- package the erhai-patched libgtk-4 set.
#
# The patched library (gdk/wayland/gdkseat-wayland.c:
# _gdk_wayland_seat_get_implicit_grab_serial() must not dereference a missing
# touch -- see gtk-implicit-grab-null-guard.patch and README.md) is swapped into
# Ubuntu's OWN deb; a full `dpkg-buildpackage gtk4` is NOT usable on this pad
# because dh_shlibdeps refuses on the /usr mesa replacement's libgbm.so.1.
#
# THREE packages, all at <base>+erhai1:
#   libgtk-4-1          Ubuntu's deb + the patched library (+ new md5sums)
#   libgtk-4-dev        only Version + its `libgtk-4-1 (= <base>)` pin bumped
#   libgtk-4-1-dbgsym   only Version + the same pin bumped (payload untouched)
# libgtk-4-dev and libgtk-4-1-dbgsym pin the library EXACTLY, so leaving them at
# <base> would make apt refuse every install/upgrade ("apt --fix-broken
# install", which would remove the desktop).  The dev package's pin on
# gir1.2-gtk-4.0 is deliberately left at <base>: the typelibs are Ubuntu's,
# unchanged.
#
#   ./build-gtk4-deb.sh                     # build (+ publish to TESTING suite)
#   NO_PPA=1 ./build-gtk4-deb.sh            # build locally only
#   PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-gtk4-deb.sh     # straight to MAIN
#   ./build-gtk4-deb.sh --install           # build + dpkg -i all three
#   BASE=4.22.5+ds-0ubuntu0.1 ./build-gtk4-deb.sh           # after an Ubuntu update
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/../../ppa-common.sh" 2>/dev/null || true
DEB_DIR=${DEB_DIR:-$HERE/../debs}       # every built .deb lands in <repo>/debs
mkdir -p "$DEB_DIR"
WORK=${WORK:-$HERE/build}
LIBPKG=libgtk-4-1
DEVPKG=libgtk-4-dev
DBGPKG=libgtk-4-1-dbgsym
SUFFIX=+erhai1
PORTS_MIRROR=${PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
DDEBS_MIRROR=${DDEBS_MIRROR:-http://ddebs.ubuntu.com}
DBGSYM_AS_DEB=${DBGSYM_AS_DEB:-1}
NO_PPA=${NO_PPA:-0}
INSTALL=0
[ "${1:-}" = --install ] && INSTALL=1

say() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# ---- the patched library artifact -------------------------------------------------
LIB=$(ls "$HERE"/libgtk-4.so.1.*.patched 2>/dev/null | head -1) || true
[ -n "$LIB" ] || die "no libgtk-4.so.1.*.patched in $HERE (build GTK4 and strip it first)"
LIB_BASE=$(basename "${LIB%.patched}")
LIB_MD5=$(md5sum "$LIB" | cut -d' ' -f1)
say "artifact: $(basename "$LIB") md5 $LIB_MD5"
# The md5 is AUTHORITATIVE (the same model as oneplus-erhai-mutter): it pins the
# one artifact that was verified against the crash (md5 6095562e…, GTK 4.22.4).
EXPECT_LIB_MD5=${EXPECT_LIB_MD5:-6095562e07c3aa9c741c5c91a48b9bb6}
if [ "$LIB_MD5" != "$EXPECT_LIB_MD5" ]; then
  say "  WARNING: artifact md5 differs from the verified one ($EXPECT_LIB_MD5)"
  say "  (set EXPECT_LIB_MD5=<md5> if you knowingly rebuilt it)"
fi
# Best-effort instruction check (NON-fatal, like the mutter builder): in the
# patched function the g_hash_table_iter_next() call is followed by a
# conditional branch -- in Ubuntu's build the very next instruction dereferences
# `touch' unconditionally (that unguarded `ldr w0, [x0, #32]' IS the crash).
python3 - "$LIB" <<'PY' || say "  note: guard instruction not located (stripped artifact?); the md5 stands"
import re, subprocess, sys
out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', sys.argv[1]],
                     capture_output=True, text=True).stdout
have_sym = re.search(r'<[^>]*get_implicit_grab_serial[^>]*>:', out)
if have_sym:
    body = out.split('<_gdk_wayland_seat_get_implicit_grab_serial>:', 1)[1]
    body = body.split('\n\n', 1)[0]
    sys.exit(0 if re.search(r'iter_next@plt>\n.*\n\s*[0-9a-f]+:\s+(cbz|cbnz|b\.eq|b\.ne)',
                            body) else 1)
lines = out.splitlines()
for i, l in enumerate(lines):
    if 'g_hash_table_iter_next@plt>' in l and i + 1 < len(lines):
        if re.match(r'\s*[0-9a-f]+:\s+(cbz|cbnz|b\.eq|b\.ne)', lines[i + 1]):
            sys.exit(0)
sys.exit(1)
PY
say "guard instruction present"

# ---- pristine Ubuntu packages (sha256-checked against the apt metadata) -----------
apt_field() { apt-cache show "$1=$2" 2>/dev/null | sed -n "s/^$3: //p" | head -1; }
fetch() { # <pkg> <ver> <mirror> <ext> -> local path
  local pkg=$1 ver=$2 mirror=$3 ext=$4 out fname url want got
  mkdir -p "$WORK"
  out="$WORK/${pkg}_${ver}_arm64.${ext}"
  if [ ! -f "$out" ]; then
    fname=$(apt_field "$pkg" "$ver" Filename)
    [ -n "$fname" ] || die "no archive metadata for $pkg=$ver (apt lists stale?)"
    say "downloading $mirror/$fname" >&2
    curl -fSL --retry 3 -o "$out.part" "$mirror/$fname"
    mv "$out.part" "$out"
  fi
  want=$(apt_field "$pkg" "$ver" SHA256)
  if [ -n "$want" ]; then
    got=$(sha256sum "$out" | cut -d' ' -f1)
    [ "$want" = "$got" ] || die "sha256 mismatch for $out (want $want, got $got)"
  fi
  printf '%s' "$out"
# ---- which base version? ----------------------------------------------------------
}
BASE=${BASE:-$(dpkg-query -W -f='${Version}' "$LIBPKG" 2>/dev/null || true)}
BASE=${BASE%$SUFFIX}
[ -n "$BASE" ] || die "cannot determine BASE (pass BASE=<version> or install $LIBPKG)"
say "base $BASE -> ${BASE}${SUFFIX}"

# ---- libgtk-4-1: Ubuntu's deb with the patched library swapped in -----------------
say "== $LIBPKG =="
SRC=$(fetch "$LIBPKG" "$BASE" "$PORTS_MIRROR" deb)
TREE="$WORK/build-$LIBPKG"
rm -rf "$TREE"; mkdir -p "$TREE"
dpkg-deb -R "$SRC" "$TREE"
TARGET="$TREE/usr/lib/aarch64-linux-gnu/$LIB_BASE"
[ -f "$TARGET" ] || die "no $LIB_BASE inside $SRC"
say "  swapping library ($(stat -c%s "$TARGET") -> $(stat -c%s "$LIB") bytes)"
install -m644 "$LIB" "$TARGET"
python3 - "$TREE/DEBIAN/control" "$BASE" "$SUFFIX" <<'PY'
import re, sys
path, base, suffix = sys.argv[1:4]
c = open(path).read()
assert re.search(r'^Version: %s$' % re.escape(base), c, re.M), 'unexpected Version'
c = re.sub(r'^Version: %s$' % re.escape(base), 'Version: %s%s' % (base, suffix),
           c, count=1, flags=re.M)
c = c.rstrip('\n') + (
    '\n .\n'
    ' Locally rebuilt package: the shared library is the erhai build of GTK\n'
    ' %s carrying ONE guard (gdk/wayland/gdkseat-wayland.c:\n'
    ' _gdk_wayland_seat_get_implicit_grab_serial() no longer dereferences a\n'
    ' touch the compositor already cancelled -- the crash behind "a 3-finger\n'
    ' workspace swipe closes Files").  Everything else is Ubuntu\'s %s.\n'
    ' See the oneplus-erhai-gtk4 project directory / README.md.\n' % (base, base))
open(path, 'w').write(c)
PY
# the payload changed -> regenerate the manifest
( cd "$TREE" && find . -path ./DEBIAN -prune -o -type f -print0 |
    sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
OUT_LIB="$DEB_DIR/${LIBPKG}_${BASE}${SUFFIX}_arm64.deb"
rm -f "$OUT_LIB"
dpkg-deb --root-owner-group -Z zstd -b "$TREE" "$OUT_LIB" >/dev/null
# verify the PACKED deb: the library inside must be the artifact, byte for byte
V=$(mktemp -d); dpkg-deb -x "$OUT_LIB" "$V"
got=$(md5sum "$V/usr/lib/aarch64-linux-gnu/$LIB_BASE" | cut -d' ' -f1)
[ "$got" = "$LIB_MD5" ] || die "packed library md5 $got != artifact $LIB_MD5"
rm -rf "$V"
say "  built $(basename "$OUT_LIB") ($(stat -c%s "$OUT_LIB") B)"

# ---- the two siblings: only Version + the exact pin --------------------------------
bump() { # <pkg> <ext> <mirror> <note> -> built path
  local pkg=$1 ext=$2 mirror=$3 note=$4 src tree out as_deb=0
  src=$(fetch "$pkg" "$BASE" "$mirror" "$ext")
  tree="$WORK/build-$pkg"
  rm -rf "$tree"; mkdir -p "$tree"
  dpkg-deb -R "$src" "$tree"
  [ "$ext" = ddeb ] && [ "$DBGSYM_AS_DEB" = 1 ] && as_deb=1
  python3 - "$tree/DEBIAN/control" "$BASE" "$SUFFIX" "$note" "$as_deb" "$LIBPKG" <<'PY'
import re, sys
path, base, suffix, note, as_deb, libpkg = sys.argv[1:7]
c = open(path).read()
assert re.search(r'^Version: %s$' % re.escape(base), c, re.M), 'unexpected Version'
assert '%s (= %s)' % (libpkg, base) in c, 'unexpected dependency'
c = re.sub(r'^Version: %s$' % re.escape(base), 'Version: %s%s' % (base, suffix),
           c, count=1, flags=re.M)
c = c.replace('%s (= %s)' % (libpkg, base), '%s (= %s%s)' % (libpkg, base, suffix))
if as_deb == '1':
    c = re.sub(r'^Package-Type: ddeb\n', '', c, count=1, flags=re.M)
note = '\n'.join((' ' + l) if l.strip() else ' .' for l in note.strip().splitlines())
open(path, 'w').write(c.rstrip('\n') + '\n .\n' + note + '\n')
PY
  ( cd "$tree" && md5sum -c --quiet DEBIAN/md5sums ) || die "$pkg: payload md5sums mismatch"
  [ "$as_deb" = 1 ] && ext=deb
  out="$DEB_DIR/${pkg}_${BASE}${SUFFIX}_arm64.${ext}"
  rm -f "$out"
  dpkg-deb --root-owner-group -Z zstd -b "$tree" "$out" >/dev/null
  # NOTE: progress goes to STDERR only -- this function's stdout is captured by
  # the caller's $(...) (the mutter sibling script famously shipped a "package"
  # that was a string) */
  say "  built $(basename "$out") ($(stat -c%s "$out") B)" >&2
  printf '%s' "$out"
}

say "== $DEVPKG =="
OUT_DEV=$(bump "$DEVPKG" deb "$PORTS_MIRROR" \
"Locally rebuilt package: only the version and its exact pin on $LIBPKG are bumped
to ${BASE}${SUFFIX} (the patched library).  Payload unchanged.
See /usr/share/doc/oneplus-erhai-gtk4/README.md.")

say "== $DBGPKG =="
if dpkg-query -W -f='${Status}' "$DBGPKG" 2>/dev/null | grep -q '^install ok' || [ "$DBGSYM_AS_DEB" = 1 ]; then
  OUT_DBG=$(bump "$DBGPKG" ddeb "$DDEBS_MIRROR" \
"Locally rebuilt package: only the version and the exact pin on $LIBPKG are bumped
to ${BASE}${SUFFIX}.  The symbols inside are those of the UNPATCHED ${BASE} build.
See /usr/share/doc/oneplus-erhai-gtk4/README.md.")
else
  say "  not installed here -- skipped"
  OUT_DBG=""
fi

say
say "built: $(basename "$OUT_LIB") $(basename "$OUT_DEV")${OUT_DBG:+ $(basename "$OUT_DBG")}"
say "sha256: $(sha256sum "$OUT_LIB" | cut -d' ' -f1)"
[ -n "$OUT_DEV" ] && say "        $(sha256sum "$OUT_DEV" | cut -d' ' -f1)"
[ -n "$OUT_DBG" ] && say "        $(sha256sum "$OUT_DBG" | cut -d' ' -f1)"

if [ "$INSTALL" = 1 ]; then
  say "installing (dpkg -i) ..."
  # shellcheck disable=SC2086
  sudo dpkg -i "$OUT_LIB" "$OUT_DEV" ${OUT_DBG:+"$OUT_DBG"}
  say "apt-get -s check:"; apt-get -s check 2>&1 | sed 's/^/  /' || true
fi

if [ "$NO_PPA" = 1 ]; then
  say "NO_PPA=1: local only (nothing copied/published)"
  exit 0
fi
: "${PPA_DEB_DIR:?set PPA_DEB_DIR or NO_PPA=1}"
for d in "$OUT_LIB" "$OUT_DEV" ${OUT_DBG:+"$OUT_DBG"}; do
  install -m644 "$d" "$PPA_DEB_DIR/"
  say "copied $(basename "$d") -> $PPA_DEB_DIR"
done
say "now run:  (cd .. && ./update-ppa.sh)"