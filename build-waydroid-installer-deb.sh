#!/bin/bash
# Build oneplus-erhai-waydroid-installer_<version>_arm64.deb from the
# oneplus-erhai-waydroid-installer/ staging tree.
#
#   ./build-waydroid-installer-deb.sh -> debs/oneplus-erhai-waydroid-installer_<version>_arm64.deb
#
# The staging tree carries the maintainer scripts, the installer helper, the
# apt post-invoke hook and the Waydroid apt repository (key + deb822 source, so
# the package can install waydroid itself).  Two payload pieces are GENERATED
# here (never committed, see .gitignore):
#
#   usr/lib/waydroid/tools/erhai_vendor_patch.py
#       the patcher, built from ../../tmp/waydroid-patcher/src/ by its build.py
#       WITHOUT an embedded payload (PAYLOAD_SHA256 is baked in, the five files
#       travel separately) -- it lands in waydroid's OWN tools package so
#       tools/helpers/images.py can `from tools import erhai_vendor_patch`
#   usr/share/erhai-waydroid-mesa/payload/*.so
#       the five stripped Mesa shared objects (the source of truth is
#       ../../tmp/waydroid-patcher/stripped/)
#
# arm64: the payload is architecture-specific.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-waydroid-installer"
PATCHER_DIR="${PATCHER_DIR:-$HERE/../tmp/waydroid-patcher}"

PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

PAYLOAD="$SRC/usr/share/erhai-waydroid-mesa/payload"
TOOLS="$SRC/usr/lib/waydroid/tools"

[ -x "$PATCHER_DIR/build.py" ] ||
	{ echo "missing $PATCHER_DIR/build.py (the patcher source tree)" >&2; exit 1; }
for f in libgallium_dri.so libvulkan_freedreno.so libEGL_mesa.so \
	 libGLESv1_CM_mesa.so libGLESv2_mesa.so; do
	[ -s "$PATCHER_DIR/stripped/$f" ] ||
		{ echo "payload file missing/empty: $PATCHER_DIR/stripped/$f" >&2; exit 1; }
done

# ---- 1. the patcher ------------------------------------------------------- #
# No --embed: the .deb ships the payload as real files, so it is compressed
# once instead of twice.  build.py refuses to produce a file whose sha256 table
# does not match the payload on disk.
python3 "$PATCHER_DIR/build.py"
install -d -m755 "$TOOLS"
install -m755 "$PATCHER_DIR/dist/erhai_vendor_patch.py" \
	"$TOOLS/erhai_vendor_patch.py"

# ---- 2. the payload ------------------------------------------------------- #
install -d -m755 "$PAYLOAD"
install -m644 "$PATCHER_DIR/stripped/libgallium_dri.so" \
	"$PATCHER_DIR/stripped/libvulkan_freedreno.so" \
	"$PATCHER_DIR/stripped/libEGL_mesa.so" \
	"$PATCHER_DIR/stripped/libGLESv1_CM_mesa.so" \
	"$PATCHER_DIR/stripped/libGLESv2_mesa.so" "$PAYLOAD/"

# The baked-in table must describe exactly these bytes, else waydroid would
# "patch" the image with a payload the caller cannot verify.
PYTHONDONTWRITEBYTECODE=1 python3 - "$TOOLS/erhai_vendor_patch.py" "$PAYLOAD" <<'PY'
import hashlib, importlib.util, os, sys
sys.dont_write_bytecode = True
mod_path, pdir = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("_p", mod_path)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
bad = []
for _, name in m.TARGETS:
    p = os.path.join(pdir, name)
    h = hashlib.sha256(open(p, "rb").read()).hexdigest()
    if m.PAYLOAD_SHA256.get(name) != h:
        bad.append(name)
if m.PAYLOAD_XZ_B64:
    bad.append("payload is embedded (build.py must run without --embed)")
if bad:
    sys.exit("payload/sha256 mismatch: %s" % " ".join(bad))
print("payload   %d files, sha256 table matches" % len(m.TARGETS))
PY

# ---- 3. permissions, symlink, manifest ------------------------------------ #
# convenience CLI: the same file waydroid imports, on PATH
install -d -m755 "$SRC/usr/local/bin"
ln -sfn /usr/lib/waydroid/tools/erhai_vendor_patch.py \
	"$SRC/usr/local/bin/erhai-waydroid-vendor-patch"

# A bytecode cache is build scratch, never payload (the sha256 check above
# IMPORTS the patcher, which is what dropped one in on the first build) -- and
# the same guard build-desktop-deb.sh has, so it cannot come back.
PYC="$TOOLS/__pycache__"
if [ -d "$PYC" ]; then
	find "$PYC" -type f -delete
	rmdir "$PYC"
fi
if find "$SRC" -name __pycache__ -print -quit | grep -q .; then
	echo "refusing to pack: a __pycache__ inside $SRC" >&2
	find "$SRC" -name __pycache__ >&2
	exit 1
fi

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm" \
	"$SRC/usr/local/sbin/waydroid-erhai-installer.sh" \
	"$SRC/usr/local/sbin/erhai-waydroid-container-guard" \
	"$TOOLS/erhai_vendor_patch.py"
chmod 644 "$SRC/DEBIAN/control" "$SRC/DEBIAN/conffiles" \
	"$SRC/etc/apt/apt.conf.d/99erhai-waydroid-mesa" \
	"$SRC/etc/apt/sources.list.d/waydroid.sources" \
	"$SRC/usr/share/keyrings/waydroid.gpg" \
	"$SRC/usr/lib/systemd/system/erhai-waydroid-container-guard.service" \
	"$PAYLOAD"/*.so

cd "$SRC"
find etc usr -type f -exec md5sum {} \; >DEBIAN/md5sums
cd "$HERE"

dpkg-deb --build --root-owner-group "$SRC" \
	"$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
	echo "built ${PKG}_${VER}_${ARCH}.deb"
else
	echo "NO_PPA=1: built ${PKG}_${VER}_${ARCH}.deb (NOT published; stays in $DEB_DIR)"
fi
