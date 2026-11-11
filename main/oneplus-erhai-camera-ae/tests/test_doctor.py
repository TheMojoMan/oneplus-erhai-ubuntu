import os
import tempfile
import unittest

from camera_ae import doctor

REC = 'libcamera0.7=0.7.0-1ubuntu2\nlibcamera-ipa=0.7.0-1ubuntu2\nuncalibrated.yaml=abc\n'


class Doctor(unittest.TestCase):
    def test_install_drift(self):
        self.assertEqual(doctor.install_drift(REC, REC), [])
        now = REC.replace('uncalibrated.yaml=abc', 'uncalibrated.yaml=def')
        now = now.replace('libcamera0.7=0.7.0-1ubuntu2', 'libcamera0.7=0.7.1-1')
        self.assertEqual(doctor.install_drift(REC, now),
                         ['libcamera0.7: 0.7.0-1ubuntu2 -> 0.7.1-1',
                          'uncalibrated.yaml: abc -> def'])

    def test_drift_warnings_from_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'installed.txt')
            self.assertEqual(doctor.drift_warnings(path, lambda: REC), [])
            with open(path, 'w') as f:
                f.write(REC)
            self.assertEqual(doctor.drift_warnings(path, lambda: REC), [])
            changed = REC.replace('abc', 'zzz')
            w = doctor.drift_warnings(path, lambda: changed)
            self.assertEqual(len(w), 1)
            self.assertIn('uncalibrated.yaml', w[0])

    def test_journal_verdict(self):
        d = '/usr/share/oneplus-erhai-camera-ae/ipa'
        sys_line = ("WARN IPAProxy ipa_proxy.cpp:192 Configuration file "
                    "'sc820cs.yaml' not found for IPA module 'simple', falling "
                    "back to '/usr/share/libcamera/ipa/simple/uncalibrated.yaml'")
        ours = sys_line.replace('/usr/share/libcamera/ipa', d)
        self.assertEqual(doctor.journal_verdict([sys_line], d), 'system')
        self.assertEqual(doctor.journal_verdict([ours], d), 'ours')
        self.assertEqual(doctor.journal_verdict([], d), 'no-fallback')


if __name__ == '__main__':
    unittest.main()
