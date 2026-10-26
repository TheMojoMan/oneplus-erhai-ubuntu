#!/bin/bash
# Shared helper for all erhai .deb builders: where the apt repo (PPA) lives and
# how to publish a freshly built .deb into it.
#
# Source this from a builder script:
#   . "$(dirname "$0")/ppa-common.sh"
# then call:
#   ppa_copy path/to/pkg_version_arch.deb [more.deb ...]
#
# Fresh builds land in the TESTING suite first:
#   <workspace>/themojoman.github.io/ppa/oneplus-erhai/ubuntu/testing/*.deb
# The testing suite is NOT in the public package sources -- only the owner's
# own device opts into it. After a build proved itself it is promoted to the
# public main suite with:
#   ../update-ppa.sh --cptesting   (moves testing/*.deb -> main/*.deb)
# followed by ../update-ppa.sh (without the flag) to regenerate Packages/
# Release signatures and push to GitHub.
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
PPA_TESTING_DEB_DIR="${PPA_TESTING_DEB_DIR:-$(_ppa_repo_root)/ppa/oneplus-erhai/ubuntu/testing}"

# ----------------------------------------------------------- release series --
# main carries only packages that are INDEPENDENT of the Ubuntu release (erhai's
# own kernel, firmware, scripts, the compiled helpers).  Packages that repackage
# or divert an Ubuntu artifact of the same name (the mutter/GTK4/Mesa repacks,
# steam, the ptyxis/nautilus divert overrides) are bound to one release's ABI
# and live in a PER-RELEASE suite:  ppa/oneplus-erhai/ubuntu/<series>/
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
ERHAI_SERIES="${ERHAI_SERIES:-$(_ppa_host_series)}"
PPA_SERIES_DEB_DIR="${PPA_SERIES_DEB_DIR:-$(_ppa_repo_root)/ppa/oneplus-erhai/ubuntu/$ERHAI_SERIES}"
PPA_SERIES_LIST="${PPA_SERIES_LIST:-$_PPA_COMMON_DIR/ppa-series-packages.txt}"
export ERHAI_SERIES PPA_SERIES_DEB_DIR PPA_SERIES_LIST

# ppa_is_series_pkg <package-name>  -> 0 when the package belongs to the
# per-release suite instead of the universal main suite.
ppa_is_series_pkg() {
	[ -f "$PPA_SERIES_LIST" ] || return 1
	grep -qxF -- "$1" "$PPA_SERIES_LIST"
}

# Builders publish to testing by default; ../update-ppa.sh --cptesting promotes
# tested debs to main (universal packages) or to the release suite (the ones
# listed in ppa-series-packages.txt).
PPA_DEB_DIR="${PPA_DEB_DIR:-$PPA_TESTING_DEB_DIR}"

# --------------------------------------------------------------- build output --
# Every builder packs the .deb it just produced into ONE place inside this repo
# -- <repo>/debs -- instead of dropping it next to the builder or into a project
# directory.  The directory is .gitignore'd, so a build can never dirty the
# repository, and there is exactly one place to look for a fresh artifact.
# (PUBLISHING is a separate step: ppa_copy/ppa_mv below still install a copy
# into the selected suite; DEB_DIR only says where the file is built.)
# Override with DEB_DIR=<dir>.
DEB_DIR="${DEB_DIR:-$_PPA_COMMON_DIR/debs}"
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
