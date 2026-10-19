#!/bin/sh
# Reset the GDM greeter/initial-setup config dir at boot.
#
# /var/lib/gdm3/seat0/config IS the greeter's XDG_CONFIG_HOME. Greeter sessions
# run as a DYNAMIC user (fresh uid every boot) which mutter/dconf update files
# in there as -- and after an initial-setup run the dir may be owned by a dead
# uid with 0700 dirs, locking out every future greeter (login screen then
# falls back to scale 1). This runs once per boot, BEFORE gdm:
#   * make the dir world-writable again,
#   * reinstall the rate-proof monitors.xml (one <configuration> block per
#     panel rate -- mutter matches on refresh rate too; single-rate files lose
#     whenever the panel comes up at another rate) and the dconf seed
#     (OSK on, scaling fallback).
log() { echo "erhai-greeter-config: $*" > /dev/kmsg 2>/dev/null; }

dir=/var/lib/gdm3/seat0/config
[ -d "$dir" ] || { log "no $dir"; exit 0; }

chmod -R a+rwX "$dir" 2>/dev/null

if [ -f /usr/share/erhai/greeter-monitors.xml ]; then
	install -m 644 /usr/share/erhai/greeter-monitors.xml "$dir/monitors.xml" &&
		log "monitors.xml reinstalled"
fi
if [ -f /usr/share/erhai/greeter-dconf-user ]; then
	mkdir -p "$dir/dconf"
	install -m 644 /usr/share/erhai/greeter-dconf-user "$dir/dconf/user" &&
		log "dconf seed reinstalled"
fi
exit 0
