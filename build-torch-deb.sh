#!/bin/bash
# Build oneplus-erhai-torch_<version>_all.deb from the
# oneplus-erhai-torch/ staging tree.
#
#   ./build-torch-deb.sh -> debs/oneplus-erhai-torch_<version>_all.deb
#
# Pure shell + JS package (Architecture: all): the erhai-torch@erhai.local
# GNOME Shell quick slider plus the udev rule that hands the rear-flash LED
# attributes to the erhai-torch group.
#
#   NO_PPA=1 ./build-torch-deb.sh   build locally, do not publish
#
# The release policy is the usual one: publish to the PPA only when the owner
# asks for it, normally once the package is feature-complete and owner-tested.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/ppa-common.sh"
SRC="$HERE/oneplus-erhai-torch"
PKG=$(awk '/^Package:/ {print $2}' "$SRC/DEBIAN/control")
VER=$(awk '/^Version:/ {print $2}' "$SRC/DEBIAN/control")
ARCH=$(awk '/^Architecture:/ {print $2}' "$SRC/DEBIAN/control")

EXT="usr/share/gnome-shell/extensions/erhai-torch@erhai.local"

for f in DEBIAN/postinst \
	 DEBIAN/postrm \
	 etc/udev/rules.d/71-erhai-torch.rules \
	 "$EXT/extension.js" \
	 "$EXT/metadata.json"; do
	[ -f "$SRC/$f" ] || { echo "missing $f in $SRC" >&2; exit 1; }
done

# Refuse to ship anything the package is not supposed to contain. dpkg-deb
# copies the whole staging tree, so a scratch script left at the package root
# (or an editor backup next to a shipped file) would silently land in the deb.
# Everything below the shipped paths is checked against the file list above.
python3 - "$SRC" "$EXT" <<'PY'
import pathlib, sys
src, ext = pathlib.Path(sys.argv[1]), sys.argv[2]
allowed = {
    'DEBIAN/control', 'DEBIAN/postinst', 'DEBIAN/postrm',
    'etc/udev/rules.d/71-erhai-torch.rules',
    f'{ext}/extension.js', f'{ext}/metadata.json',
}
found = {str(p.relative_to(src)) for p in src.rglob('*') if p.is_file()}
extra = sorted(found - allowed)
if extra:
    sys.exit('unexpected files in the package tree (remove or move to tmp/):\n  '
             + '\n  '.join(extra))
missing = sorted(allowed - found)
if missing:
    sys.exit('missing package files:\n  ' + '\n  '.join(missing))
print(f'package tree: {len(found)} files, all expected')
PY

# Lint the extension the way gnome-shell will load it: ES modules, so a syntax
# error means the whole extension fails to import rather than failing at the
# first bad call. Doing it here keeps a broken build from ever reaching a deb.
python3 - "$SRC/$EXT/extension.js" "$SRC/$EXT/metadata.json" <<'PY'
import json, pathlib, subprocess, sys
ext, meta = (pathlib.Path(p) for p in sys.argv[1:3])

data = json.loads(meta.read_text())
for key in ('uuid', 'name', 'description', 'shell-version'):
    if key not in data:
        sys.exit(f'{meta}: missing "{key}"')
if not data['uuid'].endswith('@erhai.local'):
    sys.exit(f'{meta}: unexpected uuid {data["uuid"]}')

# node is not a dependency of this build, so only run the syntax check when it
# happens to be available (it is, on a dev host that has extensions tooling).
if subprocess.run(['sh', '-c', 'command -v node'], capture_output=True).returncode == 0:
    r = subprocess.run(['node', '--input-type=module', '--check'], stdin=ext.open(),
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{ext}: syntax error\n{r.stderr}')
print(f'{ext.name}: ok ({len(ext.read_text())} bytes)')
PY

find "$SRC" -type d -exec chmod 755 {} \;
chmod 755 "$SRC"/DEBIAN/postinst "$SRC"/DEBIAN/postrm
chmod -R u=rwX,go=rX "$SRC"/etc "$SRC"/usr/share
dpkg-deb --build --root-owner-group "$SRC" "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"

# NO_PPA=1 builds locally without publishing (in-development iterations; the
# PPA only gets the package once it is feature-complete and owner-tested).
if [ "${NO_PPA:-0}" != 1 ]; then
	ppa_copy "$DEB_DIR/${PKG}_${VER}_${ARCH}.deb"
fi
echo "built ${PKG}_${VER}_${ARCH}.deb"