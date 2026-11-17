# SPDX-License-Identifier: GPL-2.0-only
"""A second, passive PipeWire reader on a camera node: 320x240 BGRx frames.

gst-launch writes raw frames to a dedicated pipe fd (not stdout, so stray
status text cannot desync the 307200-byte framing).  The child gets
PDEATHSIG via setpriv so it never outlives the engine.
"""
import os
import subprocess
import threading
import time

from .focus import centre, tenengrad
from .stats import _grey, luma_hi_lo_grey

W, H = 320, 240
FRAME_BYTES = W * H * 4
WATCHDOG_S = 5.0
WRAP = ('setpriv', '--pdeathsig', 'TERM', '--')
STREAM_PROPS = ('props,node.dont-reconnect=true,node.dont-fallback=true,'
                'node.passive=true')


def sampler_argv(serial, fd=3):
    return ['gst-launch-1.0', '-q', 'pipewiresrc', f'target-object={serial}',
            'client-name=camera-tone', f'stream-properties={STREAM_PROPS}',
            '!', 'videoscale', 'method=nearest-neighbour', 'add-borders=false',
            '!', 'videoconvert', '!',
            f'video/x-raw,format=BGRx,width={W},height={H}',
            '!', 'queue', 'leaky=downstream', 'max-size-buffers=1',
            '!', 'fdsink', f'fd={fd}']


class Frame:
    """One delivered frame; the grey image is computed at most once and
    shared by the AE statistics and the AF sharpness score."""

    def __init__(self, data):
        self.data = data
        self._grey = None

    def grey(self):
        if self._grey is None:
            self._grey = _grey(self.data, W, H, W * 4)
        return self._grey

    def stats(self):
        return luma_hi_lo_grey(self.grey())

    def sharp(self):
        return tenengrad(*centre(self.grey(), W, H))


class GstSampler:
    def __init__(self, serial, on_frame, spawn=subprocess.Popen,
                 argv_fn=sampler_argv, wrap=WRAP, clock=time.monotonic,
                 frame_bytes=FRAME_BYTES):
        self.serial = serial
        self.on_frame = on_frame
        self.clock = clock
        self.frame_bytes = frame_bytes
        self.stopped_by_us = False
        self.frames = 0
        self.error = None
        self._last = None
        r, w = os.pipe()
        try:
            self.proc = spawn(list(wrap) + argv_fn(serial, w),
                              stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL,
                              pass_fds=(w,), close_fds=True)
        except BaseException:
            os.close(r)
            raise
        finally:
            os.close(w)
        self._thread = threading.Thread(target=self._read, args=(r,),
                                        name=f'sampler-{serial}', daemon=True)
        self._thread.start()

    @property
    def pid(self):
        return self.proc.pid

    def _read(self, fd):
        with os.fdopen(fd, 'rb', buffering=0) as f:
            while True:
                buf = bytearray(self.frame_bytes)
                view = memoryview(buf)
                got = 0
                while got < self.frame_bytes:
                    n = f.readinto(view[got:])
                    if not n:
                        return                       # EOF: partial frame dropped
                    got += n
                self.frames += 1
                self._last = self.clock()
                frame = Frame(bytes(buf))
                try:
                    self.on_frame(frame.stats, frame.sharp)
                except Exception as e:           # engine wraps its own; belt
                    self.error = e
                    return

    @property
    def reader_alive(self):
        return self._thread.is_alive()

    @property
    def alive(self):
        if self.proc.poll() is not None or not self._thread.is_alive():
            return False
        return self._last is None or self.clock() - self._last < WATCHDOG_S

    def stop(self):
        self.stopped_by_us = True
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(3)
        if threading.current_thread() is not self._thread:
            self._thread.join(3)
