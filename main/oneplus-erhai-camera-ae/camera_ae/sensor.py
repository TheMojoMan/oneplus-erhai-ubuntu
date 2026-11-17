# SPDX-License-Identifier: GPL-2.0-only
"""SmartSens sensor registers over Linux I2C_RDWR (no bit-15 read trick).

    exposure  0x3e00..02  24-bit, lines << 5
    VTS       0x320e..0f  16-bit
    gain      0x3e07 fine (0x80 = 1x), coarse 0x3e08 (front) / 0x3e09 (rear),
              0x0f = 4x total with fine 0x80 -- the drivers' write_gain(64)
    chip ID   0x3107..08  16-bit: front 0xd154, rear 0xc658
"""
import ctypes
import fcntl
import os
import re
import sys
from dataclasses import dataclass

from .controller import FRONT, REAR, SensorLimits

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001

REG_EXPOSURE = 0x3e00
REG_VTS = 0x320e
REG_FINE_GAIN = 0x3e07
REG_GAIN_BASE = 0x3e06
REG_CHIP_ID = 0x3107


@dataclass(frozen=True)
class Spec:
    name: str
    addr: int
    gain_reg: int
    chip_id: int
    limits: SensorLimits
    node_suffix: str


SPECS = {
    'front': Spec('sc820cs', 0x10, 0x3e08, 0xd154, FRONT, 'camera-sensor_10'),
    'rear': Spec('sc1320cs', 0x36, 0x3e09, 0xc658, REAR, 'camera-sensor_36'),
}


class SensorError(OSError):
    def __init__(self, sensor, reg, errno):
        super().__init__(errno, os.strerror(errno) if errno else 'I2C error')
        self.sensor = sensor
        self.reg = reg


def find_bus(name, addr, root='/sys/bus/i2c/devices'):
    pat = re.compile(r'^(\d+)-00%02x$' % addr)
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return None
    for entry in entries:
        m = pat.match(entry)
        if not m:
            continue
        try:
            with open(os.path.join(root, entry, 'name')) as f:
                if f.read().strip() == name:
                    return int(m.group(1))
        except OSError:
            continue
    return None


class _Msg(ctypes.Structure):
    _fields_ = [('addr', ctypes.c_uint16), ('flags', ctypes.c_uint16),
                ('len', ctypes.c_uint16), ('buf', ctypes.POINTER(ctypes.c_uint8))]


class _RdWr(ctypes.Structure):
    _fields_ = [('msgs', ctypes.POINTER(_Msg)), ('nmsgs', ctypes.c_uint32)]


def i2c_rdwr(bus, addr):
    """The default transfer: one I2C_RDWR ioctl per call on /dev/i2c-<bus>."""
    path = f'/dev/i2c-{bus}'

    def xfer(msgs):
        bufs = []
        arr = (_Msg * len(msgs))()
        for i, (flags, data) in enumerate(msgs):
            n = data if flags & I2C_M_RD else len(data)
            b = (ctypes.c_uint8 * n)()
            if not flags & I2C_M_RD:
                ctypes.memmove(b, bytes(data), n)
            bufs.append((flags, b, n))
            arr[i] = _Msg(addr, flags, n, ctypes.cast(b, ctypes.POINTER(ctypes.c_uint8)))
        req = _RdWr(ctypes.cast(arr, ctypes.POINTER(_Msg)), len(msgs))
        fd = os.open(path, os.O_RDWR)
        try:
            fcntl.ioctl(fd, I2C_RDWR, req)
        finally:
            os.close(fd)
        return [bytes(b) for flags, b, n in bufs if flags & I2C_M_RD]

    return xfer


class Sensor:
    def __init__(self, spec, bus, xfer=None):
        self.spec = spec
        self.bus = bus
        self.xfer = xfer or i2c_rdwr(bus, spec.addr)

    def _call(self, msgs, reg):
        try:
            return self.xfer(msgs)
        except SensorError:
            raise
        except OSError as e:
            raise SensorError(self.spec.name, reg, e.errno or 0) from e

    def read(self, reg, n):
        return self._call([(0, bytes([reg >> 8, reg & 0xff])), (I2C_M_RD, n)],
                          reg)[0]

    def write(self, reg, data):
        self._call([(0, bytes([reg >> 8, reg & 0xff]) + bytes(data))], reg)

    def read_exposure(self):
        return int.from_bytes(self.read(REG_EXPOSURE, 3), 'big') >> 5

    def write_exposure(self, lines):
        self.write(REG_EXPOSURE, (lines << 5).to_bytes(3, 'big'))

    def read_vts(self):
        return int.from_bytes(self.read(REG_VTS, 2), 'big')

    def write_vts(self, vts):
        self.write(REG_VTS, vts.to_bytes(2, 'big'))

    def write_gain_4x(self):
        if self.spec.gain_reg == REG_FINE_GAIN + 1:     # front: contiguous
            self.write(REG_FINE_GAIN, b'\x80\x0f')
        else:
            self.write(REG_FINE_GAIN, b'\x80')
            self.write(self.spec.gain_reg, b'\x0f')

    def read_gain(self):
        raw = self.read(REG_GAIN_BASE, 4)
        return {REG_GAIN_BASE + i: b for i, b in enumerate(raw)}

    def chip_ok(self):
        return int.from_bytes(self.read(REG_CHIP_ID, 2), 'big') == self.spec.chip_id

    def writable(self):
        return os.access(f'/dev/i2c-{self.bus}', os.R_OK | os.W_OK)


def open_sensor(kind, root='/sys/bus/i2c/devices'):
    spec = SPECS[kind]
    bus = find_bus(spec.name, spec.addr, root)
    return None if bus is None else Sensor(spec, bus)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] not in SPECS:
        print('usage: python3 -m camera_ae.sensor <front|rear> '
              '[read|vts|gain|chip|write-exposure N]', file=sys.stderr)
        return 2
    s = open_sensor(argv[0])
    if s is None:
        print(f'{SPECS[argv[0]].name}: not found on any I2C bus', file=sys.stderr)
        return 1
    cmd = argv[1] if len(argv) > 1 else 'read'
    try:
        if cmd == 'read':
            print(f'bus={s.bus} exposure={s.read_exposure()} vts={s.read_vts()}')
        elif cmd == 'vts':
            print(s.read_vts())
        elif cmd == 'gain':
            print(' '.join(f'0x{r:04x}=0x{v:02x}' for r, v in s.read_gain().items()))
        elif cmd == 'chip':
            raw = int.from_bytes(s.read(REG_CHIP_ID, 2), 'big')
            print(f'chip=0x{raw:04x} expected=0x{s.spec.chip_id:04x} '
                  f'{"ok" if raw == s.spec.chip_id else "MISMATCH"}')
        elif cmd == 'write-exposure' and len(argv) > 2:
            s.write_exposure(int(argv[2]))
            print(f'exposure={s.read_exposure()}')
        else:
            print(f'unknown command {cmd!r}', file=sys.stderr)
            return 2
    except SensorError as e:
        print(f'{e.sensor}: I2C error at 0x{e.reg:04x}: {e.strerror} '
              f'(errno {e.errno}; powered off?)', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
