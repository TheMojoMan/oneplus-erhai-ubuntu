#!/usr/bin/env python3
"""erhai-greeter-brightness - keep the GDM login screen at a fixed backlight.

WHY (2026-09-22): the GDM greeter's dconf configuration can end up UNREADABLE.
GDM 50 runs the greeter as a dynamic user (gdm-greeter, uid 60578 here) whose
HOME (/run/gdm3/home/gdm-greeter) is not always accessible to that uid -- the
journal of a broken greeter shows hundreds of
    Failed to open /run/gdm3/home/gdm-greeter/.config/...: Permission denied
    .../home/gdm-greeter/.config/dconf/user: Permission denied
When dconf cannot be read, GSettings falls back to schema defaults, and Ubuntu's
10_ubuntu-settings.gschema.override makes the DEFAULTS "ubuntu" for
org.gnome.desktop.session session-name and (upstream default) true for
org.gnome.settings-daemon.plugins.power ambient-enabled.  On the login screen
that means a full desktop instead of the greeter, plus gsd-power's unanchored
ALS curve collapsing the panel to its minimum level.

The dconf-side fixes cover the readable case (greeter dconf seed,
/usr/share/gdm/dconf/95-erhai-greeter, the ambient gschema default override, and
the greeter .desktop pinning --session=gnome-login under /usr/local/share/gdm).
This daemon makes the BRIGHTNESS deterministic regardless of what the greeter's
own session does: when a greeter session appears the backlight is put back to
LEVEL percent immediately, a few times while the session settles, then slowly
re-asserted while a greeter session is on screen.  Sessions of class 'user'
(the real logged-in desktop) are never touched.

Knobs (systemd drop-in or Environment=):
    GREETER_BRIGHTNESS_LEVEL   percent of max_brightness (default 50)
"""
import os
import subprocess
import sys

import gi

gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

BUS_NAME = 'org.freedesktop.login1'
OBJ_PATH = '/org/freedesktop/login1'
MANAGER_IFACE = 'org.freedesktop.login1.Manager'
SESSION_IFACE = 'org.freedesktop.login1.Session'
PROPS_IFACE = 'org.freedesktop.DBus.Properties'

APPLY = '/usr/local/sbin/erhai-backlight-fixed.sh'
LEVEL = os.environ.get('GREETER_BRIGHTNESS_LEVEL', '50')

# A session-starting gnome-shell can write its own brightness a few seconds in,
# hence the burst; after that a slow re-assert keeps the level stable.
SETTLE_S = (0, 2, 5, 10)
REASSERT_S = 15


def log(msg):
    print('erhai-greeter-brightness: %s' % msg, flush=True)


def apply(level, reason):
    try:
        subprocess.run([APPLY, level], timeout=15, check=False)
    except Exception as exc:                 # defensive: never die on this
        log('apply failed (%s): %s' % (reason, exc))
        return
    log('%s: backlight -> %s%%' % (reason, level))


class GreeterWatcher:
    """Pin the backlight while a logind greeter session exists."""

    def __init__(self, conn):
        self.conn = conn
        self.greeters = 0
        self.sessions = set()

    @staticmethod
    def session_prop(conn, opath, name):
        try:
            res = conn.call_sync(
                BUS_NAME, opath, PROPS_IFACE, 'Get',
                GLib.Variant('(ss)', (SESSION_IFACE, name)),
                None, Gio.DBusCallFlags.NONE, 5000, None)
            return res.unpack()[0]
        except GLib.Error as exc:
            log('cannot read %s of %s: %s' % (name, opath, exc.message))
            return None

    def note_greeter(self, session_id, opath):
        self.sessions.add(opath)
        self.greeters += 1
        log('greeter session %s up (%s) - %d on screen'
            % (session_id, opath, self.greeters))
        for delay in SETTLE_S:
            if delay == 0:
                apply(LEVEL, 'greeter %s' % session_id)
            else:
                GLib.timeout_add_seconds(delay, self._burst, session_id, delay)

    def _burst(self, session_id, delay):
        if self.greeters > 0:
            apply(LEVEL, 'greeter %s +%ds' % (session_id, delay))
        return False

    def reassert(self):
        if self.greeters > 0:
            apply(LEVEL, 're-assert (%d greeter session(s) on screen)'
                  % self.greeters)
        return True

    def on_session_new(self, _conn, _sender, _path, _iface, _signal, params):
        session_id, opath = params.unpack()
        cls = self.session_prop(self.conn, opath, 'Class')
        log('session %s appeared (class %s)' % (session_id, cls))
        if cls == 'greeter':
            self.note_greeter(session_id, opath)

    def on_session_removed(self, _conn, _sender, _path, _iface, _signal, params):
        session_id, opath = params.unpack()
        if opath in self.sessions:
            self.sessions.discard(opath)
            self.greeters = max(0, self.greeters - 1)
            log('greeter session %s gone - %d on screen'
                % (session_id, self.greeters))

    def scan(self):
        try:
            res = self.conn.call_sync(
                BUS_NAME, OBJ_PATH, MANAGER_IFACE, 'ListSessions',
                None, None, Gio.DBusCallFlags.NONE, 5000, None)
        except GLib.Error as exc:
            log('ListSessions failed: %s' % exc.message)
            return
        for entry in res.unpack()[0]:
            session_id, _uid, _user, _seat, opath = entry
            if self.session_prop(self.conn, opath, 'Class') == 'greeter':
                self.note_greeter(session_id, opath)


def main():
    try:
        conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    except GLib.Error as exc:
        log('cannot connect to the system bus: %s' % exc.message)
        return 1
    watcher = GreeterWatcher(conn)
    conn.signal_subscribe(BUS_NAME, MANAGER_IFACE, 'SessionNew', OBJ_PATH, None,
                          Gio.DBusSignalFlags.NONE, watcher.on_session_new)
    conn.signal_subscribe(BUS_NAME, MANAGER_IFACE, 'SessionRemoved', OBJ_PATH,
                          None, Gio.DBusSignalFlags.NONE,
                          watcher.on_session_removed)
    watcher.scan()
    GLib.timeout_add_seconds(REASSERT_S, watcher.reassert)
    log('watching logind for greeter sessions (level %s%%)' % LEVEL)
    GLib.MainLoop().run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
