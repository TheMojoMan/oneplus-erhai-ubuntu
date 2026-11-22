import random
import unittest

from camera_ae import stats

W, H = 320, 240


def frame(b, g, r):
    return bytes((b, g, r, 0)) * (W * H)


def reference(buf, w, h, stride):
    luma = hi = lo = 0.0
    for y in range(h):
        row = buf[y * stride:y * stride + w * 4]
        for x in range(w):
            b, g, r = row[4 * x], row[4 * x + 1], row[4 * x + 2]
            v = ((77 * r + 150 * g + 29 * b) >> 8) / 255
            luma += v
            hi += max(v - 0.9, 0.0)
            lo += min(v, 0.1)
    n = w * h
    return luma / n, hi / n, lo / n


class Stats(unittest.TestCase):
    def test_stats(self):
        luma, hi, lo = stats.luma_hi_lo(frame(10, 200, 250), W, H, W * 4)
        self.assertAlmostEqual(luma, 193 / 255, places=9)
        self.assertAlmostEqual(hi, max(193 / 255 - 0.9, 0.0), places=9)
        self.assertAlmostEqual(lo, 0.1, places=9)
        luma, hi, lo = stats.luma_hi_lo(frame(255, 255, 255), W, H, W * 4)
        self.assertAlmostEqual(luma, 1.0, places=9)
        self.assertAlmostEqual(hi, 0.1, places=9)
        self.assertAlmostEqual(lo, 0.1, places=9)
        _, _, lo = stats.luma_hi_lo(frame(0, 0, 0), W, H, W * 4)
        self.assertAlmostEqual(lo, 0.0, places=9)
        rg, bg = stats.tint(frame(100, 100, 110), W, H, W * 4)
        self.assertAlmostEqual(rg, 1.1, places=9)
        self.assertAlmostEqual(bg, 1.0, places=9)

    def test_stats_matches_reference_random(self):
        rnd = random.Random(7)
        w, h = 40, 30
        for i in range(20):
            stride = w * 4 + (8 if i % 2 else 0)
            buf = bytes(rnd.randrange(256) for _ in range(stride * h))
            got = stats.luma_hi_lo(buf, w, h, stride)
            want = reference(buf, w, h, stride)
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, places=9)


if __name__ == '__main__':
    unittest.main()
