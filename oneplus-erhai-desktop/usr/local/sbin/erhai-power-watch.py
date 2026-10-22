#!/usr/bin/env python3
# Watch the PMIC power key (KEY_POWER) and toggle the panel backlight on each press.
#
# PANEL INPUT GUARD (2026-10-22): while the panel is blanked by the power button
# (/run/erhai-panel-off present AND the backlight at 0) the PANEL'S OWN INPUT IS
# SUPPRESSED with EVIOCGRAB on the touchscreen + the pen node.  A blanked panel
# must not act on ghost touches, and any panel event counts as user activity for
# the compositor, which answers by waking the session behind a dark screen
# (measured: the power button blanked the panel and the folded folio lying on
# the glass delivered NVTCapacitivePen frames to a live session, so the desktop
# was awake and receiving input under a switched-off display; grabbed, that
# input reaches nobody).  The power key is NEVER grabbed - it is the only way
# out of a blank.
#
# The guard lives exactly as long as the blank: it is released BEFORE the panel
# is restored on the next press, it is re-evaluated once a second (a marker the
# panel does not agree with - e.g. the backlight was restored - drops it), and it
# dies with this process because EVIOCGRAB belongs to the fds, so a crash can
# never leave the touchscreen dead.  Disable with PANEL_INPUT_GUARD=0 in
# /etc/erhai/display.conf or ERHAI_PANEL_INPUT_GUARD=0 in the environment.
import argparse
import errno
import fcntl
import glob
import os
import select
import struct
import subprocess
import sys
import time

KEY_POWER = 116
EV_KEY = 1
TOGGLE = ['/usr/local/sbin/erhai-panel-power.sh']
_SURVLOG = '/usr/local/sbin/erhai-surv-log'

MARKER = '/run/erhai-panel-off'          # written by erhai-panel-power.sh off
BL_GLOB = '/sys/class/backlight/*/actual_brightness'
CONF = '/etc/erhai/display.conf'

# EVIOCGRAB (evdev): while grabbed only the grabbing fd gets the device's
# events; closing the fd releases the grab.
EVIOCGRAB = 0x40044590
# The devices of the panel itself (touch + pen: the folded keyboard presses the
# glass and the pen node emits frames too).  The folio keyboard/touchpad are NOT
# grabbed - the touchpad is usable in the laptop position and eating its events
# would break typing there.
GUARD_NAMES = ('NVTCapacitiveTouchScreen', 'NVTCapacitivePen')
GUARD_POLL_S = 1.0


def survlog(msg):
    """Append one durable forensic line for this boot (fsync'd)."""
    try:
        subprocess.run([_SURVLOG, msg], timeout=5)
    except Exception:
        pass


def survlog_nowait(msg):
    """Same line, but never waited for: the PRESS must reach the panel path at
    once (2026-10-22 latency fix) — the panel script writes its own synchronous
    forensic lines a few ms later, so nothing is really lost."""
    try:
        subprocess.Popen([_SURVLOG, msg],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def log(msg):
    print(f'[{time.strftime("%T")}] {msg}', flush=True)


def backlight_file(glb=BL_GLOB):
    """'actual_brightness' of the internal panel (first backlight class)."""
    for p in sorted(glob.glob(glb)):
        return p
    return ''


def read_brightness(glb=BL_GLOB):
    """Backlight duty as int, or None when it cannot be read."""
    try:
        with open(backlight_file(glb)) as fh:
            return int(fh.read().strip().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def find_input_nodes(names, sysfs_glob='/sys/class/input/input*/name'):
    """{device name: /dev/input/eventN} for the wanted input device names."""
    found = {}
    for p in sorted(glob.glob(sysfs_glob)):
        try:
            with open(p) as fh:
                nm = fh.read().strip()
        except OSError:
            continue
        if nm not in names or nm in found:
            continue
        nodes = sorted(glob.glob(os.path.join(os.path.dirname(p), 'event*')))
        if nodes:
            found[nm] = '/dev/input/' + os.path.basename(nodes[0])
    return found


def guard_enabled():
    """PANEL_INPUT_GUARD from the environment, else /etc/erhai/display.conf."""
    env = os.environ.get('ERHAI_PANEL_INPUT_GUARD')
    if env is not None and env.strip() != '':
        return env.strip().lower() not in ('0', 'no', 'off', 'false')
    try:
        with open(CONF) as fh:
            for line in fh:
                line = line.split('#', 1)[0].strip()
                if line.startswith('PANEL_INPUT_GUARD='):
                    v = line.split('=', 1)[1].strip().strip('"\'')
                    return v.lower() not in ('0', 'no', 'off', 'false')
    except OSError:
        pass
    return True


def find_pwrkey():
    for d in glob.glob('/sys/class/input/event*/device/name'):
        try:
            if open(d).read().strip() == 'pmic_pwrkey':
                return '/dev/input/' + d.split('/')[4]
        except OSError:
            pass
    return '/dev/input/event0'

class PanelInputGuard:
    """EVIOCGRAB on the panel's input devices while the power blank is on."""

    def __init__(self, names=GUARD_NAMES, marker=MARKER, bl_glob=BL_GLOB):
        self.names = tuple(names)
        self.marker = marker
        self.bl_glob = bl_glob
        self.held = {}               # device name -> (fd, node)

    def blanked(self):
        """True while the power button holds the panel off (marker + dark)."""
        if not os.path.exists(self.marker):
            return False
        return read_brightness(self.bl_glob) == 0

    def grab(self):
        if self.held:
            return
        nodes = find_input_nodes(self.names)
        if not nodes:
            log('panel input guard: none of %s present' % ', '.join(self.names))
            return
        for name, node in sorted(nodes.items()):
            try:
                fd = os.open(node, os.O_RDONLY)
            except OSError as exc:
                log(f'panel input guard: cannot open {node}: {exc}')
                continue
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 1)
            except OSError as exc:
                os.close(fd)
                if exc.errno == errno.EBUSY:
                    log(f'panel input guard: {name} already grabbed elsewhere')
                else:
                    log(f'panel input guard: {name} grab failed on {node}: {exc}')
                continue
            self.held[name] = (fd, node)
            log(f'panel input guard: {name} {node} GRABBED (panel off)')

    def release(self):
        for name, (fd, node) in list(self.held.items()):
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 0)
            except OSError as exc:
                log(f'panel input guard: {name} ungrab failed: {exc}')
            try:
                os.close(fd)
            except OSError:
                pass
            log(f'panel input guard: {name} {node} RELEASED')
        self.held = {}

    def sync(self):
        """Engage while the blank is in force, release the moment it is not."""
        if self.blanked():
            self.grab()
        else:
            self.release()


def run_toggle(cmd, guard=None, poll_s=0.05):
    """Run the panel script, keeping the input guard in step with the blank.

    The script is a two-step operation (blank/restore) that can take a second or
    more to RETURN (bounded GPU park + the compositor round trip), so a guard that
    only synced after it returned would leave the panel's input live for that
    whole window — and the session is still awake behind the dark screen for the
    park part of it.  The direction is decided BEFORE the run: a press on a
    blanked panel is a wake (the guard is released and STAYS released while the
    panel comes back), anything else is a blank (the guard engages as soon as the
    marker and the dark backlight are there, ~40 ms in).
    """
    waking = guard is not None and guard.blanked()
    if waking:
        guard.release()
    try:
        proc = subprocess.Popen(cmd)
    except Exception as exc:
        log(f'cannot run {cmd}: {exc}')
        return 1
    if guard is None:
        return proc.wait()
    while proc.poll() is None:
        if not waking:
            guard.sync()
        time.sleep(poll_s)
    guard.sync()
    return proc.returncode


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='erhai-power-watch.py',
        description='PMIC power key -> real panel power cycle, plus the panel '
                    'input guard that keeps a blanked panel dark.')
    ap.add_argument('--no-guard', action='store_true',
                    help='do not suppress the panel input while blanked')
    a = ap.parse_args(argv)

    guard = None if (a.no_guard or not guard_enabled()) else PanelInputGuard()

    dev = find_pwrkey()
    fmt = 'llHHi'
    size = struct.calcsize(fmt)
    print(f'erhai-power-watch: reading {dev} (record {size}B)', flush=True)
    if guard is None:
        print('erhai-power-watch: panel input guard DISABLED', flush=True)
    else:
        guard.sync()

    with open(dev, 'rb', buffering=0) as f:
        while True:
            try:
                ready, _, _ = select.select([f], [], [], GUARD_POLL_S)
            except InterruptedError:
                continue
            except OSError:
                break
            if ready:
                data = f.read(size)
                if not data or len(data) < size:
                    break
                _s, _u, etype, code, value = struct.unpack(fmt, data)
                if etype == EV_KEY and code == KEY_POWER and value == 1:
                    print('KEY_POWER press -> toggle', flush=True)
                    survlog_nowait('powerwatch KEY_POWER ts=%d' % int(time.time()))
                    run_toggle(TOGGLE, guard)
            if guard is not None:
                guard.sync()
    return 0


if __name__ == '__main__':
    sys.exit(main())
