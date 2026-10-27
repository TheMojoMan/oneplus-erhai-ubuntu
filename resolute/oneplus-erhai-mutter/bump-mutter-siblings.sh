#!/bin/bash
# bump-mutter-siblings.sh — keep apt CONSISTENT with the locally patched libmutter-18-0.
#
# WHY THIS EXISTS
#   libmutter-18-0 is patched locally as <base>+erhai1 (the erhai compositor
#   touch-drag guard, see touch-drag-null-guard.patch).  Two packages built from
#   the SAME mutter source pin it EXACTLY, "libmutter-18-0 (= <base>)":
#
#     gir1.2-mutter-18        a RUNTIME dependency of gnome-shell  -> must stay
#     libmutter-18-0-dbgsym   debug symbols, optional
#
#   Installing the patched library with `dpkg -i` leaves those two with an
#   unsatisfiable dependency.  dpkg does not mind, but apt treats the whole system
#   as broken and REFUSES every install/upgrade with
#       "You might want to run 'apt --fix-broken install' to correct these."
#   and that command would REMOVE gnome-shell, gdm3, ubuntu-session and
#   ubuntu-desktop (the exact pins cannot be satisfied by any available version).
#   Never run it.
#
# WHAT IT DOES
#   Re-versions the siblings to <base>+erhai1 with their exact dependency updated
#   to match, by unpacking the pristine archive debs (dpkg-deb -R), patching only
#   Version/Depends/Description and repacking (dpkg-deb -b --root-owner-group).
#   The payload is untouched, so the shipped DEBIAN/md5sums stays valid and
#   `dpkg -V` stays silent.  A full `dpkg-buildpackage` of mutter is NOT an option
#   on this pad: dh_shlibdeps refuses on the /usr mesa replacement's libgbm.so.1
#   (same reason the library itself is patched surgically — see README.md).
#
# USAGE
#   ./bump-mutter-siblings.sh                  # infer BASE from the installed lib
#   ./bump-mutter-siblings.sh --install        # build + dpkg -i (apt consistent again)
#   ./bump-mutter-siblings.sh --dbgsym-drop    # purge the dbgsym instead of bumping it
#   BASE=50.1-0ubuntu2.5 ./bump-mutter-siblings.sh      # explicit base version
#   ./bump-mutter-siblings.sh --base 50.1-0ubuntu2.5    # same thing
#
# ENV (used by build-mutter-deb.sh)
#   BASE=<ver>          base version to build for (else the installed library's)
#   OUT_DIR=<dir>       where the built packages go (default: <repo>/debs)
#   PATHS_FILE=<file>   append every built package path to this file (one per line)
#   DBGSYM_AS_DEB=1     name the dbgsym package *.deb and drop `Package-Type: ddeb`
#                       (dpkg-scanpackages/PPA indexing only picks up *.deb)
#   PORTS_MIRROR / DDEBS_MIRROR   archive mirrors of the pristine packages
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$SCRIPT_DIR/sibling-bump}
OUT_DIR=${OUT_DIR:-$SCRIPT_DIR/../debs}
PATHS_FILE=${PATHS_FILE:-}
SUFFIX=${ERHAI_SUFFIX:-+erhai1}
PORTS_MIRROR=${PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
DDEBS_MIRROR=${DDEBS_MIRROR:-http://ddebs.ubuntu.com}
LIBPKG="libmutter-18-0"
DBGPKG="libmutter-18-0-dbgsym"
DBGSYM_AS_DEB=${DBGSYM_AS_DEB:-0}

INSTALL=0
DBGSYM_DROP=0
EXPLICIT_BASE=0
[ -n "${BASE:-}" ] && EXPLICIT_BASE=1
while [ $# -gt 0 ]; do
  case "$1" in
    --install)     INSTALL=1 ;;
    --dbgsym-drop) DBGSYM_DROP=1 ;;
    --base)        BASE=${2:?--base needs a version}; EXPLICIT_BASE=1; shift ;;
    --base=*)      BASE=${1#--base=}; EXPLICIT_BASE=1 ;;
    *) echo "usage: $0 [--install|--dbgsym-drop|--base <ver>]" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$WORK" ] && [ "$WORK" != "/" ] || { echo "refusing: bad WORK dir" >&2; exit 1; }
[ -n "$OUT_DIR" ] && [ "$OUT_DIR" != "/" ] || { echo "refusing: bad OUT_DIR" >&2; exit 1; }

say() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
inst_ver() { dpkg-query -W -f='${Version}' "$1" 2>/dev/null || true; }
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q '^install ok'; }
note_path() { [ -n "$PATHS_FILE" ] && printf '%s\n' "$1" >> "$PATHS_FILE"; return 0; }

# ---- which base version? ----------------------------------------------------------
LIBV=$(inst_ver "$LIBPKG")
if [ "$EXPLICIT_BASE" = 1 ]; then
  [ -n "${BASE:-}" ] || die "BASE is empty"
else
  [ -n "$LIBV" ] || die "$LIBPKG is not installed -- pass BASE=<version> instead"
  BASE=${LIBV%$SUFFIX}
  if [ "$BASE" = "$LIBV" ]; then
    say "$LIBPKG is at $LIBV (unpatched) -- the siblings need no bumping."
    say "Nothing to do (install the patched library first, or pass BASE= to prepare a build)."
    exit 0
  fi
  [ "$LIBV" = "${BASE}${SUFFIX}" ] ||
    die "$LIBPKG is '$LIBV': expected '${BASE}${SUFFIX}' (pass BASE= explicitly)"
fi
say "base $BASE -> siblings ${BASE}${SUFFIX}"
if [ -n "$LIBV" ] && [ "$LIBV" != "${BASE}${SUFFIX}" ]; then
  say "note: the INSTALLED $LIBPKG is $LIBV, not ${BASE}${SUFFIX}"
fi

say "apt-get -s check before:"
apt-get -s check 2>&1 | sed 's/^/    /' || true

# ---- fetch the pristine siblings from the archives --------------------------------
apt_field() { apt-cache show "$1=$2" 2>/dev/null | sed -n "s/^$3: //p" | head -1; }

fetch() { # <pkg> <ver> <mirror> <ext>  -> prints the local file path
  local pkg=$1 ver=$2 mirror=$3 ext=$4 out fname url want got
  out="$WORK/${pkg}_${ver}_arm64.${ext}"
  mkdir -p "$WORK"
  if [ ! -f "$out" ] && [ -f "/var/cache/apt/archives/${pkg}_${ver}_arm64.${ext}" ]; then
    cp "/var/cache/apt/archives/${pkg}_${ver}_arm64.${ext}" "$out"
  fi
  if [ ! -f "$out" ]; then
    fname=$(apt_field "$pkg" "$ver" Filename)
    [ -n "$fname" ] || die "no archive metadata for $pkg=$ver (apt lists out of date?)"
    url="$mirror/$fname"
    say "downloading $url" >&2
    curl -fSL --retry 3 -o "$out.part" "$url"
    mv "$out.part" "$out"
  else
    say "using cached $out" >&2
  fi
  want=$(apt_field "$pkg" "$ver" SHA256)
  if [ -n "$want" ]; then
    got=$(sha256sum "$out" | cut -d' ' -f1)
    [ "$want" = "$got" ] || die "sha256 mismatch for $out (want $want, got $got)"
    say "  sha256 OK ($got)" >&2
  fi
  printf '%s' "$out"
}

# ---- patch one sibling and build the bumped package --------------------------------
bump() { # <pkg> <ver> <pristine-file> <note...>  -> prints the built path
  local pkg=$1 ver=$2 src=$3 note=$4
  local tree="$WORK/build-$pkg" ext="${src##*.}" out as_deb=0
  [ -n "$tree" ] && [ "$tree" != "/" ] && [ -n "$WORK" ] || die "refusing: bad tree path"
  rm -rf "$tree"
  mkdir -p "$tree"
  dpkg-deb -R "$src" "$tree"
  [ -f "$tree/DEBIAN/control" ] || die "no DEBIAN/control in $src"

  # Publication caveat: dpkg-scanpackages (and so the PPA index) only looks at
  # *.deb, so a dbgsym that is meant to be published must be a plain .deb.
  if [ "$DBGSYM_AS_DEB" = 1 ] && [ "$ext" = ddeb ]; then as_deb=1; fi

  python3 - "$tree/DEBIAN/control" "$ver" "$SUFFIX" "$note" "$as_deb" <<'PY'
import re, sys
path, ver, suffix, note, as_deb = sys.argv[1:6]
c = open(path).read()
assert re.search(r'^Version: %s$' % re.escape(ver), c, re.M), 'unexpected Version'
assert 'libmutter-18-0 (= %s)' % ver in c, 'unexpected dependency'
c = re.sub(r'^Version: %s$' % re.escape(ver),
           'Version: %s%s' % (ver, suffix), c, count=1, flags=re.M)
c = c.replace('libmutter-18-0 (= %s)' % ver, 'libmutter-18-0 (= %s%s)' % (ver, suffix))
if as_deb == '1':
    c = re.sub(r'^Package-Type: ddeb\n', '', c, count=1, flags=re.M)
note = '\n'.join((' ' + l) if l.strip() else ' .' for l in note.strip().splitlines())
open(path, 'w').write(c.rstrip('\n') + '\n .\n' + note + '\n')
PY

  # the payload is bit-identical to the shipped one, so its md5sums still apply
  ( cd "$tree" && md5sum -c --quiet DEBIAN/md5sums ) || die "$pkg: payload md5sums mismatch"

  if [ "$as_deb" = 1 ]; then ext=deb; fi
  out="$OUT_DIR/${pkg}_${ver}${SUFFIX}_arm64.${ext}"
  mkdir -p "$OUT_DIR"
  rm -f "$out"
  dpkg-deb --root-owner-group -Z zstd -b "$tree" "$out" >/dev/null
  say "built $out" >&2
  dpkg-deb -f "$out" Package Version Depends | sed 's/^/    /' >&2
  note_path "$out"
  printf '%s' "$out"
}

# ---- the two siblings -------------------------------------------------------------
say "== gir1.2-mutter-18 =="
GIR_SRC=$(fetch gir1.2-mutter-18 "$BASE" "$PORTS_MIRROR" deb)
GIR_DEB=$(bump gir1.2-mutter-18 "$BASE" "$GIR_SRC" \
"Locally rebuilt package: its exact dependency on libmutter-18-0 is bumped to
${BASE}${SUFFIX}, the locally patched library (erhai compositor touch-drag guard).
The payload is the unmodified Ubuntu build.")
say

DBG_DEB=""
if [ "$DBGSYM_DROP" = 1 ]; then
  say "== libmutter-18-0-dbgsym: --dbgsym-drop requested =="
elif installed "$DBGPKG" || [ "$DBGSYM_AS_DEB" = 1 ]; then
  installed "$DBGPKG" || say "note: $DBGPKG is not installed here; building it for publication"
  say "== libmutter-18-0-dbgsym =="
  DBG_SRC=$(fetch "$DBGPKG" "$BASE" "$DDEBS_MIRROR" ddeb)
  DBG_DEB=$(bump "$DBGPKG" "$BASE" "$DBG_SRC" \
"Locally rebuilt package: only the version and the exact dependency on libmutter-18-0
are bumped to ${BASE}${SUFFIX}.  The debug symbols inside are those of the UNPATCHED
${BASE} build (see Build-Ids) and do NOT match the patched library's build-id; the
patched library's own symbols live in
/usr/lib/debug/.build-id/0d/5a3252c52375a05265f5f175064253309ef4f8.debug.")
  say
else
  say "== $DBGPKG is not installed -- skipping =="
fi

# ---- install onto THIS system -----------------------------------------------------
if [ "$INSTALL" = 0 ]; then
  say "built: $GIR_DEB${DBG_DEB:+ $DBG_DEB}"
  say "(no --install: nothing was installed)"
  exit 0
fi

say "installing (dpkg -i) ..."
if [ -n "$DBG_DEB" ]; then
  sudo dpkg -i "$GIR_DEB" "$DBG_DEB"
else
  sudo dpkg -i "$GIR_DEB"
fi
[ "$DBGSYM_DROP" = 1 ] && sudo dpkg --purge "$DBGPKG"

say
say "== apt-get -s check after =="
if apt-get -s check >/dev/null 2>&1; then
  say "OK: apt is consistent again -- install/upgrade work."
else
  apt-get -s check 2>&1 | sed 's/^/    /'
  die "apt still reports broken dependencies (see above)"
fi

