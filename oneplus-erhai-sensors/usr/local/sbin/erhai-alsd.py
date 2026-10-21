#!/usr/bin/env python3
# erhai ambient-light auto-brightness (absolute log curve).
#
# gsd-power's ambient algorithm is RELATIVE: it anchors 67% at
# (lux-at-renormalization * 1.5) and maps brightness = lux/anchor. On this
# pad that squeezes the whole 0-100% range into the tiny lux band of a dim
# room (anchor ~4 lux), so a hand shadow sends the screen to black and a
# lamp to maximum. This daemon instead uses an ABSOLUTE Android-style log
# curve, which keeps dark rooms readable and bright rooms bright:
#
#     pct = 1.0 + 99.0 * ( log10(1 + lux) / log10(1 + 2906) ) ^ 0.618
#
# The three constants are FITTED to the owner's own measurements in a dark room
# on 2026-10-20 - each point is the brightness level he picked for that light:
#     lux 0.0   ->   20 of 2047  ( 1.0 %)  dark room  = the floor
#     lux 2.72  ->  685 of 2047  (33.5 %)  dim room
#     lux 95.7  -> 1457 of 2047  (71.2 %)  normally lit room
# (the lux values come from monitor-sensor, which streams the sensor; reading
# the property without claiming it reports a stale 0).  The curve reaches 100 %
# at ~2900 lux and is clamped there, so daylight just stays at full.
#
#   lux      pct      raw (~2047)
#     0     1.0%        20    floor: the panel must stay readable in the dark
#   0.5    16.7%       341
#     1    22.8%       468
#     2    30.1%       615
#     3    34.6%       707
#     5    40.3%       825
#    10    48.1%       984
#    20    55.6%      1138
#    40    62.7%      1284
#   100    71.6%      1465
#   200    77.9%      1595
#   400    84.0%      1719
#  1000    91.6%      1875
#  2906   100.0%      2047    and above (clamped)
#
# Runs as the SESSION user and writes brightness through logind
# SetBrightness (polkit grants that to active sessions) — no root needed.
# Claims the sensor-proxy light client itself (refcounted); gsd's own
# ambient is forced off (the erhai-sensors toggle drives
# org.erhai.sensors ambient-enabled instead).
#
# It is enabled GLOBALLY, so it also starts for GDM's dynamic greeter user and
# for the gnome-initial-setup session.  Those are logind class 'greeter' and
# are NOT a logged-in desktop: tick() idles there (no sensor claim, no
# brightness write), see GREETER_SESSION_CLASSES / brightness_allowed().
#
# One-time migration: if the legacy gsd key is true (the old toggle), copy
# it to the new key and switch the legacy key off.
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

import glob
import math
import os
import sys

SCHEMA_ID = 'org.erhai.sensors'
LEGACY_SCHEMA = 'org.gnome.settings-daemon.plugins.power'
LEGACY_KEY = 'ambient-enabled'

LUX_MAX = 2906.0           # lux at which the curve reaches 100 %
PCT_FLOOR = 1.0            # 0 lux -> 20 of 2047: the flat dark-room floor
GAMMA = 0.618              # curve exponent (see the three measured points above)
SMOOTH_TC_S = 1.5          # EWMA time constant
POLL_S = 1.0
MIN_WRITE_DELTA_PCT = 1.0

# logind session classes this daemon must NEVER act in.  erhai-alsd is enabled
# GLOBALLY (every user, see oneplus-erhai-sensors' postinst and build-root.sh),
# so GDM's dynamic greeter user gets it too -- and so does the
# gnome-initial-setup session, which GDM registers with class 'greeter' as
# well.  Those sessions are not a logged-in desktop: the panel level there is
# owned by erhai-backlight.service / erhai-greeter-brightness.py (fixed 50 %),
# and the daemon's 2.5 % floor (~51 of 2047, its "never fully black" value)
# would look like a black screen and would also claim the light sensor away
# from the real user's session.  The class is read from logind, never guessed
# from the user name: unknown (no logind answer) stays permissive.
GREETER_SESSION_CLASSES = ('greeter',)


def open_settings():
    schema_dir = os.environ.get('GSETTINGS_SCHEMA_DIR')
    if schema_dir:
        source = Gio.SettingsSchemaSource.new_from_directory(
            schema_dir, Gio.SettingsSchemaSource.get_default(), False)
        schema = source.lookup(SCHEMA_ID, False)
        if schema is None:
            print(f'schema {SCHEMA_ID} not found in {schema_dir}', flush=True)
            sys.exit(1)
        return Gio.Settings.new_full(schema, None, None)
    return Gio.Settings.new(SCHEMA_ID)


def find_backlight():
    hits = sorted(glob.glob('/sys/class/backlight/*/max_brightness'))
    return os.path.dirname(hits[0]) if hits else None


class AlsDaemon:
    def __init__(self):
        self.settings = open_settings()
        self.system_bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.sensor_proxy = Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SYSTEM, Gio.DBusProxyFlags.NONE, None,
            'net.hadess.SensorProxy', '/net/hadess/SensorProxy',
            'net.hadess.SensorProxy', None)
        self.backlight = find_backlight()
        self.max_brightness = 1
        if self.backlight:
            self.max_brightness = int(
                open(os.path.join(self.backlight, 'max_brightness')).read())
        self.claimed = False
        self.ewma = None
        self.last_pct = None
        self.deferred_pct = None
        self.renamed_legacy = False
        self.session_path = None
        self._class_for = None
        self._class_value = None
        self.greeter_noted = False

    # --- legacy migration: old gsd toggle -> new key --------------------
    def migrate_legacy(self):
        if self.renamed_legacy:
            return
        try:
            legacy = Gio.Settings.new(LEGACY_SCHEMA)
            if legacy.get_boolean(LEGACY_KEY):
                self.settings.set_boolean('ambient-enabled', True)
                print('migrated legacy gsd ambient-enabled -> '
                      'org.erhai.sensors ambient-enabled', flush=True)
            legacy.set_boolean(LEGACY_KEY, False)
            self.renamed_legacy = True
        except GLib.Error as e:
            print(f'migration failed: {e.message}', flush=True)

    # --- sensor-proxy light claim (refcounted) --------------------------
    def _sensor_call(self, method):
        try:
            self.system_bus.call_sync(
                'net.hadess.SensorProxy', '/net/hadess/SensorProxy',
                'net.hadess.SensorProxy', method, None,
                None, Gio.DBusCallFlags.NONE, -1, None)
            return True
        except GLib.Error as e:
            print(f'{method} failed: {e.message}', flush=True)
            return False

    def claim(self):
        if not self.claimed:
            if self._sensor_call('ClaimLight'):
                self.claimed = True
                print('light sensor claimed', flush=True)

    def release(self):
        if self.claimed:
            self.claimed = not self._sensor_call('ReleaseLight')
            if not self.claimed:
                print('light sensor released', flush=True)

    # --- brightness ------------------------------------------------------
    def panel_blanked(self):
        """True while the panel backlight is blanked (brightness 0).

        The power button (erhai-power-toggle) and the folio-fold watcher
        (erhai-folio-fold) blank the panel by writing brightness 0; this
        daemon must NOT light it back up while it is blanked — it only
        remembers its target and applies it once the panel comes back.
        """
        try:
            with open(os.path.join(self.backlight, 'actual_brightness')) as f:
                return int(f.read().strip()) == 0
        except (OSError, ValueError):
            return False

    def read_lux(self):
        v = self.sensor_proxy.get_cached_property('LightLevel')
        if v is None:
            return None
        lux = v.get_double()
        return lux if lux > 0 else 0.0

    def resolve_session(self):
        """Find our uid's active session object path (alsd itself runs
        outside any session, so GetSessionByPID is useless here)."""
        try:
            res = self.system_bus.call_sync(
                'org.freedesktop.login1', '/org/freedesktop/login1',
                'org.freedesktop.login1.Manager', 'ListSessions',
                None, None, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error as e:
            print(f'ListSessions failed: {e.message}', flush=True)
            return None
        uid = os.getuid()
        entries = res.unpack()[0]
        # prefer the graphical (seated) session; the user manager session
        # has no seat and logind refuses SetBrightness on it
        candidates = [entry[4] for entry in entries
                      if entry[1] == uid and entry[3]]
        if not candidates:
            candidates = [entry[4] for entry in entries if entry[1] == uid]
        for path in candidates:
            try:
                active = self.system_bus.call_sync(
                    'org.freedesktop.login1', path,
                    'org.freedesktop.DBus.Properties', 'Get',
                    GLib.Variant('(ss)', ('org.freedesktop.login1.Session',
                                           'Active')),
                    None, Gio.DBusCallFlags.NONE, -1, None)
                if active.unpack()[0]:
                    return path
            except GLib.Error:
                continue
        return candidates[0] if candidates else None

    def session_class(self):
        """logind Class of the session we would write brightness through.

        'user' for a real login; 'greeter' for GDM's login screen AND for the
        gnome-initial-setup session (GDM registers both with that class).
        Cached per session object path, so it costs one D-Bus Get per session,
        not one per tick.
        """
        path = self.session_path or self.resolve_session()
        if not path:
            return None
        self.session_path = path
        if self._class_for == path:
            return self._class_value
        cls = None
        try:
            res = self.system_bus.call_sync(
                'org.freedesktop.login1', path,
                'org.freedesktop.DBus.Properties', 'Get',
                GLib.Variant('(ss)', ('org.freedesktop.login1.Session',
                                      'Class')),
                None, Gio.DBusCallFlags.NONE, -1, None)
            cls = res.unpack()[0]
        except GLib.Error as e:
            print(f'cannot read the session class of {path}: {e.message}',
                  flush=True)
        self._class_for = path
        self._class_value = cls
        return cls

    def brightness_allowed(self):
        """True only in a session that is a logged-in desktop.

        Unknown (logind did not answer) stays permissive on purpose: the guard
        exists to keep the daemon out of the greeter, not to disable
        auto-brightness when a lookup hiccups.
        """
        return self.session_class() not in GREETER_SESSION_CLASSES

    def set_brightness_pct(self, pct):
        if not self.backlight:
            return
        value = max(1, round(self.max_brightness * pct / 100.0))
        if not self.session_path:
            self.session_path = self.resolve_session()
            if not self.session_path:
                return
        try:
            self.system_bus.call_sync(
                'org.freedesktop.login1', self.session_path,
                'org.freedesktop.login1.Session', 'SetBrightness',
                GLib.Variant('(ssu)', ('backlight',
                                       os.path.basename(self.backlight),
                                       value)),
                None, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error as e:
            print(f'SetBrightness failed: {e.message}', flush=True)
            self.session_path = None

    # --- main tick --------------------------------------------------------
    def tick(self):
        self.migrate_legacy()
        enabled = self.settings.get_boolean('ambient-enabled')
        if not enabled:
            self.release()
            self.ewma = None
            self.last_pct = None
            self.deferred_pct = None
            return GLib.SOURCE_CONTINUE

        if not self.brightness_allowed():
            # GDM's greeter and the gnome-initial-setup session (both class
            # 'greeter' in logind) get erhai-alsd too, because the unit is
            # enabled globally.  They must never drive the panel: the level
            # there belongs to erhai-backlight.service /
            # erhai-greeter-brightness.py, and this daemon's floor is 2.5 %
            # (~51 of 2047) -- which reads as a black screen.  Do not even
            # claim the light sensor (that would steal it from the real
            # user's session).  Not an exit: the unit is Restart=always and
            # the class can change under us (greeter -> user at login).
            if not self.greeter_noted:
                print('session class %s is not a logged-in desktop - '
                      'auto-brightness idles here (the panel level is the '
                      'greeter/boot policy)' % self.session_class(),
                      flush=True)
                self.greeter_noted = True
            self.release()
            self.ewma = None
            self.last_pct = None
            self.deferred_pct = None
            return GLib.SOURCE_CONTINUE
        self.greeter_noted = False

        self.claim()
        lux = self.read_lux()
        if lux is None:
            return GLib.SOURCE_CONTINUE

        alpha = SMOOTH_TC_S / (SMOOTH_TC_S + POLL_S)
        self.ewma = lux if self.ewma is None \
            else alpha * lux + (1.0 - alpha) * self.ewma

        frac = (math.log10(1.0 + self.ewma) / math.log10(1.0 + LUX_MAX)) ** GAMMA
        pct = PCT_FLOOR + (100.0 - PCT_FLOOR) * min(1.0, max(0.0, frac))
        pct = min(100.0, max(0.0, pct))

        if self.panel_blanked():
            # Blanked by the power button / folio-fold watcher: keep tracking
            # the ambient level but never write — that would light the panel
            # back up behind the blank.
            self.deferred_pct = pct
            return GLib.SOURCE_CONTINUE

        force = self.deferred_pct is not None
        if force:
            pct = self.deferred_pct
            self.deferred_pct = None

        if force or self.last_pct is None \
                or abs(pct - self.last_pct) >= MIN_WRITE_DELTA_PCT:
            print(f'lux={self.ewma:.2f} -> brightness {pct:.1f}%', flush=True)
            self.set_brightness_pct(pct)
            self.last_pct = pct
        return GLib.SOURCE_CONTINUE


def main():
    daemon = AlsDaemon()
    if not daemon.backlight:
        print('no backlight found; exiting', flush=True)
        sys.exit(1)

    loop = GLib.MainLoop()

    def stop(*_a):
        daemon.release()
        loop.quit()

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, stop)  # SIGTERM
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, stop)   # SIGINT
    GLib.timeout_add_seconds(int(POLL_S), daemon.tick)

    print(f'alsd started (backlight {os.path.basename(daemon.backlight)}, '
          f'max {daemon.max_brightness})', flush=True)
    loop.run()


if __name__ == '__main__':
    main()
