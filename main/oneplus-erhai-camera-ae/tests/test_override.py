import os
import tempfile
import unittest

from camera_ae.override import OverrideCheck, override_active, pipewire_pid

GOOD = '%YAML 1.1\n---\nversion: 1\nalgorithms:\n  - BlackLevel:\n  - Awb:\n...\n'
WITH_AGC = GOOD.replace('  - Awb:\n', '  - Awb:\n  - Agc:\n')


def tuning_dir(root, content=GOOD, skip=()):
    d = os.path.join(root, 'ipa')
    os.makedirs(os.path.join(d, 'simple'))
    for f in ('sc820cs.yaml', 'sc1320cs.yaml', 'uncalibrated.yaml'):
        if f not in skip:
            with open(os.path.join(d, 'simple', f), 'w') as fh:
                fh.write(content)
    return d


class Override(unittest.TestCase):
    def test_override_active_exact(self):
        with tempfile.TemporaryDirectory() as root:
            d = tuning_dir(root)
            env = {'LIBCAMERA_IPA_CONFIG_PATH': d, 'HOME': '/x'}
            self.assertTrue(override_active(1, d, read_environ=lambda p: env))
            self.assertFalse(override_active(1, d, read_environ=lambda p: {}))
            other = {'LIBCAMERA_IPA_CONFIG_PATH': d + '-other'}
            self.assertFalse(override_active(1, d, read_environ=lambda p: other))

    def test_override_requires_tuning_file_without_agc(self):
        with tempfile.TemporaryDirectory() as root:
            d = tuning_dir(root, WITH_AGC)
            env = {'LIBCAMERA_IPA_CONFIG_PATH': d}
            self.assertFalse(override_active(1, d, read_environ=lambda p: env))
        with tempfile.TemporaryDirectory() as root:
            d = tuning_dir(root, skip=('sc820cs.yaml',))
            env = {'LIBCAMERA_IPA_CONFIG_PATH': d}
            self.assertFalse(override_active(1, d, read_environ=lambda p: env))

    def test_override_pid_none_no_log(self):
        logs = []
        chk = OverrideCheck(lambda: None, '/nonexistent',
                            lambda lvl, msg: logs.append(msg),
                            check=lambda pid, d: True)
        self.assertFalse(chk())
        self.assertEqual(logs, [])

    def test_override_rechecked_on_new_pid(self):
        logs, calls = [], []
        pid = [100]
        results = {100: False, 200: True}

        def check(p, d):
            calls.append(p)
            return results[p]

        chk = OverrideCheck(lambda: pid[0], '/d', lambda l, m: logs.append(m),
                            check=check)
        self.assertFalse(chk())
        self.assertFalse(chk())                    # cached: no re-check
        self.assertEqual(calls, [100])
        self.assertEqual(len(logs), 1)
        self.assertIn('tuning override not active', logs[0])
        pid[0] = 200
        self.assertTrue(chk())
        self.assertEqual(calls, [100, 200])

    def test_pipewire_pid(self):
        class R:
            def __init__(self, out, rc=0):
                self.stdout, self.returncode = out, rc
        self.assertEqual(pipewire_pid(run=lambda *a, **k: R('8500\n')), 8500)
        self.assertIsNone(pipewire_pid(run=lambda *a, **k: R('0\n')))
        self.assertIsNone(pipewire_pid(run=lambda *a, **k: R('', 1)))


if __name__ == '__main__':
    unittest.main()
