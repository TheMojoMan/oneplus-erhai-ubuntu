#!/bin/bash
# Build the OnePlus Pad 3 (erhai) kernel .deb and publish it to the apt repo,
# plus build the UKI for the dualboot installer's ESP tree.
#
#   ./build-kernel.sh [branch]
#
# Workflow:
#   1. The kernel sources live in ../oneplus-erhai-linux. If the folder does not
#      exist yet it is cloned from GitHub (themojoman/oneplus-erhai-linux).
#   2. If it already exists you are asked whether to pull the newest online
#      version (fetch + reset --hard origin/<branch>). Answering "no" (or
#      NO_PULL=1 / non-interactive stdin) builds the tree as it stands -- use
#      that for local kernel hacking.
#   3. The kernel is built by the tree's own build-kernel.sh, which stages
#      vmlinuz, dtb and modules into oneplus-erhai-linux-<ver>/ inside the
#      kernel tree (its own .deb/.tar are discarded in step 4 -- since
#      2026-09-04 the workspace snapshot is the canonical payload).
#   4. The payload snapshot oneplus-erhai-ubuntu/oneplus-erhai-linux[-<x.y>]/
#      is refreshed -- one snapshot PER LINE, resolved from the built package's
#      own name (the canonical oneplus-erhai-linux/ is preferred whenever its
#      DEBIAN/control names that same package, so the 7.2 line keeps its paths):
#      boot/ and usr/lib/modules/ are wiped, the fresh kernel, dtb
#      and modules are copied in, DEBIAN/control gets the frozen kernelrelease
#      + a "+b<N>" Debian revision (LINE-AWARE auto counter in the kernel tree's
#      build-kernel.sh -- a new kernelrelease starts at +b1; override with
#      DEB_REV=<n>), postinst/postrm are refreshed from the
#      kernel tree, and the .deb is packed into debs/ and LEFT THERE: a kernel
#      build stays local until the owner publishes it with
#      ../update-ppa.sh --mvtomain (which moves every newer .deb from debs/
#      into main/).
#   5. A UKI is built with ukify (same recipe as the deb's postinst, but with
#      root=PARTLABEL=linux -- the SHIPPED image's label) and moved to
#      ../dualboot/EFI/ubuntu/<kver>-linux.efi, where
#      ../dualboot/build-installer-zip.sh packs it into the installer zip.
#
# Environment: BRANCH (default master or $1), NO_PULL=1 (skip step 2),
#              JOBS, KERNEL_TREE, DEB_REV, PPA_DEB_DIR (see ppa-common.sh).
#              NO_PPA is accepted but does nothing any more (the kernel deb is
#              never published automatically).

set -euo pipefail

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"

# Never run this script as root: the payload snapshot oneplus-erhai-linux/
# and the PPA suites would end up root-owned and the next user-run build
# dies at the snapshot-wipe step (2026-09-09: +b15 failed with
# "rm: cannot remove ... Permission denied" after a root-run +b14 build).
if [ "$(id -u)" = 0 ] && [ "${FORCE_ROOT:-0}" != 1 ]; then
	echo "ERROR: refusing to run as root (would leave the payload snapshot" >&2
	echo "       $HERE/oneplus-erhai-linux and the PPA root-owned). Run this" >&2
	echo "       script as the regular user; override with FORCE_ROOT=1 only if" >&2
	echo "       you really know why." >&2
	exit 1
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"

KERNEL_REPO="${KERNEL_REPO:-https://github.com/themojoman/oneplus-erhai-linux.git}"
# The kernel tree lives at the WORKSPACE root (next to oneplus-erhai-ubuntu/),
# not next to this builder -- $HERE/.. is the ubuntu repo, whose
# oneplus-erhai-linux* dirs are the deb payload snapshots.
KERNEL_TREE="${KERNEL_TREE:-$(dirname "$_PPA_COMMON_DIR")/oneplus-erhai-linux}"
BRANCH="${BRANCH:-${1:-master}}"
NO_PULL="${NO_PULL:-1}"
JOBS="${JOBS:-$(nproc)}"

# The +b<N> Debian revision is computed by the KERNEL TREE's build-kernel.sh
# (its next_deb_rev, LINE-AWARE: it counts only revisions of the current
# kernelrelease across the PPA suites, the locally built debs and the installed
# version, so a new release string starts at +b1).  This wrapper reuses the
# revision of the inner deb it harvested (see step 2 below), so exactly one auto
# counter exists; DEB_REV=<n> exported by the caller overrides it in both.

# ------------------------------------------------------------------ source ---
if [ ! -d "$KERNEL_TREE/.git" ]; then
	echo "kernel tree $KERNEL_TREE not found -- cloning $KERNEL_REPO ($BRANCH)"
	git clone --branch="$BRANCH" "$KERNEL_REPO" "$KERNEL_TREE"
else
	pull=no
	if [ "$NO_PULL" = 1 ]; then
		echo "NO_PULL=1: building $KERNEL_TREE as it stands"
	elif [ -t 0 ]; then
		read -r -p "Fetch newest online version of $KERNEL_TREE ($BRANCH)? [Y/n] " ans \
			|| { echo; echo "non-interactive: building as it stands"; }
		case "${ans:-y}" in y|Y|yes|Yes|"") pull=yes ;; *) echo "keeping local state" ;; esac
	else
		echo "non-interactive: building $KERNEL_TREE as it stands (set NO_PULL=0 with a tty to be asked)"
	fi
	if [ "$pull" = yes ]; then
		echo "Updating $KERNEL_TREE ($BRANCH)"
		git -C "$KERNEL_TREE" fetch origin
		git -C "$KERNEL_TREE" checkout "$BRANCH"
		git -C "$KERNEL_TREE" reset --hard "origin/$BRANCH"
	fi
fi

# The defconfig normally ships in-tree; fall back to the workspace copy so an
# older checkout still configures.
DEFCONFIG=erhai_defconfig
BUILD_FILES="$(dirname "$HERE")/kernel-build-files"
if [ ! -f "$KERNEL_TREE/arch/arm64/configs/$DEFCONFIG" ] &&
   [ -f "$BUILD_FILES/$DEFCONFIG" ]; then
	echo "installing $DEFCONFIG from $BUILD_FILES"
	cp "$BUILD_FILES/$DEFCONFIG" "$KERNEL_TREE/arch/arm64/configs/$DEFCONFIG"
fi

# ------------------------------------------------------------------- build ---
# The inner script does defconfig -> build -> modules staging -> its own
# dpkg-deb. It re-clones/pulls only when told to; we pass NO_PULL=1 because the
# source decision was made above.
(cd "$KERNEL_TREE" && NO_PULL=1 JOBS="$JOBS" ./build-kernel.sh)

# ------------------------------------------------- harvest the inner staging --
# The inner script stages the payload into oneplus-erhai-linux-<short>/ inside
# the kernel tree and packs its own .deb there.  Since 2026-09-04 the CANONICAL
# payload is the workspace snapshot oneplus-erhai-ubuntu/oneplus-erhai-linux/
# (usr-merged usr/lib/modules layout): it is refreshed from the inner staging
# dir, re-versioned, packed HERE and published.  The inner .deb/.tar are
# discarded -- exactly one canonical artefact per built kernel.
INNER_DEB="$(ls -t "$KERNEL_TREE"/oneplus-erhai-linux-*_arm64.deb 2>/dev/null | head -n1 || true)"
[ -n "$INNER_DEB" ] || { echo "no kernel .deb produced in $KERNEL_TREE" >&2; exit 1; }
INNER_NAME="$(basename "${INNER_DEB%_arm64.deb}")"      # oneplus-erhai-linux-7.3_<ver>+b<N>
STAGE_DIR="$KERNEL_TREE/${INNER_NAME%%_*}"              # .../oneplus-erhai-linux-7.3
KVER="${INNER_NAME#*_}"; KVER="${KVER%+b*}"             # kernelrelease
CLEAN_KVER="${KVER//-sm8750-themojoman/}"               # 7.3.0-rc5
# The release must stay EXACTLY one of the frozen strings below (stable vermagic,
# module paths, UKI name).  A -dirty/-gSHA suffix means tracked tree edits were
# not committed + tagged (git tag -f -a v7.3-rc5 HEAD) -- refuse to ship that.
# Each erhai kernel LINE has its own frozen release string and its own stable
# tag that must sit on HEAD before building, or scripts/setlocalversion appends
# '+' and the case below rejects the build:
#   7.3-rc5 line (branch 7.3-rc5, CURRENT)  -> 7.3.0-rc5-sm8750-themojoman  tag v7.3-rc5
#   7.3-rc4 line (branch 7.3-rc4)           -> 7.3.0-rc4-sm8750-themojoman  tag v7.3-rc4
#   7.3-rc2 line (branch linux-7.3)         -> 7.3.0-rc2-sm8750-themojoman  tag v7.3-rc2
#   7.2 line     (branch linux-7.2)         -> 7.2.0-sm8750-themojoman      tag v7.2
case "$KVER" in
7.3.0-rc5-sm8750-themojoman)
	TAG=v7.3-rc5 ;;
7.3.0-rc4-sm8750-themojoman)
	TAG=v7.3-rc4 ;;
7.3.0-rc2-sm8750-themojoman)
	TAG=v7.3-rc2 ;;
7.2.0-sm8750-themojoman)
	TAG=v7.2 ;;
*)
	echo "ERROR: kernelrelease '$KVER' is not a frozen erhai release." >&2
	echo "       Expected 7.3.0-rc5-sm8750-themojoman (7.3-rc5 line)," >&2
	echo "                7.3.0-rc4-sm8750-themojoman (7.3-rc4 line)," >&2
	echo "                7.3.0-rc2-sm8750-themojoman (7.3-rc2 line)" >&2
	echo "             or 7.2.0-sm8750-themojoman (7.2 line)." >&2
	echo "       Commit kernel-tree edits and 'git tag -f -a <v7.3-rc5|v7.3-rc4|v7.3-rc2|v7.2> HEAD' first." >&2
	exit 1 ;;
esac
# TAG is the stable tag the line's release string depends on.  Check it here so
# the trap it guards against (an untagged HEAD -> kernelrelease '...+' -> the
# case above aborts the build) is named with the exact command to run.
if [ -d "$KERNEL_TREE/.git" ]; then
	HAVE_TAG="$(git -C "$KERNEL_TREE" describe --exact-match HEAD 2>/dev/null || true)"
	[ "$HAVE_TAG" = "$TAG" ] || \
		echo "NOTE: HEAD of $KERNEL_TREE is tagged '${HAVE_TAG:-nothing}', not '$TAG'." \
		     "If the release comes out with a '+' suffix: git -C $KERNEL_TREE tag -f -a $TAG HEAD"
fi
[ -f "$STAGE_DIR/boot/vmlinuz-$KVER" ] && [ -f "$STAGE_DIR/boot/dtb-$KVER" ] \
	&& [ -d "$STAGE_DIR/lib/modules/$KVER" ] || {
	# Since 2026-09-08 (kernel commit 31e4e7004) the inner build-kernel.sh
	# removes its staging tree right after dpkg-deb so the root-owned tree
	# never leaks into git status or the PPA.  Harvest from the inner .deb
	# instead: it contains the identical payload (boot/, lib/modules/).
	echo "staging tree absent -- reconstructing from $INNER_DEB"
	dpkg-deb -x "$INNER_DEB" "$STAGE_DIR"
}
[ -f "$STAGE_DIR/boot/vmlinuz-$KVER" ] && [ -f "$STAGE_DIR/boot/dtb-$KVER" ] \
	&& [ -d "$STAGE_DIR/lib/modules/$KVER" ] \
	|| { echo "inner staging incomplete in $STAGE_DIR" >&2; exit 1; }

# ------------------------------------------------------- refresh the snapshot --
# The payload snapshot is PER LINE: oneplus-erhai-linux-<major.minor> for a line,
# with the canonical oneplus-erhai-linux/ holding the 7.2 line.  This used to be
# hardcoded to oneplus-erhai-linux/, which meant a 7.3 build through the wrapper
# packed a deb named after whatever line owned that snapshot (and wiped its
# payload).  Resolve it from the package name of the inner deb we just built;
# prefer the canonical dir first when its DEBIAN/control names the same package,
# so the 7.2 line keeps its historical paths.
BUILD_PKG="${INNER_NAME%%_*}"                            # oneplus-erhai-linux-7.3
SNAP=""
for cand in "$HERE/oneplus-erhai-linux" "$HERE/$BUILD_PKG"; do
	[ -d "$cand/DEBIAN" ] || continue
	[ "$(awk '/^Package:/{print $2; exit}' "$cand/DEBIAN/control" 2>/dev/null)" = "$BUILD_PKG" ] || continue
	SNAP="$cand"
	break
done
[ -n "$SNAP" ] || {
	echo "ERROR: no payload snapshot for package $BUILD_PKG" >&2
	echo "       expected $HERE/$BUILD_PKG (or $HERE/oneplus-erhai-linux) with a" >&2
	echo "       DEBIAN/control whose Package: is $BUILD_PKG" >&2
	exit 1; }
echo "payload snapshot: $SNAP  (package $BUILD_PKG)"
# A root-run build (or root dpkg-deb -x) leaves the snapshot root-owned; the
# wipe below would then fail per-file with "Permission denied" and, worse,
# leave a half-stale payload behind. Refuse up front with the exact fix.
ROOTOWNED="$(find "$SNAP" -user root -print -quit 2>/dev/null || true)"
if [ -n "$ROOTOWNED" ]; then
	echo "ERROR: root-owned entries in the payload snapshot (e.g. $ROOTOWNED)." >&2
	echo "       A previous run executed as root. Fix ownership, then rebuild:" >&2
	echo "         sudo -S sh -c 'chown -R \$(id -un):\$(id -gn) $SNAP'   # rootdo.py is GONE (2026-10-23)" >&2
	exit 1
fi
mkdir -p "$SNAP/boot" "$SNAP/usr/lib/modules"

# 1. wipe the old payload: every file (and stale symlink) below boot/ and
#    usr/lib/modules/, plus the dirs this empties -- explicit rm per path,
#    never rm -rf.
while read -r f; do rm -f "$f"; done < <(
	find "$SNAP/boot" "$SNAP/usr/lib/modules" -mindepth 1 \( -type f -o -type l \))
find "$SNAP/boot" "$SNAP/usr/lib/modules" -mindepth 1 -type d -empty -delete
# 1b. a legacy top-level lib/ (pre-usr-merged payload, e.g. extracted from an
#     old inner deb or staged by the older lib/modules layout) MUST NOT survive:
#     on the merged-usr target ./lib/modules/... and ./usr/lib/modules/... are
#     the SAME directory and dpkg dies mid-unpack on the duplicate tree
#     (broke the +b14 install on sda18 2026-09-09 -> unbootable). Wipe it.
if [ -e "$SNAP/lib" ]; then
	echo "removing legacy payload dir $SNAP/lib (modules must ship only under usr/)"
	while read -r f; do rm -f "$f"; done < <(
		find "$SNAP/lib" -mindepth 1 \( -type f -o -type l \))
	find "$SNAP/lib" -mindepth 1 -depth -type d -empty -delete
	rmdir "$SNAP/lib" 2>/dev/null || {
		echo "ERROR: $SNAP/lib still not empty -- refusing to continue" >&2; exit 1; }
fi

# 2. version: kernelrelease stays frozen; the +b<N> Debian revision is appended
#    to the deb Version ONLY (postinst/postrm strip it back off).  N is PER LINE
#    (a new kernelrelease starts at b1), computed by the tree's build-kernel.sh;
#    see right below.  Override with DEB_REV=<n>.
PKG_NAME="$(awk '/^Package:/{print $2; exit}' "$SNAP/DEBIAN/control")"
[ -n "$PKG_NAME" ] || { echo "no Package: in $SNAP/DEBIAN/control" >&2; exit 1; }
# The +b<N> revision comes from the INNER deb name: the tree's own build-kernel.sh
# is the single, LINE-AWARE auto counter (it counts only revisions of THIS
# kernelrelease, across the PPA suites, the locally built debs and the installed
# version) and a DEB_REV=<n> exported by the caller wins there too -- so deriving
# it here keeps the wrapper from packing the very same payload under a different
# revision than the tree computed.  The revision is appended to the deb Version
# ONLY; postinst/postrm strip it back off.  See DEB_REV in the header.  (This file
# used to carry its own PACKAGE-wide next_deb_rev, which is what made a new
# release line continue the old line's numbering -- dropped 2026-09-22.)
DEB_REV="${DEB_REV:-${INNER_NAME##*+b}}"
# 2026-10-21 (.clinerules): bare "+b<N>" is the RELEASE form.  A PR / TEST /
# GPU / display / any other LOCAL build uses "<N>-<topic>-<no>", e.g.
# 28-test-01, 32-gp-01, 33-panel-02, 34-PD-01 -> Version
# 7.3.0-rc5-sm8750-themojoman+b34-PD-01, UKI 7.3.0-rc5-l1-b34-PD-01.efi.
# N is the CURRENT line number, i.e. REUSED, never advanced: the +b<N> counter
# belongs to released builds only, so a local build can never be mistaken for a
# release and the next real build supersedes it.  The topic is FREE FORM
# ([A-Za-z0-9.]+, case preserved) so a new investigation needs no edit here.
# EVERY "<N>-<topic>-<no>" form ranks ABOVE "+b<N>" for dpkg (the split is at
# the LAST '-'), so such a deb must NEVER be published -- keep it in debs/ or
# debs/test-builds/.
# BEGIN deb-rev-check  (tmp/test-debrev-forms.sh evals exactly this block)
case "$DEB_REV" in
''|*[!0-9]*)
	if ! [[ "$DEB_REV" =~ ^[0-9]+-[A-Za-z0-9.]+-[0-9]+$ ]]; then
		echo "ERROR: DEB_REV='$DEB_REV' (from '$INNER_NAME') is neither the release form <n> nor a local <line-N>-<topic>-<no> form (e.g. DEB_REV=34-PD-01)." >&2
		exit 1
	fi ;;
esac
# END deb-rev-check
DEB_VERSION="${KVER}+b${DEB_REV}"
sed -i "s/^Version: .*/Version: ${DEB_VERSION}/" "$SNAP/DEBIAN/control"

# 3. ship the CURRENT postinst/postrm -- dpkg -i reruns the baked-in copies.
install -m755 "$KERNEL_TREE/postinst" "$SNAP/DEBIAN/postinst"
install -m755 "$KERNEL_TREE/postrm"   "$SNAP/DEBIAN/postrm"

# 4. fresh modules -> usr/lib/modules (usr-merged layout on the target).
cp -a "$STAGE_DIR/lib/modules/." "$SNAP/usr/lib/modules/"
# build/source point back into the kernel tree, which does not exist on device.
rm -f "$SNAP/usr/lib/modules/$KVER/build" "$SNAP/usr/lib/modules/$KVER/source"

# 5. kernel + dtb -> boot/
install -m644 "$STAGE_DIR/boot/vmlinuz-$KVER" "$SNAP/boot/vmlinuz-$KVER"
install -m644 "$STAGE_DIR/boot/dtb-$KVER"     "$SNAP/boot/dtb-$KVER"

# The reconstructed staging tree is huge (and root-owned when run via sudo) --
# wipe it again, exactly like the inner script does after its own dpkg-deb:
# explicit find-based deletion, never rm -rf.
find "$STAGE_DIR" -mindepth 1 \( -type f -o -type l \) -delete
find "$STAGE_DIR" -mindepth 1 -depth -type d -empty -delete
rmdir "$STAGE_DIR" 2>/dev/null || true

# sane modes; dpkg-deb --root-owner-group normalises the ownership.
find "$SNAP" -type d -exec chmod 755 {} +
find "$SNAP" -type f -exec chmod 644 {} +
chmod 755 "$SNAP/DEBIAN/postinst" "$SNAP/DEBIAN/postrm"
chmod 644 "$SNAP/DEBIAN/control"

# ------------------------------------------------------------- pre-pack guard --
# The deb must carry the modules tree under EXACTLY ONE path (usr/lib/modules,
# usr-merged layout). A second tree (lib/modules) makes dpkg fail mid-unpack on
# the merged-usr rootfs and can leave the target unbootable -- refuse to pack
# anything malformed. Also re-assert the boot payload exists.
[ -d "$SNAP/usr/lib/modules/$KVER" ] || { echo "ERROR: $SNAP/usr/lib/modules/$KVER missing" >&2; exit 1; }
[ -f "$SNAP/boot/vmlinuz-$KVER" ] && [ -f "$SNAP/boot/dtb-$KVER" ] \
	|| { echo "ERROR: $SNAP/boot payload incomplete" >&2; exit 1; }
if [ -e "$SNAP/lib" ]; then
	echo "ERROR: $SNAP/lib exists -- the deb would double-ship the modules" >&2
	echo "       tree (lib/modules + usr/lib/modules) and break dpkg on the" >&2
	echo "       merged-usr target. Remove it and rebuild." >&2
	exit 1
fi
KO_COUNT=$(find "$SNAP/usr/lib/modules/$KVER" -name '*.ko' | wc -l)
[ "$KO_COUNT" -gt 0 ] || { echo "ERROR: no .ko files in the staged modules tree" >&2; exit 1; }
[ -s "$SNAP/usr/lib/modules/$KVER/modules.dep" ] \
	|| { echo "ERROR: modules.dep missing/empty in the staged modules tree" >&2; exit 1; }
echo "payload check OK: $KO_COUNT modules, single usr/lib/modules tree, boot/ complete"

# ------------------------------------------------------------------ package ---
OUT_DEB="$DEB_DIR/${PKG_NAME}_${DEB_VERSION}_arm64.deb"
dpkg-deb --build --root-owner-group "$SNAP" "$OUT_DEB"

# The .deb STAYS in debs/ ($DEB_DIR): a kernel is a test build until the owner
# says otherwise, and debs/ is where it can be installed from locally
# (dpkg -i debs/<deb>).  Publishing is a separate, explicit step:
# update-ppa.sh --mvtomain picks up every newer .deb from debs/ and moves it into
# main/.  (Owner rule, 2026-10-26: no automatic PPA move -- the owner decides how
# to share a kernel.  NO_PPA=1 is kept for old callers and changes nothing.)
# ppa-guard still runs now, so a package carrying personal data is caught at
# build time instead of at publish time.
_ppa_guard "$OUT_DEB" || {
	echo "ERROR: ppa-guard rejected $OUT_DEB -- do not publish it as is" >&2
	exit 1
}
echo "kernel deb kept local: $OUT_DEB"

# The inner script's own .deb/.tar are superseded -- drop them so the kernel
# tree keeps no stale duplicates of a package that was never published.
rm -f "$INNER_DEB" "$KERNEL_TREE/${INNER_NAME%%_*}.tar"

# --------------------------------------- UKI for the dualboot installer's ESP --
# Same recipe as the deb's postinst, but with the SHIPPED image's root label
# (PARTLABEL=linux -- the rootfs build-root.sh creates).  It is written to
# ../dualboot/EFI/ubuntu/ but is NOT shipped: an erhai image boots the UKI that
# the deb's postinst writes into its own /boot, and build-installer-zip.sh ships
# a raw fixed-size ESP image (no tree), which never carries EFI/ubuntu
# (2026-10-18).  The file is kept as the manual/fallback UKI for an ESP-only
# install.
# KEEP THE CMDLINE IN SYNC WITH the kernel tree's postinst!
ERHAI_CMDLINE="root=PARTLABEL=linux rootwait rw rootfstype=ext4 fw_devlink=off \
console=tty0 loglevel=4 clk_ignore_unused pd_ignore_unused \
panic=30 usbcore.autosuspend=-1 log_buf_len=8M"
UKI_OUT="$HERE/../../dualboot/EFI/ubuntu/${CLEAN_KVER}-b${DEB_REV}.efi"
mkdir -p "$(dirname "$UKI_OUT")"
ukify build \
	--linux "$SNAP/boot/vmlinuz-$KVER" \
	--devicetree "$SNAP/boot/dtb-$KVER" \
	--efi-arch aa64 \
	--cmdline "$ERHAI_CMDLINE" \
	--output "$UKI_OUT"
[ -s "$UKI_OUT" ] || { echo "ukify produced no UKI" >&2; exit 1; }

echo
echo "done: $(basename "$OUT_DEB")  (kernelrelease $KVER)"
echo "      UKI -> $UKI_OUT"
echo "next: ../update-ppa.sh (then install from the repo or dpkg -i the deb)"
echo "      ../dualboot/build-installer-zip.sh (packs ubuntu.img.zst + esp.img.zst; no UKI -- /boot boots)"
