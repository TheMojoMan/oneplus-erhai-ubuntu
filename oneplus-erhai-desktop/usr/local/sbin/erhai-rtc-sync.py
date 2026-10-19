#!/usr/bin/env python3
"""erhai: keep the PMK8550 PMIC RTC's epoch offset fresh.

The PMK8550 RTC (driver rtc-pm8xxx) is a 1 Hz free-running counter in the
PMIC's always-on, battery-backed domain: it keeps counting while the pad is
off but has no wall-clock epoch of its own.  Since kernel +b8 the DTB gives it
an "offset" nvmem cell (erhai.dts, PMIC SDAM 0x7150), so

    read_time = counter + offset

and RTC_SET_TIME stores ``system_time - counter`` into that cell.  That is what
makes the clock right from second zero at boot, before WiFi/NTP -- the same
number Android's time_daemon keeps in /mnt/vendor/persist/time/ats_1.

This helper is the only writer:
  * once at boot (erhai-rtc-sync.service, WantedBy=multi-user.target) -- almost
    always a no-op, it re-stores what hctosys just read;
  * hourly (erhai-rtc-sync.timer) -- after systemd-timesyncd has fixed the
    clock, this is what makes the NEXT boot right.
The driver skips a write whose offset changed by < 30 s and flushes it on
shutdown instead, so the cell is not rewritten for ordinary drift.

Safety:
  * refuses to store an implausible clock (< 2020-01-01), so a wrong system
    time can never overwrite a good offset;
  * tolerates a kernel without the offset cell (RTC_SET_TIME -> ENODEV, any
    pre-+b8 kernel): reports it once and exits 0, so a timer never fails;
  * reads the RTC back and logs what it now says.

Options: --dry-run (print, change nothing), --verbose.
Audit/seed the cell by hand with workspace tmp/rtc-offset-probe.py.
"""

import calendar
import errno
import fcntl
import glob
import os
import struct
import sys
import time

RTC_RD_TIME = 0x80247009           # _IOR('p', 0x09, struct rtc_time), 36 bytes
RTC_SET_TIME = 0x4024700a          # _IOW('p', 0x0a, struct rtc_time)
PLAUSIBLE_MIN = 1577836800         # 2020-01-01 UTC
DRIVER_NAME = "rtc-pm8xxx"
TOLERANCE = 5                      # seconds accepted on the read-back


def log(msg):
    print("erhai-rtc-sync: %s" % msg, flush=True)


def find_rtc(verbose=False):
    """The PMK8550 RTC -- never hardcode an index: rtc0/rtc1 depends on probe
    order (a broken rtc-efi used to own rtc0 before +b8)."""
    for d in sorted(glob.glob("/sys/class/rtc/rtc*")):
        try:
            with open(os.path.join(d, "name")) as fh:
                name = fh.read().strip()
        except OSError:
            continue
        if DRIVER_NAME in name:
            dev = "/dev/" + os.path.basename(d)
            if verbose:
                log("using %s (%s)" % (dev, name))
            return dev
    if verbose:
        log("no %s RTC found, falling back to /dev/rtc0" % DRIVER_NAME)
    return "/dev/rtc0"


def fmt(tm9):
    return "%04d-%02d-%02d %02d:%02d:%02d UTC" % (tm9[5] + 1900, tm9[4] + 1,
                                                  tm9[3], tm9[2], tm9[1],
                                                  tm9[0])


def main(argv):
    dry = "--dry-run" in argv
    verbose = "--verbose" in argv or "-v" in argv

    now = int(time.time())
    if now < PLAUSIBLE_MIN:
        log("system clock is %s -- refusing to store it"
            % time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(now)))
        return 0

    dev = find_rtc(verbose)
    gm = time.gmtime(now)
    blob = struct.pack("9i", gm.tm_sec, gm.tm_min, gm.tm_hour, gm.tm_mday,
                       gm.tm_mon - 1, gm.tm_year - 1900, gm.tm_wday,
                       gm.tm_yday, 0)

    try:
        fd = os.open(dev, os.O_RDONLY if dry else os.O_RDWR)
    except OSError as exc:
        log("cannot open %s: %s" % (dev, exc.strerror))
        return 1

    try:
        buf = bytearray(36)
        try:
            fcntl.ioctl(fd, RTC_RD_TIME, buf)
            log("RTC before: %s" % fmt(struct.unpack("9i", bytes(buf))))
        except OSError as exc:
            log("RTC before: unreadable (%s)" % exc.strerror)

        if dry:
            log("DRY RUN: would store %s" % fmt(struct.unpack("9i", blob)))
            return 0

        try:
            fcntl.ioctl(fd, RTC_SET_TIME, bytearray(blob))
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                log("kernel has no RTC offset storage (needs the +b8 DTB "
                    "cell); nothing stored")
                return 0
            log("RTC_SET_TIME failed: %s" % exc.strerror)
            return 1

        buf = bytearray(36)
        fcntl.ioctl(fd, RTC_RD_TIME, buf)
        t = struct.unpack("9i", bytes(buf))
        log("RTC now:    %s" % fmt(t))
        stored = calendar.timegm((t[5] + 1900, t[4] + 1, t[3], t[2], t[1],
                                  t[0], 0, 0, 0))
        if abs(stored - now) > TOLERANCE:
            log("WARNING: read-back differs from the system clock by %d s"
                % (stored - now))
            return 1
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
