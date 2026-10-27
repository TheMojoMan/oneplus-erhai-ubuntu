#!/bin/bash
# make-dconf-seed.sh -- turn a LIVE dconf user database into a shippable seed.
#
#   ./tools/make-dconf-seed.sh <dconf-file> <out-dconf-file> [account-name]
#
# The optional third argument is the ACCOUNT whose name must not survive in the
# seed.  It defaults to the invoking user, which is wrong when the database comes
# from another system or another login (e.g. the sda17 test image, user "test"):
# pass it explicitly then.
#
# WHY: the image ships /etc/skel/.config/dconf/user so that a freshly created
# account starts with the erhai desktop defaults (extension list, panel, power,
# input, OSK, ...).  Copying a live database straight in ships the OWNER's
# personal state with it -- recent files, online accounts, alarms, weather
# locations, chat accounts, home paths in picture URIs, ... and `ppa-guard.sh`
# would (rightly) refuse to publish a package containing that.
#
# What is stripped (section prefixes, then a value scan):
#   file/session history : /org/gnome/desktop/recent-files, /org/gtk/RecentFiles,
#                          /ca/desrt/dconf-editor, /org/gnome/console,
#                          /org/gnome/gnome-system-monitor
#   accounts/identity    : /org/gnome/online-accounts, /org/gnome/goa,
#                          /org/gnome/evolution*, /org/gnome/InitialSetup,
#                          /org/gnome/control-center, /org/gnome/desktop/user-accounts
#   personal content     : /org/gnome/clocks, /org/gnome/weather, /org/gnome/maps,
#                          /org/gnome/shell/world-clocks, /org/gnome/TextEditor,
#                          /org/gnome/Notes, /org/gnome/desktop/notifications/applications
#   per-machine hardware : /org/gnome/desktop/peripherals/touchpad?  -> KEPT (device defaults)
#                          /org/gnome/settings-daemon/plugins/color (external ICC)
#   picture URIs         : /org/gnome/desktop/background/picture-uri*,
#                          /org/gnome/desktop/screensaver/picture-uri*  (contain $HOME paths)
# Value scan: any key whose value mentions /home/, /media/, the user name, the
# host name or a MAC-like address is dropped as well, whatever its section.
#
# The result is re-loaded into a FRESH database (dconf load), so nothing of the
# source file survives that the filter did not keep, and the output is checked
# for the personal patterns before it is written.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
SRC="${1:-}"
OUT="${2:-}"
SEED_USER="${3:-$(id -un)}"
[ -n "$SRC" ] && [ -n "$OUT" ] || { sed -n '3,9p' "$0" >&2; exit 2; }
[ -f "$SRC" ] || { echo "no such dconf database: $SRC" >&2; exit 2; }
command -v dconf >/dev/null || { echo "dconf missing" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dconf-seed.XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/src/.config/dconf" "$WORK/dst/.config/dconf"
cp "$SRC" "$WORK/src/.config/dconf/user"

# 1. dump the source db (as the invoking user, through a private session bus)
HOME="$WORK/src" dbus-run-session -- dconf dump / > "$WORK/dump.txt"
echo "seed: dumped $(grep -c '^\[' "$WORK/dump.txt") section(s) from $SRC"

# 2. filter
python3 - "$WORK/dump.txt" "$WORK/clean.txt" "$SEED_USER" "$(hostname)" <<'PY'
import re, sys
src, out, user, host = sys.argv[1:5]
# NOTE: dconf writes section headers WITHOUT a leading slash ("[org/gnome/...]"),
# so the list is normalised before comparing -- comparing against the leading-""
# form silently dropped nothing until 2026-10-27 (sections survived, their
# keys were only lost when a value tripped the value scan).
DROP_SECTIONS = [
    "/org/gnome/desktop/recent-files", "/org/gtk/RecentFiles", "/ca/desrt/dconf-editor",
    "/org/gnome/console", "/org/gnome/gnome-system-monitor", "/org/gnome/usage/",
    "/org/gnome/online-accounts", "/org/gnome/goa", "/org/gnome/evolution",
    "/org/gnome/InitialSetup", "/org/gnome/control-center", "/org/gnome/desktop/user-accounts",
    "/org/gnome/clocks", "/org/gnome/weather", "/org/gnome/maps", "/org/gnome/shell/world-clocks",
    "/org/gnome/TextEditor", "/org/gnome/Notes", "/org/gnome/desktop/notifications/applications",
    "/org/gnome/settings-daemon/plugins/color",
    "/org/gnome/deja-dup", "/com/ubuntu/update-notifier",
    # background/screensaver are KEPT: their picture-uri is part of the shipped
    # look (a /usr/share/backgrounds file) and the value scan above already drops
    # any URI that points into a home directory.
    "/org/gnome/desktop/search-providers", "/org/gnome/desktop/thumbnailers",
]
DROP_KEYS = re.compile(r"(password|passwd|secret|token|api[-_]?key|username|user-name|"
                       r"email|account|last-login|hostname)", re.I)
# NOTE: the host name is NOT used as a pattern -- this project's machine is
# called "erhai" and every extension uuid contains it.  The user name is only a
# hit as a STANDALONE word: it also appears inside legitimate identifiers such as
# the app id 'com.github.themojoman.ErhaiCam.desktop' (whose loss silently
# emptied the dash favourites on 2026-10-27).
BAD_VALUE = [re.compile(p, re.I) for p in
             (r"/home/", r"/media/", r"/mnt/",
              r"(?<![\w.-])%s(?![\w.-])" % re.escape(user),
              r"\b([0-9a-f]{2}:){5}[0-9a-f]{2}\b")]
DROP_NORM = [d.strip("/") for d in DROP_SECTIONS]
cur = None
kept = dropped_sec = dropped_key = 0
for line in open(src, encoding="utf-8", errors="replace"):
    if line.startswith("["):
        cur = line.strip().strip("[]")
        if any(cur == d or cur.startswith(d + "/") for d in DROP_NORM):
            cur = None
            dropped_sec += 1
            continue
        out_fh = open(out, "a", encoding="utf-8")
        out_fh.write(line)
        out_fh.close()
        kept += 1
        continue
    if cur is None or "=" not in line:
        continue
    key, value = line.split("=", 1)
    if DROP_KEYS.search(key) or any(p.search(value) for p in BAD_VALUE):
        dropped_key += 1
        continue
    with open(out, "a", encoding="utf-8") as fh:
        fh.write(line)
print("seed: dropped %d section(s) and %d key(s), kept %d section(s)"
      % (dropped_sec, dropped_key, kept), file=sys.stderr)
PY
[ -f "$WORK/clean.txt" ] || : > "$WORK/clean.txt"

# 3. load into a FRESH database
HOME="$WORK/dst" dbus-run-session -- dconf load / < "$WORK/clean.txt"
[ -f "$WORK/dst/.config/dconf/user" ] || { echo "dconf load produced no database" >&2; exit 1; }

# 4. prove there is no personal data left.  The TEXT dump is checked with the
# full pattern set (host name as a whole word: "erhai" is a substring of every
# erhai extension uuid, so a plain substring test would always fire); the binary
# database only for the path/user patterns, which can never be legitimate.
if grep -qE "/home/|/media/|/mnt/|(^|[^A-Za-z0-9._-])$SEED_USER([^A-Za-z0-9._-]|$)|([0-9a-f]{2}:){5}[0-9a-f]{2}" "$WORK/clean.txt" ||
   grep -aqE "/home/|/media/" "$WORK/dst/.config/dconf/user"; then
	echo "REFUSING: personal data survived the filter" >&2
	grep -nE "/home/|/media/|/mnt/|$SEED_USER" "$WORK/clean.txt" | head -5 >&2
	exit 1
fi

install -d -m755 "$(dirname "$OUT")"
install -m644 "$WORK/dst/.config/dconf/user" "$OUT"
echo "seed: wrote $OUT ($(stat -c %s "$OUT") bytes)"
