#!/bin/bash
# =============================================================================
# ppa-guard.sh — refuse to ship PERSONAL DATA inside a .deb
#
#   ppa-guard.sh [--quiet] [--no-warn] <file.deb> ...
#   ppa-guard.sh --print-lists        # show the categories, never the values
#
# Every package that is about to be published (builders call ppa_copy/ppa_mv,
# update-ppa.sh --mvtomain moves testing/ -> main/) is scanned payload-first:
# the .deb is streamed through `dpkg-deb --fsys-tarfile | tar --to-command`,
# so a 800 MB kernel package is never unpacked to disk, and the CONTROL area +
# the member NAMES are checked as well.
#
# HARD findings (exit 1, publish must stop):
#   * a known SECRET — the content of /etc/erhai/*.pass, any credential found in
#     the workspace .clinerules, or a line of an explicit denylist file
#   * private keys, keyring/browser credential stores, ssh/gpg material
#   * API tokens (github, gitlab, aws, slack, openai…)
#   * this machine's MAC addresses, Wi-Fi SSIDs and LAN IPv4 addresses
#   * an absolute path into the real user's home or into this workspace
# WARN findings (reported, publish continues):
#   * the user name, the host name, e-mail addresses — these are already public
#     in the PPA metadata / package names, but a payload must not need them.
#
# NO VALUE IS EVER PRINTED: secrets are matched with `grep -q` against a private
# pattern file and reported by CATEGORY only; MAC/SSID/IP hits are masked.
#
# Tunables (environment):
#   ERHAI_GUARD_DENYLIST   file with one literal string per line to forbid
#                          (default: ~/.config/erhai/ppa-denylist)
#   ERHAI_GUARD_WORKSPACE  workspace root whose path must never appear in a deb
#                          (default: parent of this script's repo)
# =============================================================================
set -u

QUIET=0
WARN_ON=1
ARGS=()
for a in "$@"; do
	case "$a" in
	--quiet) QUIET=1 ;;
	--no-warn) WARN_ON=0 ;;
	-p|--print-lists) PRINT_LISTS=1 ;;
	-h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) echo "ppa-guard: unknown option $a" >&2; exit 2 ;;
	*) ARGS+=("$a") ;;
	esac
done

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }
say_err() { printf '%s\n' "$*" >&2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="${ERHAI_GUARD_WORKSPACE:-$(dirname "$HERE")}"
REALUSER="$(id -un)"
HOST="$(uname -n)"

G_TMP="${ERHAI_GUARD_TMPDIR:-}"
if [ -z "$G_TMP" ]; then
	if [ -d "${WORKSPACE:-/nonexistent}/tmp" ] && [ -w "${WORKSPACE}/tmp" ]; then
		G_TMP="$WORKSPACE/tmp"
	else
		G_TMP="${TMPDIR:-/tmp}"
	fi
fi
mkdir -p "$G_TMP" 2>/dev/null || true
WORK="$(mktemp -d "$G_TMP/ppa-guard.XXXXXX")" || exit 2
trap 'rm -rf "$WORK"' EXIT

P_SECRETS="$WORK/secrets.txt"      # literal, one per line, never printed
P_HARD="$WORK/hard.txt"            # extended regexps
P_WARN="$WORK/warn.txt"
P_NAMES="$WORK/names.txt"          # forbidden member NAMES (shell globs)
export P_SECRETS P_HARD P_WARN P_NAMES
: >"$P_SECRETS"; : >"$P_HARD"; : >"$P_WARN"; : >"$P_NAMES"

add_literal() {   # $1 = value, $2 = min length
	case "$1" in ''|*[[:space:]]*) return 0 ;; esac
	[ "${#1}" -ge "${2:-6}" ] || return 0
	printf '%s\n' "$1" >>"$P_SECRETS"
}

# ---------------------------------------------------------------- secrets -----
# 1. the keyring / any other *.pass file the desktop ships a contract for
for f in /etc/erhai/*.pass; do
	[ -r "$f" ] || continue
	while IFS= read -r line || [ -n "$line" ]; do
		add_literal "$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')" 6
	done <"$f"
done

# 2. the workspace rules file: its credential lines are the login password(s)
for rules in "$WORKSPACE/.clinerules" "$HERE/.clinerules"; do
	[ -r "$rules" ] || continue
	# "echo <pw> | sudo -S", "user <name> / <pw>", "password **<pw>**",
	# "printf '<pw>\n' | su"
	grep -oE "(echo[[:space:]]+[^ |]+[[:space:]]*\|[[:space:]]*sudo|user[[:space:]]+[A-Za-z0-9_.-]+[[:space:]]*/[[:space:]]*[^[:space:]]+|password[[:space:]]+\*\*[^*]+\*\*|printf[[:space:]]+'[^']{4,}'[[:space:]]*\|)" \
		"$rules" 2>/dev/null |
		sed -e 's/^echo[[:space:]]*//' -e 's/[[:space:]]*|.*$//' \
		    -e 's/^user[[:space:]]*[^[:space:]]*[[:space:]]*\/[[:space:]]*//' \
		    -e 's/^password[[:space:]]*\*\*//' -e 's/\*\*$//' \
		    -e "s/^printf[[:space:]]*'//" -e "s/'[[:space:]]*|$//" |
		while IFS= read -r tok; do
			tok="${tok%\\n}"
			case "$tok" in *[![:alnum:]_.@!#%+^-]*) continue ;; esac
			case "$tok" in themojoman|password|sudo) continue ;; esac
			add_literal "$tok" 6
		done
done

# 3. an explicit denylist file (one literal per line)
for deny in "${ERHAI_GUARD_DENYLIST:-$HOME/.config/erhai/ppa-denylist}" \
            "$WORKSPACE/.ppa-denylist"; do
	[ -r "$deny" ] || continue
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in ''|'#'*) continue ;; esac
		add_literal "$line" 4
	done <"$deny"
done
sort -u -o "$P_SECRETS" "$P_SECRETS"

# ------------------------------------------------------------------ hard ------
cat >>"$P_HARD" <<'REGEX'
-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----
-----BEGIN PGP PRIVATE KEY BLOCK-----
(gh[pousr]|github_pat)_[A-Za-z0-9_]{20,}
glpat-[A-Za-z0-9_-]{20,}
xox[abprs]-[A-Za-z0-9-]{10,}
AKIA[0-9A-Z]{16}
sk-[A-Za-z0-9]{32,}
ssh-(rsa|ed25519|dss) +AAAA[A-Za-z0-9+/]{20,}
REGEX
[ -n "$REALUSER" ] && printf '%s\n' "/home/$REALUSER(/|\"|'|[[:space:]]|$)" >>"$P_HARD"
[ -n "$WORKSPACE" ] && printf '%s\n' "$(printf '%s' "$WORKSPACE" | sed 's/[.[\*^$/]/\\&/g')" >>"$P_HARD"
# the developer tree the workspace lives in (this pad keeps it under /media/data)
printf '%s\n' "/media/data/develop/" >>"$P_HARD"
# MAC addresses of THIS machine's interfaces (any spelling)
for macf in /sys/class/net/*/address; do
	[ -r "$macf" ] || continue
	mac="$(cat "$macf" 2>/dev/null)"
	case "$mac" in ''|00:00:00:00:00:00) continue ;; esac
	# colon- and dash-separated only: the BARE 12-hex form matches random binary
	# data in almost every shared object (measured), which is pure noise.
	printf '%s\n' "$(printf '%s' "$mac" | tr 'A-F' 'a-f')" >>"$P_HARD"
	printf '%s\n' "$(printf '%s' "$mac" | tr 'A-F' 'a-f' | tr ':' '-')" >>"$P_HARD"
done
# Wi-Fi SSIDs this pad knows (NetworkManager connection names, without the secret
# files themselves, so no root is needed)
if command -v nmcli >/dev/null 2>&1; then
	while IFS= read -r ssid; do
		case "$ssid" in ''|'--'|lo|'Wired connection'*) continue ;; esac
		[ "${#ssid}" -ge 4 ] || continue
		printf '%s\n' "$ssid" >>"$P_HARD"
	done < <(nmcli -t -f NAME,TYPE connection show 2>/dev/null | awk -F: '$2 ~ /wireless/ { print $1 }')
fi
sort -u -o "$P_HARD" "$P_HARD"

# ------------------------------------------------------------------ warn ------
{
	# NOTE: the HOST name is deliberately not checked -- on this pad it is the
	# project name ("erhai") and appears in every path of every package.
	[ -n "$REALUSER" ] && printf '%s\n' "\\b$REALUSER\\b"
	# only the OWNER's address: a generic e-mail regex matches third-party
	# extension UUIDs ("gjsosk@vishram1123.com") and every control file.
	printf '%s\n' 'themojoman@gmx\.net'
	if command -v ip >/dev/null 2>&1; then
		ip -o -4 addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1 |
			while IFS= read -r a; do
				case "$a" in ''|127.*) continue ;; esac
				printf '%s\n' "$(printf '%s' "$a" | sed 's/\./\\./g')"
			done
	fi
} | sort -u >"$P_WARN"

# member NAMES that must never be shipped
cat >>"$P_NAMES" <<'GLOBS'
*.keyring
*.keystore
*Login Data*
*cookies.sqlite*
*logins.json*
*key4.db*
*id_rsa*
*id_ed25519*
*/etc/shadow
*.clinerules
GLOBS

if [ -n "${PRINT_LISTS:-}" ]; then
	say "categories (values are never printed):"
	say "  secrets : $(wc -l <"$P_SECRETS") literal string(s) from /etc/erhai/*.pass,"
	say "            $WORKSPACE/.clinerules and the explicit denylist"
	say "  hard    : private keys, keyring/browser stores, API tokens,"
	say "            /home/$REALUSER, $WORKSPACE, /media/data/develop/,"
	say "            this machine's MAC addresses and Wi-Fi SSIDs"
	say "  warn    : user name, e-mail addresses, local IPv4 addresses"
	[ "$QUIET" = 1 ] || { echo "  hard patterns:"; sed 's/^/    /' "$P_HARD";
	                      echo "  name globs:"; sed 's/^/    /' "$P_NAMES"; }
	exit 0
fi

[ ${#ARGS[@]} -gt 0 ] || { say_err "ppa-guard: nothing to scan (pass .deb files)"; exit 2; }

# per-member scanner, run by GNU tar for every archive member
MEMBER_SCANNER="$WORK/scan-member"
P_MEMBER="$WORK/member.bin"
export P_MEMBER
cat >"$MEMBER_SCANNER" <<'EOS'
#!/bin/sh
# env: TAR_FILENAME, P_SECRETS, P_HARD, P_WARN, P_MEMBER
# tars --to-command hands the member on stdin and a grep -q would swallow it, so
# the member is buffered ONCE and then matched three times.
rc=0
cat >"$P_MEMBER" 2>/dev/null || exit 0
if [ -s "$P_SECRETS" ] && grep -aqF -f "$P_SECRETS" "$P_MEMBER" 2>/dev/null; then
	printf 'SECRET\t%s\n' "$TAR_FILENAME"; rc=1
fi
if grep -aqE -f "$P_HARD" "$P_MEMBER" 2>/dev/null; then
	printf 'HARD\t%s\n' "$TAR_FILENAME"; rc=1
fi
if grep -aqE -f "$P_WARN" "$P_MEMBER" 2>/dev/null; then
	printf 'WARN\t%s\n' "$TAR_FILENAME"
fi
exit $rc
EOS
chmod +x "$MEMBER_SCANNER"

scan_deb() {
	local deb=$1 fails=0 hits sec hits_hard hits_warn name member
	[ -f "$deb" ] || { say_err "ppa-guard: no such file: $deb"; return 2; }
	say "ppa-guard: $(basename "$deb")"

	# 1. member NAMES (fast, from the listing)
	while IFS= read -r member; do
		[ -n "$member" ] || continue
		while IFS= read -r glob; do
			[ -n "$glob" ] || continue
			case "$member" in
			$glob) say_err "  HARD  member name $member matches '$glob'"; fails=1 ;;
			esac
		done <"$P_NAMES"
	done < <(dpkg-deb -c "$deb" 2>/dev/null |
		sed -E 's/^.*[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2} //')

	# 2. CONTROL area
	# the control area legitimately carries the maintainer/e-mail: WARN is noise
	# there, HARD/SECRET are not
	hits=$(dpkg-deb --ctrl-tarfile "$deb" 2>/dev/null |
		TAR_FILENAME=CONTROL "$MEMBER_SCANNER" 2>/dev/null | grep -v '^WARN')
	if [ -n "$hits" ]; then
		printf '%s\n' "$hits" | sed 's/^/  control: /' >&2
		case "$hits" in *SECRET*|*HARD*) fails=1 ;; esac
	fi

	# 3. payload, streamed (never unpacked)
	hits=$(dpkg-deb --fsys-tarfile "$deb" 2>/dev/null |
		tar -x --ignore-command-error --to-command="$MEMBER_SCANNER" 2>/dev/null)
	if [ -n "$hits" ]; then
		local nwarn=0
		printf '%s\n' "$hits" | while IFS="$(printf '\t')" read -r kind member; do
			case "$kind" in
			SECRET) say_err "  HARD  $member contains a known secret (value not shown)" ;;
			HARD)   say_err "  HARD  $member contains personal/machine data" ;;
			WARN)   say_err "  warn  $member mentions the user/e-mail/local IP" ;;
			esac
		done
		nwarn=$(printf '%s\n' "$hits" | grep -c '^WARN')
		[ "$nwarn" -gt 8 ] && say_err "        ($nwarn warnings in total)"
		case "$hits" in *SECRET*|*HARD*) fails=1 ;; esac
	fi

	if [ "$fails" = 0 ]; then
		say "  clean"
		return 0
	fi
	return 1
}

fail=0
for target in "${ARGS[@]}"; do
	if [ -d "$target" ]; then
		for d in "$target"/*.deb; do
			[ -f "$d" ] || continue
			scan_deb "$d" || fail=1
		done
	else
		scan_deb "$target" || fail=1
	fi
done

if [ "$fail" != 0 ]; then
	say_err "ppa-guard: REFUSING to publish — personal data found above."
	say_err "            Fix the package (or add an approved exception to"
	say_err "            ${ERHAI_GUARD_DENYLIST:-$HOME/.config/erhai/ppa-denylist})."
	exit 1
fi
say "ppa-guard: all packages clean."
exit 0
