#!/bin/bash
# Build oneplus-erhai-llamacpp_<version>_arm64.deb from the
# oneplus-erhai-llamacpp/ staging tree.
#
#   ./build-llamacpp-deb.sh                 # stage + build + publish to TESTING
#   NO_PPA=1 ./build-llamacpp-deb.sh        # build locally, do not publish
#   STRIP=0 ./build-llamacpp-deb.sh         # keep the symbols
#   LLAMA_BIN=<dir> VENDOR_DIR=<dir> LLAMA_SRC=<tree> ./build-llamacpp-deb.sh
#
# The payload is NOT compiled here.  It stages the working erhai build of
# llama.cpp (LLAMA_BIN, default ../tmp/llama-cpp-ref/build-opencl/bin), rewrites
# every ELF's RUNPATH to $ORIGIN (the build tree bakes in its own absolute
# path), strips it, and then verifies -- on a COPY outside the build tree, so a
# stale RUNPATH cannot mask a failure -- that ldd resolves every library of
# llama-cli, llama-server, the OpenCL backend and the Hexagon backend.
#
# The vendor Hexagon backend (VENDOR_DIR, default ../tmp/npu-llamacpp:
# libggml-hexagon.so + libggml-htp-v79.so, Qualcomm's public GenieX /
# qai_appbuilder binaries) is staged NEXT TO the ggml libraries because
# libggml-hexagon.so carries RUNPATH $ORIGIN and links libggml-base.so.
#
# The patch set is REGENERATED on every build from LLAMA_SRC (a git diff of the
# patched tree, preceded by the base commit), so the deb's patches/ always
# matches the binaries it ships.  A saved patch is included too when present
# (LLAMACPP_OFFLOAD_PATCH, default ../tmp/npu-backup/llamacpp-offload-spec.patch).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-llamacpp"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

BIN_SRC="${LLAMA_BIN:-$HERE/../tmp/llama-cpp-ref/build-opencl/bin}"
VENDOR="${VENDOR_DIR:-$HERE/../tmp/npu-llamacpp}"
LSRC="${LLAMA_SRC:-$HERE/../tmp/llama-cpp-ref}"
OFFLOAD_PATCH="${LLAMACPP_OFFLOAD_PATCH:-$HERE/../tmp/npu-backup/llamacpp-offload-spec.patch}"

PAY="$SRC/opt/llama.cpp"
DOC="$SRC/usr/share/doc/oneplus-erhai-llamacpp"
BIND="$PAY/bin"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$BIN_SRC/llama-cli" ]    || die "no llama-cli in $BIN_SRC (set LLAMA_BIN)"
[ -x "$BIN_SRC/llama-server" ] || die "no llama-server in $BIN_SRC (set LLAMA_BIN)"
[ -f "$VENDOR/libggml-hexagon.so" ] || die "no libggml-hexagon.so in $VENDOR (set VENDOR_DIR)"
[ -f "$VENDOR/libggml-htp-v79.so" ] || die "no libggml-htp-v79.so in $VENDOR"
[ -f "$SRC/usr/local/bin/llama-erhai" ] || die "missing usr/local/bin/llama-erhai"
[ -s "$SRC/usr/share/doc/oneplus-erhai-llamacpp/README.md" ] || die "missing the package README"
command -v patchelf >/dev/null || die "patchelf is required"

# Absolute symlinks would break the moment the package is installed elsewhere;
# the upstream build tree only produces relative ones.
abs="$(find "$BIN_SRC" -type l -lname '/*' -print -quit)"
[ -z "$abs" ] || die "absolute symlink in the payload: $abs"

echo "==> staging from $BIN_SRC"
for d in "$BIND" "$DOC/patches"; do
	[ -d "$d" ] && find "$d" -mindepth 1 \( -type f -o -type l \) -exec rm -f {} +
	[ -d "$d" ] && find "$d" -mindepth 1 -depth -type d -empty -delete
done
mkdir -p "$BIND" "$DOC/patches"
cp -a "$BIN_SRC/." "$BIND/"

# ------------------------------------------------------------------- RUNPATH ---
# The build tree's RUNPATH points into the scratch build directory; $ORIGIN makes
# the payload relocatable and keeps every library next to the binaries.
# ONLY host ELFs are touched: the vendor Hexagon skel libggml-htp-v79.so is a
# 32-bit QUALCOMM DSP6 ELF that patchelf/strip would corrupt (measured: patchelf
# even "succeeds" on it), so the vendor files are staged AFTER this step.
na64=0
while read -r f; do
	file -b "$f" | grep -q 'ARM aarch64' || continue
	patchelf --set-rpath '$ORIGIN' "$f"
	na64=$((na64 + 1))
done < <(find "$BIND" -type f)
[ "$na64" -gt 0 ] || die "no aarch64 ELF files found in $BIND"
echo "==> rewrote RUNPATH to \$ORIGIN on $na64 host ELFs"

if [ "${STRIP:-1}" = 1 ]; then
	while read -r f; do
		file -b "$f" | grep -q 'ARM aarch64' || continue
		strip --strip-unneeded "$f"
	done < <(find "$BIND" -type f)
	echo "==> stripped payload ($(du -sh "$BIND" | cut -f1))"
fi

# ------------------------------------------------------------------- vendor ---
# Qualcomm's prebuilt Hexagon backend + HTP skel, byte-identical to the source
# (they are DSP/host vendor blobs -- never patched, never stripped).
install -m644 "$VENDOR/libggml-hexagon.so" "$VENDOR/libggml-htp-v79.so" "$BIND/"
cmp -s "$VENDOR/libggml-htp-v79.so" "$BIND/libggml-htp-v79.so" \
	|| die "the staged HTP skel differs from the source"

# The server twin is a symlink to the one script (it dispatches on $0 basename),
# so the two can never drift apart.
ln -sfn llama-erhai "$SRC/usr/local/bin/llama-erhai-server"

# ----------------------------------------------------------------- ldd check ---
T="$(mktemp -d "${TMPDIR:-/tmp}/llamacpp-ldd.XXXXXX")"
cp -a "$BIND/." "$T/"
bad=0
for b in llama-cli llama-server libggml-opencl.so.0.24.0 libggml-hexagon.so; do
	[ -e "$T/$b" ] || { echo "  missing in payload: $b"; bad=1; continue; }
	if ! out="$(ldd "$T/$b" 2>&1)"; then
		echo "  ldd failed for $b"; echo "$out"; bad=1; continue
	fi
	if printf '%s\n' "$out" | grep -q 'not found'; then
		echo "  unresolved library for $b:"; printf '%s\n' "$out" | grep 'not found'; bad=1
	fi
done
find "$T" -mindepth 1 \( -type f -o -type l \) -delete
rmdir "$T" 2>/dev/null || true
[ "$bad" = 0 ] || die "the staged payload does not resolve its own libraries"
echo "==> ldd check: clean (all libraries resolve from \$ORIGIN)"


# ---------------------------------------------------------------- patch set ---
if git -C "$LSRC" rev-parse --git-dir >/dev/null 2>&1; then
	{
		echo "# erhai llama.cpp patch set"
		echo "# base commit: $(git -C "$LSRC" rev-parse HEAD) -- $(git -C "$LSRC" log -1 --format=%s)"
		echo "# regenerated by oneplus-erhai-ubuntu/build-llamacpp-deb.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "# apply with: git apply -p1 <this file>   (inside an upstream llama.cpp tree)"
		echo
		git -C "$LSRC" diff
	} > "$DOC/patches/llama-cpp-erhai.patch"
else
	die "$LSRC is not a git tree -- cannot regenerate the patch set (set LLAMA_SRC)"
fi
[ -s "$DOC/patches/llama-cpp-erhai.patch" ] || die "generated patch set is empty"
npatch="$(grep -c '^+++ ' "$DOC/patches/llama-cpp-erhai.patch" || true)"
echo "==> patch set: $npatch files -> $DOC/patches/llama-cpp-erhai.patch"
if [ -f "$OFFLOAD_PATCH" ]; then
	install -m644 "$OFFLOAD_PATCH" "$DOC/patches/llamacpp-offload-spec.patch"
fi

# ------------------------------------------------------------ file-list guard --
# dpkg-deb packs the WHOLE tree, so a scratch file dropped at the package root
# would silently ship.  Refuse anything outside the intended payload.
python3 - "$SRC" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])
allowed = ('DEBIAN/', 'opt/llama.cpp/', 'usr/local/bin/',
           'usr/share/doc/oneplus-erhai-llamacpp/')
found = sorted(str(p.relative_to(src)) for p in src.rglob('*') if p.is_file())
extra = [f for f in found if not f.startswith(allowed)]
if extra:
    sys.exit('unexpected files in the package tree (move them to tmp/):\n  '
             + '\n  '.join(extra))
print(f'package tree: {len(found)} files, all expected')
PY

# ------------------------------------------------------------------ metadata ---
find "$SRC" -type d -exec chmod 755 {} +
find "$SRC" -type f -exec chmod 644 {} +
# The llama.cpp payload is executables AND dlopen'ed shared libraries, all 755 in
# the build tree -- a blanket 644 would ship a package whose binaries cannot run
# (caught by the smoke test below on 2026-09-22).
find "$BIND" -type f -exec chmod 755 {} +
chmod 755 "$SRC/usr/local/bin/llama-erhai"
chmod 644 "$SRC/DEBIAN/control"

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
dpkg-deb --build --root-owner-group "$SRC" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
dpkg-deb --info "$OUT" | sed -n '1,14p'

# ----------------------------------------------------------------- smoke test ---
# Validate what is actually SHIPPED, not the staging tree: extract the .deb
# elsewhere and run the wrapper from it.  `--version` neither loads a backend nor
# touches a device, so this is safe from a live session.  (This test is what
# caught the 2026-09-22 packaging bug where a blanket chmod 644 left the payload
# binaries non-executable.)
if [ "${SKIP_RUN_CHECK:-0}" != 1 ]; then
	T="$(mktemp -d "${TMPDIR:-/tmp}/llamacpp-smoke.XXXXXX")"
	dpkg-deb -x "$OUT" "$T"
	# Capture BOTH streams: llama.cpp logs its banner with llama_log (stderr), and
	# Mesa's rusticl prints a libclc warning before it, so the version is not line 1.
	out="$(LLAMA_ERHAI_PREFIX="$T/opt/llama.cpp" "$T/usr/local/bin/llama-erhai" \
		--version 2>&1 || true)"
	find "$T" -mindepth 1 \( -type f -o -type l \) -delete
	find "$T" -mindepth 1 -depth -type d -empty -delete
	rmdir "$T" 2>/dev/null || true
	banner="$(printf '%s\n' "$out" | grep -m1 -E '^version: ' || true)"
	[ -n "$banner" ] || die "smoke test FAILED -- the shipped wrapper printed:
$out"
	echo "==> smoke test OK: $banner"
fi

if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$OUT"
else
	echo "NO_PPA=1: not published"
fi
