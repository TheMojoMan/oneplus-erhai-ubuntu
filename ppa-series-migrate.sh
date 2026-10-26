#!/bin/bash
# Move the RELEASE-BOUND packages of the erhai PPA out of the universal main
# suite and into their per-release suite:
#
#     ppa/oneplus-erhai/ubuntu/main/<deb>  ->  ppa/oneplus-erhai/ubuntu/<series>/<deb>
#
# "Release-bound" = the package names in ppa-series-packages.txt (mutter, GTK4,
# Mesa, steam, ptyxis, nautilus -- everything that repackages or diverts an
# Ubuntu artifact of the same name; its ABI belongs to ONE Ubuntu release).
# main keeps only the release-independent packages (kernel, firmware, scripts,
# the compiled erhai helpers).
#
#   ./ppa-series-migrate.sh --dry-run            # default: host codename
#   ./ppa-series-migrate.sh --dry-run stonking
#   ./ppa-series-migrate.sh resolute             # the real move (git mv)
#   ../update-ppa.sh                             # regenerate + sign + push
#
# BEFORE the move goes public, the machines that already have these packages
# installed must list the series suite, or they lose their update source:
#   cp root-setup-files/apt/oneplus-erhai.sources /etc/apt/sources.list.d/
#   sed -i "s/@ERHAI_SERIES@/<series>/g" /etc/apt/sources.list.d/oneplus-erhai.sources
# (images built by build-root.sh get exactly that file for their own release).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(dirname "$HERE")"
PPA="$W/themojoman.github.io/ppa/oneplus-erhai/ubuntu"
LIST="$HERE/ppa-series-packages.txt"

DRY=0
args=()
for a in "$@"; do
	case "$a" in
		--dry-run) DRY=1 ;;
		*) args+=("$a") ;;
	esac
done
SERIES="${args[0]:-$(sed -n 's/^VERSION_CODENAME=//p' /etc/os-release 2>/dev/null | tr -d '"')}"
SERIES="${SERIES:-resolute}"

[ -d "$PPA/main" ] || { echo "no $PPA/main" >&2; exit 1; }
[ -f "$LIST" ] || { echo "missing $LIST" >&2; exit 1; }
[ "$SERIES" != main ] && [ "$SERIES" != testing ] || { echo "series must be a codename, not '$SERIES'" >&2; exit 2; }

mkdir -p "$PPA/$SERIES" 2>/dev/null || true
n=0
shopt -s nullglob
for f in "$PPA"/main/*.deb; do
	bn="$(basename "$f")"; pkg="${bn%%_*}"
	grep -qxF -- "$pkg" "$LIST" || continue
	n=$((n + 1))
	if [ "$DRY" = 1 ]; then
		printf 'would move  %-58s -> %s/\n' "$bn" "$SERIES"
	else
		[ -d "$PPA/$SERIES" ] || install -d -m755 "$PPA/$SERIES"
		if git -C "$W/themojoman.github.io" ls-files --error-unmatch "ppa/oneplus-erhai/ubuntu/main/$bn" >/dev/null 2>&1; then
			git -C "$W/themojoman.github.io" mv "ppa/oneplus-erhai/ubuntu/main/$bn" "ppa/oneplus-erhai/ubuntu/$SERIES/$bn"
		else
			mv -f "$f" "$PPA/$SERIES/$bn"
		fi
		printf 'moved       %-58s -> %s/\n' "$bn" "$SERIES"
	fi
done
shopt -u nullglob

echo "---"
echo "$n release-bound package(s) $([ "$DRY" = 1 ] && echo 'would be' || echo 'were') moved to '$SERIES'."
if [ "$DRY" = 0 ] && [ "$n" -gt 0 ]; then
	echo "next: ../update-ppa.sh   (regenerates Packages/Release for main AND $SERIES, signs, pushes)"
fi
