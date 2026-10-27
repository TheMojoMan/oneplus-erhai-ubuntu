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
#   4. The payload snapshot oneplus-erhai-ubuntu/oneplus-erhai-linux/ is
#      refreshed: boot/ and usr/lib/modules/ are wiped, the fresh kernel, dtb
#      and modules are copied in, DEBIAN/control gets the frozen kernelrelease
#      + a "+b<N>" Debian revision (auto-increments past what the PPA already
#      has; override with DEB_REV=<n>), postinst/postrm are refreshed from the
#      kernel tree, and the .deb is packed and MOVED to
#      themojoman.github.io/ppa/oneplus-erhai/ubuntu/main/ (the private
#      suite; ../update-ppa.sh --cptesting moves it into main/ and republishes).
#   5. A UKI is built with ukify (same recipe as the deb's postinst, but with
#      root=PARTLABEL=linux -- the SHIPPED image's label) and moved to
#      ../dualboot/EFI/ubuntu/<kver>-linux.efi, where
#      ../dualboot/build-installer-zip.sh packs it into the installer zip.
#
# Environment: BRANCH (default master or $1), NO_PULL=1 (skip step 2),
#              JOBS, KERNEL_TREE, DEB_REV, PPA_DEB_DIR (see ppa-common.sh).

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
KERNEL_TREE="${KERNEL_TREE:-$HERE/../oneplus-erhai-linux}"
BRANCH="${BRANCH:-${1:-master}}"
NO_PULL="${NO_PULL:-1}"
JOBS="${JOBS:-$(nproc)}"

# Debian revision for the frozen kernelrelease (see the inner script for the
# rationale): highest +b<N> visible in any PPA suite (Packages metadata and
# actual .deb files) plus one.  Override with DEB_REV=<n>.
next_deb_rev() {
	local pkg="$1" max=0 f v n
	local root="$HERE/../../themojoman.github.io/ppa/oneplus-erhai/ubuntu"
	if [ -d "$root" ]; then
		# Published metadata (may lag behind freshly moved .debs) ...
		for f in "$root"/*/Packages; do
			[ -f "$f" ] || continue
			while read -r v; do
				case "$v" in *+b[0-9]*) n="${v##*+b}" ;; *) continue ;; esac
				if [ "$n" -gt "$max" ] 2>/dev/null; then max="$n"; fi
			done < <(awk -v pkg="$pkg" '
				$1 == "Package:" { inp = ($2 == pkg); next }
				inp && $1 == "Version:" { print $2; inp = 0 }' "$f")
		done
		# ... and the actual .deb files sitting in the suites.
		for f in "$root"/*/"${pkg}"_*_arm64.deb; do
			[ -f "$f" ] || continue
			v="${f##*/}"; v="${v#"${pkg}"_}"; v="${v%_arm64.deb}"
			case "$v" in *+b[0-9]*) n="${v##*+b}" ;; *) continue ;; esac
			if [ "$n" -gt "$max" ] 2>/dev/null; then max="$n"; fi
		done
	fi
	echo $((max + 1))
}

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
INNER_NAME="$(basename "${INNER_DEB%_arm64.deb}")"      # oneplus-erhai-linux-7.2_<ver>+b<N>
STAGE_DIR="$KERNEL_TREE/${INNER_NAME%%_*}"              # .../oneplus-erhai-linux-7.2
KVER="${INNER_NAME#*_}"; KVER="${KVER%+b*}"             # frozen kernelrelease
CLEAN_KVER="${KVER//-sm8750-themojoman/}"               # 7.2.0
# The release must stay EXACTLY this (stable vermagic, module paths, UKI name).
# A -dirty/-gSHA suffix means tracked tree edits were not committed + tagged
# (git tag -f -a v7.2 HEAD) -- refuse to ship that.
[ "$KVER" = "7.2.0-sm8750-themojoman" ] || {
	echo "ERROR: kernelrelease '$KVER' is not the frozen 7.2.0-sm8750-themojoman." >&2
	echo "       Commit kernel-tree edits and 'git tag -f -a v7.2 HEAD' first." >&2
	exit 1
}
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
SNAP="$HERE/oneplus-erhai-linux"
[ -d "$SNAP/DEBIAN" ] || { echo "kernel deb payload snapshot missing: $SNAP" >&2; exit 1; }
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
#    to the deb Version ONLY (postinst/postrm strip it back off).  N
#    auto-increments past what the PPA suites already carry.
PKG_NAME="$(awk '/^Package:/{print $2; exit}' "$SNAP/DEBIAN/control")"
[ -n "$PKG_NAME" ] || { echo "no Package: in $SNAP/DEBIAN/control" >&2; exit 1; }
DEB_REV="${DEB_REV:-$(next_deb_rev "$PKG_NAME")}"
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

# Move (not copy) into the PPA suite: no leftover copy anywhere, so there
# is exactly one canonical location per built kernel package.
# NO_PPA=1 keeps in-development kernels local (release policy: the PPA gets a
# kernel only when the owner asks for it).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_mv "$OUT_DEB"
else
	echo "NO_PPA=1: kernel deb NOT published ($(basename "$OUT_DEB"))"
fi

# The inner script's own .deb/.tar are superseded -- drop them so the kernel
# tree keeps no stale duplicates of a package that was never published.
rm -f "$INNER_DEB" "$KERNEL_TREE/${INNER_NAME%%_*}.tar"

# --------------------------------------- UKI for the dualboot installer's ESP --
# Same recipe as the deb's postinst, but with the SHIPPED image's root label
# (PARTLABEL=linux -- the rootfs build-root.sh creates): this UKI goes to
# ../dualboot/EFI/ubuntu/ and is packed into erhai-ubuntu-installer.zip.
# KEEP THE CMDLINE IN SYNC WITH the kernel tree's postinst!
ERHAI_CMDLINE="root=PARTLABEL=linux rootwait rw rootfstype=ext4 fw_devlink=off \
earlycon=efifb console=tty0 loglevel=4 clk_ignore_unused pd_ignore_unused \
panic=30 consoleblank=0 usbcore.autosuspend=-1 log_buf_len=8M"
UKI_OUT="$HERE/../../dualboot/EFI/ubuntu/${CLEAN_KVER}-linux.efi"
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
echo "      ../dualboot/build-installer-zip.sh (packs ubuntu.img.zst + esp.img.zst)"
