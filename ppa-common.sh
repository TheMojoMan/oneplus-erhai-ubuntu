#!/bin/bash
# Shared helper for all erhai .deb builders: where the apt repo (PPA) lives and
# how to publish a freshly built .deb into it.
#
# Layout (2026-10-27): the repo is split into one folder per PPA suite --
#   main/       release-independent packages + their builders + debs/
#   resolute/   Ubuntu 26.04 (resolute) ABI-bound packages + builders + debs/
#   stonking/   Ubuntu 26.10 (stonking) ABI-bound packages + builders + debs/
# The folder a builder lives in IS its suite: it decides DEB_DIR and the publish
# target, so nothing has to be passed around.
#
# Source this from a builder script (one level below the repo root, or from a
# package directory inside a suite folder):
#   . "$HERE/../ppa-common.sh"        # builder in <suite>/
#   . "$HERE/../../ppa-common.sh"     # builder in <suite>/<package>/
# then call:
#   ppa_copy path/to/pkg_version_arch.deb [more.deb ...]
#
# A build leaves its .deb in <repo>/<suite>/debs (gitignored).  Publishing is a
# separate, deliberate step:
#   ../update-ppa.sh --mv     moves every <suite>/debs/*.deb into the PPA suite
#                             of the same name (main|resolute|stonking)
#   ../update-ppa.sh          regenerates Packages/Release signatures + pushes
# There is NO 'testing' suite any more (removed 2026-10-27): a package that is
# not ready stays in <suite>/debs, which the PPA never sees.
#
# Override the publish target with PPA_DEB_DIR (e.g. to publish straight to
# the public suite: PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-wifi-deb.sh).

_PPA_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_ppa_repo_root() {
	# workspace root = parent of this repo
	local root
	root="$(dirname "$_PPA_COMMON_DIR")"
	if [ -d "$root/themojoman.github.io" ]; then
		echo "$root/themojoman.github.io"
	fi
}
PPA_MAIN_DEB_DIR="${PPA_MAIN_DEB_DIR:-$(_ppa_repo_root)/ppa/oneplus-erhai/ubuntu/main}"

# ----------------------------------------------------------- release series --
# main carries only packages that are INDEPENDENT of the Ubuntu release (erhai's
# own kernel, firmware, scripts, the compiled helpers).  Packages that repackage
# or divert an Ubuntu artifact of the same name (the mutter/GTK4/Mesa repacks,
# the ptyxis/nautilus divert overrides) are bound to one release's ABI and live
# in a PER-RELEASE suite:  ppa/oneplus-erhai/ubuntu/<series>/
# (<series> = the Ubuntu codename: resolute = 26.04, stonking = 26.10).
# The package list is one file, shared with ../update-ppa.sh:
#     ppa-series-packages.txt
# Builds default to the HOST release; override for a 26.10 chroot build:
#     ERHAI_SERIES=stonking ./build-mutter-deb.sh
_ppa_host_series() {
	local c
	c="$(sed -n 's/^VERSION_CODENAME=//p' /etc/os-release 2>/dev/null | tr -d '\"')"
	echo "${c:-resolute}"
}
# The suite is the folder the CALLING builder lives in (main/resolute/stonking);
# walk up until the parent is this repo root.  ERHAI_SERIES still wins when set
# explicitly (that is how a builder is pointed at another suite on purpose).
# Which file asked for us?  Inside a FUNCTION the frame list starts with the
# file the function is defined in (this one), so scan the whole BASH_SOURCE
# stack and take the first entry that is not ppa-common.sh itself.
_ppa_caller_dir() {
	local i f d
	for i in "${!BASH_SOURCE[@]}"; do
		f="${BASH_SOURCE[$i]}"
		[ -n "$f" ] || continue
		d="$(cd "$(dirname "$f")" 2>/dev/null && pwd)" || continue
		[ "$d" = "$_PPA_COMMON_DIR" ] && continue
		printf '%s\n' "$d"
		return 0
	done
	return 1
}
# The suite is the top-level folder of the repo the caller sits in
# (main|resolute|stonking), no matter how deep the script is nested.
_ppa_series_of_caller() {
	local d
	d="$(_ppa_caller_dir)" || return 1
	while [ "$d" != "/" ] && [ "$d" != "$_PPA_COMMON_DIR" ]; do
		if [ "$(dirname "$d")" = "$_PPA_COMMON_DIR" ]; then
			printf '%s\n' "$(basename "$d")"
			return 0
		fi
		d="$(dirname "$d")"
	done
	return 1
}
_ppa_series_dir() {	# absolute path of the calling builder's suite folder
	local s
	s="$(_ppa_series_of_caller)" || return 1
	case "$s" in
		main|resolute|stonking) printf '%s\n' "$_PPA_COMMON_DIR/$s" ;;
		*) return 1 ;;
	esac
}
ERHAI_SERIES="${ERHAI_SERIES:-$(_ppa_series_of_caller || _ppa_host_series)}"
PPA_SERIES_DEB_DIR="${PPA_SERIES_DEB_DIR:-$(_ppa_repo_root)/ppa/oneplus-erhai/ubuntu/$ERHAI_SERIES}"
PPA_SERIES_LIST="${PPA_SERIES_LIST:-$_PPA_COMMON_DIR/ppa-series-packages.txt}"
export ERHAI_SERIES PPA_SERIES_DEB_DIR PPA_SERIES_LIST

# ppa_is_series_pkg <package-name>  -> 0 when the package belongs to the
# per-release suite instead of the universal main suite.
ppa_is_series_pkg() {
	[ -f "$PPA_SERIES_LIST" ] || return 1
	grep -qxF -- "$1" "$PPA_SERIES_LIST"
}

# ppa_copy/ppa_mv write into the builder's OWN suite (the folder it lives in):
# main/ builders -> .../ubuntu/main, resolute/ -> .../resolute, stonking/ ->
# .../stonking.  There is no private staging suite any more; a .deb that is not
# ready simply stays in <suite>/debs until `update-ppa.sh --mv` is run.
PPA_DEB_DIR="${PPA_DEB_DIR:-$PPA_SERIES_DEB_DIR}"

# --------------------------------------------------------------- build output --
# Every builder packs the .deb it just produced into ONE place inside this repo
# -- <repo>/debs -- instead of dropping it next to the builder or into a project
# directory.  The directory is .gitignore'd, so a build can never dirty the
# repository, and there is exactly one place to look for a fresh artifact.
# (PUBLISHING is a separate step: ppa_copy/ppa_mv below still install a copy
# into the selected suite; DEB_DIR only says where the file is built.)
# Override with DEB_DIR=<dir>.
# One debs/ folder PER SUITE: <repo>/<series>/debs (main|resolute|stonking).
DEB_DIR="${DEB_DIR:-$(_ppa_series_dir || echo "$_PPA_COMMON_DIR")/debs}"
mkdir -p "$DEB_DIR"

# ----------------------------------------------------------- ppa_scrub_debug --
# A build made with -g records the ABSOLUTE paths of this pad (workspace tree,
# /media/data/develop/.../tmp/<dep>/build, include dirs) in the DWARF, and those
# shipped inside published .debs (personal-data audit 2026-10-26: camera,
# erhaicam, sensors, video, fastrpc, llamacpp, the desktop helpers).  Every
# builder that compiles, or installs a compiled artifact, calls this before
# packing: debug info is stripped from every ELF in the payload and a tree that
# still carries the build path is a HARD ERROR (no .deb is produced).
#   ppa_scrub_debug "$SRC"
ppa_scrub_debug() {
	local tree f ws bad=0
	ws="$(dirname "$_PPA_COMMON_DIR")"
	for tree in "$@"; do
		[ -d "$tree" ] || continue
		while IFS= read -r f; do
			[ -f "$f" ] || continue
			[ "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
			strip --strip-debug "$f" 2>/dev/null || strip "$f" 2>/dev/null || true
		done < <(find "$tree" -type f)
		if grep -rqlaF "$ws" "$tree" 2>/dev/null; then
			echo "ppa-common: refusing to pack $tree - the build path $ws is embedded in:" >&2
			grep -rlaF "$ws" "$tree" 2>/dev/null | head -5 >&2
			bad=1
		fi
	done
	return $bad
}

# ---------------------------------------------------------------- ppa-guard ---
# NOTHING is published before ppa-guard.sh has seen it: personal data (a
# password, a keyring/browser store, a private key, a token, this machine's MAC
# or Wi-Fi SSID, the developer's home/workspace path) must never reach a .deb in
# the apt repo.  The guard streams the payload, so a big kernel package costs
# seconds, and it FAILS CLOSED: a missing guard also stops the publish.
# A deliberate bypass exists for a package you have inspected by hand:
#     ERHAI_GUARD_ALLOW=1 ./build-…-deb.sh
_ppa_guard() {
	if [ "${ERHAI_GUARD_ALLOW:-0}" = 1 ]; then
		echo "ppa-common: WARNING: ppa-guard BYPASSED (ERHAI_GUARD_ALLOW=1) for $*" >&2
		return 0
	fi
	local g="$_PPA_COMMON_DIR/ppa-guard.sh"
	if [ ! -x "$g" ]; then
		echo "ppa-common: REFUSING to publish - $g is missing (fail closed)" >&2
		return 1
	fi
	"$g" "$@" || {
		echo "ppa-common: REFUSING to publish $* - see the ppa-guard report above" >&2
		return 1
	}
}

ppa_copy() {
	[ -n "$PPA_DEB_DIR" ] || {
		echo "ppa-common: cannot locate themojoman.github.io (set PPA_DEB_DIR)" >&2
		return 1
	}
	install -d -m755 "$PPA_DEB_DIR"
	_ppa_guard "$@" || return 1
	local f
	for f in "$@"; do
		install -m644 "$f" "$PPA_DEB_DIR/"
		echo "published $(basename "$f") -> $PPA_DEB_DIR"
	done
}

ppa_mv() {
	# Same target as ppa_copy, but MOVES the .deb into the suite instead of
	# copying: the build tree keeps no leftover copy, so there is exactly one
	# canonical location per built package.
	[ -n "$PPA_DEB_DIR" ] || {
		echo "ppa-common: cannot locate themojoman.github.io (set PPA_DEB_DIR)" >&2
		return 1
	}
	install -d -m755 "$PPA_DEB_DIR"
	_ppa_guard "$@" || return 1
	local f
	for f in "$@"; do
		mv -f "$f" "$PPA_DEB_DIR/"
		chmod 644 "$PPA_DEB_DIR/$(basename "$f")"
		echo "moved $(basename "$f") -> $PPA_DEB_DIR"
	done
}
