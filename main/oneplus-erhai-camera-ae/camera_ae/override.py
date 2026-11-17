# SPDX-License-Identifier: GPL-2.0-only
"""Is the user tuning override (libcamera Agc/Adjust disabled) in force?

It is when the running pipewire daemon -- where the camera nodes and their
soft-ISP IPA live -- was started with LIBCAMERA_IPA_CONFIG_PATH pointing
at our tuning dir, and every file libcamera may pick there (the per-sensor
files win over uncalibrated.yaml) lists no Agc and no Adjust.
"""
import os
import re
import subprocess

VAR = 'LIBCAMERA_IPA_CONFIG_PATH'
FILES = ('sc820cs.yaml', 'sc1320cs.yaml', 'uncalibrated.yaml')
FORBIDDEN = {'Agc', 'Adjust'}
DEFAULT_DIR = '/usr/share/oneplus-erhai-camera-ae/ipa'   # deb: system-wide
_ALGO = re.compile(r'^\s*-\s*([A-Za-z][A-Za-z0-9]*)\s*:', re.M)


def read_environ(pid):
    try:
        with open(f'/proc/{pid}/environ', 'rb') as f:
            raw = f.read()
    except OSError:
        return {}
    env = {}
    for item in raw.split(b'\0'):
        k, sep, v = item.partition(b'=')
        if sep:
            env[k.decode(errors='replace')] = v.decode(errors='replace')
    return env


def read_file(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def algorithms(text):
    return _ALGO.findall(text or '')


def override_active(pid, tuning_dir, read_environ=read_environ,
                    read_file=read_file):
    value = read_environ(pid).get(VAR)
    if not value or os.path.normpath(value) != os.path.normpath(tuning_dir):
        return False
    for name in FILES:
        algos = algorithms(read_file(os.path.join(tuning_dir, 'simple', name)))
        if not algos or FORBIDDEN & set(algos):
            return False
    return True


def pipewire_pid(run=subprocess.run):
    try:
        r = run(['systemctl', '--user', 'show', '-p', 'MainPID', '--value',
                 'pipewire.service'], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    try:
        pid = int(r.stdout.strip())
    except ValueError:
        return None
    return pid or None


class OverrideCheck:
    """override_fn for the Engine: checked per call, cached by pipewire pid.

    The environment of a running process cannot change, so one answer per
    pid is final; a new pid (pipewire restarted) is checked afresh."""

    def __init__(self, pid_fn, tuning_dir, log, check=override_active):
        self.pid_fn, self.tuning_dir, self.log, self.check = (
            pid_fn, tuning_dir, log, check)
        self._pid = None
        self._result = False

    def __call__(self):
        pid = self.pid_fn()
        if not pid:
            return False
        if pid != self._pid:
            self._pid = pid
            self._result = bool(self.check(pid, self.tuning_dir))
            if not self._result:
                self.log('info', f'tuning override not active in pipewire '
                         f'(pid {pid}); AE off (libcamera AE in charge)')
        return self._result
