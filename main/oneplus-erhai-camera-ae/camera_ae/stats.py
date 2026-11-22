# SPDX-License-Identifier: GPL-2.0-only
"""Frame statistics on BGRx frames, matching ErhaiCam's sample_gray/stats_cpu.

grey = (77R + 150G + 29B) >> 8, v = grey / 255
luma = mean(v)      hi = mean(max(v - 0.9, 0))      lo = mean(min(v, 0.1))

Computed with big-int lanes (each pixel in a 16-bit lane, so the weighted
sum never carries into a neighbour) and a histogram of the grey bytes:
exact, and several times faster than a per-pixel Python loop.
"""
import sys
import time

W, H = 320, 240
FRAME = W * H * 4


def _packed(buf, w, h, stride):
    row = w * 4
    if stride == row and len(buf) >= row * h:
        return bytes(buf[:row * h])
    return b''.join(bytes(buf[y * stride:y * stride + row]) for y in range(h))


def _lane(plane):
    lanes = bytearray(2 * len(plane))
    lanes[0::2] = plane
    return int.from_bytes(lanes, 'little')


def _grey(buf, w, h, stride):
    px = _packed(buf, w, h, stride)
    total = (77 * _lane(px[2::4]) + 150 * _lane(px[1::4])
             + 29 * _lane(px[0::4]))
    return total.to_bytes(2 * w * h, 'little')[1::2]


def luma_hi_lo(buf, w, h, stride):
    return luma_hi_lo_grey(_grey(buf, w, h, stride))


def luma_hi_lo_grey(grey):
    n = len(grey)
    luma = sum(grey) / 255 / n
    hi = sum(grey.count(bytes([g])) * (g / 255 - 0.9) for g in range(230, 256))
    lo_dark = sum(grey.count(bytes([g])) * (g / 255) for g in range(1, 26))
    dark = sum(grey.count(bytes([g])) for g in range(0, 26))
    lo = lo_dark + 0.1 * (n - dark)
    return luma, hi / n, lo / n


def tint(buf, w, h, stride):
    px = _packed(buf, w, h, stride)
    g = sum(px[1::4])
    if not g:
        return 1.0, 1.0
    return sum(px[2::4]) / g, sum(px[0::4]) / g


def main(inp=None, out=None, clock=time.monotonic):
    """Read 320x240 BGRx frames; once a second print stats of the last one."""
    inp = inp or sys.stdin.buffer
    out = out or sys.stdout
    t0 = last = clock()
    frames = 0
    buf = None
    while True:
        b = inp.read(FRAME)
        if len(b) < FRAME:
            break
        frames += 1
        buf = b
        now = clock()
        if now - last >= 1.0:
            luma, hi, lo = luma_hi_lo(buf, W, H, W * 4)
            rg, bg = tint(buf, W, H, W * 4)
            print(f'{now - t0:.1f} {luma:.3f} {hi:.4f} {lo:.4f} '
                  f'{frames / (now - last):.1f} {rg:.3f} {bg:.3f}',
                  file=out, flush=True)
            frames, last = 0, now


if __name__ == '__main__':
    main()
