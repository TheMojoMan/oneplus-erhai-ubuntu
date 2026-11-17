import sys
import threading
import time
import unittest

from camera_ae.sampler import FRAME_BYTES, GstSampler, sampler_argv
from tests.fakes import FakeClock


def child(fd, frames, partial=0, sleep=0.0):
    code = (f'import os, time\n'
            f'f = os.fdopen({fd}, "wb")\n'
            f'for i in range({frames}):\n'
            f'    f.write(bytes([i % 256]) * {FRAME_BYTES})\n'
            f'f.write(b"x" * {partial})\n'
            f'f.flush()\n'
            f'time.sleep({sleep})\n')
    return [sys.executable, '-c', code]


def wait_for(cond, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return cond()


class Sampler(unittest.TestCase):
    def test_sampler_argv(self):
        argv = sampler_argv(82)
        self.assertEqual(argv[:3], ['gst-launch-1.0', '-q', 'pipewiresrc'])
        self.assertIn('target-object=82', argv)
        props = [a for a in argv if a.startswith('stream-properties=')]
        self.assertEqual(len(props), 1)
        for p in ('node.dont-fallback=true', 'node.dont-reconnect=true',
                  'node.passive=true'):
            self.assertIn(p, props[0])
        self.assertIn('client-name=camera-tone', argv)
        self.assertEqual(argv[-2:], ['fdsink', 'fd=3'])

    def test_gst_sampler_reads_frames(self):
        got = []
        done = threading.Event()

        def on_frame(get_stats, get_sharp):
            got.append(1)

        s = GstSampler(1, on_frame, argv_fn=lambda serial, fd: child(fd, 7, 1000),
                       wrap=())
        self.assertTrue(wait_for(lambda: not s.alive))
        self.assertEqual(len(got), 7)
        self.assertFalse(s.stopped_by_us)
        self.assertIsInstance(s.pid, int)
        s.stop()

    def test_gst_sampler_stop_not_death(self):
        s = GstSampler(1, lambda g, s: None,
                       argv_fn=lambda serial, fd: child(fd, 1, 0, sleep=30),
                       wrap=())
        self.assertTrue(wait_for(lambda: s.frames >= 1))
        t = time.monotonic()
        s.stop()
        self.assertLess(time.monotonic() - t, 4.0)
        self.assertTrue(s.stopped_by_us)
        self.assertFalse(s.alive)
        self.assertFalse(s.reader_alive)

    def test_get_stats_returns_luma_hi_lo(self):
        seen = []
        s = GstSampler(1, lambda g, s: seen.append((g(), s())),
                       argv_fn=lambda serial, fd: child(fd, 1), wrap=())
        self.assertTrue(wait_for(lambda: seen))
        (luma, hi, lo), sharp = seen[0]
        self.assertAlmostEqual(luma, 0.0)          # frame 0 is all zeros
        self.assertEqual(sharp, 0.0)               # and flat
        s.stop()

    def test_frame_watchdog(self):
        clock = FakeClock()
        s = GstSampler(1, lambda g, s: None,
                       argv_fn=lambda serial, fd: child(fd, 1, 0, sleep=30),
                       wrap=(), clock=clock)
        self.assertTrue(wait_for(lambda: s.frames >= 1))
        self.assertTrue(s.alive)
        clock.advance(6)
        self.assertFalse(s.alive)
        s.stop()


if __name__ == '__main__':
    unittest.main()
