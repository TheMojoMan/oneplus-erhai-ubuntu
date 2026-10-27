#!/usr/bin/env python3
# erhai pinned tablet-mode switch.
#
# Mutter only treats the seat as touch-mode — and thus allows display
# auto-rotation — while a SW_TABLET_MODE switch is active, and its seat
# learns switch state from toggle events / initial EVIOCGSW state only.
# Being a tablet is a property of the pad, not of whatever (if any)
# keyboard is clipped onto it, so the pogo keyboard driver deliberately
# does NOT create such a switch; this service provides a virtual uinput
# device with SW_TABLET_MODE pinned on instead. With touch mode always
# active, mutter's PanelOrientationManaged stays true and rotation is
# gated in userspace (erhai-sensors extension: orientation-lock forced
# on keyboard attach, user preference recovered on detach, re-enable
# any time).
#
# The state is written once before the device becomes visible (libinput
# queries EVIOCGSW when mutter adds the device) and re-asserted
# periodically; duplicate reports are harmless.
import fcntl
import os
import struct
import time

UINPUT = '/dev/uinput'

# uinput ioctls ('U' = 0x55)
UI_SET_EVBIT = 0x40045564
UI_SET_SWBIT = 0x4004556d
UI_DEV_CREATE = 0x5501   # _IO('U', 1)
UI_DEV_DESTROY = 0x5502  # _IO('U', 2)

EV_SYN = 0x00
EV_SW = 0x05
SW_TABLET_MODE = 0x01
BUS_VIRTUAL = 0x06

REASSERT_S = 30


def report(fd):
    ev = struct.pack('llHHi', 0, 0, EV_SW, SW_TABLET_MODE, 1)
    syn = struct.pack('llHHi', 0, 0, EV_SYN, 0, 0)
    os.write(fd, ev + syn)


def main():
    while not os.path.exists(UINPUT):
        print(f'{UINPUT} not ready; retrying in 2 s', flush=True)
        time.sleep(2)

    fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SW)
    fcntl.ioctl(fd, UI_SET_SWBIT, SW_TABLET_MODE)

    # uinput_user_dev: name[80], id{bustype,vendor,product,version},
    # ff_effects_max, absmax[64], absmin[64], absfuzz[64], absflat[64]
    setup = struct.pack('80sHHHHi64i64i64i64i',
                        b'erhai Tablet Mode Switch',
                        BUS_VIRTUAL, 0, 0, 0, 0,
                        *([0] * 256))
    os.write(fd, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    print('erhai tablet-mode switch pinned ON (uinput)', flush=True)

    try:
        while True:
            time.sleep(REASSERT_S)
            report(fd)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == '__main__':
    main()