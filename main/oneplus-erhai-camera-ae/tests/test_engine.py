import os
import select
import threading
import unittest

from dataclasses import replace

from camera_ae.controller import State
from camera_ae.engine import Engine
from camera_ae.graph import CameraNode
from camera_ae.main import ae_enabled_from_env
from camera_ae.sensor import SPECS, Sensor
from tests.fakes import FakeClock, FakeSampler, FakeXfer, regs

FRONT_ID, REAR_ID = 33, 76


class Harness:
    def __init__(self, front=(1656, 3128), rear=(2389, 6404), override=True,
                 ae=True, writable=True, front_chip=0xd154, seed=False):
        self.clock = FakeClock()
        self.logs = []
        self.samplers = []
        self.factory_calls = []
        self.xfers = {'front': FakeXfer(regs(front_chip, *front)),
                      'rear': FakeXfer(regs(0xc658, *rear))}
        self.writable = writable

        def sensor_factory(kind):
            self.factory_calls.append(kind)
            s = Sensor(SPECS[kind], 7 if kind == 'front' else 12,
                       xfer=self.xfers[kind])
            s.writable = lambda: self.writable
            return s

        def sampler_factory(serial, on_frame):
            s = FakeSampler(serial, on_frame)
            self.samplers.append(s)
            return s

        self.engine = Engine(sampler_factory, sensor_factory, lambda: override,
                             lambda lvl, msg: self.logs.append((lvl, msg)),
                             self.clock, ae, 0.45)
        if seed:            # start sessions at the planted register values
            self.engine.remembered = {'front': tuple(front), 'rear': tuple(rear)}

    def cams(self, front=False, rear=False, front_present=True, extra=None):
        c = {}
        if front_present:
            c[FRONT_ID] = CameraNode(FRONT_ID, 82, 'x_camera-sensor_10', 'front', front)
        c[REAR_ID] = CameraNode(REAR_ID, 84, 'x_camera-sensor_36', 'rear', rear)
        if extra:
            c[extra.id] = extra
        return c

    def eval(self, **kw):
        self.engine.evaluate(self.cams(**kw))

    def start_front(self):
        self.eval(front=True)
        s = self.samplers[-1]
        s.deliver()                                 # first frame: start
        return s

    def analyse(self, sampler, luma, hi=0.0, n=1):
        """Deliver frames until n more analysed frames happened."""
        target = sampler.stats_calls + n
        for _ in range(100):
            if sampler.stats_calls >= target:
                return
            sampler.deliver(luma, hi)
        raise AssertionError('no analysed frame')

    def session(self, nid=FRONT_ID):
        return self.engine.sessions.get(nid)

    def long_frame(self, exp, vts, nid=FRONT_ID):
        """Put a running session in a long frame, as the ladder writes it.
        Sessions always start at the base frame, so tests that need a
        long frame set it up here, after the start."""
        sess = self.session(nid)
        sess._write_frame(vts, exp)
        sess.state = replace(sess.state, exposure=exp, vts=vts)

    def writes(self, kind='front'):
        return self.xfers[kind].writes()

    def lines(self, needle):
        return [m for _, m in self.logs if needle in m]


EXP = lambda n: ('0x3e00', (n << 5).to_bytes(3, 'big').hex(' '))
VTS = lambda n: ('0x320e', n.to_bytes(2, 'big').hex(' '))
GAIN_FRONT = ('0x3e07', '80 0f')


class Start(unittest.TestCase):
    def test_engine_starts_from_sensor_exposure(self):
        h = Harness(front=(0, 40000))
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)
        self.assertEqual(h.xfers['front'].log, [])
        self.assertEqual(h.xfers['front'].reads, [])   # no I2C before frame 1
        h.samplers[0].deliver()
        self.assertEqual(h.writes(), [EXP(3120), VTS(3128), GAIN_FRONT])
        self.assertEqual(h.session().state, State(3120, 3128))
        h2 = Harness(front=(1656, 3128))               # nothing remembered:
        h2.start_front()                                # ErhaiCam's ae_max start
        self.assertEqual(h2.writes(), [EXP(3120), GAIN_FRONT])
        self.assertEqual(h2.session().state.exposure, 3120)

    def test_start_uses_remembered_exposure(self):
        h = Harness(front=(1656, 3128))
        s = h.start_front()
        h.analyse(s, 0.9)
        converged = h.session().state.exposure
        self.assertNotEqual(converged, 3120)
        h.eval(front=False)
        h.xfers['front'].poke(0x3e00, (1656 << 5).to_bytes(3, 'big'))
        h.xfers['front'].log.clear()
        h.start_front()
        self.assertEqual(h.writes(), [EXP(converged), GAIN_FRONT])

    def test_start_resets_remembered_long_frame(self):
        # ErhaiCam (erhai-cam.cpp stream start): the frame goes back to the
        # base length; the ladder re-escalates by itself if still dark.  A
        # remembered long frame must not carry over (Zen at ~6 fps).
        h = Harness(front=(3133, 12512))
        h.engine.remembered = {'front': (2735, 12512)}
        h.start_front()
        self.assertEqual(h.session().state, State(2735, 3128))
        self.assertEqual(h.writes(), [EXP(2735), VTS(3128), GAIN_FRONT])
        h2 = Harness(front=(3133, 12512))
        h2.engine.remembered = {'front': (9000, 12512)}   # clamped to ae_max
        h2.start_front()
        self.assertEqual(h2.session().state, State(3120, 3128))

    def test_start_nak_retries(self):
        h = Harness()
        h.eval(front=True)
        s = h.samplers[0]
        h.xfers['front'].nak(2)
        s.deliver()                       # try 1: NAK
        s.deliver()                       # < 1 s: no try
        h.clock.advance(1.0)
        s.deliver()                       # try 2: NAK
        h.clock.advance(1.0)
        s.deliver()                       # try 3: ok
        self.assertEqual(h.session().phase, 'running')
        h2 = Harness()
        h2.eval(front=True)
        s2 = h2.samplers[0]
        h2.xfers['front'].nak(3)
        for _ in range(3):
            s2.deliver()
            h2.clock.advance(1.0)
        h2.engine.handle_faults()
        self.assertIsNone(h2.session())
        self.assertEqual(len(h2.lines('start failed')), 1)
        self.assertEqual(h2.xfers['front'].log, [])

    def test_chip_mismatch_latched(self):
        h = Harness(front_chip=0xc658)
        h.start_front()
        h.engine.handle_faults()
        self.assertIsNone(h.session())
        self.assertEqual(h.xfers['front'].log, [])
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)
        self.assertTrue(h.lines('chip'))


class Policy(unittest.TestCase):
    def latched(self):
        h = Harness()
        h.eval(front=True)
        h.xfers['front'].nak(3)
        for _ in range(3):
            h.samplers[0].deliver()
            h.clock.advance(1.0)
        h.eval(front=True)
        return h

    def test_failed_session_not_restarted_until_idle(self):
        h = self.latched()
        for _ in range(5):
            h.clock.advance(1.0)
            h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)
        h.eval(front=False)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 2)

    def test_sampler_death_backoff(self):
        h = Harness()
        h.start_front()
        h.samplers[0].die()
        h.eval(front=True)
        self.assertIsNone(h.session())
        h.clock.advance(5)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)
        h.clock.advance(5.1)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 2)
        h.samplers[1].die()
        h.eval(front=True)
        h.clock.advance(15)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 2)
        h.clock.advance(5.1)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 3)

    def test_attach_timeout(self):
        h = Harness()
        h.eval(front=True)
        h.clock.advance(5.1)
        h.eval(front=True)
        self.assertIsNone(h.session())
        self.assertTrue(h.samplers[0].stopped_by_us)
        self.assertTrue(h.lines('no frames'))

    def test_no_overlap_same_sensor(self):
        h = Harness()
        h.start_front()
        h.samplers[0].reader_alive = True
        h.eval(front=False)
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)
        h.samplers[0].reader_alive = False
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 2)


class Threads(unittest.TestCase):
    def in_thread(self, fn):
        t = threading.Thread(target=fn)
        t.start()
        t.join(5)
        self.assertFalse(t.is_alive())

    def test_fault_in_reader_ends_on_main_thread(self):
        h = Harness()
        h.eval(front=True)
        s = h.samplers[0]
        h.xfers['front'].nak(3)

        def reader():
            for _ in range(3):
                s.deliver()
                h.clock.advance(1.0)
        self.in_thread(reader)
        self.assertIsNotNone(h.session())                  # not ended there
        self.assertIsNotNone(h.session().fault)
        r, _, _ = select.select([h.engine.wake_fd], [], [], 0)
        self.assertTrue(r)                                 # main was woken
        h.engine.handle_faults()
        self.assertIsNone(h.session())
        self.assertIs(s.stop_thread, threading.main_thread())

    def test_start_failure_no_self_join(self):
        h = Harness()
        h.eval(front=True)
        s = h.samplers[0]
        h.xfers['front'].nak(3)

        def reader():
            for _ in range(3):
                s.deliver()
                h.clock.advance(1.0)
        self.in_thread(reader)
        self.assertFalse(s.stopped_by_us)
        h.engine.handle_faults()
        self.assertIs(s.stop_thread, threading.main_thread())

    def test_no_io_after_end(self):
        h = Harness()
        s = h.start_front()
        h.eval(front=False)
        x = h.xfers['front']
        before = (len(x.log), len(x.reads))
        s.deliver(0.9, n=12)
        self.assertEqual((len(x.log), len(x.reads)), before)

    def test_on_frame_exception_ends_session(self):
        h = Harness()
        s = h.start_front()

        def boom():
            raise ZeroDivisionError('bad frame')
        for _ in range(9):
            s.on_frame(boom)
        h.engine.handle_faults()
        self.assertIsNone(h.session())
        self.assertTrue(h.lines('bad frame'))
        h.eval(front=True)
        self.assertEqual(len(h.samplers), 1)               # latched


class Running(unittest.TestCase):
    def test_foreign_writer_disables_ae(self):
        h = Harness()
        s = h.start_front()
        x = h.xfers['front']
        for i in range(3):                       # 2 re-attaches, then foreign
            for v in (900 + i, 950 + i):
                h.analyse(s, 0.45)
                x.poke(0x3e00, (v << 5).to_bytes(3, 'big'))
            h.analyse(s, 0.45)
            if h.session().phase == 'attaching':
                s.deliver()
        n = len(x.log)
        h.engine.handle_faults()
        self.assertIsNone(h.session())
        self.assertEqual(len(x.log), n)                     # no restore writes
        self.assertTrue(h.engine.foreign_latched)
        warn = [m for lvl, m in h.logs if lvl == 'warning' and 'foreign' in m]
        self.assertEqual(len(warn), 1)
        h.eval(front=True, rear=True)
        self.assertEqual(len(h.samplers), 1)

    def test_single_mismatch_tolerated(self):
        h = Harness()
        s = h.start_front()
        h.analyse(s, 0.45)
        h.xfers['front'].poke(0x3e00, (999 << 5).to_bytes(3, 'big'))
        h.analyse(s, 0.45)
        h.xfers['front'].poke(0x3e00, (1656 << 5).to_bytes(3, 'big'))
        h.analyse(s, 0.45)
        self.assertIsNone(h.session().fault)

    def test_setframe_write_order(self):
        h = Harness(front=(3120, 3128), seed=True)
        s = h.start_front()
        h.xfers['front'].log.clear()
        h.analyse(s, 0.1)
        self.assertEqual(h.writes(), [VTS(6256), EXP(6248)])
        h = Harness(front=(1000, 6256), seed=True)
        s = h.start_front()
        h.long_frame(1000, 6256)
        h.xfers['front'].log.clear()
        h.analyse(s, 0.9)
        self.assertEqual(h.writes(), [EXP(841), VTS(3128)])

    def test_state_unchanged_on_write_failure(self):
        h = Harness(front=(400, 3128), seed=True)
        s = h.start_front()
        h.xfers['front'].nak_writes(1)
        h.analyse(s, 0.01)
        sess = h.session()
        self.assertEqual(sess.state.exposure, 400)
        self.assertEqual(sess.exp_written, 400)
        self.assertEqual(sess.phase, 'attaching')      # re-attach, not latch

    def test_engine_detaches_when_idle(self):
        h = Harness()
        s = h.start_front()
        h.eval(front=False)
        self.assertTrue(s.stopped_by_us)
        self.assertIsNone(h.session())

    def test_engine_writes_only_used_sensor(self):
        h = Harness()
        s = h.start_front()
        h.analyse(s, 0.9)
        self.assertTrue(h.xfers['front'].log)
        self.assertEqual(h.xfers['rear'].log, [])
        self.assertEqual(h.xfers['rear'].reads, [])

    def test_node_removed_ends_session(self):
        h = Harness(front=(6248, 6256), seed=True)
        h.start_front()
        h.long_frame(6248, 6256)
        h.xfers['front'].log.clear()
        h.eval(front_present=False)
        self.assertIsNone(h.session())
        self.assertEqual(h.xfers['front'].log, [])

    def test_stop_restores_frame_length(self):
        h = Harness(front=(6248, 6256), seed=True)
        h.start_front()
        h.long_frame(6248, 6256)
        h.xfers['front'].log.clear()
        h.engine.shutdown()
        self.assertEqual(h.writes(), [EXP(3120), VTS(3128)])
        h = Harness(front=(6248, 6256), seed=True)
        h.start_front()
        h.long_frame(6248, 6256)
        h.xfers['front'].log.clear()
        h.eval(front=False)
        self.assertEqual(h.xfers['front'].log, [])

    def test_stop_restore_only_vts_when_exposure_fits(self):
        h = Harness(front=(1000, 6256), seed=True)
        h.start_front()
        h.long_frame(1000, 6256)
        h.xfers['front'].log.clear()
        h.engine.shutdown()
        self.assertEqual(h.writes(), [VTS(3128)])

    def test_restore_nak_logged_other_sessions_end(self):
        h = Harness(front=(6248, 6256), rear=(12800, 12808), seed=True)
        h.eval(front=True, rear=True)
        for s in h.samplers:
            s.deliver()
        h.long_frame(6248, 6256)
        h.long_frame(12800, 12808, REAR_ID)
        h.xfers['front'].nak_writes(5)
        h.xfers['rear'].log.clear()
        h.engine.shutdown()
        self.assertEqual(h.engine.sessions, {})
        self.assertTrue(h.lines('restore failed'))
        self.assertEqual(h.writes('rear'), [EXP(4800), VTS(6404)])


class Gates(unittest.TestCase):
    def test_unknown_node_logged_once(self):
        h = Harness()
        odd = CameraNode(99, 99, 'libcamera_input.other', None, True)
        for _ in range(3):
            h.eval(extra=odd)
        self.assertEqual(len(h.lines('unknown camera node')), 1)
        self.assertEqual(h.samplers, [])

    def test_unwritable_device_logs_and_skips(self):
        h = Harness(writable=False)
        for _ in range(3):
            h.eval(front=True)
        self.assertEqual(h.samplers, [])
        self.assertEqual(len(h.lines('cannot write')), 1)

    def test_frozen_exposure_warning(self):
        h = Harness(writable=False)
        h.eval(front=True)
        self.assertTrue(any(lvl == 'warning' and 'exposure frozen' in m
                            for lvl, m in h.logs))
        h = Harness()
        h.eval(front=True)
        h.xfers['front'].nak(3)
        for _ in range(3):
            h.samplers[0].deliver()
            h.clock.advance(1.0)
        h.engine.handle_faults()
        self.assertTrue(any(lvl == 'warning' and 'exposure frozen' in m
                            for lvl, m in h.logs))

    def test_override_inactive_writes_nothing(self):
        h = Harness(override=False)
        h.eval(front=True)
        self.assertEqual(h.samplers, [])
        self.assertEqual(h.factory_calls, [])

    def test_get_stats_lazy(self):
        h = Harness()
        s = h.start_front()
        s.deliver(0.45, n=18)
        self.assertEqual(s.stats_calls, 4)

    def test_off_switch_never_writes(self):
        for v in ('off', 'OFF', '0', 'false', 'no', ' Off '):
            self.assertFalse(ae_enabled_from_env({'CAMERA_AE': v}), v)
        for v in ('', 'on', '1'):
            self.assertTrue(ae_enabled_from_env({'CAMERA_AE': v}), v)
        self.assertTrue(ae_enabled_from_env({}))
        h = Harness(ae=False)
        h.eval(front=True, rear=True)
        self.assertEqual(h.samplers, [])
        self.assertEqual(h.factory_calls, [])

    def test_session_logs_start_stop(self):
        h = Harness(front=(3120, 3128), seed=True)
        s = h.start_front()
        self.assertTrue(h.lines('start node=33 sensor=sc820cs exposure=3120 vts=3128'))
        h.analyse(s, 0.1)
        self.assertTrue(h.lines('frame 6256 lines, exposure 6248'))
        h.clock.advance(31)
        h.analyse(s, 0.45)
        self.assertTrue(h.lines('luma='))
        h.eval(front=False)
        self.assertTrue(h.lines('stop node=33 sensor=sc820cs reason=idle'))


class FixPass(unittest.TestCase):
    def test_sampler_spawn_failure_backs_off(self):
        h = Harness()
        real = h.engine.sampler_factory
        def factory(serial, on_frame):
            if serial == 82:
                raise FileNotFoundError(2, 'No such file', 'setpriv')
            return real(serial, on_frame)
        h.engine.sampler_factory = factory
        h.eval(front=True, rear=True)                 # must not raise
        self.assertIsNotNone(h.session(REAR_ID))      # other camera still starts
        self.assertIsNone(h.session(FRONT_ID))
        h.eval(front=True, rear=True)                 # within backoff: no retry
        self.assertEqual(len(h.lines('cannot start the frame reader')), 1)
        self.assertTrue(any(lvl == 'warning' and 'exposure frozen' in m
                            for lvl, m in h.logs))

    def test_stream_restart_reattaches(self):
        h = Harness(front=(1656, 3128))
        s = h.start_front()
        h.analyse(s, 0.9)                              # engine writes 1393
        mine = h.session().exp_written
        x = h.xfers['front']
        x.poke(0x3e00, (3120 << 5).to_bytes(3, 'big'))  # kernel reset at restart
        h.analyse(s, 0.45)
        h.analyse(s, 0.45)                             # 2nd mismatch -> re-attach
        self.assertFalse(h.engine.foreign_latched)
        self.assertEqual(h.session().phase, 'attaching')
        x.log.clear()
        s.deliver()                                    # re-start
        self.assertEqual(h.session().phase, 'running')
        self.assertEqual(h.writes(), [EXP(mine), GAIN_FRONT])

    def test_reattach_cap_then_foreign(self):
        h = Harness()
        s = h.start_front()
        x = h.xfers['front']
        for i in range(3):
            for v in (900 + i, 950 + i):
                h.analyse(s, 0.45)
                x.poke(0x3e00, (v << 5).to_bytes(3, 'big'))
            h.analyse(s, 0.45)
            if h.session() and h.session().phase == 'attaching':
                s.deliver()
        h.engine.handle_faults()
        self.assertTrue(h.engine.foreign_latched)

    def test_nak_while_running_reattaches(self):
        h = Harness()
        s = h.start_front()
        h.xfers['front'].nak(1)
        h.analyse(s, 0.9)                              # readback NAKs
        self.assertIsNone(h.session().fault)
        self.assertEqual(h.session().phase, 'attaching')
        h.clock.advance(1.0)
        s.deliver()
        self.assertEqual(h.session().phase, 'running')

    def test_reattach_not_cut_by_attach_timeout(self):
        h = Harness()
        s = h.start_front()
        h.xfers['front'].nak(1)
        h.analyse(s, 0.9)
        h.clock.advance(6)                             # long after attach
        h.eval(front=True)
        self.assertIsNotNone(h.session())

    def test_fault_after_app_closed_is_quiet(self):
        h = Harness()
        h.eval(front=True)
        h.xfers['front'].nak(3)
        for _ in range(3):
            h.samplers[0].deliver()
            h.clock.advance(1.0)
        h.engine._last = h.cams(front=False)          # graph already shows idle
        h.engine.handle_faults()
        self.assertIsNone(h.session())
        self.assertFalse(any(lvl == 'warning' for lvl, _ in h.logs))
        self.assertNotIn(FRONT_ID, h.engine.blocked)


class StartState(unittest.TestCase):
    def test_stale_register_ignored_starts_at_ae_max(self):
        h = Harness(front=(1, 3128))                    # stale cached control
        h.start_front()
        self.assertEqual(h.session().state, State(3120, 3128))
        self.assertEqual(h.writes(), [EXP(3120), GAIN_FRONT])

    def test_remembered_persisted_across_engines(self):
        import tempfile, os
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'remembered.json')
            h = Harness(front=(1656, 3128))
            h.engine.remember_path = path
            s = h.start_front()
            h.analyse(s, 0.9)
            converged = h.session().exp_written
            h.eval(front=False)
            h2 = Harness(front=(1, 3128))
            h2.engine.remember_path = path
            h2.engine.load_remembered()
            h2.start_front()
            self.assertEqual(h2.session().state.exposure, converged)
            h3 = Harness(front=(1, 3128))
            h3.engine.remember_path = os.path.join(d, 'missing.json')
            h3.engine.load_remembered()                 # no file: no error
            h3.start_front()
            self.assertEqual(h3.session().state.exposure, 3120)


if __name__ == '__main__':
    unittest.main()
