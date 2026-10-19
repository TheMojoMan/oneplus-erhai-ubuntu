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
#     pct = 2.5 + 97.5 * log10(1 + lux) / log10(1 + 20000)
#
#   lux     pct      (~2047 max backlight)
#     0     2.5%     ~51   floor, never fully black
#     4    18%       ~370  dark room, comfortable
#    40    32%       ~650  indirect light
#   400    56%       ~1150 bright room
#  20000   100%      2047  outdoors
#
# Runs as the SESSION user and writes brightness through logind
# SetBrightness (polkit grants that to active sessions) — no root needed.
# Claims the sensor-proxy light client itself (refcounted); gsd's own
# ambient is forced off (the erhai-sensors toggle drives
# org.erhai.sensors ambient-enabled instead).
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

LUX_MAX = 20000.0
PCT_FLOOR = 2.5
SMOOTH_TC_S = 1.5          # EWMA time constant
POLL_S = 1.0
MIN_WRITE_DELTA_PCT = 1.0


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

        self.claim()
        lux = self.read_lux()
        if lux is None:
            return GLib.SOURCE_CONTINUE

        alpha = SMOOTH_TC_S / (SMOOTH_TC_S + POLL_S)
        self.ewma = lux if self.ewma is None \
            else alpha * lux + (1.0 - alpha) * self.ewma

        frac = math.log10(1.0 + self.ewma) / math.log10(1.0 + LUX_MAX)
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
