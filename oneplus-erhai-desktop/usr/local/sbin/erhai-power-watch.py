#!/usr/bin/env python3
# Watch the PMIC power key (KEY_POWER) and toggle the panel backlight on each press.
import struct, subprocess, glob, sys, time, os

KEY_POWER = 116
EV_KEY = 1
TOGGLE = ['/usr/local/sbin/erhai-panel-power.sh']
_SURVLOG = '/usr/local/sbin/erhai-surv-log'


def survlog(msg):
    """Append one durable forensic line for this boot (fsync'd)."""
    try:
        subprocess.run([_SURVLOG, msg], timeout=5)
    except Exception:
        pass

def find_pwrkey():
    for d in glob.glob('/sys/class/input/event*/device/name'):
        try:
            if open(d).read().strip() == 'pmic_pwrkey':
                return '/dev/input/' + d.split('/')[4]
        except OSError:
            pass
    return '/dev/input/event0'

DEV = find_pwrkey()
FMT = 'llHHi'
SIZE = struct.calcsize(FMT)
print(f'erhai-power-watch: reading {DEV} (record {SIZE}B)', flush=True)

with open(DEV, 'rb', buffering=0) as f:
    while True:
        data = f.read(SIZE)
        if not data or len(data) < SIZE:
            break
        _s, _u, etype, code, value = struct.unpack(FMT, data)
        if etype == EV_KEY and code == KEY_POWER and value == 1:
            print('KEY_POWER press -> toggle', flush=True)
            survlog('powerwatch KEY_POWER ts=%d' % int(time.time()))
            subprocess.run(TOGGLE)
