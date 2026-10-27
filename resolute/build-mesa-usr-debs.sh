#!/bin/bash
# build-mesa-usr-debs.sh -- split the /usr-staged mesa payload into the SIX
# Ubuntu-named replacement .debs (oibaf-style in-place takeover).
#
#   ./update-mesa.sh                    # fetch + build + stage (oneplus-erhai-mesa/)
#   MESA_PREFIX=/usr ./update-mesa.sh   # re-stage the payload for /usr (builds!)
#   ./build-mesa-usr-debs.sh            # split oneplus-erhai-mesa/usr -> 6 debs
#
# WHY SAME NAMES?  libglx0 hard-Depends on libglx-mesa0 and libegl1 hard-Depends
# on libegl-mesa0, and every runtime mesa package pins its siblings with an
# exact-version "=" dependency.  A differently-named personal package can never
# take over those dependencies -- the ONLY clean replacement (the oibaf model)
# is to republish the SAME binary package names at a HIGHER version, so apt
# upgrades them in place and the = pins bind inside our own set.
#
# The six packages (mirrors Ubuntu's split on this release):
#   libgbm1             libgbm.so.1{,0.0}
#   mesa-libgallium     libgallium-<ver>.so          (+ Provides the VAAPI/etc
#                                                     virtuals Ubuntu carries)
#   libgl1-mesa-dri     dri/*, libRusticlOpenCL.so.1*, share/drirc.d,
#                       etc/OpenCL/vendors/rusticl.icd   (rusticl rides along)
#   libglx-mesa0        libGLX_mesa.so.0*
#   libegl-mesa0        libEGL_mesa.so.0*, share/glvnd/egl_vendor.d/50_mesa.json
#   mesa-vulkan-drivers libvulkan_{freedreno,lvp}.so, share/vulkan/icd.d/*.json
#
# Build output lands in debs/ (see ppa-common.sh DEB_DIR); install the six debs
# together with `sudo dpkg -i <debs>/*.deb` (the retired switch-mesa-usr.sh is
# gone -- a single deb alone leaves the Mesa stack half-replaced).  A TESTED
# generation is published by COPYING
# the six built debs
# into themojoman.github.io/ppa/oneplus-erhai/ubuntu/resolute/ + ../update-ppa.sh
# (done 2026-09-28 with 26.3.0~devel.git230441 -- the six Ubuntu-named debs are
# what the images install; build-root.sh lists exactly those names).  Never
# rebuild to publish: copy the built artifact, or the PPA and the dev tree drift
# apart; and do not publish an untested generation.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
STAGE="${MESA_USR_STAGE:-$HERE/oneplus-erhai-mesa}"
PAYLOAD="$STAGE/usr"
OUT="${MESA_USR_OUT:-$HERE/debs}"
VER="$(awk '/^Version:/ {print $2}' "$STAGE/DEBIAN/control")"
MAINT="$(awk '/^Maintainer:/ {print $2" "$3}' "$STAGE/DEBIAN/control")"

[ -d "$PAYLOAD/lib/aarch64-linux-gnu" ] || {
	echo "FATAL: no /usr payload at $PAYLOAD -- run 'MESA_PREFIX=/usr ./update-mesa.sh' first" >&2
	exit 1
}
[ -n "$VER" ] || { echo "FATAL: no Version in $STAGE/DEBIAN/control" >&2; exit 1; }

MISSING=0
# The /usr payload lives at $PAYLOAD (= $STAGE/usr); a hand-built deb stores
# install paths relative to '/', so its files must be prefixed with usr/ to
# land in /usr/lib..., /usr/share... (a bare lib/ would hit /lib, a bare
# share/ would hit a NONEXISTENT /share and the registries would be lost).
copy_many() { # copy_many <pkgdir> <relpath>...
	local d="$1"; shift
	mkdir -p "$d"
	for f in "$@"; do
		if [ -e "$PAYLOAD/$f" ] || [ -L "$PAYLOAD/$f" ]; then
			mkdir -p "$d/usr/$(dirname "$f")"
			cp -a "$PAYLOAD/$f" "$d/usr/$f"
		else
			echo "FATAL: $f missing from payload" >&2
			MISSING=1
		fi
	done
}
copy_tree() { # copy_tree <pkgdir> <rel-dir>
	local d="$1" sub="$2"
	if [ -d "$PAYLOAD/$sub" ]; then
		mkdir -p "$d/usr/$(dirname "$sub")"
		cp -a "$PAYLOAD/$sub" "$d/usr/$sub"
	else
		echo "FATAL: dir $sub missing from payload" >&2
		MISSING=1
	fi
}
# one-file glob helper (libgallium soname)
require_one() { # require_one <pkgdir> <glob>
	local d="$1" pat="$2" found
	found=( "$PAYLOAD"/$pat )
	if [ "${#found[@]}" -eq 1 ] && [ -e "${found[0]}" ]; then
		rel="${found[0]#$PAYLOAD/}"
		mkdir -p "$d/usr/$(dirname "$rel")"
		cp -a "${found[0]}" "$d/usr/$rel"
	else
		echo "FATAL: expected exactly one '$pat' in payload (got ${#found[@]})" >&2
		MISSING=1
	fi
}
rm -rf "$OUT"
mkdir -p "$OUT"

echo "==> version $VER"
echo "==> splitting $PAYLOAD"

# ---- libgbm1 ---------------------------------------------------------------
D="$OUT/libgbm1"
copy_many "$D" lib/aarch64-linux-gnu/libgbm.so.1 lib/aarch64-linux-gnu/libgbm.so.1.0.0
# GBM loads its backend module relative to the libgbm soname location, so the
# gbm/ backend dir MUST ride along in libgbm1 (Ubuntu ships it there too).
# MISSING HERE = "Failed to create gbm device: No such file or directory" ->
# "Failed to setup: No GPUs found" -> the desktop never reaches a session
# (measured on sda18, 2026-09-23: greeter + autologin shell both died on it).
copy_tree "$D" lib/aarch64-linux-gnu/gbm

# ---- mesa-libgallium -------------------------------------------------------
D="$OUT/mesa-libgallium"
require_one "$D" 'lib/aarch64-linux-gnu/libgallium-*.so'

# ---- libgl1-mesa-dri -------------------------------------------------------
D="$OUT/libgl1-mesa-dri"
copy_tree "$D" lib/aarch64-linux-gnu/dri
copy_many "$D" \
	lib/aarch64-linux-gnu/libRusticlOpenCL.so.1 \
	lib/aarch64-linux-gnu/libRusticlOpenCL.so.1.0.0
# the WHOLE drirc.d tree (Ubuntu ships it whole here): the per-driver defaults
# (00-msm-defaults.conf etc.) are as load-bearing as 00-mesa-defaults.conf.
copy_tree "$D" share/drirc.d
# rusticl ICD lives outside the /usr payload (sysconfdir=/etc), like the
# /opt mode already stages it.  Route it into libgl1-mesa-dri alongside the
# rusticl runtime.
if [ -f "$STAGE/etc/OpenCL/vendors/rusticl.icd" ]; then
	mkdir -p "$D/etc/OpenCL/vendors"
	cp -a "$STAGE/etc/OpenCL/vendors/rusticl.icd" "$D/etc/OpenCL/vendors/"
else
	echo "FATAL: $STAGE/etc/OpenCL/vendors/rusticl.icd missing" >&2
	MISSING=1
fi

# ---- libglx-mesa0 -----------------------------------------------------------
D="$OUT/libglx-mesa0"
copy_many "$D" lib/aarch64-linux-gnu/libGLX_mesa.so.0 lib/aarch64-linux-gnu/libGLX_mesa.so.0.0.0

# ---- libegl-mesa0 -----------------------------------------------------------
D="$OUT/libegl-mesa0"
copy_many "$D" \
	lib/aarch64-linux-gnu/libEGL_mesa.so.0 \
	lib/aarch64-linux-gnu/libEGL_mesa.so.0.0.0 \
	share/glvnd/egl_vendor.d/50_mesa.json

# ---- mesa-vulkan-drivers ----------------------------------------------------
D="$OUT/mesa-vulkan-drivers"
copy_many "$D" \
	lib/aarch64-linux-gnu/libvulkan_freedreno.so \
	lib/aarch64-linux-gnu/libvulkan_lvp.so \
	share/vulkan/icd.d/freedreno_icd.aarch64.json \
	share/vulkan/icd.d/lvp_icd.aarch64.json

[ "$MISSING" = 0 ] || { echo "FATAL: payload incomplete, nothing built" >&2; exit 1; }

# ---- exhaustive routing self-check ------------------------------------------
# Every file the /usr payload ships (after update-mesa.sh's pruning) must be
# claimed by exactly one deb.  The carrier's own doc/template trees under
# share/doc/ and share/oneplus-erhai-mesa/ stay owned by the /opt carrier
# package, so they are the only legitimate exclusions.
echo "==> routing self-check (payload files not claimed by any deb)"
UNCLAIMED=0
for f in $(cd "$PAYLOAD" && find . -type f -o -type l | sed 's|^\./||' | sort); do
	case "$f" in
		share/doc/*|share/oneplus-erhai-mesa/*) continue ;;
	esac
	if ! [ -e "$OUT"/*/usr/"$f" ]; then
		echo "   UNCLAIMED: $f"
		UNCLAIMED=1
	fi
done
[ "$UNCLAIMED" = 0 ] || { echo "FATAL: payload files unclaimed by the deb split" >&2; exit 1; }
echo "   all payload files claimed"

echo "==> writing controls"

mkdir -p "$OUT/libgbm1/DEBIAN"
cat > "$OUT/libgbm1/DEBIAN/control" <<EOF
Package: libgbm1
Version: $VER
Section: libs
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: mesa-libgallium (= $VER), libc6 (>= 2.38), libdrm2 (>= 2.4.125-1), libexpat1 (>= 2.0.1)
Installed-Size: 0
Description: free implementation of the Generic Buffer Manager API -- runtime (erhai, from git)
 GBM is a generic buffer manager API used by the Mesa DRI drivers.
 This is the in-place replacement for Ubuntu's libgbm1, rebuilt from mesa git
 for the OnePlus Pad 3 (erhai) so the Adreno 830 is supported.
EOF

mkdir -p "$OUT/mesa-libgallium/DEBIAN"
cat > "$OUT/mesa-libgallium/DEBIAN/control" <<EOF
Package: mesa-libgallium
Version: $VER
Section: libs
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: libc6 (>= 2.43), libdrm-amdgpu1 (>= 2.4.125-1), libdrm2 (>= 2.4.125-1), libelf1t64 (>= 0.142), libexpat1 (>= 2.0.1), libgcc-s1 (>= 3.0), libllvm21 (>= 1:21.1.0), libsensors5 (>= 1:3.5.0), libstdc++6 (>= 11), libx11-xcb1, libxcb-dri3-0 (>= 1.13), libxcb-present0, libxcb-randr0, libxcb-sync1, libxcb-xfixes0, libxcb1 (>= 1.9.2), libxshmfence1, libzstd1 (>= 1.5.5), zlib1g (>= 1:1.2.3.4)
Provides: libglapi-mesa, mesa-va-drivers, mesa-vdpau-drivers, va-driver
Replaces: mesa-va-drivers (<< 25.2.8-3), mesa-vdpau-drivers (<< 25.2.8-3)
Breaks: mesa-va-drivers (<< 25.2.8-3), mesa-vdpau-drivers (<< 25.2.8-3)
Installed-Size: 0
Description: shared infrastructure for Mesa drivers (erhai, from git)
 In-place replacement for Ubuntu's mesa-libgallium, rebuilt from mesa git for
 the OnePlus Pad 3 (erhai).  Carries the freedreno/llvmpipe/zink gallium core.
EOF

mkdir -p "$OUT/libgl1-mesa-dri/DEBIAN"
cat > "$OUT/libgl1-mesa-dri/DEBIAN/control" <<EOF
Package: libgl1-mesa-dri
Version: $VER
Section: libs
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: libgbm1 (= $VER), libvulkan1, libc6 (>= 2.34)
Replaces: oneplus-erhai-mesa (<< $VER)
Installed-Size: 0
Description: free implementation of the OpenGL API -- DRI modules (erhai, from git)
 In-place replacement for Ubuntu's libgl1-mesa-dri, rebuilt from mesa git for
 the OnePlus Pad 3 (erhai).  Also carries the rusticl OpenCL ICD + runtime
 (Adreno 830 GPU compute).
EOF

mkdir -p "$OUT/libglx-mesa0/DEBIAN"
cat > "$OUT/libglx-mesa0/DEBIAN/control" <<EOF
Package: libglx-mesa0
Version: $VER
Section: libs
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: libgl1-mesa-dri, mesa-libgallium (= $VER), libc6 (>= 2.38), libdrm2 (>= 2.4.125-1), libexpat1 (>= 2.0.1), libx11-6 (>= 2:1.4.99.1), libx11-xcb1, libxcb-dri3-0, libxcb-glx0 (>= 1.8), libxcb-present0, libxcb-shm0, libxcb-xfixes0, libxcb1, libxext6, libxxf86vm1 (>= 1:1.1.4)
Provides: libglx-vendor
Breaks: glx-diversions (<< 0.8.4~), libopengl-perl (<< 0.6704+dfsg-2)
Installed-Size: 0
Description: free implementation of the OpenGL API -- GLX vendor library (erhai, from git)
 In-place replacement for Ubuntu's libglx-mesa0, rebuilt from mesa git for the
 OnePlus Pad 3 (erhai).
EOF

mkdir -p "$OUT/libegl-mesa0/DEBIAN"
cat > "$OUT/libegl-mesa0/DEBIAN/control" <<EOF
Package: libegl-mesa0
Version: $VER
Section: libs
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: libgbm1 (= $VER), mesa-libgallium (= $VER), libc6 (>= 2.38), libdrm2 (>= 2.4.125-1), libexpat1 (>= 2.0.1), libwayland-client0 (>= 1.24.0), libx11-6, libx11-xcb1, libxcb-dri3-0 (>= 1.13), libxcb-present0, libxcb-randr0 (>= 1.3), libxcb-shm0, libxcb-xfixes0, libxcb1 (>= 1.6)
Provides: libegl-vendor
Installed-Size: 0
Description: free implementation of the EGL API -- Mesa vendor library (erhai, from git)
 In-place replacement for Ubuntu's libegl-mesa0, rebuilt from mesa git for the
 OnePlus Pad 3 (erhai).
EOF

mkdir -p "$OUT/mesa-vulkan-drivers/DEBIAN"
cat > "$OUT/mesa-vulkan-drivers/DEBIAN/control" <<EOF
Package: mesa-vulkan-drivers
Version: $VER
Section: graphics
Priority: optional
Architecture: arm64
Maintainer: $MAINT
Depends: libvulkan1, libc6 (>= 2.43), libdisplay-info3 (>= 0.3.0), libdrm-amdgpu1 (>= 2.4.125-1), libdrm2 (>= 2.4.125-1), libelf1t64 (>= 0.142), libexpat1 (>= 2.0.1), libgcc-s1 (>= 4.2), libllvm21 (>= 1:21.1.0), libpng16-16t64 (>= 1.6.46), libstdc++6 (>= 13.1), libwayland-client0 (>= 1.24.0), libx11-xcb1, libxcb-dri3-0 (>= 1.17.0), libxcb-present0 (>= 1.17.0), libxcb-randr0 (>= 1.13), libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb1 (>= 1.9.2), libxshmfence1, libzstd1 (>= 1.5.5), zlib1g (>= 1:1.2.3.4)
Provides: vulkan-icd
Installed-Size: 0
Description: Mesa Vulkan graphics drivers (erhai, from git)
 In-place replacement for Ubuntu's mesa-vulkan-drivers: turnip (freedreno
 Vulkan 1.4) + lavapipe for the OnePlus Pad 3 (erhai).
EOF
echo "==> building debs (md5sums + fakeroot dpkg-deb)"
DEBS=()
for d in "$OUT"/*/; do
	d="${d%/}"
	name="$(basename "$d")"
	# honest sizes: sub in real Installed-Size, recompute md5sums every build
	size="$(du -sk --exclude=DEBIAN "$d" | cut -f1)"
	sed -i "s/^Installed-Size: .*/Installed-Size: $size/" "$d/DEBIAN/control"
	rm -f "$d/DEBIAN/md5sums"
	( cd "$d" \
	  && find . -path ./DEBIAN -prune -o -type f -print0 \
	  | sed -z 's|^\./||' \
	  | sort -z \
	  | xargs -0 -r md5sum > DEBIAN/md5sums )
	chmod 644 "$d/DEBIAN/control" "$d/DEBIAN/md5sums"
	fakeroot dpkg-deb --build --root-owner-group "$d" "$OUT/${name}_${VER}_arm64.deb"
	DEBS+=("$OUT/${name}_${VER}_arm64.deb")
done

echo "==> built:"
for b in "${DEBS[@]}"; do
	echo "   ${b##*/}"
	dpkg-deb --info "$b" | sed -n '1,6p'
	echo
done

echo "==> next (on the device, as root):"
echo "    sudo dpkg -i $OUT/*.deb"
echo "    (all six in ONE deb run -- a single deb alone leaves the Mesa stack half-replaced)"
echo "==> publish (ONLY after the generation boot-tested; copy, never rebuild):"
echo "    export PPA_DEB_DIR=$ROOT/themojoman.github.io/ppa/oneplus-erhai/ubuntu/resolute"
echo "    . $HERE/ppa-common.sh; ppa_copy $OUT/*.deb && (cd $ROOT && ./update-ppa.sh)"
echo "    (NOTE: 'PPA_DEB_DIR=... . ./ppa-common.sh' does NOT survive the source -- bash"
echo "     reverts the prefix when the '.' command finishes, so a LATER ppa_copy/ppa_mv"
echo "     sees it EMPTY.  Export it first, or prefix the ppa_copy call itself)"