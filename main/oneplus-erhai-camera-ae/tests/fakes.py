"""Test fakes shared by the sensor, engine and main tests."""
from camera_ae.sensor import I2C_M_RD


class FakeXfer:
    """An I2C bus with one 16-bit-addressed register file.

    A write message followed by a read message is an address-set + read;
    a lone write message writes its payload starting at its address.
    `log` records every write as (reg, payload bytes)."""

    def __init__(self, regs=None):
        self.regs = dict(regs or {})
        self.log = []
        self.reads = []
        self._nak = []
        self._nak_writes = []

    def nak_writes(self, n=1, errno=121):
        self._nak_writes.extend([errno] * n)

    def nak(self, n=1, errno=121):
        self._nak.extend([errno] * n)

    def poke(self, reg, data):
        """A foreign writer changes registers behind our back."""
        for i, b in enumerate(data):
            self.regs[reg + i] = b

    def __call__(self, msgs):
        if self._nak:
            err = self._nak.pop(0)
            raise OSError(err, 'Remote I/O error')
        if self._nak_writes and not any(f & I2C_M_RD for f, _ in msgs):
            raise OSError(self._nak_writes.pop(0), 'Remote I/O error')
        out = []
        reg = None
        for flags, data in msgs:
            if flags & I2C_M_RD:
                self.reads.append((reg, data))
                out.append(bytes(self.regs.get(reg + i, 0)
                                 for i in range(data)))
            else:
                reg = (data[0] << 8) | data[1]
                if len(data) > 2:
                    self.log.append((reg, bytes(data[2:])))
                    self.poke(reg, data[2:])
        return out

    def writes(self):
        return [(hex(r), d.hex(' ')) for r, d in self.log]


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t

    def advance(self, s):
        self.t += s


class FakeSampler:
    """Stands in for GstSampler. deliver() calls on_frame synchronously."""
    _next_pid = 5000

    def __init__(self, serial, on_frame):
        self.serial = serial
        self.on_frame = on_frame
        self._alive = True
        self.stopped_by_us = False
        self.reader_alive = False
        self.stats_calls = 0
        self.sharp_calls = 0
        FakeSampler._next_pid += 1
        self.pid = FakeSampler._next_pid

    @property
    def alive(self):
        return self._alive

    def deliver(self, luma=0.45, hi=0.0, lo=0.05, n=1, sharp=None):
        for _ in range(n):
            def get_stats():
                self.stats_calls += 1
                return luma, hi, lo

            def get_sharp():
                self.sharp_calls += 1
                return sharp() if sharp else 0.0
            self.on_frame(get_stats, get_sharp)

    def die(self):
        self._alive = False

    def stop(self):
        import threading
        self.stop_thread = threading.current_thread()
        self.stopped_by_us = True
        self._alive = False


def regs(chip, exposure, vts):
    """A sensor register file: chip ID, exposure (lines) and VTS."""
    e = (exposure << 5).to_bytes(3, 'big')
    return {0x3107: chip >> 8, 0x3108: chip & 0xff,
            0x3e00: e[0], 0x3e01: e[1], 0x3e02: e[2],
            0x320e: vts >> 8, 0x320f: vts & 0xff}


class FakePopen:
    """pw-dump stand-in: a real pipe as stdout; terminate() closes it (EOF)."""

    def __init__(self, argv=None, **kw):
        import os
        self.argv = argv
        r, self._w = os.pipe()
        self.stdout = os.fdopen(r, 'rb', buffering=0)
        self.pid = 4242
        self.returncode = None

    def write(self, data):
        import os
        os.write(self._w, data)

    def close(self):
        import os
        if self._w is not None:
            os.close(self._w)
            self._w = None
            self.returncode = 0

    def terminate(self):
        self.close()

    kill = terminate

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        return self.returncode


class FakeLens:
    def __init__(self, position=896, fail_after=None):
        self.position = position
        self.moves = []
        self.fail_after = fail_after
        self.closed = False

    def set(self, pos):
        from camera_ae.focus import LensError
        if self.fail_after is not None and len(self.moves) >= self.fail_after:
            raise LensError(5, 'Input/output error')
        pos = min(1023, max(0, int(pos)))
        self.moves.append(pos)
        self.position = pos

    def close(self):
        self.closed = True
