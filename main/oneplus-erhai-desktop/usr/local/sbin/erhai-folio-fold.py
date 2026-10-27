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
#   GRAB_TOUCHSCREEN=1  grab the panel's own input devices while the panel is
#                       blanked by us (ghost-touch/-pen suppression; released on
#                       unfold and when this process exits — the EVIOCGRAB grab
#                       is tied to the fd).  The devices are the touchscreen AND
#                       the pen node (GRAB_DEVICES overrides the name list).
#   GRAB_DEVICES=...    comma-separated input device NAMES to suppress while
#                       folded (default: the touchscreen + the pen node, see
#                       GRAB_NAMES)
#   TOGGLE_CMD=/usr/local/sbin/erhai-display-toggle.sh
#
# Only the FOLD is ever applied at startup; a start in the open state leaves
# the backlight alone, so a power-button blank is never undone by this service.
#
# WHO OWNS THE BLANK (2.20): the backlight has TWO owners — this watcher and the
# power button (erhai-panel-power.sh) — and they share ONE owner-tagged state file
# (/run/erhai-panel.state, "<owner> <epoch>", see the note further down).  They
# must never undo each other:
#   * a FOLD only claims the state when the panel was still lit when the fold
#     started (i.e. when OUR toggle is the one that blanked it).  If the panel was
#     already off (power button, idle DPMS) the fold just suppresses the input and
#     remembers that a fold happened — it does NOT write the state, so nothing this
#     service does later (unfold, stop, crash recovery) can light the panel up.
#   * an UNFOLD is a COVER OPEN and always wakes the panel (UNFOLD_RELEASES_
#     POWER_BLANK=1, the default).  When the state is OURS we restore it with our
#     toggle; when the POWER BUTTON owns it we hand it back THROUGH THE POWER PATH
#     (`erhai-panel-power.sh on`), which clears the state and restores the lamp
#     from the brightness memory — so the button's own state machine stays in sync.
# Why the wake (owner report 2026-10-22 evening: *"power button -> display dark
# -> keyboard folded up -> waited -> folded down -> display still dark! -> power
# key -> display lit up"*): the unfold ALREADY wakes the session — the folio's
# own keys are input and gnome-shell answers by enabling the display behind our
# blank (measured: KEY 184/185 + `nvt_ts_panel_power_on` 1 s later) — so holding
# the panel dark leaves an awake desktop behind a black screen, which is both
# confusing and the state that made the blank look broken.  The earlier fix
# (leaving the power blank alone and only clearing the state, no restore) made
# the next press a no-op; restoring through the power path has neither problem.
import argparse
import errno
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
# THE BLANK STATE (2.20): ONE owner-tagged file shared with the power button —
# one line "<owner> <epoch>", owner = power | folio (format defined by
# erhai-panel-power.sh, pinned by tmp/folio-test/panel-state-test.sh).  The file
# EXISTING means "the panel is wanted OFF" and the owner says who asked, which is
# all this watcher and erhai-power-watch.py need to keep from undoing each other.
STATE_DEFAULT = "/run/erhai-panel.state"
TOGGLE_DEFAULT = "/usr/local/sbin/erhai-display-toggle.sh"
POWER_DEFAULT = "/usr/local/sbin/erhai-panel-power.sh"   # the power button path
BL_GLOB = "/sys/class/backlight/*/actual_brightness"
DEBUGFS_GPIO = "/sys/kernel/debug/gpio"
TLMM_MARK = "f100000.pinctrl"
RE_XYZ = re.compile(r"X=(-?[\d.]+)\s+Y=(-?[\d.]+)\s+Z=(-?[\d.]+)")
CONFIG_RECHECK_S = 5.0
UNCONFIGURED_LOG_S = 60.0

# EVIOCGRAB (evdev): while grabbed, only our fd receives the device's events;
# closing the fd releases the grab automatically, so a crash cannot leave the
# touchscreen stuck.
EVIOCGRAB = 0x40044590

# The devices of the PANEL itself.  The touch node alone is NOT enough: while
# the folio lies on the screen the panel's pen node emits frames as well, and
# those reach the compositor as pen activity (measured 2026-10-22 09:23:57 and
# 09:24:34, i.e. DURING a fold whose touch node was already grabbed: libinput
# "event5 - NVTCapacitivePen: Ignoring pressure offset greater than 50%").
# Panel input is also what mutter counts as user activity, so an unsuppressed
# node wakes the session behind a blanked panel.  The keyboard/touchpad of the
# folio are deliberately NOT grabbed: they are usable in the laptop position
# and grabbing them would break typing if the fold were ever mis-detected.
GRAB_NAMES = ("NVTCapacitiveTouchScreen", "NVTCapacitivePen")


def backlight_file(override=None):
    """'actual_brightness' of the internal panel (first backlight class)."""
    if override:
        return override
    for p in sorted(glob.glob(BL_GLOB)):
        return p
    return ""


def state_owner(path=STATE_DEFAULT):
    """The owner recorded in the shared blank state ('power'/'folio'/'').

    Empty string means "no blank", and an unreadable owner is left as-is: the
    caller must treat "file exists but owner is garbage" as blanked (never light a
    panel on state we cannot parse).
    """
    try:
        with open(path) as fh:
            line = fh.readline().split()
    except OSError:
        return ""
    return line[0] if line else ""


def state_set(owner, path=STATE_DEFAULT):
    """Claim the blank for `owner` (see the format note above)."""
    try:
        with open(path, "w") as fh:
            fh.write(f"{owner} {int(time.time())}\n")
    except OSError as exc:
        log(f"cannot write the blank state {path}: {exc}")


def state_clear(path=STATE_DEFAULT):
    """Release the blank."""
    try:
        os.unlink(path)
    except OSError:
        pass


def state_release_if_ours(path=STATE_DEFAULT):
    """Drop the state only while WE own it (never erase the power button's)."""
    if state_owner(path) in ("folio", ""):
        state_clear(path)
        return True
    return False


def read_brightness(override=None):
    """Backlight duty as int, or None when it cannot be read."""
    try:
        with open(backlight_file(override)) as fh:
            return int(fh.read().strip().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def find_input_nodes(names, sysfs_glob="/sys/class/input/input*/name"):
    """{device name: /dev/input/eventN} for the wanted input device names."""
    found = {}
    for p in sorted(glob.glob(sysfs_glob)):
        try:
            with open(p) as fh:
                nm = fh.read().strip()
        except OSError:
            continue
        if nm not in names or nm in found:
            continue
        nodes = sorted(glob.glob(os.path.join(os.path.dirname(p), "event*")))
        if nodes:
            found[nm] = "/dev/input/" + os.path.basename(nodes[0])
    return found


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
        # The POWER path, used when an unfold has to hand a power-button blank
        # back (see the ownership note at the top of this file).
        self.power = args.power_cmd.split() if args.power_cmd else \
            shlex.split(cfg.get("POWER_CMD") or POWER_DEFAULT)
        self.unfold_wakes = cfg_int(cfg, "UNFOLD_RELEASES_POWER_BLANK", 1) == 1
        self.state_file = args.state_file
        self.dry = args.dry_run
        self.bl_file = args.backlight_file
        # suppress the PANEL's own input while the panel is blanked by us (the
        # folded keyboard presses the glass and would otherwise ghost-tap the
        # desktop — touch AND pen node, see GRAB_NAMES)
        self.grab = (not self.dry) and \
            cfg_int(cfg, "GRAB_TOUCHSCREEN", 1) == 1
        extra = (cfg.get("GRAB_DEVICES") or "").strip()
        self.grab_names = tuple(n.strip() for n in extra.split(",") if n.strip()) \
            or GRAB_NAMES
        self.grabbed = {}            # device name -> (fd, node)
        try:
            self.mtime = os.stat(conf_path).st_mtime
        except OSError:
            self.mtime = None
        self.state = None            # None | "open" | "folded"
        # A fold that found the panel ALREADY dark (power button, idle DPMS): the
        # blank is not ours, but the fold still happened — the unfold after it is
        # a cover open and may have to release the POWER button's blank.
        self.dark_fold = False
        # inherit a blank from a previous instance of this service: we own the
        # shared state only while it EXISTS and says so — a garbled owner counts as
        # ours (so a hand-edited file cannot leave a dark panel behind forever),
        # but an ABSENT file is simply "no blank" (state_owner() is "" for both)
        self.blanked = (not self.dry and os.path.exists(self.state_file)
                        and state_owner(self.state_file) in ("folio", ""))
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

    def run_power(self, action):
        """Run the POWER BUTTON's script (erhai-panel-power.sh).

        Opening the folio hands a power-button blank back through this path and
        not through our own toggle: `on` clears the shared blank state, restores
        the lamp from the brightness memory and re-pins the GPU — the button's
        state machine then agrees with the panel, which is what the old
        "restore the lamp, keep the marker" behaviour got wrong.
        """
        if self.dry:
            log(f"[dry-run] would run: {' '.join(self.power)} {action}")
            return
        try:
            r = subprocess.run([*self.power, action], timeout=20)
            if r.returncode != 0:
                log(f"panel-power {action} exited {r.returncode}")
        except Exception as exc:
            log(f"panel-power {action} failed: {exc}")

    def set_blanked(self, on):
        """Claim the shared blank for us, or release it while it is OURS."""
        self.blanked = on
        if self.dry:
            return
        if on:
            state_set("folio", self.state_file)
        else:
            state_release_if_ours(self.state_file)

    # -- blank ownership ---------------------------------------------------
    def blank_owner(self):
        """Whoever already holds the panel off, or None while it is LIT.

        The shared state file names the owner (format note at the top of this
        file): the power button blanks deeply and owns it until its next press,
        this watcher blanks lightly while the folio is folded.  A dark panel with
        NO state at all (idle DPMS, the login greeter, ...) is equally not ours to
        undo, and the backlight value is part of the test on purpose: a STALE state
        next to a lit panel is not a blank, it is state to be cleaned up.
        """
        owner = state_owner(self.state_file)
        if owner == "power":
            return "power button"
        if owner:
            return owner          # "folio" (or a garbled value, reported as-is)
        if read_brightness(self.bl_file) == 0:
            return "another blank (idle DPMS)"
        return None

    def power_button_holds_blank(self):
        return state_owner(self.state_file) == "power"

    # -- input suppression while folded (ghost-touch/-pen) ------------------
    def grab_touch(self):
        if not self.grab or self.grabbed:
            return
        nodes = find_input_nodes(self.grab_names)
        if not nodes:
            log("input suppression: none of %s present"
                % ", ".join(self.grab_names))
            return
        for name, node in sorted(nodes.items()):
            try:
                fd = os.open(node, os.O_RDONLY)
            except OSError as exc:
                log(f"{name} grab: cannot open {node}: {exc}")
                continue
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 1)
            except OSError as exc:
                os.close(fd)
                if exc.errno == errno.EBUSY:
                    log(f"{name} {node} is already grabbed elsewhere "
                        "(power-button input guard)")
                else:
                    log(f"{name} grab failed on {node}: {exc}")
                continue
            self.grabbed[name] = (fd, node)
            log(f"{name} {node} GRABBED (while folded)")

    def ungrab_touch(self):
        for name, (fd, node) in list(self.grabbed.items()):
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 0)
            except OSError as exc:
                log(f"{name} ungrab failed: {exc}")
            try:
                os.close(fd)
            except OSError:
                pass
            log(f"{name} {node} RELEASED")
        self.grabbed = {}

    def apply(self, cls, initial=False):
        if cls == "folded":
            log("FOLDED (keyboard laid onto the screen) -> backlight off")
            owner = self.blank_owner()
            self.run_toggle("off")
            if owner:
                # The panel was already off before we acted: that blank belongs
                # to someone else (the power button), so we must NOT claim it -
                # no marker, and nothing here can undo it later.  We do remember
                # that the FOLD happened, so the unfold can treat it as a cover
                # open (see the unfolded branch).
                self.blanked = False
                self.dark_fold = True
                if owner == "power button":
                    log("panel was already blanked by the power button -> fold "
                        "remembered only, that blank is never claimed by this "
                        "watcher")
                else:
                    log(f"panel was already dark ({owner}) -> fold remembered, "
                        "nothing to claim and nothing to restore")
            else:
                self.set_blanked(True)
                self.dark_fold = False
            self.grab_touch()
            return
        self.ungrab_touch()
        if not self.blanked:
            if self.dark_fold and self.power_button_holds_blank():
                self.dark_fold = False
                if self.unfold_wakes:
                    # A COVER OPEN after a fold that found the panel already dark
                    # by the power button: release it through the POWER path so
                    # the marker goes away with the lamp (see the note at the top).
                    log("UNFOLDED -> the power button's blank is released "
                        "(opening the folio wakes the panel)")
                    self.run_power("on")
                else:
                    log("UNFOLDED -> backlight left off (the power button owns that "
                        "blank; UNFOLD_RELEASES_POWER_BLANK=0)")
                return
            self.dark_fold = False
            log("starts in the OPEN state -> backlight left untouched"
                if initial else
                "UNFOLDED -> the backlight was not blanked by us, no action")
            return
        if self.power_button_holds_blank():
            if self.unfold_wakes:
                # COVER SEMANTICS: opening the folio is the same intent as
                # pressing the power key, and the unfold has already woken the
                # session (the folio's keys are input), so the panel comes back
                # through the POWER path (marker cleared, brightness memory used).
                log("UNFOLDED -> the power button's blank is released "
                    "(opening the folio wakes the panel)")
                self.run_power("on")
            else:
                log("UNFOLDED -> backlight left off (the power button owns that "
                    "blank; UNFOLD_RELEASES_POWER_BLANK=0)")
            self.set_blanked(False)
            return
        log("UNFOLDED -> backlight on")
        self.run_toggle("on")
        self.set_blanked(False)

    def restore_on_exit(self):
        if self.blanked:
            log("stopping while blanked -> restoring the backlight")
            self.ungrab_touch()
            if self.power_button_holds_blank():
                log("panel blank is owned by the power button -> left off")
            else:
                self.run_toggle("on")
            self.set_blanked(False)
        else:
            self.ungrab_touch()

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
    """Restore the panel if a previous run of THIS service left it blanked.

    Only while the shared state says the blank is OURS (owner 'folio' — or a
    garbled owner, which only this service could have written): the power button's
    blank outlives this service and only its next press may undo it.
    """
    owner = state_owner(state_file)
    if owner == "power":
        log("a stale blank state exists, but the power button owns the panel "
            "blank -> leaving it off")
        return False
    if not os.path.exists(state_file):
        return False
    log("watcher is idle/disabled while the panel is blanked -> restoring it")
    if not dry_run:
        cmd = shlex.split(toggle_cmd or TOGGLE_DEFAULT)
        try:
            subprocess.run([*cmd, "on"], timeout=10)
        except Exception as exc:
            log(f"restore failed: {exc}")
        state_release_if_ours(state_file)
    return True


# ---------------------------------------------------------------- cli
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="erhai-folio-fold.py",
        description="Blank/restore the panel backlight when the pogo keyboard "
                    "folio is folded onto the screen.")
    ap.add_argument("--config", default=CONF_DEFAULT, metavar="FILE")
    ap.add_argument("--state-file", default=STATE_DEFAULT, metavar="FILE",
                    help="the SHARED blank state file (owner-tagged; also read and "
                         "written by the power button) — default %s" % STATE_DEFAULT)
    ap.add_argument("--toggle-cmd", default=None, metavar="CMD",
                    help="override TOGGLE_CMD (used by tests)")
    ap.add_argument("--power-cmd", default=None, metavar="CMD",
                    help="override POWER_CMD, the power button's script (tests)")
    ap.add_argument("--backlight-file", default=None, metavar="FILE",
                    help="'actual_brightness' path used to tell a blank apart "
                         "(default: the first /sys/class/backlight/ entry)")
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
            f"suppress={','.join(w.grab_names) if w.grab else 'off'} "
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