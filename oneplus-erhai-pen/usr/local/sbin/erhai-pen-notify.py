#!/usr/bin/python3
# erhai-pen-notify.py — toast the pen state-of-charge on attach/detach.
#
# Watches the CPS8601 wireless pen charger debugfs (kernel +b10):
#   /sys/kernel/debug/cps-wls-charger/{pen_present,soc,ble_mac_addr}
# and shows a 5 s desktop toast whenever the pen is attached to or
# detached from the tablet's charging coil:
#   "Pen attached — 75 %"
#   "Pen detached — 75 %"
#
# The percentage is taken, in order of preference, from
#   1. /run/erhai-pen/soc          — the pen's own battery level read over
#      BLE (GATT 0x180F/0x2A19) by erhai-pen-monitor.py, i.e. what stock
#      Android's charger HAL feeds the driver as `rx_soc`.  Used while the
#      file is fresh (ERHAI_PEN_SOC_FRESH seconds, default 900) and a pen is
#      BLE-connected;
#   2. the charger debugfs `soc`   — the coil chip's POWER packet, which the
#      vendor driver only uses for a charge-full check (may stay 0);
#   otherwise the toast says "battery unknown" instead of a bogus 0 %.
#
# The toast is delivered to the ACTIVE loginctl graphical session user
# (the erhai-desktop-users / pogo-osk pattern), so it works from a root
# systemd unit.  When the charger driver is absent (kernels before +b10)
# the service stays inactive (ConditionPathExists) and this script waits
# briefly in case the driver loads later.

import os
import sys
import time
import subprocess

DEBUGFS = os.environ.get("ERHAI_PEN_DEBUGFS", "/sys/kernel/debug/cps-wls-charger")
STATE_DIR = os.environ.get("ERHAI_PEN_STATE", "/run/erhai-pen")
SOC_FRESH_SECONDS = int(os.environ.get("ERHAI_PEN_SOC_FRESH", "900") or "900")
CONF = "/etc/erhai/pen.conf"
ONCE = "--once" in sys.argv
NO_TOAST = os.environ.get("ERHAI_PEN_NO_TOAST") == "1"
TAG = "erhai-pen-notify"


def log(msg):
    print("%s: %s" % (TAG, msg), flush=True)


def conf_str(key, default):
    try:
        with open(CONF, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    except FileNotFoundError:
        pass
    return default


def read_int(name):
    try:
        with open(os.path.join(DEBUGFS, name), encoding="utf-8") as fh:
            val = fh.read().strip().split()[0]
            if val == "":
                return None
            return int(val, 0)
    except (FileNotFoundError, ValueError, OSError):
        return None


def read_str(name):
    try:
        with open(os.path.join(DEBUGFS, name), encoding="utf-8") as fh:
            return fh.read().strip()
    except (FileNotFoundError, OSError):
        return ""


def ble_soc():
    """Pen state of charge published by erhai-pen-monitor (BLE), or None.

    Uses /run/erhai-pen/soc only while a pen is BLE-connected and the value
    is fresh — the pen sleeps when idle and a stale number would lie.
    """
    try:
        with open(os.path.join(STATE_DIR, "state"), encoding="utf-8") as fh:
            if fh.read().strip() != "1":
                return None
        with open(os.path.join(STATE_DIR, "soc"), encoding="utf-8") as fh:
            pct = int(fh.read().strip().split()[0])
        with open(os.path.join(STATE_DIR, "soc_updated"),
                  encoding="utf-8") as fh:
            age = time.time() - int(fh.read().strip().split()[0])
    except (FileNotFoundError, ValueError, OSError):
        return None
    fresh = SOC_FRESH_SECONDS
    try:
        fresh = int(conf_str("SOC_FRESH_SECONDS", str(SOC_FRESH_SECONDS)))
    except ValueError:
        pass
    if age > fresh:
        return None
    return pct if 0 <= pct <= 100 else None


def soc_now():
    """BLE value if available, else the coil debugfs value (if sane)."""
    soc = ble_soc()
    if soc is not None:
        return soc
    coil = read_int("soc")
    return coil if coil and 0 < coil <= 100 else None


def active_session_user():
    """Return (uid, username) of the active graphical session, or (None, None)."""
    try:
        out = subprocess.run(["loginctl", "list-sessions", "--no-legend"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None, None
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        try:
            info = subprocess.run(
                ["loginctl", "show-session", parts[0],
                 "-p", "Active", "-p", "Type", "-p", "User", "-p", "Name"],
                capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        if "Active=yes" not in info:
            continue
        if "Type=wayland" not in info and "Type=x11" not in info:
            continue
        uid = name = None
        for kv in info.splitlines():
            if kv.startswith("User="):
                uid = kv.split("=", 1)[1]
            elif kv.startswith("Name="):
                name = kv.split("=", 1)[1]
        if uid and name:
            return uid, name
    return None, None


def notify(title, body):
    if NO_TOAST:
        log("toast (suppressed): %s | %s" % (title, body))
        return
    uid, user = active_session_user()
    if not uid or not user:
        log("no active graphical session; toast dropped: %s | %s" % (title, body))
        return
    seconds = int(conf_str("NOTIFY_SECONDS", "5000"))
    icon = conf_str("NOTIFY_ICON", "input-stylus-symbolic")
    cmd = ["runuser", "-u", user, "--", "env",
           "XDG_RUNTIME_DIR=/run/user/%s" % uid,
           "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%s/bus" % uid,
           "notify-send", "-t", str(seconds), "-a", "erhai-pen",
           "-i", icon, title, body]
    try:
        subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        log("toast: %s | %s" % (title, body))
    except OSError as e:
        log("toast failed: %s" % e)


def pen_state():
    return read_int("pen_present"), soc_now()


def main():
    log("started (debugfs=%s)" % DEBUGFS)

    # Give the driver (and thus the debugfs dir) a chance to appear: when the
    # module loads later, or during boot before this unit's condition ran.
    deadline = time.time() + 120
    while not os.path.isdir(DEBUGFS):
        if time.time() > deadline:
            log("charger debugfs never appeared; exiting")
            return 0
        time.sleep(2)

    last_present, last_soc = pen_state()
    if last_present is None:
        last_present = 0
    log("pen_present=%d soc=%s" % (last_present, last_soc))

    # On attach we give the power link up to ~2.5 s so the pen's first
    # POWER frame can land and the toast can show a real %.
    attach_wait_left = 2.5

    while True:
        present, soc = pen_state()
        if present is None:
            time.sleep(0.5)
            continue

        if present != last_present:
            if present == 1:
                # attached: wait briefly for the SoC to appear
                waited = 0.0
                while soc is None and waited < attach_wait_left:
                    time.sleep(0.25)
                    soc = soc_now()
                    waited += 0.25
                body = "%d %%" % soc if soc is not None else "battery unknown"
                notify("Pen attached", body)
            else:
                body = "%d %%" % last_soc if last_soc is not None else "battery unknown"
                notify("Pen detached", body)
            last_present = present

        if soc is not None:
            last_soc = soc

        if ONCE:
            log("done (--once)")
            return 0
        time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())