#!/bin/bash
# Build oneplus-erhai-npu_<version>_arm64.deb from the oneplus-erhai-npu/
# staging tree.
#
#   ./build-npu-deb.sh               -> oneplus-erhai-npu_<version>_arm64.deb
#   NO_PPA=1 ./build-npu-deb.sh      build locally, do not publish
#
# The NPU/HTP WORKLOAD layer of erhai's Hexagon stack (the transport layer --
# libcdsprpc, cdsprpcd, the fastrpc group, the DSP payload trees -- is
# oneplus-erhai-fastrpc, and the ADSP path is oneplus-erhai-sensors):
#   - the complete CDSP remote filesystem served to the DSP
#     (/var/lib/hexagonfs-cdsp/dsp/cdsp: fastrpc_shell_3, the _unsigned_3 user-PD
#     shell, the Hexagon CoProcessor (HTP) runtime -- libhcp_rpc_skel.so,
#     libhcpfrc.so, ubwcdma_dynlib.so -- the vendor QNN HTP v79 skel and the C++
#     runtime the skels link against);
#   - hexagonrpcd-cdsp.service, the DISABLED fallback daemon;
#   - erhai-npu-status.sh (chain health report, --pd-test proves a host-uploaded
#     shell runs on the DSP);
#   - erhai-cdsp-restart.sh (the recovery for an exhausted FastRPC arena);
#   - npu-llm + the n_gpu_layers helper (the GenieX-based NPU front end).
#
# The DSP-side binaries are the device's own OxygenOS 16.0.9.400 files
# (dsp.img /cdsp + vendor /lib64/hw/audio/libQnnHtpV79Skel.so), so they are
# Hexagon ELF => Architecture: arm64 (like the sensors package's DSP tree).
#
# Publish target is the private TESTING suite by default; NO_PPA=1 (or
# PPA_DEB_DIR=<dir>) keeps an in-development build local.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../ppa-common.sh"
SRC="$HERE/oneplus-erhai-npu"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

die() { echo "ERROR: $*" >&2; exit 1; }

for f in DEBIAN/postinst DEBIAN/postrm \
	 etc/systemd/system/hexagonrpcd-cdsp.service \
	 usr/local/sbin/erhai-npu-status.sh \
	 usr/local/sbin/erhai-cdsp-restart.sh \
	 usr/local/bin/npu-llm \
	 usr/local/bin/die-budget \
	 usr/local/lib/oneplus-erhai-npu/die-budget.sh \
	 usr/local/lib/oneplus-erhai-npu/geniex-ngl.py \
	 usr/share/oneplus-erhai-npu/README.md; do
	[ -e "$SRC/$f" ] || die "missing $SRC/$f"
done

# The README is shipped OUTSIDE /usr/share/doc as well: Ubuntu installs with
# path-exclude=/usr/share/doc/* (/etc/dpkg/dpkg.cfg.d/excludes), so a doc-only
# README is never readable on a device.
install -D -m644 "$SRC/usr/share/oneplus-erhai-npu/README.md" \
	"$SRC/usr/share/doc/oneplus-erhai-npu/README.md"
[ -n "$(ls -A "$SRC/var/lib/hexagonfs-cdsp/dsp/cdsp" 2>/dev/null)" ] \
	|| die "remotefs tree is empty: $SRC/var/lib/hexagonfs-cdsp/dsp/cdsp"
for f in fastrpc_shell_3 fastrpc_shell_unsigned_3 libhcp_rpc_skel.so libQnnHtpV79Skel.so; do
	[ -f "$SRC/var/lib/hexagonfs-cdsp/dsp/cdsp/$f" ] || die "remotefs tree lacks $f"
done

# No absolute workspace paths may survive in the shipped scripts (the whole
# point of packaging them: they must work without /media/data/develop/erhai).
leak="$(grep -l '/media/data\|/home/themojoman' \
	"$SRC/usr/local/bin/npu-llm" "$SRC/usr/local/sbin/"*.sh \
	"$SRC/usr/local/lib/oneplus-erhai-npu/"*.py 2>/dev/null || true)"
[ -z "$leak" ] || die "workspace paths leaked into: $leak"

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC/DEBIAN/postinst" "$SRC/DEBIAN/postrm" \
	  "$SRC/usr/local/sbin/erhai-npu-status.sh" \
	  "$SRC/usr/local/sbin/erhai-cdsp-restart.sh" \
	  "$SRC/usr/local/bin/npu-llm" \
	  "$SRC/usr/local/bin/die-budget" \
	  "$SRC/usr/local/lib/oneplus-erhai-npu/geniex-ngl.py"
find "$SRC/var/lib/hexagonfs-cdsp" -type f -exec chmod 644 {} \;

# Shell/python syntax + a real run of the read-only status script (a broken
# report is the one thing that must never ship: it is the first thing the owner
# runs when the NPU misbehaves).
bash -n "$SRC/usr/local/sbin/erhai-npu-status.sh"
bash -n "$SRC/usr/local/sbin/erhai-cdsp-restart.sh"
bash -n "$SRC/usr/local/bin/npu-llm"
bash -n "$SRC/usr/local/bin/die-budget"
bash -n "$SRC/usr/local/lib/oneplus-erhai-npu/die-budget.sh"
python3 -m py_compile "$SRC/usr/local/lib/oneplus-erhai-npu/geniex-ngl.py"
rm -rf "$SRC/usr/local/lib/oneplus-erhai-npu/__pycache__"
echo "==> syntax OK"
"$SRC/usr/local/sbin/erhai-npu-status.sh" > /tmp/.npu-status.$$ 2>&1 || true
if grep -q '^=== erhai NPU' /tmp/.npu-status.$$; then
	echo "==> status script runs: $(grep -c '^' /tmp/.npu-status.$$) lines, verdict: $(tail -3 /tmp/.npu-status.$$ | grep -m1 'RESULT' || echo '(no RESULT line)')"
else
	cat /tmp/.npu-status.$$
	rm -f /tmp/.npu-status.$$
	die "the status script produced no report"
fi
rm -f /tmp/.npu-status.$$

# md5sums for every payload file (DEBIAN/ excluded), sorted so the manifest is
# deterministic and `dpkg -V` has something to verify against.  Without this the
# deb ships no md5sums member at all (the 0.1-0.4 builds did not).
rm -f "$SRC/DEBIAN/md5sums"
( cd "$SRC" && find . -path ./DEBIAN -prune -o -type f -print0 |
	sed -z 's|^\./||' | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums )
echo "==> md5sums: $(grep -c '' "$SRC/DEBIAN/md5sums") files"

OUT="$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
dpkg-deb --info "$OUT" | sed -n '1,12p'

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
