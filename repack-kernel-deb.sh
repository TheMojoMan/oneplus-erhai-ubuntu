#!/bin/bash
# repack-kernel-deb.sh -- publish an ALREADY BUILT kernel payload; no recompile.
#
# build-kernel.sh resolves its payload snapshot from the package name of the
# inner .deb it just built, so it packages whichever line that snapshot belongs
# to (oneplus-erhai-linux-<major.minor>/; the 7.2 line keeps the canonical
# oneplus-erhai-linux/).  This script still has its own job: turn an ALREADY
# BUILT inner .deb into the CANONICAL erhai kernel package without compiling
# anything -- re-version/re-publish a build, or refresh a line's snapshot + UKI.
#
#   ./repack-kernel-deb.sh ../oneplus-erhai-linux/oneplus-erhai-linux-7.3_7.3.0-rc4-sm8750-themojoman+b33_arm64.deb
#   ./repack-kernel-deb.sh <inner.deb> --rev 34 --no-ppa --no-uki
#
# Steps (the same harvest/package/UKI steps build-kernel.sh runs):
#   1. reads the kernelrelease and the +b<N> revision from the inner .deb name
#      (--rev overrides the revision); control, postinst and postrm are copied
#      FROM THE INNER DEB -- the exact scripts of the tree state it was built
#      from, so a repack never invents packaging behaviour,
#   2. rebuilds the per-line payload snapshot oneplus-erhai-linux-<x.y>, moving
#      the inner legacy lib/modules layout to the canonical usr-merged
#      usr/lib/modules one and dropping build/source symlinks,
#   3. applies build-kernel.sh's pre-pack guards: exactly ONE modules tree,
#      .ko count > 0, modules.dep non-empty, boot/ payload complete,
#   4. packs <Package>_<kernelrelease>+b<N>_arm64.deb and MOVES it into the
#      private PPA suite (ppa_mv -- the kernel convention); NO_PPA=1 or
#      --no-ppa keeps it local,
#   5. builds the UKI for the dualboot installer's ESP tree,
#      ../dualboot/EFI/ubuntu/<clean-kver>-b<N>.efi, with the SHIPPED image's
#      root label (PARTLABEL=linux).  --no-uki skips it.
#
# KEEP THE UKI CMDLINE IN SYNC WITH build-kernel.sh AND the tree's postinst.
#
# The script refuses to write into oneplus-erhai-linux/ (build-kernel.sh's own
# snapshot), so a repack can never clobber a real build's staging tree.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"

FROZEN_RELEASES="7.3.0-rc5-sm8750-themojoman
7.3.0-rc4-sm8750-themojoman
7.3.0-rc2-sm8750-themojoman
7.2.0-sm8750-themojoman"

ERHAI_CMDLINE="root=PARTLABEL=linux rootwait rw rootfstype=ext4 fw_devlink=off \
console=tty0 loglevel=4 clk_ignore_unused pd_ignore_unused \
panic=30 usbcore.autosuspend=-1 log_buf_len=8M"

INNER=""
REV=""
DO_PPA=1
DO_UKI=1
while [ $# -gt 0 ]; do
	case "$1" in
		--rev)      REV="${2:?--rev needs a number}"; shift 2 ;;
		--no-ppa)   DO_PPA=0; shift ;;
		--no-uki)   DO_UKI=0; shift ;;
		-h|--help)  sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		-*)         echo "unknown option: $1" >&2; exit 2 ;;
		*)          INNER="$1"; shift ;;
	esac
done
[ -n "$INNER" ] || { echo "usage: $0 <inner-kernel.deb> [--rev N] [--no-ppa] [--no-uki]" >&2; exit 2; }
[ -f "$INNER" ] || { echo "no such deb: $INNER" >&2; exit 2; }
[ "${NO_PPA:-0}" = 1 ] && DO_PPA=0

# ------------------------------------------------------------ inner deb facts --
BASE="$(basename "$INNER")"
case "$BASE" in
	*_arm64.deb|*_all.deb) ;;
	*) echo "not a deb filename: $BASE" >&2; exit 2 ;;
esac
STAMPED="${BASE%.deb}"                    # <pkg>_<version>_<arch>
ARCH_FIELD="${STAMPED##*_}"
case "$ARCH_FIELD" in
	arm64|all) ;;
	*) echo "unsupported architecture '$ARCH_FIELD' in $BASE" >&2; exit 2 ;;
esac
STAMPED="${STAMPED%_*}"                   # <pkg>_<version>
PKG_NAME="${STAMPED%%_*}"
PKG_VERSION="${STAMPED#*_}"               # e.g. 7.3.0-rc2-sm8750-themojoman+b31
VERSION_FIELD="$(dpkg-deb -f "$INNER" Version)"
case "$PKG_VERSION" in
	*+b[0-9]*) INNER_REV="${PKG_VERSION##*+b}" ;;
	*)         INNER_REV="${VERSION_FIELD##*+b}"
		   [ "$INNER_REV" = "$VERSION_FIELD" ] &&
			{ echo "cannot determine +b<N> from the name (use --rev)" >&2; exit 2; } ;;
esac
KVER="${PKG_VERSION%%+b*}"
CLEAN_KVER="${KVER//-sm8750-themojoman/}"
case "$KVER" in
	*-dirty*|*-g????????)
		echo "ERROR: '$KVER' is not a frozen erhai release (dirty/uncommitted build)" >&2
		exit 1 ;;
esac
case "$(printf '%s\n' "$FROZEN_RELEASES" | grep -Fx "$KVER" || true)" in
	"") echo "ERROR: kernelrelease '$KVER' is not a frozen erhai release." >&2
	    echo "       Expected one of: $(printf '%s ' $FROZEN_RELEASES)" >&2
	    exit 1 ;;
esac
REPACK_REV="${REV:-$INNER_REV}"
SNAP="$HERE/main/$PKG_NAME"

echo "==> inner deb    : $BASE"
echo "==> package      : $PKG_NAME (inner version $VERSION_FIELD, revision +b$INNER_REV)"
echo "==> kernelrelease: $KVER (clean $CLEAN_KVER)"
echo "==> repack as    : ${KVER}+b${REPACK_REV}"
echo "==> snapshot     : $SNAP"

if [ "$SNAP" = "$HERE/main/oneplus-erhai-linux" ]; then
	echo "ERROR: refusing to repack into build-kernel.sh's snapshot ($SNAP)." >&2
	echo "       Repack into a per-line snapshot (oneplus-erhai-linux-<major.minor>)." >&2
	exit 1
fi

# --------------------------------------------------------------- unpack inner --
WORK="$(mktemp -d "${TMPDIR:-/tmp}/repack-kernel.XXXXXX")"
cleanup() {
	find "$WORK" -mindepth 1 \( -type f -o -type l \) -delete 2>/dev/null || true
	find "$WORK" -mindepth 1 -depth -type d -empty -delete 2>/dev/null || true
	rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

PAY="$WORK/payload"; CTL="$WORK/control"
mkdir -p "$PAY" "$CTL"
dpkg-deb -x "$INNER" "$PAY"
dpkg-deb -e "$INNER" "$CTL"

[ -f "$PAY/boot/vmlinuz-$KVER" ] || { echo "ERROR: $INNER has no boot/vmlinuz-$KVER" >&2; exit 1; }
[ -f "$PAY/boot/dtb-$KVER" ]     || { echo "ERROR: $INNER has no boot/dtb-$KVER" >&2; exit 1; }
[ -x "$CTL/postinst" ] && [ -x "$CTL/postrm" ] \
	|| { echo "ERROR: $INNER has no maintainer scripts" >&2; exit 1; }
MODSRC=""
for c in "$PAY/usr/lib/modules/$KVER" "$PAY/lib/modules/$KVER"; do
	[ -d "$c" ] && MODSRC="$c" && break
done
[ -n "$MODSRC" ] || { echo "ERROR: $INNER ships no modules/$KVER tree" >&2; exit 1; }
echo "==> inner modules: ${MODSRC#$PAY/}"

# ----------------------------------------------------- rebuild the snapshot ----
if [ -e "$SNAP/lib" ] && [ ! -L "$SNAP/lib" ]; then
	echo "removing legacy payload dir $SNAP/lib (modules ship under usr/ only)"
	find "$SNAP/lib" -mindepth 1 \( -type f -o -type l \) -exec rm -f {} +
	find "$SNAP/lib" -mindepth 1 -depth -type d -empty -delete
	rmdir "$SNAP/lib" 2>/dev/null || { echo "ERROR: $SNAP/lib still not empty" >&2; exit 1; }
fi
mkdir -p "$SNAP/boot" "$SNAP/usr/lib/modules" "$SNAP/DEBIAN"
for d in boot usr/lib/modules; do
	[ -d "$SNAP/$d" ] && find "$SNAP/$d" -mindepth 1 \( -type f -o -type l \) -exec rm -f {} +
	[ -d "$SNAP/$d" ] && find "$SNAP/$d" -mindepth 1 -depth -type d -empty -delete
done

cp -a "$MODSRC/." "$SNAP/usr/lib/modules/$KVER/"
rm -f "$SNAP/usr/lib/modules/$KVER/build" "$SNAP/usr/lib/modules/$KVER/source"
install -m644 "$PAY/boot/vmlinuz-$KVER" "$SNAP/boot/vmlinuz-$KVER"
install -m644 "$PAY/boot/dtb-$KVER"     "$SNAP/boot/dtb-$KVER"

# control/scripts straight from the inner deb (exact tree state), re-versioned
install -m644 "$CTL/control" "$SNAP/DEBIAN/control"
install -m755 "$CTL/postinst" "$SNAP/DEBIAN/postinst"
install -m755 "$CTL/postrm" "$SNAP/DEBIAN/postrm"
sed -i "s/^Package: .*/Package: $PKG_NAME/"                    "$SNAP/DEBIAN/control"
sed -i "s/^Version: .*/Version: ${KVER}+b${REPACK_REV}/"       "$SNAP/DEBIAN/control"

find "$SNAP" -type d -exec chmod 755 {} +
find "$SNAP" -type f -exec chmod 644 {} +
chmod 755 "$SNAP/DEBIAN/postinst" "$SNAP/DEBIAN/postrm"

# ------------------------------------------------------------ pre-pack guards --
[ -d "$SNAP/usr/lib/modules/$KVER" ] || { echo "ERROR: modules tree missing" >&2; exit 1; }
[ -f "$SNAP/boot/vmlinuz-$KVER" ] && [ -f "$SNAP/boot/dtb-$KVER" ] \
	|| { echo "ERROR: boot payload incomplete" >&2; exit 1; }
[ -e "$SNAP/lib" ] && { echo "ERROR: $SNAP/lib exists -- double modules tree" >&2; exit 1; }
KO_COUNT=$(find "$SNAP/usr/lib/modules/$KVER" -name '*.ko' | wc -l)
[ "$KO_COUNT" -gt 0 ] || { echo "ERROR: no .ko files staged" >&2; exit 1; }
[ -s "$SNAP/usr/lib/modules/$KVER/modules.dep" ] \
	|| { echo "ERROR: modules.dep missing/empty" >&2; exit 1; }
echo "==> payload check OK: $KO_COUNT modules, single usr/lib/modules tree, boot/ complete"

# ------------------------------------------------------------------- package ---
OUT_DEB="$DEB_DIR/${PKG_NAME}_${KVER}+b${REPACK_REV}_arm64.deb"
dpkg-deb --build --root-owner-group "$SNAP" "$OUT_DEB"
if [ "$DO_PPA" = 1 ]; then
	ppa_mv "$OUT_DEB"
else
	echo "no-ppa: kernel deb NOT published ($(basename "$OUT_DEB"))"
fi

# ----------------------------------------------------------------------- UKI ---
UKI_OUT="$HERE/../dualboot/EFI/ubuntu/${CLEAN_KVER}-b${REPACK_REV}.efi"
if [ "$DO_UKI" = 1 ]; then
	mkdir -p "$(dirname "$UKI_OUT")"
	ukify build \
		--linux "$SNAP/boot/vmlinuz-$KVER" \
		--devicetree "$SNAP/boot/dtb-$KVER" \
		--efi-arch aa64 \
		--cmdline "$ERHAI_CMDLINE" \
		--output "$UKI_OUT"
	[ -s "$UKI_OUT" ] || { echo "ukify produced no UKI" >&2; exit 1; }
else
	UKI_OUT="(skipped)"
fi

echo
echo "done: $(basename "$OUT_DEB")  (kernelrelease $KVER)"
echo "      UKI -> $UKI_OUT"
echo "next: ../update-ppa.sh (regenerate suite metadata + push)"
