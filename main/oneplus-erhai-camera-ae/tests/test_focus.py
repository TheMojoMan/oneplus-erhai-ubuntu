# SPDX-License-Identifier: GPL-2.0-only
import math
import os
import random
import struct
import tempfile
import unittest

from camera_ae.focus import (COARSE, VIDIOC_S_CTRL, CID_FOCUS_ABSOLUTE,
                             Autofocus, Lens, LensError, centre, find_lens,
                             tenengrad)


def ref_tenengrad(g, w, h):
    k = 1 / 255
    s = 0.0
    for y in range(h - 2):
        for x in range(w - 2):
            a, b, c = g[y * w + x], g[y * w + x + 1], g[y * w + x + 2]
            d, f = g[(y + 1) * w + x], g[(y + 1) * w + x + 2]
            gg, hh, i = (g[(y + 2) * w + x], g[(y + 2) * w + x + 1],
                         g[(y + 2) * w + x + 2])
            gx = ((c - a) + 2 * (f - d) + (i - gg)) * k
            gy = ((gg - a) + 2 * (hh - b) + (i - c)) * k
            s += gx * gx + gy * gy
    return s / ((w - 2) * (h - 2))


class Metric(unittest.TestCase):
    def test_tenengrad_flat_is_zero(self):
        self.assertEqual(tenengrad(bytes([128]) * 100, 10, 10), 0.0)

    def test_tenengrad_edge(self):
        img = bytes([0, 0, 255] * 3)                 # 3x3, edge on the right
        self.assertAlmostEqual(tenengrad(img, 3, 3), 16.0)

    def test_tenengrad_matches_reference(self):
        rnd = random.Random(3)
        w, h = 23, 17
        g = bytes(rnd.randrange(256) for _ in range(w * h))
        self.assertAlmostEqual(tenengrad(g, w, h), ref_tenengrad(g, w, h),
                               places=9)

    def test_centre_crop(self):
        w, h = 320, 240
        g = bytes((x + y) % 256 for y in range(h) for x in range(w))
        c, cw, ch = centre(g, w, h)
        self.assertEqual((cw, ch), (192, 144))
        self.assertEqual(c[0], (64 + 48) % 256)
        self.assertEqual(len(c), cw * ch)


def peaked(peak, width=150.0, floor=1e-4):
    return lambda pos: 0.01 * math.exp(-((pos - peak) / width) ** 2) + floor


class Driver:
    """Feeds frames to an Autofocus with a lens whose sharpness depends on
    the current lens position."""

    def __init__(self, af, scene, pos=500):
        self.af, self.scene, self.pos = af, scene, pos
        self.t = 0.0
        self.scores = 0

    def score(self):
        self.scores += 1
        return self.scene(self.pos)

    def start(self):
        self.pos = self.af.start(self.pos)

    def frame(self, n=1, dt=0.04):
        for _ in range(n):
            self.t += dt
            move = self.af.on_frame(self.t, self.score)
            if move is not None:
                self.pos = move


class Sweep(unittest.TestCase):
    def test_sweep_finds_peak(self):
        d = Driver(Autofocus(), peaked(600))
        d.start()
        self.assertEqual(d.pos, 0)
        self.assertTrue(d.af.wants_short_frames)
        d.frame(2 * len(COARSE) + 2 * 3)
        self.assertEqual(d.af.mode, 'monitor')
        self.assertEqual(d.pos, 576)
        self.assertFalse(d.af.wants_short_frames)
        self.assertIn('locked', ' '.join(d.af.drain_events()))

    def test_sweep_frame_count(self):
        d = Driver(Autofocus(), peaked(600))
        d.start()
        d.frame(2 * len(COARSE) + 2 * 3 - 1)
        self.assertEqual(d.af.mode, 'sweep')
        d.frame()
        self.assertEqual(d.af.mode, 'monitor')

    def test_flat_scene_returns_to_previous(self):
        for scene in (lambda p: 1e-5, lambda p: 1e-3 + p * 1e-7):
            d = Driver(Autofocus(), scene, pos=333)
            d.start()
            d.frame(2 * len(COARSE))
            self.assertEqual(d.af.mode, 'monitor')
            self.assertEqual(d.pos, 333)
            self.assertIn('no detail', ' '.join(d.af.drain_events()))

    def test_fine_positions_clamped(self):
        d = Driver(Autofocus(), peaked(1023))
        d.start()
        d.frame(2 * len(COARSE) + 2 * 2)               # fine set {959, 1023}
        self.assertEqual(d.af.mode, 'monitor')
        self.assertEqual(d.pos, 1023)

    def test_score_lazy_in_sweep(self):
        d = Driver(Autofocus(), peaked(600))
        d.start()
        d.frame(4)
        self.assertEqual(d.scores, 2)                  # settle 1, score 1


class Monitor(unittest.TestCase):
    def locked(self):
        d = Driver(Autofocus(), peaked(600))
        d.start()
        d.frame(2 * len(COARSE) + 2 * 3)
        d.af.drain_events()
        return d

    def test_hold_then_monitor_cadence(self):
        d = self.locked()
        d.scores = 0
        d.frame(60, dt=0.04)                           # 2.4 s < 5 s hold
        self.assertEqual(d.scores, 0)
        d.frame(100, dt=0.04)                          # past the hold
        self.assertGreater(d.scores, 0)
        before = d.scores
        d.frame(18, dt=0.04)
        self.assertEqual(d.scores - before, 3)          # every 6th frame

    def test_refocus_on_sharpness_drop(self):
        d = self.locked()
        d.frame(130, dt=0.04)                           # past the hold
        d.scene = lambda pos: 1e-4                      # scene changed: blurry
        n = 0
        while d.af.mode == 'monitor' and n < 300:
            d.frame()
            n += 1
        self.assertEqual(d.af.mode, 'sweep')
        self.assertGreaterEqual(n, 3 * 18)              # 4 low checks needed
        self.assertLessEqual(n, 5 * 18)
        self.assertEqual(d.pos, 0)
        self.assertIn('refocus', ' '.join(d.af.drain_events()))

    def test_stable_scene_no_refocus(self):
        d = self.locked()
        d.frame(2000, dt=0.04)
        self.assertEqual(d.af.mode, 'monitor')


class LensTests(unittest.TestCase):
    def test_find_lens(self):
        with tempfile.TemporaryDirectory() as root:
            for node, name in (('v4l-subdev16', 'sc820cs 7-0010'),
                               ('v4l-subdev18', 'cn3927 focus'), ('video0', 'x')):
                os.mkdir(os.path.join(root, node))
                with open(os.path.join(root, node, 'name'), 'w') as f:
                    f.write(name + '\n')
            self.assertEqual(find_lens(root), '/dev/v4l-subdev18')
            os.rename(os.path.join(root, 'v4l-subdev18'),
                      os.path.join(root, 'v4l-subdev99'))
            self.assertEqual(find_lens(root), '/dev/v4l-subdev99')
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(find_lens(root))

    def test_lens_set_ioctl(self):
        calls = []

        def ioctl(fd, req, buf):
            calls.append((req, bytes(buf)))
            return 0

        lens = Lens('/dev/null', ioctl=ioctl, opener=lambda p: 99)
        lens.set(1100)
        req, buf = calls[-1]
        self.assertEqual(req, VIDIOC_S_CTRL)
        self.assertEqual(struct.unpack('=Ii', buf), (CID_FOCUS_ABSOLUTE, 1023))
        self.assertEqual(lens.position, 1023)

    def test_lens_error(self):
        def ioctl(fd, req, buf):
            raise OSError(5, 'Input/output error')

        lens = Lens('/dev/null', ioctl=ioctl, opener=lambda p: 99)
        with self.assertRaises(LensError):
            lens.set(10)


if __name__ == '__main__':
    unittest.main()
