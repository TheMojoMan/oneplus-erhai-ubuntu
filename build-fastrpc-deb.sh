#!/bin/bash
# Build oneplus-erhai-fastrpc_<version>_arm64.deb from the
# oneplus-erhai-fastrpc/ staging tree.
#
#   ./build-fastrpc-deb.sh                 # stage + build + publish to TESTING
#   NO_PPA=1 ./build-fastrpc-deb.sh        # build locally, do not publish
#   STAGE=<dir> RFSA_SRC=<dir> QCOM_SRC=<dir> ./build-fastrpc-deb.sh
#
# Sources of the payload:
#   STAGE    (default ../tmp/qnn/fastrpc-stage) the pristine `make install
#            DESTDIR` tree of github.com/qualcomm/fastrpc -> usr/local/*;
#   RFSA_SRC (default /usr/lib/rfsa) + QCOM_SRC (default
#            /usr/share/qcom/hexagon/v79): the DSP payload trees, vendor blobs
#            staged by hand during bring-up (OxygenOS dsp.img, QNN/QAIRT, GenieX).
# The DEBIAN/, etc/ and usr/lib/sysusers.d files in the package tree are
# hand-maintained copies of the working system and are shipped as they are.
#
# Deliberately dropped from the payload: the libtool *.la files, the
# adsprpcd/sdsprpcd/gdsprpcd daemons (and their units -- the ADSP path belongs to
# oneplus-erhai-sensors), and the QNN skel backups (*.previous,
# *.qai_appbuilder).  See the package README ("Deliberately not shipped").
#
# The script verifies the payload instead of trusting it: cdsprpcd must be an
# ELF, libcdsprpc must carry SONAME libcdsprpc.so.1 with a working symlink chain
# and resolve all of its libraries, the shells/QNN skel/C++ runtime must be in
# each rfsa tree, and EVERY staged file that also exists on this machine must be
# byte-identical to it (the package has to reproduce the running state).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-fastrpc"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

STAGE="${STAGE:-$HERE/../tmp/qnn/fastrpc-stage}"
RFSA_SRC="${RFSA_SRC:-/usr/lib/rfsa}"
QCOM_SRC="${QCOM_SRC:-/usr/share/qcom/hexagon/v79}"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$STAGE/usr/local/sbin/cdsprpcd" ] || die "no cdsprpcd in $STAGE (set STAGE)"
[ -d "$RFSA_SRC/adsp" ] || die "no $RFSA_SRC/adsp (set RFSA_SRC)"
[ -d "$RFSA_SRC/cdsp" ] || die "no $RFSA_SRC/cdsp (set RFSA_SRC)"
[ -d "$QCOM_SRC" ] || die "no $QCOM_SRC (set QCOM_SRC)"
for f in DEBIAN/postinst DEBIAN/postrm etc/systemd/system/cdsprpcd.service \
	 etc/systemd/system/cdsprpcd.service.d/erhai.conf \
	 etc/udev/rules.d/60-fastrpc.rules etc/environment.d/57-erhai-fastrpc-unsigned.conf \
	 usr/lib/sysusers.d/fastrpc.conf usr/share/oneplus-erhai-fastrpc/README.md; do
	[ -e "$SRC/$f" ] || die "missing $f in $SRC"
done

# Documentation lives OUTSIDE /usr/share/doc on purpose: Ubuntu's dpkg installs
# with path-exclude=/usr/share/doc/* (/etc/dpkg/dpkg.cfg.d/excludes), so a package
# README under doc/ is never readable on a device.  This copy is, and the policy
# path is generated from it (same content, both shipped).
install -D -m644 "$SRC/usr/share/oneplus-erhai-fastrpc/README.md" \
	"$SRC/usr/share/doc/oneplus-erhai-fastrpc/README.md"
install -D -m644 "$SRC/usr/share/oneplus-erhai-fastrpc/build-fastrpc.sh" \
	"$SRC/usr/share/doc/oneplus-erhai-fastrpc/build-fastrpc.sh"

echo "==> staging Qualcomm userspace from $STAGE"
for d in usr/local usr/lib/rfsa usr/share/qcom; do
	[ -d "$SRC/$d" ] && find "$SRC/$d" -mindepth 1 \( -type f -o -type l \) -exec rm -f {} +
	[ -d "$SRC/$d" ] && find "$SRC/$d" -mindepth 1 -depth -type d -empty -delete
done
mkdir -p "$SRC/usr/local"
cp -a "$STAGE/usr/local/." "$SRC/usr/local/"

# Drop the daemons/unistalled-unit payload and the libtool metadata.
for f in usr/local/sbin/adsprpcd usr/local/sbin/sdsprpcd usr/local/sbin/gdsprpcd; do
	rm -f "$SRC/$f"
done
find "$SRC/usr/local" -name '*.la' -type f -delete
[ -x "$SRC/usr/local/sbin/cdsprpcd" ] || die "cdsprpcd did not survive staging"
for f in adsprpcd sdsprpcd gdsprpcd; do
	[ -e "$SRC/usr/local/sbin/$f" ] && die "$f must not be shipped"
done
# The systemd units come from this package tree (etc/), not from the upstream
# build: our cdsprpcd.service is ConditionPathExists-gated and hardened, and the
# adsprpcd/sdsprpcd/... units would reference daemons we deliberately do not ship.
rm -rf "$SRC/usr/lib/systemd"

echo "==> staging the DSP payload trees"
mkdir -p "$SRC/usr/lib/rfsa" "$SRC/usr/share/qcom/hexagon/v79"
cp -a "$RFSA_SRC/." "$SRC/usr/lib/rfsa/"
cp -a "$QCOM_SRC/." "$SRC/usr/share/qcom/hexagon/v79/"
# nothing loads a ".previous"/".qai_appbuilder" backup copy
find "$SRC/usr/lib/rfsa" "$SRC/usr/share/qcom" \
	\( -name '*.previous' -o -name '*.qai_appbuilder' \) -type f -delete

# ------------------------------------------------------------------- checks ----
file -b "$SRC/usr/local/sbin/cdsprpcd" | grep -q 'ARM aarch64' \
	|| die "cdsprpcd is not an aarch64 ELF"
LIB="$SRC/usr/local/lib/libcdsprpc.so"
[ -f "$LIB.1.0.0" ] || die "libcdsprpc.so.1.0.0 missing"
[ -L "$LIB.1" ] && [ -L "$LIB" ] || die "libcdsprpc symlink chain incomplete"
[ "$(readelf -d "$LIB.1.0.0" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" = "libcdsprpc.so.1" ] \
	|| die "unexpected libcdsprpc SONAME"
lddout="$(ldd "$LIB.1.0.0" 2>&1)" || die "ldd failed on libcdsprpc"
printf '%s\n' "$lddout" | grep -q 'not found' && { printf '%s\n' "$lddout"; die "libcdsprpc has unresolved libs"; }
for tree in usr/lib/rfsa/adsp usr/lib/rfsa/cdsp; do
	for f in fastrpc_shell_3 fastrpc_shell_unsigned_3 libc++.so.1; do
		[ -f "$SRC/$tree/$f" ] || die "$tree/$f missing"
	done
done
[ -f "$SRC/usr/lib/rfsa/adsp/libQnnHtpV79Skel.so" ] || die "QNN HTP v79 skel missing in rfsa/adsp"
[ -f "$SRC/usr/share/qcom/hexagon/v79/libQnnHtpV79Skel.so" ] || die "QNN HTP v79 skel missing in qcom/v79"


# ------------------------------------------- payload must match this machine ---
# Everything staged here also exists on this device (installed by hand during the
# NPU bring-up), so a byte-for-byte comparison is the strongest possible check
# that the package reproduces the state that works.
# A difference is EXPECTED in one direction only: usr/local comes from the
# pristine build tree, and this machine still carries an older install of two
# libraries (libadsprpc/libsdsprpc were installed at 17:59, before the
# FASTRPC_UNSIGNED_PD patch was rebuilt at 18:24) -- the package ships the newer,
# patched build and REPLACES them on install.  The check therefore fails only
# when the live copy is NEWER than the payload (that would mean the build tree is
# older than what runs, i.e. the payload is stale).
same=0 stale=0 newer=0 absent=0
stale_list=""
while read -r f; do
	rel="${f#"$SRC"/}"
	if [ ! -e "/$rel" ]; then absent=$((absent + 1)); continue; fi
	if cmp -s "$f" "/$rel"; then same=$((same + 1)); continue; fi
	if [ "/$rel" -nt "$f" ]; then
		echo "  STALE PAYLOAD: /$rel is newer than the staged copy"
		newer=$((newer + 1))
	else
		stale_list="$stale_list $rel"
		stale=$((stale + 1))
	fi
done < <(find "$SRC/usr/local" "$SRC/usr/lib/rfsa" "$SRC/usr/share/qcom" "$SRC/etc" \
		"$SRC/usr/lib/sysusers.d" -type f)
echo "==> live comparison: $same identical to this machine, $stale to be REPLACED by a newer build, $absent not installed here"
[ -z "$stale_list" ] || echo "    replaced on install:$stale_list"
[ "$newer" = 0 ] || die "the build tree is older than the running system (see above)"

# --------------------------------------------- no file conflicts with others ---
# The whole point of the npu/sensors/fastrpc split is unambiguous ownership: no
# path this package ships may already belong to another package.  One dpkg-query
# call for all paths (a per-file loop over ~1500 files takes ~30 s).
mapfile -d '' -t SHIPPED < <(find "$SRC/usr" "$SRC/etc" \( -type f -o -type l \) \
	! -path '*/DEBIAN/*' -print0)
conflicts=0
while IFS= read -r line; do
	pkgs="${line%%:*}"
	path="${line#*: }"
	case ",$pkgs," in
	*",$PKG,"*) continue ;;
	esac
	echo "  CONFLICT: $path is owned by $pkgs"
	conflicts=$((conflicts + 1))
done < <(dpkg-query -S "${SHIPPED[@]}" 2>/dev/null || true)
[ "$conflicts" = 0 ] || die "$conflicts file(s) already belong to another package"
echo "==> file ownership: ${#SHIPPED[@]} shipped paths, none owned by another package"

# --------------------------------------------------------------- permissions ---
find "$SRC" -type d -exec chmod 755 {} +
find "$SRC" -type f -exec chmod 644 {} +
find "$SRC/usr/local/bin" "$SRC/usr/local/sbin" -type f -exec chmod 755 {} +
chmod 755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm"

# ------------------------------------------------------------ file-list guard --
python3 - "$SRC" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])
allowed = ('DEBIAN/', 'etc/', 'usr/')
found = sorted(str(p.relative_to(src)) for p in src.rglob('*') if p.is_file())
extra = [f for f in found if not f.startswith(allowed)]
if extra:
    sys.exit('unexpected files in the package tree (move them to tmp/):\n  '
             + '\n  '.join(extra))
print(f'package tree: {len(found)} files, all expected')
PY

# ------------------------------------------------------------------ metadata ---
SIZE=$(du -sk --exclude=DEBIAN "$SRC" | cut -f1)
if grep -q '^Installed-Size:' "$SRC/DEBIAN/control"; then
	sed -i "s/^Installed-Size: .*/Installed-Size: $SIZE/" "$SRC/DEBIAN/control"
else
	sed -i "/^Priority:/a Installed-Size: $SIZE" "$SRC/DEBIAN/control"
fi
rm -f "$SRC/DEBIAN/md5sums"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )

# --------------------------------------------------------------------- build ---
OUT="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
# personal data: no build path in the payload, no DWARF in the ELFs
ppa_scrub_debug "$SRC"
dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
dpkg-deb --info "$OUT" | sed -n '1,12p'
echo "payload: $(du -sh "$SRC" | cut -f1) installed, ${SIZE} KiB"

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
