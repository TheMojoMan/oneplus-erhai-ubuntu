#!/usr/bin/env python3
# Watch the PMIC power key (KEY_POWER) and toggle the panel backlight on each press.
#
# PANEL INPUT GUARD (2026-10-22): while the panel is blanked (the state file
# /run/erhai-panel.state exists AND the backlight is 0) the PANEL'S OWN INPUT IS
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
# is restored on the next press, it is re-evaluated once a second (a state the
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
import shlex
import signal
import struct
import subprocess
import sys
import time

KEY_POWER = 116
EV_KEY = 1
TOGGLE = ['/usr/local/sbin/erhai-panel-power.sh']
_SURVLOG = '/usr/local/sbin/erhai-surv-log'

"""
THE BLANK STATE (2.20): ONE owner-tagged file, /run/erhai-panel.state, one line
"<owner> <epoch>", owner = power | folio.  The file EXISTING means "the panel is
wanted OFF" and its owner says who asked; both watchers (this daemon and
erhai-folio-fold.py) read and write exactly this file, and erhai-panel-power.sh
defines the format.  FORMAT is pinned by tmp/folio-test/panel-state-test.sh.
"""
STATE = '/run/erhai-panel.state'         # owner-tagged blank state (see above)
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

# BLANK INTEGRITY (2026-10-22, sensor-MEMORY-2026-10-22.md §9).  The blank has
# ONE owner, and this keeps it that way: a panel/session that comes back BEHIND
# our back is put back dark.  Why the half-on state is not cosmetic - on boot
# 866f5cf7b7464aa7 the DSI panel was re-enabled 12 min into a 51-minute blank
# (1 s after a folio fold, `nvt_ts_panel_power_on`), the compositor was AWAKE
# behind the stale marker 51 min later, and from then on every power press drove
# the DPU and the backlight correctly and the screen stayed BLACK until a reboot.
KMSG = '/dev/kmsg'                    # the kernel log as a stream (root only)
PANEL_ON_MARK = 'nvt_ts_panel_power_on'   # xn242_enable() = a real CRTC enable
SS_DEST = 'org.gnome.ScreenSaver'
SS_PATH = '/org/gnome/ScreenSaver'
INTEGRITY_MIN_GAP_S = 3.0             # at most one re-assert per this many s
INTEGRITY_PRESS_SUPPRESS_S = 5.0      # ignore the detectors around a press
INTEGRITY_WARN_AFTER = 10             # shout when the blank keeps being undone
MON_RETRY_S = 15.0                    # how often to look for the session bus
MON_MAX_TRIES = 40                    # give up after this many failed starts

# PRESS-AND-HOLD -> CLEAN REBOOT (the blind escape, 2026-10-22).
# A black screen with a live SoC had exactly ONE exit: a HARD reset - a power cut
# that loses the journal and every forensic marker - and the logind long-press
# poweroff never fired (measured 13:13:10 + 13:14:03: "Power key pressed long."
# with no shutdown started).  POWER_HOLD_REBOOT_S (default 8 s, i.e. before
# whatever the PMIC's own long-press reset does) now asks systemd for a CLEAN
# reboot instead.  The short press keeps toggling the panel at once; the hold only
# adds the escape.
HOLD_REBOOT_DEFAULT_S = 8.0
REBOOT_CMD = ['systemctl', 'reboot']

# DISPLAY-PIPELINE ERRORS (2.22).  The kernel can lose the picture while the
# compositor stays awake, the CRTC reports "enabled" and the backlight is lit -
# measured 2026-10-22 16:40-16:41 (boot -1): press -> blank -> fold -> unfold,
# `on: done bl=823` from the real power path, gnome-shell relaunching DING on
# every unblank (its unblank path), `nvt_ts_panel_power_on` + a successful touch
# firmware reload on every press, and a BLACK screen - with NOT ONE kernel error.
# Whatever the kernel DOES print about the display is therefore worth keeping:
# these markers are logged and written to a marker file the diag tools read.
PIPELINE_ERRORS = (
    'dpu error',
    'frame done timeout',
    'dsi_cmds2buf_tx',
    'vblank timeout',
    'wait for commit done returned',
    'commit done failed',
    'failed to map buffer',
)
PIPELINE_MARKER = '/run/erhai-display-error'   # last error, for the diag tools


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


def conf_flag(key, env_key, default=True, conf=None):
    """Boolean knob: environment first, then /etc/erhai/display.conf.

    `conf` resolves to the module-level CONF at CALL time (not as a default
    argument, which would be bound at import and ignore a test's override).
    """
    conf = CONF if conf is None else conf
    env = os.environ.get(env_key)
    if env is not None and env.strip() != '':
        return env.strip().lower() not in ('0', 'no', 'off', 'false')
    try:
        with open(conf) as fh:
            for line in fh:
                line = line.split('#', 1)[0].strip()
                if line.startswith(key + '='):
                    v = line.split('=', 1)[1].strip().strip('"\'')
                    return v.lower() not in ('0', 'no', 'off', 'false')
    except OSError:
        pass
    return default


def guard_enabled():
    """PANEL_INPUT_GUARD from the environment, else /etc/erhai/display.conf."""
    return conf_flag('PANEL_INPUT_GUARD', 'ERHAI_PANEL_INPUT_GUARD')


def integrity_enabled():
    """BLANK_INTEGRITY=0 disables the re-assert of an externally undone blank."""
    return conf_flag('BLANK_INTEGRITY', 'ERHAI_BLANK_INTEGRITY')


def conf_number(key, env_key, default, conf=None):
    """Numeric knob: environment first, then /etc/erhai/display.conf."""
    conf = CONF if conf is None else conf
    raw = os.environ.get(env_key)
    if raw is None or raw.strip() == '':
        try:
            with open(conf) as fh:
                for line in fh:
                    line = line.split('#', 1)[0].strip()
                    if line.startswith(key + '='):
                        raw = line.split('=', 1)[1].strip().strip('"\'')
                        break
        except OSError:
            pass
    if raw is None or raw.strip() == '':
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def pipeline_error(text, patterns=PIPELINE_ERRORS):
    """The first display-pipeline marker in a kernel log line, or ''."""
    for p in patterns:
        if p in text:
            return p
    return ''


def _uptime_seconds():
    """Seconds since boot (None when unreadable): turns a kmsg record's µs
    timestamp into a wall-clock time."""
    try:
        with open('/proc/uptime') as fh:
            return float(fh.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


class PipelineWatch:
    """Record display-pipeline errors durably (see the constants above).

    The marker file (/run/erhai-display-error) is overwritten with the NEWEST
    error so `tmp/panel-blank-diag/grabstate.sh` (or a support thread) can see it
    without asking the user to grep the journal; the journal gets one line per
    `min_gap` seconds so a repeating error cannot flood it.
    """

    def __init__(self, marker=PIPELINE_MARKER, min_gap=5.0, now=None):
        self.marker = marker
        self.min_gap = min_gap
        self.now = time.monotonic if now is None else now
        self.last = None
        self.count = 0
        self.pending = b''

    def feed(self, line, when=None):
        """One kernel log line -> the matched marker (and a durable record).

        `when` is the wall-clock time the RECORD was made (decoded from the
        /dev/kmsg prefix by feed_chunk): a fresh open of /dev/kmsg replays the
        whole ring buffer, so without it the first hit after every daemon restart
        would look like it had just happened.
        """
        hit = pipeline_error(line)
        if not hit:
            return ''
        self.count += 1
        try:
            with open(self.marker, 'w') as fh:
                # TIMESTAMP FIRST: the hit can contain a space ("dpu error"), so
                # `<epoch> <hit> <line>` is the parseable order (the diag tools
                # read `read ts hit rest`).
                fh.write(f'{int(when if when is not None else time.time())} '
                         f'{hit} {line.strip()}\n')
        except OSError:
            pass
        if self.last is None or self.now() - self.last >= self.min_gap:
            self.last = self.now()
            log(f'display pipeline error #{self.count}: {hit} :: '
                f'{line.strip()[:160]}')
            survlog_nowait('powerwatch DISPLAY-ERROR %s' % hit)
        return hit

    def feed_chunk(self, data):
        """A /dev/kmsg chunk: split it into records (partial lines are kept)."""
        hits = []
        uptime = _uptime_seconds()
        self.pending += data
        while b'\n' in self.pending:
            raw, self.pending = self.pending.split(b'\n', 1)
            text = raw.decode('utf-8', 'replace')
            msg, when = text, None
            if ';' in text:
                msg = text.split(';', 1)[1]
                try:
                    # "<prio>,<seq>,<usec>,<flags>;<message>"
                    usec = int(text.split(';', 1)[0].split(',')[2])
                    if uptime is not None:
                        when = time.time() - (uptime - usec / 1e6)
                except (IndexError, ValueError):
                    when = None
            hit = self.feed(msg, when)
            if hit:
                hits.append(hit)
        return hits


def hold_reboot_seconds(conf=None):
    """POWER_HOLD_REBOOT_S (ERHAI_HOLD_REBOOT_S); 0 or negative disables it."""
    v = conf_number('POWER_HOLD_REBOOT_S', 'ERHAI_HOLD_REBOOT_S',
                    HOLD_REBOOT_DEFAULT_S, conf)
    return v if v > 0 else 0.0


def open_kmsg(path=KMSG):
    """Non-blocking fd for /dev/kmsg, or None (then that detector is off)."""
    try:
        return os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError as exc:
        log(f'blank integrity: cannot read {path}: {exc} (panel-on detector off)')
        return None


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

    def __init__(self, names=None, state_file=None, bl_glob=None):
        # The defaults resolve to the MODULE globals at CALL time: a default
        # argument is bound at class-definition time, which silently ignores a
        # caller (or a test harness) that overrides GUARD_NAMES/STATE/BL_GLOB.
        self.names = tuple(GUARD_NAMES if names is None else names)
        self.state_file = STATE if state_file is None else state_file
        self.bl_glob = BL_GLOB if bl_glob is None else bl_glob
        self.held = {}               # device name -> (fd, node)

    def blanked(self):
        """True while the power button holds the panel off.

        The state file says WHICH owner wants the panel off (2.20); the panel's own
        input nodes are suppressed whenever that file exists AND the panel is dark,
        which is the only state in which ghost input could reach a live session.
        """
        if not os.path.exists(self.state_file):
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


class BlankIntegrity:
    """Keep an externally undone blank undone.

    Two detectors, because the blank can be undone from two sides (both were
    live on boot 866f5cf7b7464aa7):

      * 'panel-on'      - the KERNEL enabled the panel while we hold the blank
                          (`nvt_ts_panel_power_on` in /dev/kmsg, i.e. a CRTC
                          enable the compositor never asked for);
      * 'session-awake' - the SESSION woke up while we hold the blank
                          (`org.gnome.ScreenSaver.ActiveChanged (false)`; an
                          input event woke it - the guard covers only the panel's
                          own nodes, NOT the pogo keyboard/touchpad).

    The action is `erhai-panel-power.sh reassert [--cycle]` and nothing else: the
    marker, the brightness MEMORY and the GPU park belong to the press that
    started the blank, so this must not touch them.
    """

    def __init__(self, guard, script=None, min_gap=INTEGRITY_MIN_GAP_S,
                 now=None, runner=None):
        # script/now resolve at CALL time from the module globals (see the guard
        # above): a default argument would bind TOGGLE - and time.monotonic - at
        # import and defeat every override.
        self.guard = guard
        self.script = list(TOGGLE if script is None else script)
        self.min_gap = min_gap
        self.now = time.monotonic if now is None else now
        self.runner = runner or self._run
        self.last = None             # monotonic time of the last action
        self.suppress_until = 0.0    # set by the daemon around its own runs
        self.actions = 0
        self.warned = False
        self.kmsg_pending = b''      # a record can be split across two reads

    def blanked(self):
        return bool(self.guard is not None and self.guard.blanked())

    def suppress(self, seconds):
        """Ignore the detectors for a while (a transition we started ourselves).

        A press runs `erhai-panel-power.sh on`, and for the ~0.2-0.5 s between
        its `SetActive false` and its `rm -f /run/erhai-panel.state` the panel is
        lit while the marker still says blanked — a panel-on record arriving in
        that window would otherwise start a re-assert in the middle of the
        restore and fight it.
        """
        self.suppress_until = self.now() + seconds

    def decide(self, kind):
        """kind -> argv to run, or None (not blanked / suppressed / rate limited)."""
        if not self.blanked():
            return None
        if self.now() < self.suppress_until:
            return None
        if self.last is not None and (self.now() - self.last) < self.min_gap:
            return None
        self.last = self.now()
        return self.script + ['reassert'] + (['--cycle'] if kind == 'panel-on' else [])

    def act(self, kind):
        cmd = self.decide(kind)
        if cmd is None:
            return None
        self.actions += 1
        log(f'blank integrity: {kind} while the blank is ours -> '
            f'{" ".join(cmd[-2:])} (action #{self.actions})')
        if self.actions >= INTEGRITY_WARN_AFTER and not self.warned:
            self.warned = True
            log('blank integrity: the blank keeps being undone - look for a stuck '
                'input device or a repeating panel re-enable')
        self.runner(cmd)
        return cmd

    def feed_kmsg(self, data):
        """Consume a /dev/kmsg chunk ('prio,seq,ts,flags;message') and act."""
        done = []
        self.kmsg_pending += data
        while b'\n' in self.kmsg_pending:
            raw, self.kmsg_pending = self.kmsg_pending.split(b'\n', 1)
            text = raw.decode('utf-8', 'replace')
            msg = text.split(';', 1)[1] if ';' in text else text
            if PANEL_ON_MARK in msg:
                cmd = self.act('panel-on')
                if cmd:
                    done.append(cmd)
        return done

    def feed_ss(self, line):
        """Consume one line of the ScreenSaver signal monitor."""
        if 'ActiveChanged (false' in line:
            return self.act('session-awake')
        return None

    def _run(self, cmd):
        try:
            subprocess.run(cmd, timeout=10)
        except Exception as exc:
            log(f'blank integrity: {cmd} failed: {exc}')


class PowerHold:
    """Press-and-hold on the power key -> a CLEAN reboot (the blind escape).

    A black screen with a live SoC had exactly one exit: a HARD reset, a power cut
    that loses the journal and every forensic marker.  Holding the key for
    POWER_HOLD_REBOOT_S (default 8 s) now asks systemd for a CLEAN reboot instead.

    The short press still toggles the panel AT ONCE - the hold only adds the
    escape, it never delays the normal press.  Details that matter:
      * the threshold is measured from the press event, so it works even if the
        driver never emits anything in between;
      * one escape per hold: the release re-arms it (and a release is what the
        driver sends when the key comes up);
      * the PMIC's own long-press reset (if any) fires LATER than this, so a clean
        shutdown gets its chance to start first;
      * `systemctl reboot` is logged, never retried - if the shutdown cannot
        complete, the user still has the hardware reset.
    """

    def __init__(self, seconds=None, cmd=None, now=None, runner=None):
        self.seconds = hold_reboot_seconds() if seconds is None else seconds
        self.cmd = list(REBOOT_CMD if cmd is None else cmd)
        self.now = time.monotonic if now is None else now
        self.runner = runner or self._run
        self.since = None
        self.fired = False

    def feed(self, etype, code, value):
        """Feed one input event; returns the argv when the hold fires, else None."""
        if etype != EV_KEY or code != KEY_POWER:
            return None
        if value == 1:
            if self.since is None:
                self.since = self.now()
            return None
        if value == 0:                    # released: re-arm
            self.since = None
            self.fired = False
        return None

    def check(self):
        """Called on every poll tick; fires once per hold."""
        if self.seconds <= 0 or self.since is None or self.fired:
            return None
        if self.now() - self.since < self.seconds:
            return None
        self.fired = True
        log(f'power key held {self.seconds:g}s -> CLEAN REBOOT '
            f'({" ".join(self.cmd)})')
        survlog_nowait('powerwatch HOLD-REBOOT %gs' % self.seconds)
        self.runner(self.cmd)
        return self.cmd

    def _run(self, cmd):
        try:
            subprocess.run(cmd, timeout=30)
        except Exception as exc:
            log(f'hold reboot failed ({cmd}): {exc}')


def _session_env(uid, rt):
    """env(1) arguments that point a client at the session bus."""
    return ['env', f'XDG_RUNTIME_DIR={rt}',
            f'DBUS_SESSION_BUS_ADDRESS=unix:path={rt}/bus']


def ss_get_active(uid=1000, gid=1000, rt=None):
    """True / False / None: the session's blank state, asked once."""
    rt = rt or f'/run/user/{uid}'
    if not os.path.exists(os.path.join(rt, 'bus')):
        return None
    cmd = ['setpriv', f'--reuid={uid}', f'--regid={gid}', '--init-groups'] \
        + _session_env(uid, rt) + ['timeout', '5', 'gdbus', 'call', '--session',
                                   '--dest', 'org.gnome.Shell', '--object-path',
                                   SS_PATH, '--method', f'{SS_DEST}.GetActive']
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=8)
    except Exception:
        return None
    if 'false' in out.stdout:
        return False
    if 'true' in out.stdout:
        return True
    return None


def ss_monitor(uid=1000, gid=1000, rt=None):
    """Start `gdbus monitor` for ActiveChanged; returns Popen or None."""
    rt = rt or f'/run/user/{uid}'
    if not os.path.exists(os.path.join(rt, 'bus')):
        log(f'blank integrity: no session bus at {rt} (session detector off)')
        return None
    cmd = ['setpriv', f'--reuid={uid}', f'--regid={gid}', '--init-groups'] \
        + _session_env(uid, rt) + ['timeout', '86400', 'gdbus', 'monitor',
                                   '--session', '--dest', SS_DEST,
                                   '--object-path', SS_PATH]
    try:
        return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
    except Exception as exc:
        log(f'blank integrity: cannot start the session monitor: {exc}')
        return None


SESSION_USER = os.environ.get('ERHAI_SESSION_USER', 'themojoman')


def session_ids(user=SESSION_USER):
    """(uid, gid) of the desktop user, the same default the panel script uses."""
    try:
        import pwd
        p = pwd.getpwnam(user)
        return p.pw_uid, p.pw_gid
    except Exception:
        return 1000, 1000


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='erhai-power-watch.py',
        description='PMIC power key -> real panel power cycle, plus the panel '
                    'input guard (a blanked panel stays dark and deaf), the '
                    'blank integrity monitor (a blank something else undid is '
                    'put back) and the press-and-hold escape (a CLEAN reboot).')
    ap.add_argument('--no-guard', action='store_true',
                    help='do not suppress the panel input while blanked')
    ap.add_argument('--no-integrity', action='store_true',
                    help='do not re-assert a blank something else undid')
    ap.add_argument('--pwrkey', default=None, metavar='DEV',
                    help='power key event device (default: the pmic_pwrkey node)')
    ap.add_argument('--panel-cmd', default=None, metavar='CMD',
                    help='override the panel script (used by tests)')
    ap.add_argument('--reboot-cmd', default=None, metavar='CMD',
                    help='what the hold escape runs (default: systemctl reboot)')
    ap.add_argument('--hold-seconds', type=float, default=None, metavar='S',
                    help='press-and-hold reboot threshold (0 disables; default '
                         'POWER_HOLD_REBOOT_S or %g s)' % HOLD_REBOOT_DEFAULT_S)
    a = ap.parse_args(argv)

    # A DEFAULT SIGTERM exits the interpreter WITHOUT unwinding, i.e. without
    # running the finally that reaps the `gdbus monitor` child - measured
    # 2026-10-22: an orphaned `timeout 86400 gdbus monitor` pair survived a
    # killed daemon.  Turn the stop signals into a clean exit instead.  (For the
    # service systemd also kills the whole cgroup, so this is the belt to that
    # pair of braces.)
    def _bail(*_a):
        sys.exit(0)

    for _sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        try:
            signal.signal(_sig, _bail)
        except (OSError, ValueError):
            pass

    pipeline = PipelineWatch()
    guard = None if (a.no_guard or not guard_enabled()) else PanelInputGuard()
    panel_cmd = shlex.split(a.panel_cmd) if a.panel_cmd else list(TOGGLE)
    hold = PowerHold(seconds=a.hold_seconds,
                     cmd=shlex.split(a.reboot_cmd) if a.reboot_cmd else None)

    dev = a.pwrkey or find_pwrkey()
    fmt = 'llHHi'
    size = struct.calcsize(fmt)
    print(f'erhai-power-watch: reading {dev} (record {size}B)', flush=True)
    print('erhai-power-watch: press-and-hold escape %s'
          % (f'at {hold.seconds:g}s -> {" ".join(hold.cmd)}'
             if hold.seconds > 0 else 'DISABLED'), flush=True)
    if guard is None:
        print('erhai-power-watch: panel input guard DISABLED', flush=True)
    else:
        guard.sync()

    # -- blank integrity -----------------------------------------------------
    integrity = None
    kmsg_fd = None
    mon = None
    mon_buf = b''
    uid, gid = session_ids()
    if guard is None or a.no_integrity or not integrity_enabled():
        print('erhai-power-watch: blank integrity DISABLED', flush=True)
    else:
        integrity = BlankIntegrity(guard)
        kmsg_fd = open_kmsg()
        # The session monitor is started here and then kept alive by the retry
        # block in the loop (this service starts around multi-user.target, i.e.
        # possibly before the user session bus exists at all).
        if ss_get_active(uid, gid) is False:
            # The session was already awake while the marker says blanked (a
            # daemon restart in the middle of a broken blank): fix it at once.
            integrity.act('session-awake')
        mon = ss_monitor(uid, gid)
        print('erhai-power-watch: blank integrity ON (kmsg=%s, session monitor=%s)'
              % (f'fd {kmsg_fd}' if kmsg_fd is not None else 'off',
                 f'pid {mon.pid}' if mon else 'retrying'), flush=True)

    def pump_ss():
        """Drain the session monitor; the retry loop restarts it if it ends."""
        nonlocal mon, mon_buf, mon_tries
        try:
            chunk = os.read(mon.stdout.fileno(), 4096)
        except OSError:
            chunk = b''
        if not chunk:
            log('blank integrity: the session monitor ended')
            try:
                mon.wait(timeout=1)
            except Exception:
                pass
            mon = None
            mon_buf = b''
            return
        mon_buf += chunk
        while b'\n' in mon_buf:
            raw, mon_buf = mon_buf.split(b'\n', 1)
            integrity.feed_ss(raw.decode('utf-8', 'replace'))

    stop = False
    next_mon = 0.0        # the first (re)check happens at once
    mon_tries = 0
    try:
        with open(dev, 'rb', buffering=0) as f:
            while not stop:
                if integrity is not None and mon is None and time.monotonic() >= next_mon:
                    # This service starts around multi-user.target, i.e. possibly
                    # BEFORE the user session bus exists, and a logout/login
                    # replaces that bus - so keep looking for it (bounded, and
                    # the budget is refunded by every successful start).  The
                    # session detector is what catches "an ungrabbed input device
                    # woke the desktop behind the blank".
                    next_mon = time.monotonic() + MON_RETRY_S
                    if mon_tries < MON_MAX_TRIES:
                        mon_tries += 1
                        if ss_get_active(uid, gid) is False:
                            integrity.act('session-awake')
                        mon = ss_monitor(uid, gid)
                        if mon is not None:
                            mon_tries = 0
                watch = [f]
                if kmsg_fd is not None:
                    watch.append(kmsg_fd)
                if mon is not None and mon.stdout is not None:
                    watch.append(mon.stdout)
                try:
                    ready, _, _ = select.select(watch, [], [], GUARD_POLL_S)
                except InterruptedError:
                    continue
                except OSError:
                    break
                for r in ready:
                    if r is f:
                        data = f.read(size)
                        if not data or len(data) < size:
                            stop = True
                            break
                        _s, _u, etype, code, value = struct.unpack(fmt, data)
                        hold.feed(etype, code, value)
                        if etype == EV_KEY and code == KEY_POWER and value == 1:
                            print('KEY_POWER press -> toggle', flush=True)
                            survlog_nowait('powerwatch KEY_POWER ts=%d'
                                           % int(time.time()))
                            if integrity is not None:
                                # A press is OUR transition: ignore the detectors
                                # for its duration (the lit-panel-with-state window
                                # would otherwise start a re-assert), and protect a
                                # fresh blank the moment it exists.
                                integrity.suppress(INTEGRITY_PRESS_SUPPRESS_S)
                                integrity.last = None
                            run_toggle(panel_cmd, guard)
                    elif kmsg_fd is not None and r == kmsg_fd:
                        try:
                            chunk = os.read(kmsg_fd, 65536)
                        except OSError:
                            chunk = b''
                        if chunk:
                            # every kernel line is scanned for display-pipeline
                            # errors (2.22) before the integrity monitor sees the
                            # chunk: the picture can vanish with NO error logged,
                            # so the ones that DO appear must be kept
                            pipeline.feed_chunk(chunk)
                            integrity.feed_kmsg(chunk)
                    elif mon is not None and mon.stdout is not None and r is mon.stdout:
                        pump_ss()
                if guard is not None:
                    guard.sync()
                # the press-and-hold escape is measured on this tick (one second)
                hold.check()
    finally:
        # The monitor is a `timeout 86400 gdbus monitor` child: never leave it
        # behind on a daemon restart (Restart=always).
        if mon is not None:
            try:
                mon.terminate()
            except Exception:
                pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
