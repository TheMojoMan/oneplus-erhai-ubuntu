#!/usr/bin/env python3
# Watch the OnePlus pogo keyboard state and drive the GNOME on-screen
# keyboard: keyboard attached (stream alive) -> OSK off, detached -> OSK on.
#
# Source of truth: the "attached" sysfs attribute exported by the
# oneplus-pogo-kbd driver (kernels with the pinned SW_TABLET_MODE switch).
# On older kernels that attribute does not exist; fall back to watching the
# tablet-mode switch evdev node (its state follows the keyboard there).
import fcntl
import glob
import os
import struct
import subprocess
import time

DRV_GLOB = '/sys/bus/serial/drivers/oneplus-pogo-kbd/*/attached'
SWITCH_NAME = 'OnePlus Pad 3 Tablet Mode Switch'
SCHEMA = 'org.gnome.desktop.a11y.applications'
KEY = 'screen-keyboard-enabled'
POLL_S = 1

# input_event (64-bit timeval on 64-bit): {timeval, type, code, value}
FMT = 'llHHi'
EV_SW = 5
SW_TABLET_MODE = 1


def find_attached_file():
    hits = glob.glob(DRV_GLOB)
    return hits[0] if hits else None


def read_attached(path):
    try:
        return int(open(path).read().strip())
    except (OSError, ValueError):
        return None


def find_switch():
    for d in glob.glob('/sys/class/input/event*/device/name'):
        try:
            if open(d).read().strip() == SWITCH_NAME:
                return '/dev/input/' + d.split('/')[4]
        except OSError:
            pass
    return None


def read_state(fd):
    """Initial SW_TABLET_MODE state via EVIOCGSW(8)."""
    eviocgsw = (2 << 30) | (8 << 16) | (0x45 << 8) | 0x1b
    buf = fcntl.ioctl(fd, eviocgsw, b'\x00' * 8)
    return (buf[0] >> SW_TABLET_MODE) & 1


def graphical_session():
    """(uid, name) of the active graphical session, or None."""
    out = subprocess.run(['loginctl', 'list-sessions', '--no-legend'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if not f:
            continue
        props = subprocess.run(
            ['loginctl', 'show-session', f[0], '-p', 'Active', '-p', 'Type',
             '-p', 'User', '-p', 'Name'],
            capture_output=True, text=True).stdout
        if 'Active=yes' not in props:
            continue
        if 'Type=wayland' not in props and 'Type=x11' not in props:
            continue
        uid = name = None
        for p in props.splitlines():
            if p.startswith('User='):
                uid = p.split('=', 1)[1]
            elif p.startswith('Name='):
                name = p.split('=', 1)[1]
        if uid and name:
            return uid, name
    return None


def set_osk(enable):
    sess = graphical_session()
    if not sess:
        print('no active graphical session; skipping', flush=True)
        return
    uid, name = sess
    env = dict(os.environ,
               DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{uid}/bus',
               XDG_RUNTIME_DIR=f'/run/user/{uid}')
    r = subprocess.run(['runuser', '-u', name, '--', 'gsettings', 'set',
                        SCHEMA, KEY, 'true' if enable else 'false'],
                       env=env, capture_output=True, text=True)
    if r.returncode == 0:
        print(f'{name} (uid {uid}): OSK {"on" if enable else "off"}',
              flush=True)
    else:
        print(f'gsettings failed: {r.stderr.strip()}', flush=True)


def watch_sysfs(path):
    """Poll the driver's attached attribute; OSK off while attached."""
    print(f'polling {path} every {POLL_S} s', flush=True)
    last = read_attached(path)
    while last is None:
        print('attached attr not readable yet; retrying', flush=True)
        time.sleep(POLL_S)
        last = read_attached(path)
    print('initial state: keyboard '
          f'{"attached" if last else "detached"}', flush=True)
    set_osk(not last)
    while True:
        time.sleep(POLL_S)
        val = read_attached(path)
        if val is None or val == last:
            continue
        last = val
        set_osk(not val)


def watch_switch():
    """Fallback for kernels where the switch follows the keyboard."""
    path = find_switch()
    while not path:
        print(f'{SWITCH_NAME} not found yet; retrying in 5 s', flush=True)
        time.sleep(5)
        path = find_switch()
    print(f'watching {path}', flush=True)

    size = struct.calcsize(FMT)
    with open(path, 'rb', buffering=0) as f:
        tablet = read_state(f.fileno())
        print('initial state: '
              f'{"tablet (detached)" if tablet else "laptop (attached)"}',
              flush=True)
        set_osk(bool(tablet))
        while True:
            data = f.read(size)
            if not data or len(data) < size:
                break
            _s, _u, etype, code, value = struct.unpack(FMT, data)
            if etype == EV_SW and code == SW_TABLET_MODE and value in (0, 1):
                tablet = value
                set_osk(bool(tablet))


def main():
    attached_path = find_attached_file()
    if attached_path:
        watch_sysfs(attached_path)
    else:
        watch_switch()


if __name__ == '__main__':
    main()
