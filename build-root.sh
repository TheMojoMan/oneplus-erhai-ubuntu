#!/bin/bash
# Build an Ubuntu rootfs image for the OnePlus Pad 3 (erhai / SM8750).
#
# Everything that gets copied into the image lives in root-setup-files/:
#
#   root-setup-files/overlay/   mirrors the rootfs, copied in with `cp -a`
#   root-setup-files/apt/       repo definitions: Mozilla Firefox, Brave
#                               Browser, and the erhai apt repo (themojoman.gpg
#                               key, oneplus-erhai.sources)
#
# The erhai packages (kernel, firmware, Mesa, audio/wifi/camera/..., plus the
# sensors, pen, OTG, NPU and llama.cpp stacks) are NOT staged locally any more
# -- they are installed from the online apt repo
# (https://themojoman.github.io/ppa/oneplus-erhai/ubuntu/main/, publish new
# versions with build-kernel.sh / build-all-debs.sh + ../update-ppa.sh
# --mvtomain; the package-set check in the 'packages' stage below is the
# authoritative list).
#
# A few UBUNTU-NAMED components are locally patched and ship from the same repo
# (the mutter set and the GTK4 set).  They keep Ubuntu's own package names so
# every existing dependency still resolves -- see the 'erhai packages LAST'
# block for the install-order consequences.
#
# WiFi (WCN7880) is assembled from a few places:
#   - the kernel deb ships the cnss platform modules + the PCIe/pwrseq/MHI core
#   - the firmware deb ships the WCN7880 "peach" blobs (lib/firmware)
#   - the oneplus-erhai-wifi deb carries the bring-up scripts (erhai-wcn-wifi.sh,
#     save-wcn-devcd.sh, wl_drvcmd), the erhai-wcn-*.service units, the modprobe.d
#     policy (blacklist-ath12k, cnss2-noauto, ...) and the qcacld INI (with
#     scan_mode_6ghz=0); its postinst enables the services
#   - wlan.ko (qcacld-3.0 mission driver) is built out-of-tree and staged into
#     the image from ./oneplus-erhai-wcn/ (see the "wifi (wlan)" stage below).
#
# Usage:  sudo ./build-root.sh                 image with NO user (default):
#                                             gdm stays at the greeter and runs
#                                             gnome-initial-setup on first boot
#         sudo PRESET_USER=1 ./build-root.sh   image with a ready-made user
# Result: rootfs.img, ready to dd to the "linux" partition (PARTLABEL=linux).
#
# PRESET_USER=0 (default)
#   Creates no account. GDM starts gnome-initial-setup, which asks for name,
#   password, language and timezone, then creates the user from /etc/skel — so
#   monitors.xml, the GL templates, .bash_aliases and the dconf seed still land
#   in the new home. The erhai services do not care either way: they resolve the
#   desktop user at runtime instead of having it baked in at build time.
#
# PRESET_USER=1
#   Creates $USERNAME/$USERPASS and turns on GDM autologin. Boots straight to a
#   desktop, which is what you want while testing hardware.

set -euo pipefail

VERSION="26.04"
# Build artifacts live OUTSIDE this directory tree (it may be published
# online); override via environment if your layout differs:
IMG="${ERHAI_ROOTFS_IMG:-../rootfs.img}"
IMG_SIZE="8G"
ROOTDIR="${ERHAI_ROOTDIR:-../rootdir}"
HOSTNAME="erhai"
USERNAME="erhai"
USERUID="1000"
USERPASS="erhai"
PRESET_USER="${PRESET_USER:-0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BASETGZ="${ERHAI_BASETGZ:-../ubuntu-base.tar.gz}"
SETUP="${ROOT_SETUP:-$HERE/root-setup-files}"
MESA_TREE="${MESA_TREE:-$HERE/../mesa}"
EXTRA_PACKAGES="${EXTRA_PACKAGES:-}"

# Set SKIP_BOOTSTRAP=1 to re-run the chroot stages against an image that has
# already been created and extracted (fast iteration on the package/overlay
# stages).
SKIP_BOOTSTRAP="${SKIP_BOOTSTRAP:-0}"

BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/$VERSION/release/ubuntu-base-$VERSION-base-arm64.tar.gz"
# 26.04 is served out of beta/ until release day; fall back to it automatically.
BASE_URL_BETA="https://cdimage.ubuntu.com/ubuntu-base/releases/$VERSION/beta/ubuntu-base-$VERSION-beta-base-arm64.tar.gz"

[ "$(id -u)" = 0 ] || { echo "run me as root (sudo $0)" >&2; exit 1; }
[ -d "$SETUP/overlay" ] || { echo "missing $SETUP/overlay" >&2; exit 1; }

cleanup() {
	set +e
	umount "$ROOTDIR/sys" "$ROOTDIR/proc" "$ROOTDIR/dev/pts" "$ROOTDIR/dev"
	rm -f "$ROOTDIR/qemu-aarch64-static"
	umount "$ROOTDIR"
}
trap cleanup EXIT

# ------------------------------------------------- heredoc hygiene guard -----
# Every chroot stage is written with an UNQUOTED heredoc delimiter
# (cat > "$ROOTDIR/tmp/erhai-stage-<name>.sh" <<EOF), so the HOST shell expands
# the body while WRITING it.  A backtick or $( ) inside such a body is therefore
# executed ON THE HOST -- outside the chroot -- and its output is spliced into
# the stage script.  A comment that merely MENTIONED 'update-mesa.sh' in
# backticks once ran the mesa builder AND ../update-ppa.sh (which commits and
# pushes the public PPA) during an image build.  Refuse to start if any
# unquoted stage body contains a command substitution: write such text in
# ' single quotes ', and escape a deliberate one as \$( or \`.
# (The pattern is anchored on /^cat >/ so this program cannot match its own
#  source lines, and a preceding backslash is ignored so the intentional
#  \$(dpkg-query ...) escapes inside the stages stay legal.)
awk '
	/^cat > "[^"]*erhai-stage-[^"]*\.sh" <<EOF$/ { body = 1; next }
	body && /^EOF$/                             { body = 0; next }
	body && /(^|[^\\])`/ {
		printf "FATAL: %s:%d backtick inside an unquoted chroot-stage heredoc: %s\n", FILENAME, FNR, $0 > "/dev/stderr"; bad = 1 }
	body && /(^|[^\\])\$\(/ {
		printf "FATAL: %s:%d command substitution inside an unquoted chroot-stage heredoc: %s\n", FILENAME, FNR, $0 > "/dev/stderr"; bad = 1 }
	END { exit bad ? 1 : 0 }
' "$0" || {
	echo "FATAL: fix the stage-heredoc line(s) above before building" \
	     "(they would be executed on the HOST while the stage script is written)" >&2
	exit 1
}

# ---------------------------------------------------------------- image ------
if [ "$SKIP_BOOTSTRAP" != 1 ]; then
	rm -f "$IMG"
	truncate -s "$IMG_SIZE" "$IMG"
	# The filesystem LABEL is cosmetic here and only the BOOT LOADER reads it:
	# rEFInd names an entry's icon after the VOLUME the loader sits on, and the
	# image is now booted from the UKI in its own /boot (built below by the
	# kernel deb's postinst), so `ubuntu` gives those entries the proper
	# os_ubuntu.png where `linux` gave them the generic penguin (2026-10-17).
	# NOTHING mounts by label: root= and /etc/fstab use PARTLABEL=linux, which
	# stays `linux` (mkfs.ext4 -L only sets the fs label, not the GPT name).
	mkfs.ext4 -q -L ubuntu "$IMG"
fi
mkdir -p "$ROOTDIR"
mount -o loop "$IMG" "$ROOTDIR"

if [ "$SKIP_BOOTSTRAP" != 1 ]; then
	if [ ! -f "$BASETGZ" ]; then
		wget -q "$BASE_URL" -O "$BASETGZ" ||
			wget -q "$BASE_URL_BETA" -O "$BASETGZ"
	fi
	tar xzf "$BASETGZ" -C "$ROOTDIR"
fi

# ---------------------------------------------------------- qemu / binfmt ----
#if [ ! -f qemu-aarch64-static ]; then
#	wget -q https://github.com/multiarch/qemu-user-static/releases/download/v7.2.0-1/qemu-aarch64-static
#fi
#install -m755 qemu-aarch64-static "$ROOTDIR/"
#if [ ! -e /proc/sys/fs/binfmt_misc/aarch64 ]; then
#	echo ':aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7:\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff:/qemu-aarch64-static:' > /proc/sys/fs/binfmt_misc/register
#	echo ':aarch64ld:M::\x7fELF\x02\x01\x01\x03\x00\x00\x00\x00\x00\x00\x00\x00\x03\x00\xb7:\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff:/qemu-aarch64-static:' > /proc/sys/fs/binfmt_misc/register
#fi

mount --bind /dev "$ROOTDIR/dev"
mount --bind /dev/pts "$ROOTDIR/dev/pts"
mount --bind /proc "$ROOTDIR/proc"
mount --bind /sys "$ROOTDIR/sys"

# ------------------------------------------------------- network / naming ----
echo "nameserver 1.1.1.1" > "$ROOTDIR/etc/resolv.conf"
echo "$HOSTNAME" > "$ROOTDIR/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 %s\n' "$HOSTNAME" > "$ROOTDIR/etc/hosts"

# Never let a maintainer script prompt. An interactive conffile question
# (gdm3 asking about our pre-seeded /etc/gdm3/custom.conf) reads its answer
# from stdin -- and when a chroot stage is fed as a heredoc, stdin IS the
# pipe carrying the stage script: dpkg eats/desyncs the remaining lines and
# later steps silently never run (2026-08-25: firefox/ubuntu-desktop missing
# from a "successful" build). confold keeps overlay-shipped conffiles,
# confdef takes the maintainer version where we ship none.
cat > "$ROOTDIR/etc/apt/apt.conf.d/90erhai-noninteractive" <<'APTCFG'
Dpkg::Options {
	"--force-confdef";
	"--force-confold";
};
APTCFG

# ------------------------------------------------------------- overlay -------
# The overlay mirrors the rootfs, so a plain copy places every file. The GDM
# autologin line is the one place left that has to name a user at build time;
# everything else resolves it at runtime.
cp -a "$SETUP/overlay/." "$ROOTDIR/"
# cp -a preserves the workspace ownership of the overlay (uid/gid >= 1000).
# If those leak into the image, sandboxed system services (ProtectSystem=
# strict + trimmed CapabilityBoundingSet => no CAP_DAC_OVERRIDE) get EACCES
# writing under /etc even as root: hostnamed/accounts-daemon fail with
# "FileIsProtected", gnome-initial-setup cannot create the user or set the
# hostname and its Next button stays dead. Normalize everything outside
# /home (preset-user homes are legitimately user-owned).
find "$ROOTDIR" -xdev ! -path "$ROOTDIR/home/*" \
\( -uid +999 -o -gid +999 \) -exec chown root:root {} +
if [ "$PRESET_USER" = 1 ]; then
	grep -rlZ -e '@USERNAME@' -e '@USERUID@' "$ROOTDIR/etc" "$ROOTDIR/usr/local/sbin" |
		xargs -0 -r sed -i -e "s|@USERNAME@|$USERNAME|g" -e "s|@USERUID@|$USERUID|g"
else
	# No account exists, so autologin has nothing to log in as: leave GDM at the
	# greeter and let it run gnome-initial-setup.
	sed -i -e 's|^AutomaticLoginEnable=.*|AutomaticLoginEnable=False|' \
	       -e '/^AutomaticLogin=/d' "$ROOTDIR/etc/gdm3/custom.conf"
	# Nothing else may name a user at build time in this mode.
	if grep -rl -e '@USERNAME@' -e '@USERUID@' "$ROOTDIR/etc" "$ROOTDIR/usr/local/sbin"; then
		echo "the files above still hardcode the desktop user; they must resolve it" \
		     "at runtime for PRESET_USER=0" >&2
		exit 1
	fi
fi
chmod 755 "$ROOTDIR"/usr/local/sbin/erhai-*

# CLOSED 2026-10-23: the autologin login-keyring unlock NOW SHIPS in
# oneplus-erhai-desktop (2.24) -- /usr/local/sbin/erhai-keyring-unlock.sh is a
# package payload file, it resolves the account at RUN TIME (human accounts only:
# the uid from PAM_USER, plus the ONE account the pass file's group names -- no
# name is baked in any more), and that deb's postinst/postrm own the
#     session optional pam_exec.so seteuid /usr/local/sbin/erhai-keyring-unlock.sh
# line in /etc/pam.d/gdm-autologin, inserted BEFORE pam_gnome_keyring's
# auto_start (it must run first: gnome-keyring-daemon can only be STARTED with the
# password, it cannot unlock a daemon that is already running).  Without it the
# first secret reader at login (goa-daemon, Brave/Chromium Safe Storage) pops a
# gcr prompt.  Tests: tmp/keyring-test/keyring-unlock-test.sh (38 checks, the pam
# logic driven through the ERHAI_PAM_DIR seam).
# STILL MANUAL (per pad, on purpose): /etc/erhai/keyring.pass holds the password
# that unlocks the login KEYRING -- it is the user's, not the package's, and after
# a ROOT-side password change the keyring keeps the OLD password (see
# sensor-MEMORY-2026-10-22.md §10 and the tools in tmp/keyring-test/).
# PRESET_USER=0 images never autologin, so nothing is missing for them.

# ------------------------------------------------- system shell extensions ---
# GNOME Shell extensions that ship through the overlay (gnome-shell loads
# /usr/share/gnome-shell/extensions/<uuid>/ for every user, exactly like
# Ubuntu's own ding@rastersoft.com / ubuntu-dock@ubuntu.com; the user's
# `enabled-extensions` is what decides whether one is actually loaded).
#
# MODES MATTER: these trees are copies of a live USER-level extension
# (gnome-extensions writes metadata.json as 0600) and the overlay `cp -a`
# preserves modes.  gnome-shell runs as the DESKTOP USER, so a root-owned 0600
# metadata.json under /usr/share makes it refuse the extension silently -- it
# cannot even read the uuid and shell-version.  Normalize the tree here.
# (The only such extension today is gjs-osk, delivered below.)
if [ -d "$ROOTDIR/usr/share/gnome-shell/extensions" ]; then
	find "$ROOTDIR/usr/share/gnome-shell/extensions" -type d -exec chmod 755 {} +
	find "$ROOTDIR/usr/share/gnome-shell/extensions" -type f -exec chmod 644 {} +
fi

# ------------------------------------------------------------------- osk ------
# Default on-screen keyboard = gjs-osk, shipped WITHOUT a package:
#   * the extension        overlay/usr/share/gnome-shell/extensions/
#                          gjsosk@vishram1123.com/
#   * settings + enabling  overlay/etc/skel/.config/dconf/user -- the per-user
#                          dconf SEED: `enabled-extensions` lists
#                          gjsosk@vishram1123.com, and the
#                          org/gnome/shell/extensions/gjsosk keys carry this
#                          pad's two hand-made layouts with
#                          layout-portrait = layout-landscape = 11
#                          ("Custom Layout 2") at 35 % height.
#   * GNOME's OWN OSK stays OFF for user sessions (the same seed sets
#     org.gnome.desktop.a11y.applications screen-keyboard-enabled=false) --
#     otherwise two keyboards answer the same text field.
#   * The LOGIN SCREEN is the deliberate exception: gjs-osk's metadata.json has
#     no `session-modes`, so it never loads in the greeter (session mode "gdm");
#     the greeter keeps GNOME's built-in OSK, enabled by its own seed
#     (overlay/var/lib/gdm3/seat0/config/dconf/user, source of record
#     dconf-seed/00-erhai-greeter).
#
# SINCE 2.2 THE DEB SHIPS THE SAME TREE: oneplus-erhai-desktop (>= 2.2) carries
# these files under /usr/share/gnome-shell/extensions/ as well, so the fix
# reaches a device over apt and the payload is dpkg-tracked (md5sums); its
# build-desktop-deb.sh COPIES them FROM THIS OVERLAY on every build, so the two
# can never diverge.  The overlay copy stays: a flashed image must not depend on
# the package being present in main.  PRECEDENCE: gnome-shell collects the USER
# data dir first, so a user-level copy of the uuid (an existing
# ~/.local/share/gnome-shell/extensions/gjsosk@vishram1123.com/) wins and the
# system copy is skipped with "Extension ... already installed in ...".
#
# The shipped extension is NOT upstream: it carries local patches (SETTINGS and
# CLOSE as real keys, plus the `topbar: false` grid offset fix), so it must be
# copied from the live ~/.local/share/gnome-shell/extensions/gjsosk@vishram1123.com/
# and never from the extensions.gnome.org tarball.  To change the shipped
# layout: configure gjs-osk in a session, then refresh the seed with
#     python3 ../tmp/seed-gjsosk-refresh.py --apply
# which patches ONLY the two gjsosk groups of the seed, proves that nothing
# else drifted and that the values equal the live ones, and installs the GVDB
# as root:root 0644.  (Wholesale-copying ~/.config/dconf/user into the seed
# also works, but drags unrelated personal state into the image defaults.)
#
# FOLIO GATE (2026-09-29): pogoGate.js + the [erhai] blocks in extension.js
# keep the OSK from coming up by itself while the pogo folio keyboard is
# attached (no touch/tap auto-show, no bottom-edge swipe, no restore at
# login), retract an automatically opened keyboard on attach, and make the
# on-screen CLOSE key clear the sticky `keyboard-visible` flag -- which is
# why the SEED below must ship `keyboard-visible=false`: a `true` inherited
# from a live session made every login pop an OSK nobody had asked for.
GJSOSK="$ROOTDIR/usr/share/gnome-shell/extensions/gjsosk@vishram1123.com"
for f in metadata.json extension.js pogoGate.js prefs.js physicalLayouts.json \
	 keycodes.tar.xz schemas/gschemas.compiled \
	 ui/icons/hicolor/scalable/actions/backspace.svg; do
	[ -f "$GJSOSK/$f" ] || {
		echo "FATAL: gjs-osk payload missing from the rootfs: $GJSOSK/$f" >&2
		echo "       (copy it from the live ~/.local/share/gnome-shell/extensions/)" >&2
		exit 1; }
done
[ -f "$ROOTDIR/etc/skel/.config/dconf/user" ] || {
	echo "FATAL: per-user dconf seed missing -- gjs-osk would not be enabled" >&2
	exit 1; }
echo "[build-root] on-screen keyboard: gjs-osk payload + dconf seed present"

# GDM greeter/wizard theme: erhai wallpaper on the lock/setup background.
# Registered as a higher-priority gdm-theme.gresource alternative so the
# packaged Yaru theme stays untouched and dpkg updates stay clean.
chroot "$ROOTDIR" update-alternatives --install \
	/usr/share/gnome-shell/gdm-theme.gresource gdm-theme.gresource \
	/usr/share/gnome-shell/theme/erhai/gnome-shell-theme.gresource 100 >/dev/null

# GDM greeter dconf: this script creates NO profile of its own.  The
# oneplus-erhai-desktop deb ships /etc/dconf/profile/gdm (user-db:user +
# system-db:gdm + file-db:/var/lib/gdm3/greeter-dconf-defaults) together with
# /usr/share/gdm/dconf/95-erhai-greeter, which gdm's own ExecStartPre compiles
# into that file-db (greeter session name + GNOME's ambient ALS OFF).  What must
# never come back is a greeter that lacks the session PIN: gnome-session's
# selection then falls back to the ubuntu session and the login screen comes up
# as a FULL UBUNTU DESKTOP running as the dynamic gdm-greeter user.  The pin
# lives in the .desktop (usr/local/share/gdm/greeter/wayland-sessions/
# gnome-greeter.desktop, Exec=gnome-session --session=gnome-login) and, belt-
# and-braces, in 95-erhai-greeter.
# Greeter scale/OSK come from static files in the overlay:
#   var/lib/gdm3/seat0/config/{monitors.xml,dconf/user}
# (that dir IS the greeter's XDG_CONFIG_HOME; keep it writable, see below).


# The greeter's XDG_CONFIG_HOME is /var/lib/gdm3/seat0/config; mutter/dconf
# update files there as the DYNAMIC greeter user (uid differs every boot),
# so the overlay-shipped copies must be world-writable.
if [ -d "$ROOTDIR/var/lib/gdm3/seat0/config" ]; then
	chmod -R a+rwX "$ROOTDIR/var/lib/gdm3/seat0/config"
fi

# Persistent systemd journal: without /var/log/journal journald is volatile,
# and post-mortem debugging of greeter/initial-setup issues from a test boot
# (mounted offline) is impossible.
mkdir -p "$ROOTDIR/var/log/journal"

# /etc/skel gets the desktop defaults every new user should start with:
# monitors.xml (3392x2400 @ scale 2), the environment.d GL templates, the shell
# aliases, and a seeded dconf database. The dconf database is a binary GVDB;
# the pre-built copy in the overlay (etc/skel/.config/dconf/user) is the source
# of truth and is copied verbatim by the `cp -a` above. The old recompile step
# from skel-dconf.ini was REMOVED (2026-09-05): whenever the host had dconf it
# silently OVERWROTE the overlay binary with a database compiled from the stale
# skel-dconf.ini (last touched Jul 22), so refreshed seeds never landed in the
# image. To change the seed: boot the running system, tweak settings, and copy
# ~/.config/dconf/user into root-setup-files/overlay/etc/skel/.config/dconf/.
# (skel-dconf.ini is kept only as a historical reference; nothing reads it.)

# Firefox: Mozilla's own .deb repo (Ubuntu's "firefox" package is a snap stub).
# The .sources FILE of every third-party repo is installed AFTER the 'locale'
# stage (see "third-party apt sources" below): they are all HTTPS and apt can
# only verify an HTTPS repo once ca-certificates exists.  The keyrings and
# preferences here are local files, so they stay together with the archive
# definitions above.
install -d -m755 "$ROOTDIR/etc/apt/keyrings"
install -m644 "$SETUP/apt/packages.mozilla.org.asc" "$ROOTDIR/etc/apt/keyrings/"
install -m644 "$SETUP/apt/mozilla-firefox.pref" "$ROOTDIR/etc/apt/preferences.d/"
install -m644 "$SETUP/apt/mozilla-unattended-upgrades.conf" \
	"$ROOTDIR/etc/apt/apt.conf.d/51erhai-mozilla"

# Brave Browser: Brave's own .deb repo (keyring + deb822 sources verbatim from
# https://brave.com — the keyring lives in /usr/share/keyrings, the path the
# upstream .sources file signs against; arm64 is published alongside amd64).
install -d -m755 "$ROOTDIR/usr/share/keyrings"
install -m644 "$SETUP/apt/brave-browser-archive-keyring.gpg" \
	"$ROOTDIR/usr/share/keyrings/"

# Chromium: xtradeb/apps PPA (real .deb, arm64; Ubuntu's own "chromium" is a
# snap stub, which erhai must never ship). The deb822 file carries the PPA
# signing key inline; "Suites: resolute" must track the base release.
# Its .sources file is installed after the 'locale' stage (see below).

# The erhai packages (kernel, firmware, Mesa, audio/UCM, wifi, ...) are NOT
# baked into the image any more -- they install from the online apt repo
# (https://themojoman.github.io/ppa/oneplus-erhai/ubuntu/main/) inside the
# chroot below. Only the repo key + source definition ship in the image.
[ -f "$SETUP/apt/themojoman.gpg" ] ||
	{ echo "missing $SETUP/apt/themojoman.gpg -- copy it from /etc/apt/keyrings/" >&2; exit 1; }
[ -f "$SETUP/apt/oneplus-erhai.sources" ] ||
	{ echo "missing $SETUP/apt/oneplus-erhai.sources -- copy it from /etc/apt/sources.list.d/" >&2; exit 1; }
install -m644 "$SETUP/apt/themojoman.gpg" "$ROOTDIR/etc/apt/keyrings/"

# --------------------------------------------------------------- locale ------
# Stages are written to a file INSIDE the chroot and executed with stdin
# closed: no postinst can ever consume the script itself (see note above).
mkdir -p "$ROOTDIR/tmp"
cat > "$ROOTDIR/tmp/erhai-stage-locale.sh" <<EOF
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
apt update
apt install -y locales ca-certificates gnupg
echo "en_US.UTF-8 UTF-8" > /etc/locale.gen
locale-gen en_US.UTF-8
update-locale LANG=en_US.UTF-8
touch /run/erhai-stage-locale-done
EOF
chroot "$ROOTDIR" /bin/bash /tmp/erhai-stage-locale.sh </dev/null
rm -f "$ROOTDIR/tmp/erhai-stage-locale.sh"

# A chroot heredoc that dies mid-stream (heredoc corruption, apt hook, ...)
# can return 0 while having run only part of its steps -- that once shipped
# an image without ubuntu-desktop/firefox/terminal. Verify every stage
# actually ran to its last line.
test -f "$ROOTDIR/run/erhai-stage-locale-done" || {
	echo "FATAL: chroot stage 'locale' did not complete" >&2; exit 1; }

# ------------------------------------------------- third-party apt sources ---
# Installed HERE -- after the 'locale' stage brought ca-certificates in -- and
# NOT with the keyrings/preferences above.  Reason: ubuntu-base ships no
# /etc/ssl at all (its tarball contains no etc/ssl entry), so until
# ca-certificates is installed apt cannot verify a SINGLE HTTPS repo, and every
# repo below is HTTPS.  With them installed earlier, the first update printed an
# Ign:/Err: pair per repo ("SSL connection failed: ... certificate verify
# failed") followed by "Some index files failed to download. They have been
# ignored, or old ones used instead.": apt exits 0 on partial index failures and
# the 'locale' stage needs none of these repos, so the build continued and still
# produced a correct image (the erhai packages install much later, by which time
# the CA store exists) -- but that noise buried real errors (2026-10-17).
# Ordering rule: nothing that needs an HTTPS repo may run before this block.
install -m644 "$SETUP/apt/mozilla.sources" "$ROOTDIR/etc/apt/sources.list.d/"
install -m644 "$SETUP/apt/brave-browser.sources" "$ROOTDIR/etc/apt/sources.list.d/"
install -m644 "$SETUP/apt/xtradeb-apps.sources" "$ROOTDIR/etc/apt/sources.list.d/"
install -m644 "$SETUP/apt/oneplus-erhai.sources" "$ROOTDIR/etc/apt/sources.list.d/"

# -------------------------------------------------------------- packages -----
cat > "$ROOTDIR/tmp/erhai-stage-packages.sh" <<EOF
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive LANG=en_US.UTF-8

apt update

# Base system + the tools the erhai services actually call.
# console-setup: applies /etc/default/console-setup (Terminus 16x32) at boot;
# without it the boot console falls back to an unreadable 8x16 font.
# xz-utils: Ubuntu's tar does NOT link liblzma (verified: 'ldd /bin/tar' shows
# no lzma, and with an empty PATH 'tar -J' dies with "tar (child): xz: Cannot
# exec"), so -J shells out to the xz BINARY.  The shipped gjs-osk extension
# unpacks its keycodes.tar.xz with exactly that command -- without xz-utils the
# default on-screen keyboard would come up with no key tables.
apt install -y bash-completion sudo ssh nano micro curl wget rsync console-setup \\
	python3 systemd-boot-efi systemd-ukify zram-config xz-utils

# The erhai package layer is deliberately installed LAST -- after
# ubuntu-desktop and every other apt run of this stage.  See the
# "erhai packages LAST" block below the desktop/upgrade steps; the full
# rationale (main-suite-only listing, the single kernel line, the six Mesa
# /usr replacement debs, the retry policy) lives there.

# NOTE: the Qualcomm remote-processor userspace that only serves modem/ath12k
# style remoteproc paths is NOT installed on purpose (rmtfs,
# protection-domain-mapper, qrtr-tools): WiFi here is WCN7880 via cnss2/qcacld
# and audio runs over AudioReach/GLINK.  The QRTR pieces the ADSP sensor/NPU
# stack DOES need are pulled in by oneplus-erhai-sensors' own hard
# dependencies (hexagonrpcd, tqftpserv, libqrtr-glib0, libqmi-glib5,
# libmbim-glib4) -- do not add them by hand here.

# Audio: alsa-ucm-conf supplies the ucm2 conf.d lookup + shared includes, and
# alsa-utils the amixer/alsaucm/alsactl tools. The erhai UCM profile itself (and
# the AW88261 amp un-mute) ships in the oneplus-erhai-audio.deb; PipeWire/
# WirePlumber's ACP layer applies the HiFi verb at session start.
apt install -y alsa-ucm-conf alsa-utils

# Bluetooth (WCN7880 over serdev) and rfkill for debugging. The per-device
# address machinery (erhai-bt-addr/-bt-up) is retired since oneplus-erhai-bluetooth
# 2.2: the DT's shared local-bd-address + bluetooth.service's AutoEnable is all
# BT needs.
# The overlay ships /etc/bluetooth/main.conf with FilterDiscoverable = false
# (dpkg --force-confold keeps it): the OnePlus Pencil advertises Flags 0x04 =
# BR/EDR-Not-Supported with NO discoverable bit, which bluez's own filter drops
# before the device object is even created -- so the pen could never be paired.
# oneplus-erhai-bluetooth 2.3 does the same to a live system from its postinst,
# but it is not published yet, so the overlay is what a fresh image gets.
apt install -y bluez rfkill

# Camera (libcamera "cam" + media-ctl) and the iris3 HW video codec path.
# i2c-tools provides i2ctransfer, which the erhai-cam-focus/-exposure CLI
# helpers use to poke the SC1320CS VCM + sensor.  The
# erhai-cam-focus@erhai.local GNOME extension that used to drive them is
# RETIRED: ErhaiCam itself owns focus (cn3927 lens subdev,
# V4L2_CID_FOCUS_ABSOLUTE) and exposure (sensor reg 0x3e00 over I2C_RDWR).
# gstreamer1.0-gtk4 is ErhaiCam's GTK4 sink and a hard dependency of both
# camera packages -- listed explicitly so the image's gst stack is complete
# even if a dependency changes upstream.
apt install -y i2c-tools libcamera-tools v4l-utils \\
	gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libcamera \
	gstreamer1.0-gtk4

# ErhaiCam's AF/AE statistics run on the Hexagon NPU through its helper
# erhai-npu-analyze.py, which needs an onnxruntime carrying the QNN execution
# provider (only the MS/QNN wheels have it) -- not packaged yet, so a shipped
# image analyzes with the helper's numpy backend / the app's own CPU metric
# instead.  Same numbers, just more CPU: nothing extra to install here.

# Desktop.
apt install -y ubuntu-desktop
# Ptyxis is the erhai terminal (ubuntu-desktop only RECOMMENDS it, and
# xdg-terminal-exec is the indirection that selects it), so install it
# explicitly.  gnome-terminal is deliberately NOT part of the image -- it was
# installed here until 2026-10-17 and nothing needs it (ubuntu-desktop neither
# depends nor recommends it); the purge below also cleans an image built on top
# of an older base.  alacritty must not come back either.
apt install -y ptyxis
apt purge -y gnome-terminal alacritty || true
# GNOME's camera app (gnome-snapshot -- the "Camera"/Snapshot app) comes in as a
# RECOMMEND of ubuntu-desktop.  ErhaiCam (oneplus-erhai-erhaicam) is THE camera
# app on this pad: it owns the sensors, the AE/AF buttons and the NPU
# statistics.  Nothing depends on gnome-snapshot, so it goes; the absence guard
# further down keeps it from coming back, and its app-grid position is dropped
# from the dconf seed (root-setup-files/skel-dconf.txt).
apt purge -y gnome-snapshot || true
apt install -y firefox
# Brave Browser (its own repo, staged above); guard like firefox so a transient
# repo hiccup is reconciled by the final 'apt --fix-broken install'.
apt install -y brave-browser || true
# Chromium (xtradeb/apps .deb, staged above) -- NOT Ubuntu's snap stub.
apt install -y chromium || true
${EXTRA_PACKAGES:+apt install -y $EXTRA_PACKAGES}

apt upgrade -y
apt --fix-broken install -y

# ------------------------------------------------------ erhai packages LAST --
# erhai kernel + firmware + Mesa + every device bring-up deb, straight from
# the online repo.  Installed LAST -- AFTER ubuntu-desktop (and every other apt
# run of this stage, including the 'apt upgrade' just above) so the erhai
# packages are the FINAL state of the image:
#   * the SIX Mesa /usr replacement debs (Ubuntu's own package names!) must win
#     over whatever ubuntu-desktop pulled in -- installing them first leaves
#     room for a later ubuntu-desktop/upgrade transaction to touch the same
#     packages;
#   * oneplus-erhai-desktop/-camera/-audio flip files and services that the
#     Ubuntu desktop packages lay down, so they belong on top;
#   * ubuntu-desktop and its dependency closure can only ever pull, never be
#     pulled by, the erhai layer (nothing erhai ships is a dependency of the
#     desktop), so the erhai install resolves against the finished desktop.
#
# ONLY packages already promoted to the PUBLIC main suite may be listed here:
# the image's /etc/apt/sources.list.d/oneplus-erhai.sources ships main alone
# (the TESTING suite is the owner's private opt-in), so a deb still sitting in
# testing fails this apt run. Publish with ./build-*-deb.sh and promote with
# ../update-ppa.sh --mvtomain before an image build.
#
# The kernel is the CURRENT 7.3-rc5 line (7.3.0-rc5-sm8750-themojoman; main
# carries +b7..+b10 and +b12..+b18 -- +b11 was superseded before it was
# published -- and apt picks +b18). It carries the whole September work --
# CDSP remoteproc + the fastrpc mapping pool/fixes (erhai_release_at_close,
# erhai_alloc_report), the pogo-keyboard attach/detach uevent the folio stack
# needs, the ADSP handover log fix -- plus the October work: the PMIC RTC
# offset cell (b8), the SVOOC cell-voltage and dual-CP settle work (b9/b10),
# the CPS8601 wireless-pen charger bring-up (b11..b17: real i2c hub, wake
# order, keep-powerered attach, stop at 100%), the NT36523 pen-probe honesty
# fix (b15) and the pogo keyboard's NFC/RFID presence decode (b18).  The
# retired 7.2 line (7.2.0-sm8750-themojoman) is NOT an equivalent fallback --
# it predates the CDSP work, so booting it means "no NPU" -- and main no longer
# offers it at all (a few advisory Suggests/Recommends fields still name it, but
# apt leaves those unsatisfied when the package does not exist).  Keep exactly
# ONE kernel line in the image: the 7.2 purge below is a belt-and-braces no-op
# that keeps an image built on top of an older base clean.
#
# Mesa: the SIX Ubuntu-named /usr replacement debs are what ship (published to
# the PPA main suite 2026-09-28): libgbm1, mesa-libgallium, libgl1-mesa-dri,
# libglx-mesa0, libegl-mesa0, mesa-vulkan-drivers.  The legacy /opt carrier
# oneplus-erhai-mesa is in NO suite any more (switch-mesa-usr.sh --finalize also
# stripped the overlay's /opt opt-in on 2026-09-23) -- listing it here aborts the
# image build with an unresolvable package.  Refresh the set with
# 'MESA_PREFIX=/usr ./update-mesa.sh' -> './build-mesa-usr-debs.sh' -> copy the
# six debs into ppa/oneplus-erhai/ubuntu/main/ + '../update-ppa.sh'.
#
# Ubuntu-named LOCAL PATCHES ride along the same way (they must win over the
# copy ubuntu-desktop pulled in, hence LAST):
#   * libmutter-18-0 + gir1.2-mutter-18, 50.1-0ubuntu2.4+erhai1 -- one guard in
#     src/wayland/meta-wayland-touch.c (touch_can_grab_surface(): a CLIENT touch
#     drag starting while another finger's touch had just been cancelled
#     dereferenced the cleared focus_surface and SEGV'd the compositor, which is
#     the 'a 3-finger swipe in Files logs the session out' report).  The two
#     debs pin EACH OTHER exactly, so they must be listed together or apt
#     reports unmet dependencies; libmutter-18-0-dbgsym is deliberately NOT
#     listed (an image never installs it -- it comes from ddebs -- and the
#     published one carries the UNPATCHED build's Build-Ids).
#   * oneplus-erhai-nautilus / oneplus-erhai-ptyxis divert /usr/bin/nautilus and
#     /usr/bin/ptyxis with dpkg-divert (those binaries belong to Ubuntu's
#     packages) and add the touch patches: long-press context menus, select
#     boxes, one-finger-only file drags and a rename popover that survives an
#     OSK key tap (nautilus); finger-drag scrolling, long-press marking and
#     click forwarding inside mouse-tracking apps (ptyxis).
#   * libgtk-4-1, 4.22.4+ds-0ubuntu0.1+erhai1 -- one guard in
#     gdk/wayland/gdkseat-wayland.c: _gdk_wayland_seat_get_implicit_grab_serial()
#     dereferenced a MISSING touch once the compositor had cancelled it, so a
#     drag beginning in that instant SEGV'd the app (the '3-finger swipe closes
#     Files' report).  ONLY the runtime deb is listed: the two exact-pin
#     siblings (libgtk-4-dev, libgtk-4-1-dbgsym) are published as well -- so a
#     development box that has them stays apt-consistent -- but an image
#     installs neither, and gir1.2-gtk-4.0 wants 'libgtk-4-1 (>= 4.22.0)', not
#     an exact pin.  All three published 2026-10-17 (PPA commit f56b995).
# (Quoted, never backticked: this comment sits INSIDE the unquoted <<EOF heredoc
#  below, so backticks here would be COMMAND-SUBSTITUTED ON THE HOST while the
#  stage script is written -- see the heredoc hygiene guard near the top.)
#
# NO || true here: one unresolvable package name
# (2026-09-03: the kernel deb was not yet in the repo index when the chroot's
# 'apt update' ran) aborted the WHOLE request, and 'apt --fix-broken install'
# does not resurrect never-resolved packages -- so the image shipped without a
# single erhai package and only blew up 40 minutes later at 'systemctl enable
# erhai-touchscreen.service'. One retry after a fresh 'apt update' covers
# transient repo hiccups; a real failure must abort the stage (the marker
# check below makes that fatal, and the package-set check verifies the erhai
# packages too).
# (no backticks inside the unquoted <<EOF heredocs: the OUTER shell expands
#  them while feeding the chroot, executing host commands and splicing their
#  output into the chroot script -- that once ran apt on the HOST)

apt install -y \
	oneplus-erhai-linux-7.3 oneplus-erhai-firmware \
	libgbm1 mesa-libgallium libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
	mesa-vulkan-drivers \
	libmutter-18-0 gir1.2-mutter-18 libgtk-4-1 \
	oneplus-erhai-audio oneplus-erhai-wifi oneplus-erhai-bluetooth \
	oneplus-erhai-camera oneplus-erhai-erhaicam oneplus-erhai-desktop \
	oneplus-erhai-thermal oneplus-erhai-video oneplus-erhai-torch \
	oneplus-erhai-sensors oneplus-erhai-otg oneplus-erhai-pen \
	oneplus-erhai-fastrpc oneplus-erhai-npu oneplus-erhai-llamacpp \
	oneplus-erhai-nautilus oneplus-erhai-ptyxis \
|| { sleep 5; apt update; apt install -y \
	oneplus-erhai-linux-7.3 oneplus-erhai-firmware \
	libgbm1 mesa-libgallium libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
	mesa-vulkan-drivers \
	libmutter-18-0 gir1.2-mutter-18 libgtk-4-1 \
	oneplus-erhai-audio oneplus-erhai-wifi oneplus-erhai-bluetooth \
	oneplus-erhai-camera oneplus-erhai-erhaicam oneplus-erhai-desktop \
	oneplus-erhai-thermal oneplus-erhai-video oneplus-erhai-torch \
	oneplus-erhai-sensors oneplus-erhai-otg oneplus-erhai-pen \
	oneplus-erhai-fastrpc oneplus-erhai-npu oneplus-erhai-llamacpp \
	oneplus-erhai-nautilus oneplus-erhai-ptyxis; }
apt --fix-broken install -y || true

# One kernel line only: no erhai package can pull the retired 7.2 line any more
# (oneplus-erhai-video 1.2 declares NO Recommends, and main carries no
# oneplus-erhai-linux-7.2 stanza at all), but the purge stays as a
# belt-and-braces no-op for an image built on top of an older base -- and it
# must still run LAST, after every apt action of this stage.
#
# GUARDED BY dpkg ON PURPOSE (2026-10-17): on a base that never had the 7.2 line
# a bare 'apt purge oneplus-erhai-linux-7.2' is NOT a silent no-op -- apt prints
# "Error: Unable to locate package oneplus-erhai-linux-7.2" and "Error:
# Couldn't find any package by glob 'oneplus-erhai-linux-7.2'" into the build log
# (the glob wording because the package name carries a dot), which reads like a
# broken build and cost the owner a question.  dpkg-query knows the package
# exactly when an older base installed it, so the cleanup still runs where it is
# needed and the log stays honest where it is not.
if dpkg-query -W oneplus-erhai-linux-7.2 >/dev/null 2>&1; then
	apt purge -y oneplus-erhai-linux-7.2 || true
fi

apt clean
touch /run/erhai-stage-packages-done
EOF
chroot "$ROOTDIR" /bin/bash /tmp/erhai-stage-packages.sh </dev/null
rm -f "$ROOTDIR/tmp/erhai-stage-packages.sh"

test -f "$ROOTDIR/run/erhai-stage-packages-done" || {
	echo "FATAL: chroot stage 'packages' did not complete" >&2; exit 1; }
chroot "$ROOTDIR" dpkg -l ubuntu-desktop firefox brave-browser chromium ptyxis console-setup gnome-shell \
	oneplus-erhai-linux-7.3 oneplus-erhai-firmware \
	libgbm1 mesa-libgallium libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
	mesa-vulkan-drivers libmutter-18-0 gir1.2-mutter-18 libgtk-4-1 \
	oneplus-erhai-audio oneplus-erhai-wifi oneplus-erhai-bluetooth \
	oneplus-erhai-camera oneplus-erhai-erhaicam oneplus-erhai-desktop \
	oneplus-erhai-thermal oneplus-erhai-video oneplus-erhai-torch \
	oneplus-erhai-sensors oneplus-erhai-otg oneplus-erhai-pen \
	oneplus-erhai-fastrpc oneplus-erhai-npu oneplus-erhai-llamacpp \
	oneplus-erhai-nautilus oneplus-erhai-ptyxis >/dev/null || {
	echo "FATAL: required packages missing from rootfs" >&2; exit 1; }
echo "[build-root] package set verified"

# ...and the packages that must NOT be there.  gnome-terminal: ptyxis is the
# erhai terminal and the ONLY one the image ships (see the desktop step above);
# gnome-snapshot: ErhaiCam is the erhai camera app.  Anything that re-introduces
# either means a dependency or an apt run changed behind our back.
for unwanted in gnome-terminal gnome-snapshot; do
	if chroot "$ROOTDIR" dpkg-query -W -f='${db:Status-Status}\n' "$unwanted" \
			2>/dev/null | grep -qx installed; then
		case $unwanted in
		gnome-terminal) why="ptyxis is the erhai terminal" ;;
		gnome-snapshot) why="ErhaiCam (oneplus-erhai-erhaicam) is the erhai camera app" ;;
		*)              why="it must not be in the image" ;;
		esac
		echo "FATAL: $unwanted is installed in the image ($why)" >&2
		exit 1
	fi
done

# A masked sleep/suspend target HIDES GNOME's "Suspend" entry completely: logind
# then answers CanSuspend()="no" and gnome-shell drops the menu item (measured
# 2026-09-16 on this pad; `systemctl unmask sleep.target suspend.target` flips it
# back to "yes").  The pre-2.0 overlay shipped exactly those two masks and quietly
# re-introduced the bug into every image built from it -- caught 2026-09-29 on the
# sda17 test install, where the Suspend entry was missing while the whole rest of
# the suspend chain (drop-in, erhai-suspend-gnome.sh, -s2idle.sh) was in place.
# The overlay is fixed and this guard keeps it that way: it runs AFTER the erhai
# debs, so it also catches a package that would ship the mask.
# hibernate.target / hybrid-sleep.target STAY masked on purpose -- this port has
# no hibernate and GNOME hides only that one entry.
for masked in suspend.target sleep.target; do
	if [ -L "$ROOTDIR/etc/systemd/system/$masked" ]; then
		echo "FATAL: /etc/systemd/system/$masked is masked in the image" \
		     "(-> $(readlink "$ROOTDIR/etc/systemd/system/$masked")):" \
		     "logind would report CanSuspend()=no and GNOME would hide its Suspend entry." >&2
		exit 1
	fi
done

# ------------------------------------------------------- user + services -----
cat > "$ROOTDIR/tmp/erhai-stage-usersetup.sh" <<EOF
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive

# Desktop user. /etc/skel carries monitors.xml (3392x2400 @ scale 2), the
# environment.d GL templates, .bash_aliases and the seeded dconf database — so
# whoever creates the account (useradd here, or gnome-initial-setup on first
# boot) copies them into the new home.
if [ "$PRESET_USER" = 1 ]; then
	if ! id -u $USERNAME > /dev/null 2>&1; then
		useradd -m -u $USERUID -s /bin/bash -G sudo,audio,video,input,dialout,render $USERNAME
		echo "$USERNAME:$USERPASS" | chpasswd
	fi
	# The rest of the hardware groups come from the one script that defines the
	# set (audio/video/input/dialout/render/i2c, plus erhai-torch for the
	# rear-flash LED attributes and fastrpc for /dev/fastrpc-*), so a baked
	# account and a gnome-initial-setup account end up identical.  It skips
	# groups this system does not have.
	/usr/local/sbin/erhai-user-groups.sh || true
else
	# gnome-initial-setup only runs when there is no human account at all. It
	# puts the account it creates in sudo; the hardware groups have to come from
	# somewhere, so seed them as udev/systemd defaults instead of a user list.
	apt install -y gnome-initial-setup
	# erhai-user-groups.sh (oneplus-erhai-desktop) adds audio video input
	# dialout render i2c, plus erhai-torch and fastrpc:
	#   * erhai-torch -- the rear-flash LED attributes are raw sysfs files
	#     chgrp'd to that group (an LED class device has no device node, so
	#     TAG+="uaccess" cannot hand them to the session user).  Without it the
	#     torch quick-slider only reports EACCES.
	#   * fastrpc -- /dev/fastrpc-adsp + /dev/fastrpc-cdsp are root:fastrpc 0640,
	#     which is what the Hexagon/NPU clients need.
	# With no baked account the script has to run AFTER gnome-initial-setup
	# created one, and it must be able to run MORE THAN ONCE:
	#   * the service runs it at every boot (graphical.target) and is NOT
	#     RemainAfterExit, so a later start really re-executes it;
	#   * the PATH unit runs it when /etc changes, i.e. when
	#     gnome-initial-setup writes the new account.  Without it the account
	#     created on the FIRST boot keeps NO hardware groups until the next
	#     REBOOT: the boot-time run happens before the wizard exists, and the
	#     torch slider then answers "Not allowed to drive the torch. Log out
	#     and back in so the erhai-torch group applies." for the whole session
	#     (measured 2026-10-17 on the sda17 image -- the user was in no hardware
	#     group at all, and logging out and back in did not help either).
	#     The watch is on the /etc DIRECTORY, not on /etc/passwd: accountsservice
	#     creates the account through /usr/sbin/adduser -> shadow's useradd /
	#     usermod, and those REPLACE /etc/passwd and /etc/group by rename (the
	#     inode changes; measured 2026-10-17).  systemd drops a PathChanged watch
	#     on a file that is replaced that way, so a file watch silently stops
	#     working after the first account appears.  A directory watch catches
	#     create, rename-into-place and in-place rewrites (all three measured,
	#     tmp/pathtest*.sh), and an idle /etc produced 0 events in 90 s.  The
	#     script is idempotent and silent when there is nothing to add, so the
	#     one re-trigger its own usermod causes costs nothing.
	cat > /etc/systemd/system/erhai-user-groups.service <<'UNIT'
[Unit]
Description=erhai add the desktop users to the hardware groups
After=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/erhai-user-groups.sh

[Install]
WantedBy=graphical.target
UNIT
	cat > /etc/systemd/system/erhai-user-groups.path <<'UNIT'
[Unit]
Description=erhai apply the hardware groups when a desktop account appears

[Path]
PathChanged=/etc

[Install]
WantedBy=paths.target
UNIT
	systemctl enable erhai-user-groups.service erhai-user-groups.path
fi
passwd -d root

# Compile the erhai dconf defaults (power-button-action=nothing, extensions on).
dconf update || true

# pd-mapper is not installed any more (see the note above); its packaged unit
# used to need this on non-vendor kernels:
# sed -i '/ConditionKernelVersion/d' /lib/systemd/system/pd-mapper.service

# zram: the packaged sizing math assumes a small device; pin it instead.
sed -i '/^totalmem/ s/^/#/' /usr/bin/init-zram-swapping
sed -i '/^mem/ s/.*/#&\nmem=10GiB/' /usr/bin/init-zram-swapping

systemctl enable bluetooth
systemctl enable systemd-networkd.socket || true

# erhai services.
#
# EVERY service the image needs must be enabled HERE: inside the build chroot
# /run/systemd/system does not exist, so the "[ -d /run/systemd/system ]" guard
# at the top of every deb postinst is FALSE and ALL of their systemctl enables
# are silently skipped.  This block is the image's substitute for what those
# postinsts do on a booted system -- compare it against
# "systemctl list-unit-files --state=enabled | grep erhai" there.
systemctl enable erhai-touchscreen.service   # rebinds NVT touch, then starts gdm
systemctl enable erhai-desktop-gl.service    # GL env follows the booted kernel
systemctl enable erhai-gpu-pin.service       # arm + pin the Adreno 830 pre-gdm
# (no erhai-audio-route.service any more: the oneplus-erhai-audio deb's UCM
# profile is applied by WirePlumber/ACP at session start)
# (no erhai-bt-up.service any more: retired by oneplus-erhai-bluetooth 2.2 --
#  the DT carries the shared local-bd-address, so hci0 registers CONFIGURED
#  and the stock bluetooth.service enabled above powers it via AutoEnable)
systemctl enable erhai-host-charge.service   # charge while USB data-host
systemctl enable erhai-power-toggle.service  # power button -> backlight toggle
systemctl enable erhai-backlight.service     # 50% panel level at boot (pre-gdm)
systemctl enable erhai-greeter-brightness.service  # ...and on the login screen
systemctl enable erhai-thermal.service       # CPU+GPU thermal profile
systemctl enable erhai-cam-arm.service       # CCI CSR + both camera sensors
# one-shot: waits for gnome-initial-setup to finish, then mounts PARTLABEL=data
# on /media/data (fstab entry), chowns the filesystem ROOT to the first desktop
# user and removes itself. No data partition -> removes itself immediately.
# Never touches existing data on the partition.
systemctl enable erhai-data-setup.service
# Audio (oneplus-erhai-audio): ADSP bring-up.  NOTE: erhai-dp-audio-sync.service
# is deliberately NOT enabled -- it is a STATIC unit (no [Install]) whose only
# trigger is udev rule 70-erhai-dp-audio.rules on a DRM connector change;
# "systemctl enable" on it fails, which would abort this stage under set -e.
systemctl enable erhai-audio-bringup.service

# WiFi (oneplus-erhai-wifi): PCIe floor, cold-boot calibration, wlan.ko, and
# the UNII-1 gap sweeper that compensates for the firmware's missing active
# probe.  The deb's postinst enables these on a booted system.
systemctl enable erhai-wcn-holdfloor.service
systemctl enable erhai-wcn-devcd.service
systemctl enable erhai-wcn-wifi.service
systemctl enable erhai-wifi-discover.service

# Camera (oneplus-erhai-camera): erhai-cam-arm.service is already enabled above;
# what is new here is the ACL refresh that lets the desktop user open the
# camera nodes.
systemctl enable erhai-camera-acl.service

# Sensors (oneplus-erhai-sensors): the ADSP remotefs daemon and the pinned
# tablet-mode switch.  erhai-alsd is a USER unit -- enable it globally (every
# future user) and write the symlink explicitly as well, because
# "systemctl --global" in a chroot without a running PID1 is not guaranteed.
systemctl enable hexagonrpcd.service
systemctl enable erhai-tablet-mode.service
mkdir -p /etc/systemd/user/graphical-session.target.wants
ln -sf /etc/systemd/user/erhai-alsd.service \
	/etc/systemd/user/graphical-session.target.wants/erhai-alsd.service
systemctl --global enable erhai-alsd.service >/dev/null 2>&1 || true

# Pogo keyboard (oneplus-erhai-desktop): attach -> on-screen keyboard.
systemctl enable erhai-pogo-osk.service

# Pen (oneplus-erhai-pen): CPS8601 wireless pen charger + the attach/detach
# toasts.
systemctl enable erhai-pen.service
systemctl enable erhai-pen-notify.service

# NOT enabled on purpose:
#  * erhai-folio-fold.service -- udev's 71-erhai-folio-udev.rules starts it on
#    the pogo driver's attach uevent.
#  * erhai-folio-gate.service -- the pre-uevent-kernel 1 Hz fallback.  The
#    desktop postinst arms it only when it reads a kernel deb BELOW +b46, and
#    its check queries oneplus-erhai-linux-7.2, which the 7.3-rc5 image does
#    not install -- so state the intent explicitly rather than depend on it.
#  * erhai-otg-rearm.service -- udev's SYSTEMD_WANTS on a typec port0 change.
#  * erhai-thermal@.service -- a template: the GNOME Quick Settings toggle
#    starts erhai-thermal@<profile>.service on demand (polkit-authorized).
systemctl disable erhai-folio-gate.service || true

# NPU / CDSP transport is Qualcomm's cdsprpcd: a STATIC unit (no [Install]),
# started by the fastrpc udev rule's SYSTEMD_WANTS, so "systemctl enable" is a
# no-op and nothing is enabled here.  The packaged hexagonrpcd-cdsp fallback
# must stay DISABLED: two remotefs daemons on /dev/fastrpc-cdsp fight over the
# DSP's one-shot shell load.
systemctl disable hexagonrpcd-cdsp.service || true

# gdm must NOT start on its own: the panel is in-cell, so touch only syncs to
# the display during a display-manager bring-up. erhai-touchscreen.service waits
# for "boot firmware ready" and starts gdm itself.
systemctl disable gdm || true
systemctl disable gdm3 || true

# systemd-mute-console blanks the framebuffer on this device.
systemctl mask systemd-mute-console.socket || true

# power-profiles-daemon finds no platform_profile driver here, so its GNOME
# Power Mode toggle does nothing (the real knob is the erhai Thermal toggle),
# and its After=multi-user.target + Type=dbus combination makes gnome-shell
# wait out a 25s D-Bus activation timeout on every boot.
systemctl mask power-profiles-daemon.service || true
EOF
chroot "$ROOTDIR" /bin/bash /tmp/erhai-stage-usersetup.sh </dev/null
rm -f "$ROOTDIR/tmp/erhai-stage-usersetup.sh"

# ---------------------------------------------------------------- output -----
cp "$ROOTDIR"/boot/*.efi . 2>/dev/null || echo "no EFI files in the image (kernel deb ships its own UKI?)"

# ------------------------------------------------------------ patch report ---
# The Ubuntu-named ERHAI builds -- the mutter touch guard, the GTK4 GDK guard
# and the six Mesa /usr replacement debs -- only reach the image by having a
# HIGHER version than Ubuntu's own copy of the SAME package name.  If Ubuntu
# publishes a newer base than our patched build (e.g. mutter 50.1-0ubuntu2.5 vs
# our 50.1-0ubuntu2.4+erhai1), apt silently installs Ubuntu's version: the
# install succeeds, the package-set check above only verifies NAMES, and the
# erhai fix is simply absent.  So look the versions up in the finished image and
# REPORT it here -- never abort, the image is bootable, the patch is just gone.
# Silence here (= no output) is the success case.
unpatched=0
for pkg in libmutter-18-0 gir1.2-mutter-18 libgtk-4-1 \
	libgbm1 mesa-libgallium libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
	mesa-vulkan-drivers; do
	ver="$(chroot "$ROOTDIR" dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
	case "$ver" in
		*erhai*) continue ;;
	esac
	case "$pkg" in
		libgbm1|mesa-*) what="Mesa /usr replacement (Turnip + rusticl)" ;;
		libgtk-4-1)     what="GTK4 GDK drag NULL-guard" ;;
		*)              what="mutter touch_can_grab_surface() NULL-guard" ;;
	esac
	if [ -z "$ver" ]; then
		echo "[build-root] WARNING: $pkg is NOT installed in the image -- the erhai $what is absent" >&2
	else
		echo "[build-root] WARNING: $pkg in the image is Ubuntu's own build ($ver)," >&2
		echo "[build-root]          so the erhai $what is MISSING from this image" >&2
	fi
	unpatched=$((unpatched + 1))
done
if [ "$unpatched" -gt 0 ]; then
	echo "[build-root] ===== an erhai Ubuntu-named build did NOT win in this image =====" >&2
	echo "[build-root] $unpatched package(s) above are Ubuntu's own builds, not the erhai" >&2
	echo "[build-root] ones.  Rebuild ABOVE that base, republish, then re-run this build:" >&2
	echo "[build-root]   mutter : oneplus-erhai-mutter/build-mutter-deb.sh (BASE=<new>)" >&2
	echo "[build-root]            + tmp/mutter-build/bump-mutter-siblings.sh BASE=<new> --install" >&2
	echo "[build-root]   GTK4   : oneplus-erhai-gtk4/build-gtk4-deb.sh (BASE=<new>)" >&2
	echo "[build-root]   Mesa   : MESA_PREFIX=/usr ./update-mesa.sh && ./build-mesa-usr-debs.sh" >&2
	echo "[build-root] ===============================================================" >&2
	echo "[build-root] this image is missing the erhai build(s) listed above -- check before shipping" >&2
fi

echo "done: $IMG"
if [ "$PRESET_USER" = 1 ]; then
	echo "  user $USERNAME / $USERPASS, root has an empty password"
else
	echo "  no user baked in -- gnome-initial-setup runs on first boot, root has an empty password"
fi
