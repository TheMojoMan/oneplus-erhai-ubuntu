# SPDX-License-Identifier: GPL-2.0-only
"""Sessions: one per camera node an app is streaming, AE on its sensor.

    IDLE --in use, gates pass--> ATTACHING (sampler up, 5 s first-frame deadline)
      ATTACHING --first frame (reader): chip ID, start state, 4x gain--> RUNNING
      RUNNING --tick/step/write per analysed frame (reader)--> RUNNING
      any --idle | node gone | fault | death | timeout | shutdown--> ENDED (main)
      ENDED --start/sensor/chip/exception fault--> LATCHED until the app stops
      ENDED --sampler death / no frames--> BACKOFF 10 -> 20 -> 40 -> 60 s
      foreign exposure writer --> AE off process-wide until restart

Threads: the reader thread (sampler) runs on_frame; it never ends its own
session -- it records session.fault and wakes the main loop through a
self-pipe. Only the main thread starts/ends sessions and stops samplers.
A per-sensor process-global lock serialises all register I/O on a sensor.
"""
import json
import os
import threading

from dataclasses import replace

from .controller import (SETTLE, SetExposure, SetFrame, State, ae_max,
                         max_lines, step, tick)
from .focus import Autofocus, LensError
from .sensor import SPECS, SensorError

ATTACH_TIMEOUT = 5.0
BACKOFF = (10.0, 20.0, 40.0, 60.0)
START_TRIES = 3
START_SPACING = 1.0
STATUS_EVERY = 30.0
MAX_REATTACH = 2           # stream restarts tolerated per session
FROZEN = 'exposure frozen'

_LOCKS = {}
_LOCKS_GUARD = threading.Lock()


def sensor_lock(kind):
    with _LOCKS_GUARD:
        return _LOCKS.setdefault(kind, threading.Lock())


class Session:
    def __init__(self, engine, node, spec, sensor):
        self.e = engine
        self.node_id = node.id
        self.kind = node.kind
        self.spec = spec
        self.sensor = sensor
        self.lock = sensor_lock(node.kind)
        self.phase = 'attaching'
        self.state = None
        self.stopped = False
        self.fault = None
        self.sampler = None
        self.started_at = engine.clock()
        self.tries = 0
        self.last_try = None
        self.exp_written = None
        self.vts_written = None
        self.mismatch = 0
        self.last_status = None
        self.first_frame_at = None
        self.reattaches = 0
        self.af = None            # Autofocus, rear only
        self.lens = None

    # -- reader thread -------------------------------------------------
    def on_frame(self, get_stats, get_sharp=None):
        try:
            self._on_frame(get_stats, get_sharp)
        except Exception as e:                     # never kill the reader
            self._set_fault('exception', e)

    def _set_fault(self, reason, err):
        if self.fault is None:
            self.fault = (reason, err)
        self.e.wake()

    def _on_frame(self, get_stats, get_sharp=None):
        with self.lock:
            if self.stopped or self.fault:
                return
            if self.first_frame_at is None:
                self.first_frame_at = self.e.clock()
            if self.phase == 'attaching':
                self._start()
                if self.phase == 'running' and self.af is not None:
                    self._af_start()
                return
            if self.af is not None and get_sharp is not None:
                self._af_frame(get_sharp)
            self.state, go = tick(self.state)
            if not go:
                return
        luma, hi, _lo = get_stats()                # outside the lock
        with self.lock:
            if self.stopped or self.fault:
                return
            try:
                self._analyse(luma, hi)
            except SensorError as err:
                # A NAK mid-stream is usually the app restarting the camera
                # (sensor powers down / soft-resets): start over, don't latch.
                self._reattach(f'I2C error at 0x{err.reg:04x} '
                               f'(errno {err.errno})')

    def _reattach(self, why):
        """The sensor was reset under us (a stream restart the graph may not
        show): run the start sequence again, keeping our last values."""
        self.reattaches += 1
        if self.reattaches > MAX_REATTACH:
            self._set_fault('sensor', f'{why}; gave up after '
                            f'{MAX_REATTACH} re-attaches')
            return
        if self.exp_written is not None and self.vts_written is not None:
            self.e.remembered[self.kind] = (self.exp_written, self.vts_written)
        self.phase = 'attaching'
        self.tries = 0
        self.last_try = None
        self.mismatch = 0
        self.e.log('info', f'{self.spec.name}: {why}; re-attaching')

    def _start(self):
        now = self.e.clock()
        if self.last_try is not None and now - self.last_try < START_SPACING:
            return
        self.last_try = now
        self.tries += 1
        lim = self.spec.limits
        try:
            if not self.sensor.chip_ok():
                self._set_fault('chip', f'chip ID is not 0x{self.spec.chip_id:04x}')
                return
            raw_exp = self.sensor.read_exposure()
            raw_vts = self.sensor.read_vts()
            self.exp_written, self.vts_written = raw_exp, raw_vts
            # The cached V4L2 controls the kernel restores at stream start are
            # stale (another client's last values -- seen: exposure 1), so
            # never start from them: the last settled values for this sensor,
            # else ErhaiCam's own start, the exposure ceiling at the base frame.
            exp, vts = self.e.remembered.get(
                self.kind, (ae_max(lim, lim.vts_default), lim.vts_default))
            if not (lim.vts_default <= vts <= lim.vts_max
                    and 0 < exp <= max_lines(vts)):
                exp, vts = ae_max(lim, lim.vts_default), lim.vts_default
            self._write_frame(vts, exp)
            self.sensor.write_gain_4x()
        except SensorError as err:
            if self.tries >= START_TRIES:
                self._set_fault('start', err)
            return
        self.state = State(exp, vts)
        self.phase = 'running'
        self.last_status = now
        self.e.log('info', f'start node={self.node_id} sensor={self.spec.name} '
                   f'exposure={exp} vts={vts}')

    # -- autofocus (rear) --------------------------------------------------
    def _af_lens(self, pos):
        try:
            self.lens.set(pos)
        except LensError as err:
            self.af = None
            self.e.log('warning', f'{self.spec.name}: autofocus off for this '
                       f'stream: lens error ({err.strerror}); AE continues')
            return False
        return True

    def _af_start(self):
        if self._af_lens(self.af.start(self.lens.position)):
            self._af_short_frame()

    def _af_short_frame(self):
        """A sweep wants the base frame rate (ErhaiCam start_sweep)."""
        lim = self.spec.limits
        if self.state.vts > lim.vts_default:
            exp = max_lines(lim.vts_default)
            self._write_frame(lim.vts_default, exp)
            self.state = replace(self.state, exposure=exp,
                                 vts=lim.vts_default, settle=SETTLE)
            self.e.log('info', f'{self.spec.name}: autofocus: base frame '
                       f'{lim.vts_default} lines for the sweep, exposure {exp}')

    def _af_frame(self, get_sharp):
        was = self.af.mode
        move = self.af.on_frame(self.e.clock(), get_sharp)
        for ev in self.af.drain_events():
            self.e.log('info', f'{self.spec.name}: af: {ev}')
        if move is not None and self._af_lens(move):
            if was != 'sweep' and self.af.mode == 'sweep':
                self._af_short_frame()

    def _write_frame(self, vts, exp):
        """Write whatever differs; never let exposure exceed the frame."""
        def w_exp():
            if exp != self.exp_written:
                self.sensor.write_exposure(exp)
                self.exp_written = exp

        def w_vts():
            if vts != self.vts_written:
                self.sensor.write_vts(vts)
                self.vts_written = vts

        if self.vts_written is not None and vts < self.vts_written:
            w_exp(); w_vts()                       # shortening: exposure first
        else:
            w_vts(); w_exp()

    def _analyse(self, luma, hi):
        exp = self.sensor.read_exposure()          # exclusive-ownership check
        if exp != self.exp_written:
            self.mismatch += 1
            if self.mismatch >= 2:
                if self.reattaches < MAX_REATTACH:
                    self._reattach(f'exposure reads {exp}, wrote '
                                   f'{self.exp_written} (stream restart?)')
                else:
                    self._set_fault('foreign', f'exposure reads {exp}, '
                                    f'wrote {self.exp_written}')
            return
        self.mismatch = 0
        sweeping = self.af is not None and self.af.wants_short_frames
        new, action = step(self.state, self.spec.limits, luma, hi, self.e.target,
                           short_frames=sweeping)
        if isinstance(action, SetFrame):
            self._write_frame(action.vts, action.exposure)
            self.e.log('info', f'{self.spec.name}: frame {action.vts} lines, '
                       f'exposure {action.exposure}')
        elif isinstance(action, SetExposure):
            self._write_frame(self.vts_written, action.lines)
        self.state = new
        now = self.e.clock()
        if now - self.last_status >= STATUS_EVERY:
            self.last_status = now
            self.e.log('info', f'{self.spec.name}: luma={luma:.3f} '
                       f'exposure={new.exposure} vts={new.vts} '
                       f'at_limit={new.at_limit}')

    # -- main thread -----------------------------------------------------
    def restore(self):
        """Back to the default frame length, exposure clamped first."""
        lim = self.spec.limits
        if self.vts_written == lim.vts_default:
            return
        top = ae_max(lim, lim.vts_default)
        try:
            if self.exp_written is None or self.exp_written > top:
                self.sensor.write_exposure(top)
                self.exp_written = top
            self.sensor.write_vts(lim.vts_default)
            self.vts_written = lim.vts_default
        except SensorError as err:
            self.e.log('warning', f'{self.spec.name}: restore failed at '
                       f'0x{err.reg:04x} (errno {err.errno}); {FROZEN}')


class Engine:
    def __init__(self, sampler_factory, sensor_factory, override_fn, log,
                 clock, ae_enabled, target, af_enabled=True, lens_factory=None):
        self.sampler_factory = sampler_factory
        self.sensor_factory = sensor_factory
        self.override_fn = override_fn
        self.log = log
        self.clock = clock
        self.ae_enabled = ae_enabled
        self.target = target
        self.af_enabled = af_enabled
        self.lens_factory = lens_factory
        self.remember_path = None
        self.sessions = {}
        self.blocked = {}          # node id -> reason, until the app stops
        self.backoff = {}          # node id -> (until, step index)
        self.remembered = {}       # kind -> (exposure, vts) last written
        self.foreign_latched = False
        self._dying = {}           # kind -> sampler whose reader may still run
        self._once = set()
        self._last = {}
        self.wake_fd, self._wake_w = os.pipe()
        os.set_blocking(self.wake_fd, False)
        os.set_blocking(self._wake_w, False)

    def load_remembered(self):
        try:
            with open(self.remember_path) as f:
                data = json.load(f)
        except (OSError, TypeError, ValueError):
            return
        for kind, pair in data.items():
            if kind in SPECS and isinstance(pair, list) and len(pair) == 2:
                self.remembered[kind] = (int(pair[0]), int(pair[1]))

    def _save_remembered(self):
        if not self.remember_path:
            return
        tmp = self.remember_path + '.tmp'
        try:
            os.makedirs(os.path.dirname(self.remember_path), exist_ok=True)
            with open(tmp, 'w') as f:
                json.dump({k: list(v) for k, v in self.remembered.items()}, f)
            os.replace(tmp, self.remember_path)
        except OSError as e:
            self.log('info', f'could not save {self.remember_path}: {e}')

    def wake(self):
        try:
            os.write(self._wake_w, b'x')
        except OSError:
            pass                                   # pipe full: already woken

    def own_pids(self):
        return {s.sampler.pid for s in self.sessions.values()
                if s.sampler is not None and s.sampler.pid is not None}

    def _log_once(self, key, level, msg):
        if key not in self._once:
            self._once.add(key)
            self.log(level, msg)

    def evaluate(self, cameras):
        self._last = cameras
        self.handle_faults()
        now = self.clock()
        for nid, s in list(self.sessions.items()):
            node = cameras.get(nid)
            if node is None:
                self.end(s, 'node gone', gone=True)
            elif not node.in_use:
                self.end(s, 'idle', in_use=False)
            elif (s.phase == 'attaching' and s.first_frame_at is None
                  and now - s.started_at > ATTACH_TIMEOUT):
                self.end(s, 'no frames', in_use=True)
                self._backoff(nid)
                self.log('warning', f'no frames from node {nid} '
                         f'({s.spec.name}); retrying later; {FROZEN}')
            elif not s.sampler.alive and not s.sampler.stopped_by_us:
                self.end(s, 'sampler died', in_use=True)
                self._backoff(nid)
                self.log('warning', f'sampler for node {nid} ({s.spec.name}) '
                         f'died; retrying later; {FROZEN}')
        for nid, node in cameras.items():
            if not node.in_use:
                self.blocked.pop(nid, None)
                self.backoff.pop(nid, None)
                continue
            if nid in self.sessions or nid in self.blocked:
                continue
            b = self.backoff.get(nid)
            if b is not None and now < b[0]:
                continue
            self._maybe_start(node)
        for d in (self.blocked, self.backoff):
            for nid in [n for n in d if n not in cameras]:
                d.pop(nid)

    def _backoff(self, nid):
        idx = self.backoff.get(nid, (0, 0))[1]
        self.backoff[nid] = (self.clock() + BACKOFF[min(idx, len(BACKOFF) - 1)],
                             idx + 1)

    def _maybe_start(self, node):
        if not self.ae_enabled or self.foreign_latched:
            return
        if not self.override_fn():
            return
        if node.kind is None:
            self.blocked[node.id] = 'unknown'
            self._log_once(('unknown', node.id), 'warning',
                           f'unknown camera node {node.id} ({node.name}): '
                           f'no AE; {FROZEN}')
            return
        prev = self._dying.get(node.kind)
        if prev is not None and prev.reader_alive:
            return                                 # old reader not gone yet
        self._dying.pop(node.kind, None)
        spec = SPECS[node.kind]
        sensor = self.sensor_factory(node.kind)
        if sensor is None:
            self.blocked[node.id] = 'no bus'
            self._log_once(('nobus', node.kind), 'warning',
                           f'{spec.name} not found on any I2C bus: no AE; {FROZEN}')
            return
        if not sensor.writable():
            self.blocked[node.id] = 'unwritable'
            self._log_once(('unwritable', node.kind), 'warning',
                           f'cannot write /dev/i2c-{sensor.bus} for {spec.name} '
                           f'(check 70-erhai-cam-i2c.rules / group i2c): '
                           f'no AE; {FROZEN}')
            return
        s = Session(self, node, spec, sensor)
        if node.kind == 'rear' and self.af_enabled and self.lens_factory:
            try:
                lens = self.lens_factory()
                if lens is None:
                    self._log_once(('nolens',), 'info',
                                   'no cn3927 lens subdev found; no autofocus')
            except LensError as err:
                lens = None
                self._log_once(('lens', err.errno), 'warning',
                               f'cannot open the cn3927 lens: {err.strerror}; '
                               f'no autofocus')
            if lens is not None:
                s.lens, s.af = lens, Autofocus()
        try:
            s.sampler = self.sampler_factory(node.serial, s.on_frame)
        except Exception as e:                     # gst/setpriv missing, EMFILE
            self._backoff(node.id)
            self._log_once(('spawn', node.kind), 'warning',
                           f'cannot start the frame reader for {spec.name}: '
                           f'{e}; retrying later; {FROZEN}')
            return
        self.sessions[node.id] = s

    def handle_faults(self):
        try:
            while os.read(self.wake_fd, 4096):
                pass
        except BlockingIOError:
            pass
        for s in list(self.sessions.values()):
            if s.fault is None:
                continue
            reason, err = s.fault
            node = self._last.get(s.node_id)
            if not (node and node.in_use):
                self.end(s, f'{reason} after the app stopped')   # routine close
                continue
            if reason == 'foreign':
                self.end(s, reason, in_use=True, restore=False)
                self.foreign_latched = True
                self.log('warning', f'foreign exposure writer on {s.spec.name} '
                         f'({err}) -- libcamera AGC active? AE off until '
                         f'restart; {FROZEN}')
                continue
            self.end(s, reason, in_use=True)
            self.blocked[s.node_id] = reason
            what = 'start failed' if reason == 'start' else f'{reason} fault'
            self.log('warning', f'{s.spec.name}: {what}: {err}; AE off until '
                     f'the app stops the camera; {FROZEN}')

    def end(self, s, reason, gone=False, in_use=None, restore=True):
        if in_use is None:
            node = self._last.get(s.node_id)
            in_use = bool(node and node.in_use)
        with s.lock:
            if s.stopped:
                return
            s.stopped = True
            if s.phase == 'running':
                if not gone and in_use and restore:
                    s.restore()
                if s.exp_written is not None:
                    self.remembered[s.kind] = (s.exp_written, s.vts_written)
                    self._save_remembered()
        s.sampler.stop()
        if s.lens is not None:
            s.lens.close()
        if s.sampler.reader_alive:
            self._dying[s.kind] = s.sampler
        self.sessions.pop(s.node_id, None)
        self.log('info', f'stop node={s.node_id} sensor={s.spec.name} '
                 f'reason={reason}')

    def shutdown(self):
        for s in list(self.sessions.values()):
            self.end(s, 'shutdown')
