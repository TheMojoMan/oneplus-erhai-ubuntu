#!/bin/sh
# Unlock the GNOME login keyring at autologin (erhai).  SHIPPED in
# oneplus-erhai-desktop since 2.24; the pam line that calls it is owned by that
# package's postinst/postrm (this file used to live only on the dev pad and named
# the dev pad's account -- see the rule in .clinerules).
#
# WHY THIS EXISTS: gdm-autologin has no password to hand to pam_gnome_keyring, so
# the login keyring stays locked and the first client that needs a secret
# (goa-daemon for a GNOME Online Account, Brave/Chromium "Safe Storage", ...)
# pops a gcr prompt at every login.  This pam_exec helper unlocks it from the
# password in /etc/erhai/keyring.pass (root:<desktop-user> 0640) before the
# session starts.
#
# NEVER A HARDCODED USER NAME: the account comes from PAM_USER and must be a HUMAN
# account (uid 1000..65533, so root/greeters/system accounts never match).  When
# the pass file's group is a human group, that group names the ONE account the file
# was set up for and PAM_USER must be in it -- so one user's password can never be
# fed to another user's keyring.  A file with no human group falls back to "any
# human account" (it was chgrp'ed away from the documented contract).
#
# Referenced from /etc/pam.d/gdm-autologin BEFORE pam_gnome_keyring's auto_start
# line: gnome-keyring-daemon --unlock can only START a daemon *with* the password,
# it cannot unlock a daemon that is already running (measured 2026-09-20).
#
# setpriv, not pam_exec's "seteuid": the latter sets only the effective uid, and
# gnome-keyring-daemon compares the REAL uid with the owner of the control
# directory ("The gnome-keyring control directory is not owned with the same
# credentials as the user login: /run/user/1000/keyring" was logged on the
# 2026-09-20 17:32 boot and the unlock silently did not happen).
#
# Always exits 0: this must never break a login.
set -u

PASS_FILE=${ERHAI_KEYRING_PASS:-/etc/erhai/keyring.pass}

[ "${PAM_TYPE:-}" = "open_session" ] || exit 0
[ -n "${PAM_USER:-}" ] || exit 0
[ -n "${XDG_RUNTIME_DIR:-}" ] || exit 0
[ -r "$PASS_FILE" ] || exit 0
command -v gnome-keyring-daemon >/dev/null 2>&1 || exit 0

USER_UID=$(id -u "$PAM_USER" 2>/dev/null) || exit 0
USER_GID=$(id -g "$PAM_USER" 2>/dev/null) || exit 0
[ -n "${USER_UID:-}" ] && [ -n "${USER_GID:-}" ] || exit 0
case "$USER_UID" in ''|*[!0-9]*) exit 0 ;; esac
# human accounts only
[ "$USER_UID" -ge 1000 ] 2>/dev/null || exit 0
[ "$USER_UID" -lt 65534 ] 2>/dev/null || exit 0

# the pass file belongs to ONE account's group: honour it when it is a human group
FILE_GID=$(stat -c %g "$PASS_FILE" 2>/dev/null \
	|| stat -f %g "$PASS_FILE" 2>/dev/null || echo)
case "$FILE_GID" in
'') ;;
*[!0-9]*) ;;
*)
	[ "$FILE_GID" -ge 1000 ] 2>/dev/null && [ "$FILE_GID" != "$USER_GID" ] && exit 0
	;;
esac

if [ "$(id -u)" = "$USER_UID" ]; then
	cat "$PASS_FILE" |
		gnome-keyring-daemon --unlock --components=pkcs11,secrets \
			>/dev/null 2>&1
elif command -v setpriv >/dev/null 2>&1; then
	setpriv --reuid="$USER_UID" --regid="$USER_GID" --init-groups \
		sh -c "cat '$PASS_FILE' |
			gnome-keyring-daemon --unlock --components=pkcs11,secrets \
				>/dev/null 2>&1"
else
	echo "erhai-keyring-unlock: no way to become $PAM_USER" >&2
fi

exit 0
