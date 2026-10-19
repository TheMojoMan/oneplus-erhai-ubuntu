#!/usr/bin/env python3
# erhai — pogo folio fold -> panel backlight off/on.
#
# The OnePlus Pad 3 folio can be folded UP against the screen (closed like a
# book).  This watcher follows the fold signal chosen by calibration
# (/etc/erhai/folio-fold.conf, written by `erhai-folio-calibrate`) and blanks
# the panel with the SAME script the power button uses
# (erhai-display-toggle.sh off), restoring it on unfold (… on).
#
# Lifecycle: the unit is NOT enabled — erhai-folio-gate.service starts it while
# the pogo keyboard is attached and stops it on detach (the oneplus-pogo-kbd
# driver exports `attached` but emits no uevent, so the gate polls it).
#
# Config keys (shell-style KEY=VALUE, '#' comments):
#   ENABLED=1           0/absent => stay idle, wait for a config
#   SIGNAL=mag:abs      mag:x|mag:y|mag:z|mag:abs (libssc stream),
#                       gpio:N (tlmm debugfs line), file:/path (number per poll)
#   POLARITY=above      the folded state is above/below THRESHOLD
#   THRESHOLD=-430.0
#   HYSTERESIS=20.0     dead band around the threshold
#   DWELL_MS=400        fold must persist this long before acting
#   STARTUP_DWELL_MS=1500   longer dwell for the state at service start
#   POLL_HZ=5           only used by gpio:/file: signals
#   MAG_RATE_HZ=5       requested magnetometer rate (ssccli --rate; the SSC
#                       hardware floor is 5 Hz, so 1 Hz requests yield 5 Hz)
#   LOG_SIGNAL=0        1 = log every sample
#   GRAB_TOUCHSCREEN=1  grab the in-cell touchscreen while the panel is
#                       blanked by us (ghost-touch suppression; released on
#                       unfold and when this process exits — the EVIOCGRAB
#                       grab is tied to the fd)
#   TOGGLE_CMD=/usr/local/sbin/erhai-display-toggle.sh
#
# Only the FOLD is ever applied at startup; a start in the open state leaves
# the backlight alone, so a power-button blank is never undone by this service.
import argparse
import fcntl
import glob
import math
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

CONF_DEFAULT = "/etc/erhai/folio-fold.conf"
STATE_DEFAULT = "/run/erhai-folio-fold-blanked"
TOGGLE_DEFAULT = "/usr/local/sbin/erhai-display-toggle.sh"
DEBUGFS_GPIO = "/sys/kernel/debug/gpio"
TLMM_MARK = "f100000.pinctrl"
RE_XYZ = re.compile(r"X=(-?[\d.]+)\s+Y=(-?[\d.]+)\s+Z=(-?[\d.]+)")
CONFIG_RECHECK_S = 5.0
UNCONFIGURED_LOG_S = 60.0

# EVIOCGRAB (evdev): while grabbed, only our fd receives the device's events;
# closing the fd releases the grab automatically, so a crash cannot leave the
# touchscreen stuck.
EVIOCGRAB = 0x40044590
TOUCHSCREEN_NAME = "NVTCapacitiveTouchScreen"


def find_touchscreen_node():
    for p in sorted(glob.glob("/sys/class/input/input*/name")):
        try:
            if open(p).read().strip() != TOUCHSCREEN_NAME:
                continue
        except OSError:
            continue
        nodes = sorted(glob.glob(os.path.join(os.path.dirname(p), "event*")))
        if nodes:
            return "/dev/input/" + os.path.basename(nodes[0])
    return None


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def base_cmd():
    """stdbuf keeps the child line-buffered through a pipe."""
    return ["stdbuf", "-oL"] if shutil.which("stdbuf") else []


def load_conf(path):
    cfg = {}
    try:
        with open(path) as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip().upper()] = v.strip().strip('"').strip("'")
    except OSError:
        return {}
    return cfg


def cfg_float(cfg, key, default):
    try:
        return float(cfg.get(key) or default)
    except (TypeError, ValueError):
        return default


def cfg_int(cfg, key, default):
    try:
        return int(float(cfg.get(key) or default))
    except (TypeError, ValueError):
        return default


# ---------------------------------------------------------------- signals
def read_tlmm_line(want):
    """Value of one TLMM line from the debugfs gpio dump (nan if unknown)."""
    try:
        out = subprocess.run(["cat", DEBUGFS_GPIO], capture_output=True,
                             text=True, timeout=3).stdout
    except Exception:
        return float("nan")
    in_tlmm = False
    for line in out.splitlines():
        if re.match(r"^gpiochip\d+:", line):
            in_tlmm = TLMM_MARK in line
            continue
        if not in_tlmm:
            continue
        m = re.match(r"^\s*gpio(\d+)\s*:\s*(.*)$", line)
        if not m or int(m.group(1)) != want:
            continue
        f = m.group(2).split()
        state = f[1] if (len(f) > 1 and f[0] in ("in", "out")) \
            else (f[0] if f else "?")
        return 1.0 if state == "high" else 0.0 if state == "low" \
            else float("nan")
    return float("nan")


def read_file_number(path):
    try:
        with open(path) as fh:
            return float(fh.read().strip().split()[0])
    except (OSError, ValueError, IndexError):
        return float("nan")


def mag_stream(chan, rate_hz=1.0):
    """Yield the requested magnetometer channel from a live ssccli stream.

    `rate_hz` is passed to ssccli's `--rate` (the erhai libssc patch).  The
    SSC hardware's minimum is 5 Hz (a 1 Hz request comes back at 5 Hz), and a
    concurrent compass claim pushes it up to ~50 Hz regardless of the request.
    """
    exe = None
    while True:
        if exe is None:
            exe = shutil.which("ssccli")
            if exe is None:
                log("ssccli not found (oneplus-erhai-sensors missing?)")
                yield float("nan")
                time.sleep(30)
                continue
        cmd = [*base_cmd(), exe, "--sensor", "magnetometer",
               "--timeout", "86400"]
        if rate_hz > 0:
            cmd += ["--rate", f"{rate_hz:g}"]
        p = subprocess.Popen(cmd,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, bufsize=1, errors="replace")
        try:
            for line in p.stdout:
                m = RE_XYZ.search(line)
                if not m:
                    continue
                x, y, z = (float(m.group(i)) for i in (1, 2, 3))
                if chan == "x":
                    yield x
                elif chan == "y":
                    yield y
                elif chan == "z":
                    yield z
                else:
                    yield math.sqrt(x * x + y * y + z * z)
        finally:
            try:
                p.terminate()
            except Exception:
                pass
            try:
                p.wait(timeout=2)
            except Exception:
                pass
        log("magnetometer stream ended; restarting it")
        time.sleep(1.0)


def poll_stream(kind, arg, hz):
    period = 1.0 / max(0.2, hz)
    while True:
        yield read_tlmm_line(arg) if kind == "gpio" else read_file_number(arg)
        time.sleep(period)


def open_signal(cfg):
    sig = (cfg.get("SIGNAL") or "").strip()
    if not sig:
        return None
    if sig.startswith("mag:"):
        chan = sig.split(":", 1)[1]
        if chan not in ("x", "y", "z", "abs"):
            log(f"bad SIGNAL {sig} (use mag:x|y|z|abs)")
            return None
        return mag_stream(chan, cfg_float(cfg, "MAG_RATE_HZ", 5.0))
    if sig.startswith("gpio:"):
        try:
            line = int(sig.split(":", 1)[1])
        except ValueError:
            log(f"bad SIGNAL {sig} (use gpio:N)")
            return None
        return poll_stream("gpio", line, cfg_float(cfg, "POLL_HZ", 5))
    if sig.startswith("file:"):
        return poll_stream("file", sig.split(":", 1)[1],
                           cfg_float(cfg, "POLL_HZ", 5))
    log(f"unknown SIGNAL {sig}")
    return None


# ---------------------------------------------------------------- watcher
class FoldWatcher:
    def __init__(self, cfg, conf_path, args):
        self.cfg = cfg
        self.conf_path = conf_path
        self.args = args
        self.threshold = cfg_float(cfg, "THRESHOLD", 0.0)
        self.hyst = abs(cfg_float(cfg, "HYSTERESIS", 0.0))
        self.polarity = (cfg.get("POLARITY") or "above").strip().lower()
        self.dwell = max(0.0, cfg_int(cfg, "DWELL_MS", 400) / 1000.0)
        self.startup_dwell = max(
            self.dwell, cfg_int(cfg, "STARTUP_DWELL_MS", 1500) / 1000.0)
        self.log_signal = cfg_int(cfg, "LOG_SIGNAL", 0) == 1
        self.toggle = args.toggle_cmd.split() if args.toggle_cmd else \
            shlex.split(cfg.get("TOGGLE_CMD") or TOGGLE_DEFAULT)
        self.state_file = args.state_file
        self.dry = args.dry_run
        # grab the touchscreen while the panel is blanked by us (the folded
        # keyboard presses the panel and would otherwise ghost-tap the desktop)
        self.grab = (not self.dry) and \
            cfg_int(cfg, "GRAB_TOUCHSCREEN", 1) == 1
        self.touch_node = None
        self.touch_fd = None
        try:
            self.mtime = os.stat(conf_path).st_mtime
        except OSError:
            self.mtime = None
        self.state = None            # None | "open" | "folded"
        # inherit a blank from a previous instance of this service
        self.blanked = (not self.dry) and os.path.exists(self.state_file)
        if self.blanked:
            log("inherited a blanked panel from a previous run")
            self.state = "folded"
        self.pending = None
        self.pending_since = 0.0
        self.stop = False
        self.reload = False
        self.last_conf_check = time.time()

    # -- classification --------------------------------------------------
    def classify(self, v):
        """'folded' / 'open' / None (inside the hysteresis band)."""
        lo = self.threshold - self.hyst / 2.0
        hi = self.threshold + self.hyst / 2.0
        if self.polarity == "below":
            if v <= lo:
                return "folded"
            if v >= hi:
                return "open"
            return None
        if v >= hi:
            return "folded"
        if v <= lo:
            return "open"
        return None

    # -- backlight -------------------------------------------------------
    def run_toggle(self, action):
        if self.dry:
            log(f"[dry-run] would run: {' '.join(self.toggle)} {action}")
            return
        try:
            r = subprocess.run([*self.toggle, action], timeout=10)
            if r.returncode != 0:
                log(f"toggle {action} exited {r.returncode}")
        except Exception as exc:
            log(f"toggle {action} failed: {exc}")

    def set_blanked(self, on):
        self.blanked = on
        if self.dry:
            return
        try:
            if on:
                with open(self.state_file, "w") as fh:
                    fh.write(f"{time.time():.0f}\n")
            elif os.path.exists(self.state_file):
                os.unlink(self.state_file)
        except OSError as exc:
            log(f"state file {self.state_file}: {exc}")

    # -- touchscreen grab (ghost-touch suppression while folded) ------------
    def grab_touch(self):
        if not self.grab or self.touch_fd is not None:
            return
        node = find_touchscreen_node()
        if not node:
            log("touchscreen grab: NVTCapacitiveTouchScreen not found")
            return
        fd = os.open(node, os.O_RDONLY)
        try:
            fcntl.ioctl(fd, EVIOCGRAB, 1)
        except OSError as exc:
            os.close(fd)
            log(f"touchscreen grab failed on {node}: {exc}")
            return
        self.touch_fd, self.touch_node = fd, node
        log(f"touchscreen {node} GRABBED (while folded)")

    def ungrab_touch(self):
        if self.touch_fd is None:
            return
        try:
            fcntl.ioctl(self.touch_fd, EVIOCGRAB, 0)
        except OSError as exc:
            log(f"touchscreen ungrab failed: {exc}")
        try:
            os.close(self.touch_fd)
        except OSError:
            pass
        log(f"touchscreen {self.touch_node} RELEASED")
        self.touch_fd = self.touch_node = None

    def apply(self, cls, initial=False):
        if cls == "folded":
            log("FOLDED (keyboard laid onto the screen) -> backlight off")
            self.run_toggle("off")
            self.set_blanked(True)
            self.grab_touch()
            return
        if not self.blanked:
            log("starts in the OPEN state -> backlight left untouched"
                if initial else
                "UNFOLDED -> the backlight was not blanked by us, no action")
            return
        self.ungrab_touch()
        log("UNFOLDED -> backlight on")
        self.run_toggle("on")
        self.set_blanked(False)

    def restore_on_exit(self):
        if self.blanked:
            log("stopping while blanked -> restoring the backlight")
            self.ungrab_touch()
            self.run_toggle("on")
            self.set_blanked(False)

    # -- main loop -------------------------------------------------------
    def run(self, source, duration=None):
        end = time.time() + duration if duration else None
        first = True
        for v in source:
            if self.stop or (end and time.time() >= end):
                break
            now = time.time()
            # a rewritten config (new thresholds, or ENABLED=0) must take
            # effect promptly, not only on the next fold event
            if now - self.last_conf_check >= 2.0:
                self.last_conf_check = now
                if self.conf_changed():
                    log("config changed on disk -> reloading")
                    self.reload = True
                    break
            if math.isnan(v):
                continue
            if first:
                log(f"first sample: {v:.4f} (state {self.state})")
                first = False
            if self.log_signal:
                log(f"signal {v:.4f} state={self.state}")
            cls = self.classify(v)
            if cls is None or cls == self.state:
                self.pending = None
                continue
            if self.pending != cls:
                self.pending = cls
                self.pending_since = now
                continue
            need = self.startup_dwell if self.state is None else self.dwell
            if now - self.pending_since >= need:
                self.apply(cls, initial=(self.state is None))
                self.state = cls
                self.pending = None

    def conf_changed(self):
        try:
            return os.stat(self.conf_path).st_mtime != self.mtime
        except OSError:
            return False


def unblank_stale(state_file, toggle_cmd=None, dry_run=False):
    """Restore the panel if a previous run left it blanked (marker present)."""
    if not os.path.exists(state_file):
        return False
    log("watcher is idle/disabled while the panel is blanked -> restoring it")
    if not dry_run:
        cmd = shlex.split(toggle_cmd or TOGGLE_DEFAULT)
        try:
            subprocess.run([*cmd, "on"], timeout=10)
        except Exception as exc:
            log(f"restore failed: {exc}")
        try:
            os.unlink(state_file)
        except OSError:
            pass
    return True


# ---------------------------------------------------------------- cli
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="erhai-folio-fold.py",
        description="Blank/restore the panel backlight when the pogo keyboard "
                    "folio is folded onto the screen.")
    ap.add_argument("--config", default=CONF_DEFAULT, metavar="FILE")
    ap.add_argument("--state-file", default=STATE_DEFAULT, metavar="FILE",
                    help="marker file written while we hold the panel blanked")
    ap.add_argument("--toggle-cmd", default=None, metavar="CMD",
                    help="override TOGGLE_CMD (used by tests)")
    ap.add_argument("--dry-run", action="store_true",
                    help="log the transitions but never touch the backlight")
    ap.add_argument("--duration", type=float, default=0.0, metavar="SECS",
                    help="stop after N seconds (used by tests)")
    a = ap.parse_args(argv)

    logged = 0.0
    while True:
        cfg = load_conf(a.config)
        if cfg_int(cfg, "ENABLED", 0) != 1 or not cfg.get("SIGNAL"):
            unblank_stale(a.state_file, a.toggle_cmd or cfg.get("TOGGLE_CMD"),
                          a.dry_run)
            now = time.time()
            if now - logged >= UNCONFIGURED_LOG_S:
                logged = now
                if not os.path.exists(a.config):
                    log(f"no config at {a.config} — run erhai-folio-calibrate")
                else:
                    log(f"{a.config} is not enabled (ENABLED/SIGNAL) — run "
                        "erhai-folio-calibrate")
            if a.duration:
                return 0
            time.sleep(CONFIG_RECHECK_S)
            continue

        source = open_signal(cfg)
        if source is None:
            unblank_stale(a.state_file, a.toggle_cmd or cfg.get("TOGGLE_CMD"),
                          a.dry_run)
            if a.duration:
                return 1
            time.sleep(CONFIG_RECHECK_S)
            continue

        w = FoldWatcher(cfg, a.config, a)
        rate = cfg_float(cfg, "MAG_RATE_HZ", 5.0)
        log(f"signal={cfg['SIGNAL']} polarity={w.polarity} "
            f"threshold={w.threshold:g} hysteresis={w.hyst:g} "
            f"dwell={int(w.dwell * 1000)}ms "
            f"startup_dwell={int(w.startup_dwell * 1000)}ms "
            f"mag_rate={rate:g}Hz "
            f"toggle={' '.join(w.toggle)}")
        signal.signal(signal.SIGTERM, lambda *_: setattr(w, "stop", True))
        signal.signal(signal.SIGINT, lambda *_: setattr(w, "stop", True))
        try:
            w.run(source, a.duration or None)
        except KeyboardInterrupt:
            w.stop = True
        finally:
            try:
                source.close()
            except Exception:
                pass

        if w.reload:
            continue                 # config changed: panel state carries over
        w.restore_on_exit()
        return 0


if __name__ == "__main__":
    sys.exit(main())