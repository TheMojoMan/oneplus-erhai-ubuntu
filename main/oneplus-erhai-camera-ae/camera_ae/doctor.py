# SPDX-License-Identifier: GPL-2.0-only
"""`python3 -m camera_ae.doctor`: is the AE engine set up and in charge?

Checks: the service, the tuning override in the running pipewire, which
tuning file pipewire's libcamera picked this boot, the tuning files, the
sensors (bus, access, chip ID while streaming), and libcamera upgrade drift
since install.
"""
import hashlib
import os
import subprocess
import sys

from .override import DEFAULT_DIR, FILES, FORBIDDEN, VAR, algorithms, \
    pipewire_pid, read_environ, read_file
from .focus import find_lens
from .sensor import SPECS, SensorError, open_sensor

STATE_DIR = '/var/lib/oneplus-erhai-camera-ae'          # deb: written by postinst
RECORD = os.path.join(STATE_DIR, 'installed.txt')
SERVICE = 'erhai-camera-ae.service'
SYSTEM_TUNING = '/usr/share/libcamera/ipa/simple/uncalibrated.yaml'
PACKAGES = ('libcamera0.7', 'libcamera-ipa')


def _run(argv):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ''
    return r.stdout if r.returncode == 0 else ''


def current_record():
    lines = [f'{p}={_run(["dpkg-query", "-W", "-f=${Version}", p]).strip() or "?"}'
             for p in PACKAGES]
    try:
        with open(SYSTEM_TUNING, 'rb') as f:
            sha = hashlib.sha256(f.read()).hexdigest()
    except OSError:
        sha = '?'
    lines.append(f'uncalibrated.yaml={sha}')
    return '\n'.join(lines) + '\n'


def _parse(text):
    out = {}
    for line in text.splitlines():
        k, sep, v = line.partition('=')
        if sep:
            out[k.strip()] = v.strip()
    return out


def install_drift(recorded, current):
    a, b = _parse(recorded), _parse(current)
    return [f'{k}: {a[k]} -> {b.get(k, "?")}' for k in a if a[k] != b.get(k, '?')]


def drift_warnings(path=RECORD, current_fn=current_record):
    try:
        with open(path) as f:
            recorded = f.read()
    except OSError:
        return []
    return install_drift(recorded, current_fn())


def journal_verdict(lines, tuning_dir):
    """Which tuning file did pipewire's libcamera fall back to, if any?"""
    joined = '\n'.join(lines)
    if '/usr/share/libcamera/ipa/' in joined or '/etc/libcamera/ipa/' in joined:
        return 'system'
    if tuning_dir in joined:
        return 'ours'
    return 'no-fallback'          # a per-sensor file was found: nothing logged


def main():
    ok = True

    def report(good, msg, critical=True):
        nonlocal ok
        print(('  ok  ' if good else ('  FAIL' if critical else '  warn')) + ' ' + msg)
        if critical and not good:
            ok = False

    active = _run(['systemctl', '--user', 'is-active', SERVICE]).strip()
    report(active == 'active', f'{SERVICE}: {active or "unknown"}')

    for name in FILES:
        algos = algorithms(read_file(os.path.join(DEFAULT_DIR, 'simple', name)))
        report(bool(algos) and not (FORBIDDEN & set(algos)),
               f'tuning {name}: {", ".join(algos) or "missing"}')

    pid = pipewire_pid()
    env = read_environ(pid) if pid else {}
    report(env.get(VAR) == DEFAULT_DIR,
           f'pipewire (pid {pid}) {VAR}={env.get(VAR, "<unset>")} '
           f'(log out and back in, or reboot, after install to activate)')
    if pid:
        lines = [l for l in _run(['journalctl', '--user', '-b', '-o', 'cat',
                                  f'_PID={pid}']).splitlines() if 'IPAProxy' in l]
        verdict = journal_verdict(lines, DEFAULT_DIR)
        report(verdict != 'system', f'pipewire libcamera tuning lookup: {verdict}')

    for kind, spec in SPECS.items():
        s = open_sensor(kind)
        if s is None:
            report(False, f'{spec.name}: not found on any I2C bus')
            continue
        report(s.writable(), f'{spec.name}: /dev/i2c-{s.bus} read/write access')
        try:
            report(s.chip_ok(), f'{spec.name}: chip ID 0x{spec.chip_id:04x}')
        except SensorError:
            report(True, f'{spec.name}: powered off (not streaming), chip ID '
                   f'not checked', critical=False)

    lens = find_lens()
    report(lens is not None and os.access(lens, os.R_OK | os.W_OK),
           f'rear focus lens (cn3927): {lens or "not found"}'
           + (' read/write' if lens and os.access(lens, os.W_OK) else ''),
           critical=False)

    drift = drift_warnings()
    report(not drift, 'libcamera unchanged since install'
           + ('' if not drift else ': ' + '; '.join(drift)
              + ' -- re-run the post-upgrade checklist'), critical=False)
    print('OK' if ok else 'PROBLEMS FOUND')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
