#!/bin/bash
# update-mesa.sh -- refresh the oneplus-erhai-mesa deb payload from upstream git.
#
#   ./update-mesa.sh              # fetch newest mesa main, rebuild, restage
#   FORCE=1 ./update-mesa.sh      # rebuild even if the deb version is unchanged
#   MESA_SRC=/path/to/mesa ./update-mesa.sh
#   MESA_PREFIX=/usr ./update-mesa.sh   # stage the payload for /usr (the
#                                       # replacement mode -- see
#                                       # build-mesa-usr-debs.sh)
#
# What it does:
#   1. fast-forwards the mesa git tree (MESA_SRC, default
#      /media/data/develop/mesa) to origin/main -- the tree must be clean,
#   2. (re)configures $SRC/build-erhai with the device-proven options.
#      NOTE prefix=/opt/mesa (MESA_PREFIX-overridable): the turnip ICD json
#      bakes that absolute path, and the erhai env opt-in files
#      (50-erhai-mesa-opt.conf, greeter drop-in, skel 10-desktop.conf.gpu)
#      all point there.  With MESA_PREFIX=/usr the payload is staged under
#      oneplus-erhai-mesa/usr/ (the exact Ubuntu file layout) and the
#      -dev-only artifacts (include/, pkgconfig/, unversioned .so symlinks)
#      are dropped so the replacement debs never clash with Ubuntu's -dev
#      packages.
#   3. builds incrementally and DESTDIR-installs,
#   4. replaces oneplus-erhai-mesa/opt/mesa (or oneplus-erhai-mesa/usr) with
#      the fresh payload (DEBIAN/ and the usr/share/oneplus-erhai-mesa
#      templates are kept -- in /usr mode they are moved out of the way for
#      the payload swap and restored afterwards; stale payload trees like
#      the retired /opt/turnip26 are removed),
#   5. strips the ELF payloads, bumps DEBIAN/control Version to
#      <mesa ver>~devel.git<count>.g<hash>-1erhai1 (commit count keeps the
#      version monotonic -- raw hashes compare numerically in dpkg) and adds
#      a libllvmN Depends when the build actually links libLLVM (shared-llvm).
#
# /usr mode is BUILD INPUT, not a deb:
#   next: ./build-mesa-usr-debs.sh   (splits oneplus-erhai-mesa/usr into the
#         six Ubuntu-named replacement .debs -- NEVER published to the PPA).
# /opt mode (the default, keeps the opportunistic opt-in layout):
#   next: ./build-mesa-deb.sh && ../update-ppa.sh --mvtomain
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="${MESA_SRC:-/media/data/develop/mesa}"
BUILD="$SRC/build-erhai"
BRANCH="${MESA_BRANCH:-main}"
STAGE="$HERE/oneplus-erhai-mesa"
PREFIX="${MESA_PREFIX:-/opt/mesa}"
PAYLOAD="$STAGE$PREFIX"
DEST="$HERE/../tmp/mesa-destdir.$$"

[ -d "$SRC/.git" ] || { echo "FATAL: no mesa git tree at $SRC" >&2; exit 1; }
[ -d "$STAGE/DEBIAN" ] || { echo "FATAL: no staging tree at $STAGE" >&2; exit 1; }

echo "==> updating $SRC ($BRANCH)"
[ -z "$(git -C "$SRC" status --porcelain)" ] || {
	echo "FATAL: $SRC has uncommitted changes -- commit or clean first" >&2
	exit 1
}
git -C "$SRC" fetch origin "$BRANCH"
[ "$(git -C "$SRC" branch --show-current)" = "$BRANCH" ] \
	|| git -C "$SRC" checkout "$BRANCH"
git -C "$SRC" pull --ff-only origin "$BRANCH"

REV="$(git -C "$SRC" rev-parse --short HEAD)"
# Monotonic element: dpkg compares the git-hash segment NUMERICALLY
# ("681ce7c2b8e" > "063a4640270" -- 681 > 063), so raw hashes make versions
# jump backwards. The upstream commit count is strictly increasing on main.
CNT="$(git -C "$SRC" rev-list --count HEAD)"
BASE="$(tr -d '[:space:]' < "$SRC/VERSION")"
if [[ "$BASE" == *-devel ]]; then
	VER="${BASE%-devel}~devel.git$CNT.g$REV-1erhai1"
else
	VER="$BASE+git$CNT.g$REV-1erhai1"
fi
CUR="$(awk '/^Version:/ {print $2}' "$STAGE/DEBIAN/control")"
echo "==> mesa $BASE @ $REV -> deb version $VER (staging: $CUR)"
if [ "$VER" = "$CUR" ] && [ "${FORCE:-0}" != 1 ]; then
	echo "nothing to do (staging already at $VER); FORCE=1 rebuilds anyway"
	exit 0
fi

OPTS=(
	-Dbuildtype=release
	-Dprefix="$PREFIX"
	-Dlibdir=lib/aarch64-linux-gnu
	-Dgallium-drivers=freedreno,zink,llvmpipe
	-Dvulkan-drivers=freedreno,swrast
	-Dfreedreno-kmds=msm
	-Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true
	-Dglx=dri -Dgbm=enabled
	-Dplatforms=x11,wayland
	-Dllvm=enabled -Dshared-llvm=enabled
	-Dgallium-va=disabled -Dvulkan-beta=false
	# OpenCL (rusticl) on the Adreno 830 -- the only working GPU compute stack
	# for llama.cpp/ggml on this pad (2026-09-21).  rusticl needs LLVM (clc) and
	# libclc's SPIR-V; static-libclc embeds the latter into libRusticlOpenCL.so
	# so /opt/mesa stays self-contained (no libclc runtime dependency).
	# NOTE the build needs: llvm-spirv(LLVMSPIRVLib), spirv-tools, libclc-21-dev,
	# bindgen and a rust toolchain -- they are NOT pulled in by this script.
	-Dgallium-rusticl=true
	-Dgallium-rusticl-enable-drivers=freedreno
	-Dstatic-libclc=all
)

if [ -f "$BUILD/meson-info/meson-info.json" ]; then
	echo "==> reconfiguring $BUILD"
	meson configure "$BUILD" "${OPTS[@]}" || {
		echo "reconfigure failed (meson version churn?) -- fresh configure" >&2
		find "$BUILD" -mindepth 1 -delete
		meson setup "$BUILD" "${OPTS[@]}"
	}
else
	echo "==> configuring $BUILD"
	meson setup "$BUILD" "${OPTS[@]}"
fi

echo "==> building (incremental)"
ninja -C "$BUILD"

echo "==> DESTDIR install"
find "$DEST" -mindepth 1 -delete 2>/dev/null || true
mkdir -p "$DEST"
DESTDIR="$DEST" ninja -C "$BUILD" install
[ -d "$DEST$PREFIX" ] || {
	echo "FATAL: unexpected DESTDIR layout (no $PREFIX)" >&2; exit 1; }

echo "==> swapping payload into $PAYLOAD"
# retire stale payload trees (the abandoned /opt/turnip26 layout) first
if [ -d "$STAGE/opt/turnip26" ] && [ "$PAYLOAD" != "$STAGE/opt/turnip26" ]; then
	find "$STAGE/opt/turnip26" -mindepth 1 -delete
	rmdir "$STAGE/opt/turnip26"
fi

# /usr mode: the doc/oneplus-erhai-mesa + template trees live INSIDE the
# $STAGE/usr payload path.  Park them before the wipe so the payload swap
# cannot eat them, restore them right after the copy.
KEEP_TMP="$STAGE/.usr-keep"
KEEP_PATHS=()
if [ "$PREFIX" = /usr ]; then
	KEEP_PATHS=("usr/share/doc/oneplus-erhai-mesa" "usr/share/oneplus-erhai-mesa")
	rm -rf "$KEEP_TMP"
	mkdir -p "$KEEP_TMP"
	for kp in "${KEEP_PATHS[@]}"; do
		if [ -e "$STAGE/$kp" ]; then
			# park WITHOUT the leading usr/ so the restore lands at the
			# real location ($STAGE/usr/<kp minus usr/>), not usr/usr/<...>
			kp_rel="${kp#usr/}"
			mkdir -p "$KEEP_TMP/$(dirname "$kp_rel")"
			mv "$STAGE/$kp" "$KEEP_TMP/$kp_rel"
		fi
	done
fi

[ -d "$PAYLOAD" ] && find "$PAYLOAD" -mindepth 1 -delete
mkdir -p "$PAYLOAD"
cp -a "$DEST$PREFIX/." "$PAYLOAD/"

if [ -n "${KEEP_PATHS:-}" ]; then
	cp -a "$KEEP_TMP/." "$STAGE/usr/"
	rm -rf "$KEEP_TMP"
fi

# OpenCL: meson installs rusticl's ICD json into $sysconfdir (/etc) and names the
# library by bare soname.  Ship it under that standard path but with the absolute
# path, so OpenCL clients do not depend on LD_LIBRARY_PATH (the ICD loader,
# ocl-icd-libopencl1, scans /etc/OpenCL/vendors by default).
if [ -f "$DEST/etc/OpenCL/vendors/rusticl.icd" ]; then
	echo "==> staging OpenCL ICD (absolute $PREFIX path)"
	mkdir -p "$STAGE/etc/OpenCL/vendors"
	echo "$PREFIX/lib/aarch64-linux-gnu/libRusticlOpenCL.so.1" > "$STAGE/etc/OpenCL/vendors/rusticl.icd"
fi

if [ "$PREFIX" = /usr ]; then
	# /usr replacement mode: drop the -dev-only artifacts so the runtime debs
	# claim exactly the files Ubuntu's runtime packages own.  Ubuntu's -dev
	# packages (libgl1-mesa-dev/libegl1-mesa-dev/mesa-common-dev) stay installed
	# and keep owning include/, pkgconfig/ and the unversioned .so symlinks --
	# an unclaimed extra copy would collide with them on install.
	echo "==> /usr mode: dropping -dev-only artifacts (include/, pkgconfig/, unversioned symlinks)"
	rm -rf "$PAYLOAD/include"
	rm -rf "$PAYLOAD/lib/aarch64-linux-gnu/pkgconfig"
	rm -f "$PAYLOAD/lib/aarch64-linux-gnu/libEGL_mesa.so" \
	      "$PAYLOAD/lib/aarch64-linux-gnu/libGLX_mesa.so" \
	      "$PAYLOAD/lib/aarch64-linux-gnu/libgbm.so" \
	      "$PAYLOAD/lib/aarch64-linux-gnu/libRusticlOpenCL.so"
fi

find "$DEST" -mindepth 1 -delete
rmdir "$DEST"

if [ "${STRIP:-1}" = 1 ] && command -v strip >/dev/null; then
	echo "==> stripping ELFs"
	find "$PAYLOAD" -type f \( -name '*.so' -o -name '*.so.*' \) \
		-exec strip --strip-unneeded {} +
fi

# Runtime deps: with -Dshared-llvm=enabled meson MAY link libLLVM into
# libgallium. Only list libllvmN in the control when the build really
# NEEDEDs it (the 26.2/26.3 builds did not).
LLVMNEED="$(find "$PAYLOAD" -type f -name '*.so*' -print0 |
	xargs -0 -r readelf -d 2>/dev/null |
	grep -o 'libLLVM-[0-9]*\.so' | sort -u | head -1 || true)"
if [ -n "$LLVMNEED" ]; then
	N="${LLVMNEED#libLLVM-}"; N="${N%.so}"
	if ! grep -q "libllvm$N" "$STAGE/DEBIAN/control"; then
		sed -i "0,/^Depends:/s//Depends: libllvm$N,\\n /" "$STAGE/DEBIAN/control"
		echo "==> added libllvm$N to Depends"
	fi
fi

sed -i "s/^Version: .*/Version: $VER/" "$STAGE/DEBIAN/control"
sed -i "s/^Description: Mesa .*/Description: Mesa $BASE (turnip\/freedreno) for the OnePlus Pad 3 (erhai)/" \
	"$STAGE/DEBIAN/control"
echo "==> staged mesa $VER ($(du -sh "$PAYLOAD" | cut -f1))"
if [ "$PREFIX" = /usr ]; then
	echo "next: $HERE/build-mesa-usr-debs.sh   (split oneplus-erhai-mesa/usr into the 6 replacement debs)"
else
	echo "next: $HERE/build-mesa-deb.sh && $ROOT/update-ppa.sh --mvtomain"
fi