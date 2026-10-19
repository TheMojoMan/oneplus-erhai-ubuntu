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
# Builders publish to testing by default; ../update-ppa.sh --cptesting promotes
# tested debs to main.
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

ppa_copy() {
	[ -n "$PPA_DEB_DIR" ] || {
		echo "ppa-common: cannot locate themojoman.github.io (set PPA_DEB_DIR)" >&2
		return 1
	}
	install -d -m755 "$PPA_DEB_DIR"
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
	local f
	for f in "$@"; do
		mv -f "$f" "$PPA_DEB_DIR/"
		chmod 644 "$PPA_DEB_DIR/$(basename "$f")"
		echo "moved $(basename "$f") -> $PPA_DEB_DIR"
	done
}
