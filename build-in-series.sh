#!/bin/bash
# Build ONE .deb for ANOTHER Ubuntu release, in a throwaway chroot.
#
#   sudo ./build-in-series.sh stonking build-mutter-deb.sh [args...]
#   sudo ./build-in-series.sh resolute build-gtk4-deb.sh
#
# WHY: the erhai PPA is split into a UNIVERSAL suite (main) and one
# PER-RELEASE suite per codename (ppa/oneplus-erhai/ubuntu/<series>/).  The
# packages in a series suite repack or divert an UBUNTU artifact of the same
# name (mutter, GTK4, Mesa, steam, ptyxis, nautilus -- see
# ppa-series-packages.txt), so they must be built ON that release: the patched
# library, the soname and the package version all come from it.  This pad runs
# 26.04 (resolute) natively, so a 26.10 (stonking) build needs its own root.
#
# The root lives OUTSIDE the three repos: ../series-build/<series>/ (gitignored
# by nothing -- it is simply not in a repo).  It is created once with
# debootstrap (native arm64, no qemu) and reused afterwards; RM_ROOT=1 recreates
# it.  The workspace is bind-mounted read-write, so the builder writes its
# artifact into exactly the usual places (oneplus-erhai-ubuntu/debs/,
# the project's build/ dir) and the per-series publish target is selected by
# exporting ERHAI_SERIES=<series> before ppa-common.sh is sourced.
#
# The release is VERIFIED before anything is built: a 26.10 chroot that reports
# resolute (or a root created from the wrong tarball) aborts the run instead of
# publishing a wrong-ABI .deb into a public suite.
#
#   SERIES_TARBALL=/path/ubuntu-base-26.10-base-arm64.tar.gz   root from a tarball
#   SERIES_MIRROR=http://...    debootstrap mirror (default ports.ubuntu.com)
#   NO_PPA=1                    keep the deb local (db still lands in debs/)
#   RM_ROOT=1                   drop and recreate the root first
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(dirname "$HERE")"
[ "$(id -u)" = 0 ] || { echo "run me as root (sudo $0 <series> <builder> [args...])" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '3,8p' "$0" >&2; exit 2; }

SERIES="$1"; shift
BUILDER="$1"; shift
case "$SERIES" in
	resolute|stonking) ;;
	*) echo "unknown series '$SERIES' (resolute = 26.04, stonking = 26.10)" >&2; exit 2 ;;
esac
[ -x "$HERE/$BUILDER" ] || { echo "no such builder: $HERE/$BUILDER" >&2; exit 2; }

MIRROR="${SERIES_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"
ROOT="${SERIES_ROOT:-$W/series-build/$SERIES}"
PROCS_MARK="$ROOT/.erhai-mounted"

if [ "${RM_ROOT:-0}" = 1 ] && [ -d "$ROOT" ]; then
	echo "RM_ROOT=1: removing $ROOT"
	for d in proc sys dev; do umount "$ROOT/$d" 2>/dev/null || true; done
	umount "$ROOT$W" 2>/dev/null || true
	case "$ROOT" in "$W"/series-build/*) rm -rf -- "$ROOT" ;; esac
fi

if [ ! -d "$ROOT" ]; then
	install -d -m755 "$(dirname "$ROOT")"
	if [ -n "${SERIES_TARBALL:-}" ]; then
		echo "creating root from tarball: $SERIES_TARBALL"
		mkdir -p "$ROOT"
		tar xzf "$SERIES_TARBALL" -C "$ROOT"
	else
		echo "debootstrap $SERIES -> $ROOT (minbase)"
		debootstrap --variant=minbase --arch=arm64 --no-check-gpg "$SERIES" "$ROOT" "$MIRROR"
	fi
	# packages every erhai builder needs; installed once, then cached in RAM-free
	# fashion (the root is reused, so this cost is paid a single time per series)
	cat > "$ROOT/tmp/erhai-series-bootstrap.sh" <<'EOS'
set -e
export DEBIAN_FRONTEND=noninteractive PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
apt-get update
apt-get install -y --no-install-recommends ca-certificates gnupg wget curl dpkg-dev \
	build-essential devscripts fakeroot zstd xz-utils python3 patchelf file
apt-get clean
EOS
	chroot "$ROOT" /bin/bash /tmp/erhai-series-bootstrap.sh </dev/null
	rm -f "$ROOT/tmp/erhai-series-bootstrap.sh"
fi

# ---- the release MUST be the requested one -----------------------------------
CODENAME="$(sed -n 's/^VERSION_CODENAME=//p' "$ROOT/etc/os-release" | tr -d '"')"
VERSID="$(sed -n 's/^VERSION_ID=//p' "$ROOT/etc/os-release" | tr -d '"')"
[ "$CODENAME" = "$SERIES" ] || {
	echo "FATAL: $ROOT is '$CODENAME' ($VERSID), not '$SERIES' -- refusing to build" >&2
	exit 1; }
echo "build root OK: $SERIES ($VERSID) in $ROOT"

# ---- mounts ------------------------------------------------------------------
cleanup() {
	umount "$ROOT/proc" 2>/dev/null || true
	umount "$ROOT/sys" 2>/dev/null || true
	umount "$ROOT/dev/pts" 2>/dev/null || true
	umount "$ROOT/dev" 2>/dev/null || true
	umount "$ROOT$W" 2>/dev/null || true
	rm -f "$PROCS_MARK"
}
trap cleanup EXIT INT TERM
mount -t proc proc "$ROOT/proc" 2>/dev/null || true
mount -t sysfs sys "$ROOT/sys" 2>/dev/null || true
mount --bind /dev "$ROOT/dev" 2>/dev/null || true
mkdir -p "$ROOT$W"
mount --bind "$W" "$ROOT$W"
: > "$PROCS_MARK"

echo "running $BUILDER in $SERIES (ERHAI_SERIES=$SERIES)"
# ERHAI_SERIES makes ppa-common.sh (and thus the publish target) point at
# ppa/oneplus-erhai/ubuntu/<series>/; everything else is passed through.
chroot "$ROOT" /usr/bin/env -i \
	PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
	HOME=/root TERM="${TERM:-dumb}" LANG=C.UTF-8 \
	DEBIAN_FRONTEND=noninteractive \
	ERHAI_SERIES="$SERIES" \
	NO_PPA="${NO_PPA:-0}" \
	DEB_DIR="${DEB_DIR:-$HERE/debs}" \
	PPA_SERIES_DEB_DIR="${PPA_SERIES_DEB_DIR:-}" \
	${BASE:+BASE="$BASE"} ${ERHAI_SUFFIX:+ERHAI_SUFFIX="$ERHAI_SUFFIX"} \
	/bin/bash -c "cd '$HERE' && exec ./'$BUILDER' $*"

echo "done: $BUILDER ($SERIES)"
