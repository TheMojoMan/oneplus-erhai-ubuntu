import os
import tempfile
import unittest

from camera_ae.sensor import (I2C_M_RD, SPECS, Sensor, SensorError, find_bus)
from tests.fakes import FakeXfer


def raw_writes(x):
    return [bytes([r >> 8, r & 0xff]) + d for r, d in x.log]


class Encoding(unittest.TestCase):
    def setUp(self):
        self.x = FakeXfer()
        self.front = Sensor(SPECS['front'], 7, xfer=self.x)

    def test_write_exposure(self):
        self.front.write_exposure(317)
        self.assertEqual(raw_writes(self.x), [bytes.fromhex('3e 00 00 27 a0')])

    def test_exposure_encode_max(self):
        self.front.write_exposure(25608)
        self.assertEqual(raw_writes(self.x), [bytes.fromhex('3e 00 0c 81 00')])

    def test_read_exposure_msgs(self):
        seen = []

        def xfer(msgs):
            seen.append(msgs)
            return [bytes.fromhex('00 27 a0')]

        s = Sensor(SPECS['front'], 7, xfer=xfer)
        self.assertEqual(s.read_exposure(), 317)
        self.assertEqual(seen, [[(0, b'\x3e\x00'), (I2C_M_RD, 3)]])

    def test_vts(self):
        self.front.write_vts(12512)
        self.assertEqual(raw_writes(self.x), [bytes.fromhex('32 0e 30 e0')])
        self.assertEqual(self.front.read_vts(), 12512)

    def test_gain_4x(self):
        self.front.write_gain_4x()
        self.assertEqual(raw_writes(self.x), [bytes.fromhex('3e 07 80 0f')])
        x = FakeXfer()
        Sensor(SPECS['rear'], 12, xfer=x).write_gain_4x()
        self.assertEqual(raw_writes(x), [bytes.fromhex('3e 07 80'),
                                         bytes.fromhex('3e 09 0f')])
        self.assertEqual(self.front.read_gain(),
                         {0x3e06: 0, 0x3e07: 0x80, 0x3e08: 0x0f, 0x3e09: 0})

    def test_chip_id(self):
        x = FakeXfer({0x3107: 0xd1, 0x3108: 0x54})
        self.assertTrue(Sensor(SPECS['front'], 7, xfer=x).chip_ok())
        self.assertFalse(Sensor(SPECS['rear'], 12, xfer=x).chip_ok())

    def test_write_error_is_sensor_error(self):
        self.x.nak(1, 121)
        with self.assertRaises(SensorError) as cm:
            self.front.write_exposure(10)
        e = cm.exception
        self.assertIsInstance(e, OSError)
        self.assertEqual((e.sensor, e.reg, e.errno), ('sc820cs', 0x3e00, 121))


class FindBus(unittest.TestCase):
    def test_find_bus_ignores_same_addr_other_driver(self):
        with tempfile.TemporaryDirectory() as root:
            for dev, name in (('4-0036', 'aw88261'), ('12-0036', 'sc1320cs'),
                              ('6-0010', 'dhall-ak09970'), ('i2c-7', None)):
                os.mkdir(os.path.join(root, dev))
                if name:
                    with open(os.path.join(root, dev, 'name'), 'w') as f:
                        f.write(name + '\n')
            self.assertEqual(find_bus('sc1320cs', 0x36, root), 12)
            self.assertIsNone(find_bus('sc820cs', 0x10, root))


if __name__ == '__main__':
    unittest.main()
