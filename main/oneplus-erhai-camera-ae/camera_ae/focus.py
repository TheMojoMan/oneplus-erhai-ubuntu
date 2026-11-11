# SPDX-License-Identifier: GPL-2.0-only
"""Rear-camera autofocus, ported 1:1 from ErhaiCam (by TheMojoMan), whose
algorithm comes from CFM880/nabu-camera's nabu-autofocus.

A contrast (Tenengrad) sweep on the cn3927 voice-coil lens, driven through
the kernel lens subdev (V4L2_CID_FOCUS_ABSOLUTE):

  sweep   coarse 0,128..896,1023: per position settle 1 frame, score 1 frame
          no usable detail -> back to the previous position, monitor
          else fine +-64 around the best (clamped), lock on the sharpest
  monitor 5 s hold after a lock; then every 6th frame a score, median of 3;
          4 checks in a row below 55 % of the locked score -> sweep again,
          otherwise the locked score tracks the scene (EMA 0.05)

Pure state machine plus a small Lens driver; the engine wires it to frames.
"""
import fcntl
import os
import struct

FOCUS_MIN, FOCUS_MAX = 0, 1023
COARSE = (0, 128, 256, 384, 512, 640, 768, 896, FOCUS_MAX)
FINE_STEP = 64
SETTLE_FRAMES = 1
SCORE_FRAMES = 1
MONITOR_STRIDE = 6
MONITOR_FRAMES = 3
LOW_CHECKS = 4
LOW_RATIO = 0.55
FLAT_RATIO = 1.15
MIN_DETAIL = 5e-5
HOLD_S = 5.0
LOCK_EMA = 0.05

LENS_NAME = 'cn3927 focus'
CID_FOCUS_ABSOLUTE = 0x009a090a
VIDIOC_G_CTRL = 0xc008561b
VIDIOC_S_CTRL = 0xc008561c


def tenengrad(grey, w, h):
    """Mean squared Sobel gradient of a [0,1] grey image (VALID region)."""
    total = 0
    rows = [grey[y * w:(y + 1) * w] for y in range(h)]
    for y in range(h - 2):
        r0, r1, r2 = rows[y], rows[y + 1], rows[y + 2]
        for a, b, c, d, f, g, hh, i in zip(r0, r0[1:], r0[2:], r1, r1[2:],
                                           r2, r2[1:], r2[2:]):
            gx = (c - a) + 2 * (f - d) + (i - g)
            gy = (g - a) + 2 * (hh - b) + (i - c)
            total += gx * gx + gy * gy
    return total / (255.0 * 255.0) / ((w - 2) * (h - 2))


def centre(grey, w, h):
    """ErhaiCam's AF window: the middle 60 % in both directions."""
    x0, y0 = w // 5, h // 5
    x1, y1 = w - x0, h - y0
    cw = x1 - x0
    out = b''.join(grey[y * w + x0:y * w + x1] for y in range(y0, y1))
    return out, cw, y1 - y0


class Autofocus:
    def __init__(self):
        self.mode = 'idle'
        self.positions = ()
        self.idx = 0
        self.settle = 0
        self.fine = False
        self.best_pos = 0
        self.best_score = -1.0
        self.best_min = float('inf')
        self.prev_pos = 0
        self.locked = 0.0
        self.hold_until = 0.0
        self.stride = 0
        self.check = []
        self.low_checks = 0
        self.events = []
        self.pos = 0              # last lens position commanded

    @property
    def wants_short_frames(self):
        return self.mode == 'sweep'

    def drain_events(self):
        ev, self.events = self.events, []
        return ev

    def start(self, current_pos):
        """Begin a coarse sweep; returns the first lens position."""
        self.prev_pos = current_pos
        self.pos = self._sweep(COARSE, fine=False)
        return self.pos

    def _sweep(self, positions, fine):
        self.mode = 'sweep'
        self.positions = tuple(positions)
        self.idx = 0
        self.settle = 0
        self.fine = fine
        self.best_pos = self.positions[0]
        self.best_score = -1.0
        self.best_min = float('inf')
        return self.positions[0]

    def on_frame(self, now, score):
        """One delivered frame. `score()` is called only when needed.
        Returns a new lens position, or None."""
        move = self._on_frame(now, score)
        if move is not None:
            self.pos = move
        return move

    def _on_frame(self, now, score):
        if self.mode == 'sweep':
            if self.settle < SETTLE_FRAMES:
                self.settle += 1
                return None
            s = score()
            self.best_min = min(self.best_min, s)
            if s > self.best_score:
                self.best_score = s
                self.best_pos = self.positions[self.idx]
            self.idx += 1
            self.settle = 0
            if self.idx < len(self.positions):
                return self.positions[self.idx]
            return self._finish(now)
        if self.mode == 'monitor':
            self.stride += 1
            if self.stride < MONITOR_STRIDE:
                return None
            self.stride = 0
            if now < self.hold_until:
                return None
            self.check.append(score())
            if len(self.check) < MONITOR_FRAMES:
                return None
            med = sorted(self.check)[len(self.check) // 2]
            self.check = []
            if med < self.locked * LOW_RATIO:
                self.low_checks += 1
                if self.low_checks >= LOW_CHECKS:
                    self.low_checks = 0
                    self.events.append('refocus: sharpness dropped')
                    return self.start(self.pos)
            else:
                self.low_checks = 0
                self.locked = self.locked * (1 - LOCK_EMA) + med * LOCK_EMA
        return None

    def _finish(self, now):
        if not self.fine:
            if (self.best_score - self.best_min < MIN_DETAIL
                    or self.best_score < self.best_min * FLAT_RATIO):
                self._monitor(max(self.best_score, 0.0), now, hold=False)
                self.events.append(f'no detail to lock on; back to {self.prev_pos}')
                return self.prev_pos
            fine = sorted({min(FOCUS_MAX, max(FOCUS_MIN, self.best_pos + o))
                           for o in (-FINE_STEP, 0, FINE_STEP)})
            return self._sweep(fine, fine=True)
        self._monitor(self.best_score, now, hold=True)
        self.events.append(f'locked at {self.best_pos} '
                           f'(score {self.best_score:.2e})')
        return self.best_pos

    def _monitor(self, locked, now, hold):
        self.mode = 'monitor'
        self.locked = locked
        self.stride = 0
        self.check = []
        self.low_checks = 0
        self.hold_until = now + HOLD_S if hold else 0.0


class LensError(OSError):
    pass


def find_lens(root='/sys/class/video4linux'):
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return None
    for node in entries:
        if not node.startswith('v4l-subdev'):
            continue
        try:
            with open(os.path.join(root, node, 'name')) as f:
                if f.read().strip() == LENS_NAME:
                    return '/dev/' + node
        except OSError:
            continue
    return None


def open_lens():
    """The engine's lens_factory: the cn3927 subdev, or None."""
    path = find_lens()
    return None if path is None else Lens(path)


class Lens:
    def __init__(self, path, ioctl=fcntl.ioctl,
                 opener=lambda p: os.open(p, os.O_RDWR | os.O_CLOEXEC)):
        self.path = path
        self._ioctl = ioctl
        try:
            self.fd = opener(path)
        except OSError as e:
            raise LensError(e.errno, f'cannot open {path}: {e.strerror}') from e
        self.position = self._get()

    def _ctrl(self, req, value):
        buf = bytearray(struct.pack('=Ii', CID_FOCUS_ABSOLUTE, value))
        try:
            self._ioctl(self.fd, req, buf)
        except OSError as e:
            raise LensError(e.errno, f'{self.path}: {e.strerror}') from e
        return struct.unpack('=Ii', bytes(buf))[1]

    def _get(self):
        try:
            return self._ctrl(VIDIOC_G_CTRL, 0)
        except LensError:
            return 0

    def set(self, position):
        position = min(FOCUS_MAX, max(FOCUS_MIN, int(position)))
        self._ctrl(VIDIOC_S_CTRL, position)
        self.position = position

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
