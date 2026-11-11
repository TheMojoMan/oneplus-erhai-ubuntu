# SPDX-License-Identifier: GPL-2.0-only
import math
import unittest

from camera_ae.focus import COARSE
from tests.fakes import FakeLens
from tests.test_engine import EXP, FRONT_ID, REAR_ID, VTS, Harness

SWEEP = 2 * len(COARSE) + 2 * 3


def af_harness(lens=None, af=True, rear=(2389, 6404), **kw):
    h = Harness(rear=rear, **kw)
    h.lens = lens if lens is not None else FakeLens()
    h.lens_calls = 0

    def lens_factory():
        h.lens_calls += 1
        return h.lens
    h.engine.lens_factory = lens_factory
    h.engine.af_enabled = af
    return h


def start_rear(h):
    h.eval(rear=True)
    s = h.samplers[-1]
    s.deliver()
    return s


def sharp_at(lens, peak=600):
    return lambda: 0.01 * math.exp(-((lens.position - peak) / 150) ** 2) + 1e-4


class AfEngine(unittest.TestCase):
    def test_af_rear_only(self):
        h = af_harness()
        h.start_front()
        self.assertEqual(h.lens_calls, 0)
        self.assertIsNone(h.session(FRONT_ID).af)
        start_rear(h)
        self.assertEqual(h.lens_calls, 1)
        self.assertEqual(h.lens.moves, [0])             # sweep begins at 0

    def test_af_sweep_locks(self):
        h = af_harness()
        s = start_rear(h)
        s.deliver(sharp=sharp_at(h.lens), n=SWEEP)
        self.assertEqual(h.lens.position, 576)
        self.assertEqual(h.session(REAR_ID).af.mode, 'monitor')
        self.assertTrue(h.lines('af: locked at 576'))
        self.assertEqual(h.session(REAR_ID).phase, 'running')

    def test_af_sweep_resets_long_frame(self):
        h = af_harness(rear=(12800, 12808), seed=True)
        start_rear(h)
        self.assertEqual(h.writes('rear')[-2:], [EXP(6396), VTS(6404)])
        st = h.session(REAR_ID).state
        self.assertEqual((st.exposure, st.vts), (6396, 6404))
        self.assertTrue(h.lines('base frame 6404 lines for the sweep'))

    def test_af_blocks_ladder_up(self):
        h = af_harness(rear=(4800, 6404), seed=True)
        s = start_rear(h)
        h.xfers['rear'].log.clear()
        h.analyse(s, 0.1)                               # dark, at ae_max, sweeping
        self.assertNotIn(VTS(12808), h.writes('rear'))
        self.assertEqual(h.session(REAR_ID).af.mode, 'sweep')

    def test_lens_error_disables_af_only(self):
        h = af_harness(lens=FakeLens(fail_after=1))
        s = start_rear(h)
        s.deliver(sharp=lambda: 1e-3, n=4)
        sess = h.session(REAR_ID)
        self.assertIsNone(sess.af)
        self.assertIsNone(sess.fault)
        self.assertEqual(sess.phase, 'running')
        self.assertTrue(h.lines('autofocus off'))

    def test_af_off_switch(self):
        h = af_harness(af=False)
        start_rear(h)
        self.assertEqual(h.lens_calls, 0)
        self.assertIsNone(h.session(REAR_ID).af)

    def test_no_lens_found(self):
        h = af_harness()
        h.engine.lens_factory = lambda: None
        for _ in range(2):
            start_rear(h)
            h.eval(rear=False)
        self.assertEqual(len(h.lines('no cn3927 lens')), 1)

    def test_lens_closed_at_end(self):
        h = af_harness()
        start_rear(h)
        h.eval(rear=False)
        self.assertTrue(h.lens.closed)

    def test_sharp_lazy(self):
        h = af_harness()
        s = start_rear(h)
        s.deliver(sharp=sharp_at(h.lens), n=4)
        self.assertEqual(s.sharp_calls, 2)              # settle 1, score 1


if __name__ == '__main__':
    unittest.main()
