#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
erhai — pogo keyboard folio FOLD-state probe  (step 1 of "fold the folio ->
screen off / unfold -> screen on")

The OnePlus Pad 3 magnetic pogo keyboard folio can be used open (keyboard flat
in front of the screen, "laptop mode") or folded UP against the screen (closed
like a book).  We want to turn the screen off while it is folded up and back on
when it is folded down again.  Before any daemon can do that we must know WHICH
signal actually follows the fold.

This script runs a guided A/B/A measurement: it samples every plausible signal
with wall-clock timestamps while the owner keeps the folio OPEN, then FOLDS IT
UP (holds it there for --window seconds), then OPENS IT again, and finally
prints a per-channel verdict (state means/min/max, noise, separation score and
a suggested threshold).

Signals captured
  mag        SSC magnetometer (compass IC) via libssc   <- prime candidate
  accel      SSC accelerometer via libssc               <- attitude / propping
  light      SSC ambient light via libssc               <- control (should not move)
  monitor    iio-sensor-proxy stream (compass heading, orientation, tilt, light)
  attached   pogo-kbd driver sysfs "attached" attribute
  gpio       tlmm GPIO 17/83/85/96 levels (debugfs; needs root)
  kbd        evtest stream of "OnePlus Pad 3 Keyboard"      (needs root)
  touchpad   evtest stream of "OnePlus Pad 3 Touchpad"      (needs root)
  touchscreen evtest stream of the NVT in-cell panel        (needs root)
  kern       kernel log lines from oneplus-pogo-kbd (journalctl -k -f)

Usage
  ./pogo-fold-probe.py                     # guided run, 5 s per state
  ./pogo-fold-probe.py --selftest          # 3 s windows, plumbing check only
  ./pogo-fold-probe.py --watch 20          # live readout of the candidates
  ./pogo-fold-probe.py --analyze DIR       # re-print the report of a capture
  ./pogo-fold-probe.py --window 8 --repeat 2
  ./pogo-fold-probe.py --write-config FILE # report + fold-watcher config

The shipped copy of this script (installed as erhai-folio-probe.py) is what
`erhai-folio-calibrate` runs: it writes /etc/erhai/folio-fold.conf for the
erhai-folio-fold watcher from the best channel this report found.

Notes
  * Root is only needed for GPIO + evtest.  If sudo needs a password the
    script asks for it once (`sudo -v`) and skips those sources otherwise.
  * The script only READS.  It does not touch the backlight or any service.
  * Beeps (canberra-gtk-play) mark the fold/unfold moments so the run works
    even though the screen is covered while the folio is folded up.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import threading
import time

WORKSPACE = "/media/data/develop/erhai"


def _default_out():
    """Workspace-local tmp/ when the dev workspace exists, else /var/lib."""
    env = os.environ.get("ERHAI_FOLIO_PROBE_OUT")
    if env:
        return env
    if os.path.isdir(os.path.dirname(WORKSPACE)):
        return os.path.join(WORKSPACE, "tmp", "pogo-fold-probe")
    return "/var/lib/erhai/folio-probe"


DEFAULT_OUT = _default_out()
ATTACHED_GLOB = "/sys/bus/serial/devices/*/attached"
TLMM_MARK = "f100000.pinctrl"          # gpiochip line of the SM8750 TLMM
GPIO_LINES = (16, 17, 18, 83, 85, 96)  # 17 hall-irq, 83 tx-en, 85 pwr, 96 wake
SOUND_DIR = "/usr/share/sounds/freedesktop/stereo"
DEVNULL = subprocess.DEVNULL

# sources that are started as long-running subprocesses (key -> argv tail)
EVTEST_SOURCES = {
    "kbd": "OnePlus Pad 3 Keyboard",
    "touchpad": "OnePlus Pad 3 Touchpad",
    "touchscreen": "NVTCapacitiveTouchScreen",
}


def say(*a, **kw):
    print(*a, flush=True, **kw)


def beep(name, enabled=True):
    """Play a freedesktop sound; fall back to the terminal bell."""
    if not enabled:
        return
    path = os.path.join(SOUND_DIR, name)
    if os.path.exists(path):
        try:
            subprocess.run(["canberra-gtk-play", "--file", path],
                           stdout=DEVNULL, stderr=DEVNULL, timeout=4)
            return
        except Exception:
            pass
    sys.stdout.write("\a")
    sys.stdout.flush()


def cue(kind, enabled=True):
    if kind == "ready":
        beep("dialog-information.oga", enabled)
    elif kind == "fold":
        for _ in range(3):
            beep("bell.oga", enabled)
            time.sleep(0.12)
    elif kind == "open":
        for _ in range(2):
            beep("complete.oga", enabled)
            time.sleep(0.12)


def banner(text, char="="):
    line = char * max(len(text) + 4, 60)
    say()
    say(line)
    say(f"  {text}")
    say(line)


def countdown(secs, what):
    say(f"    {what}")
    n = int(secs)
    for i in range(n, 0, -1):
        say(f"      ... {i}")
        time.sleep(1.0)


# --------------------------------------------------------------------------
# line parsers: raw tool output line -> {channel: number|string}
# --------------------------------------------------------------------------
RE_XYZ = re.compile(r"X=(-?[\d.]+)\s+Y=(-?[\d.]+)\s+Z=(-?[\d.]+)")
RE_LUX = re.compile(r"([\d.]+)\s+Lux")


def parse_mag(text):
    m = RE_XYZ.search(text)
    if not m:
        return None
    x, y, z = float(m.group(1)), float(m.group(2)), float(m.group(3))
    return {"x": x, "y": y, "z": z, "abs": (x * x + y * y + z * z) ** 0.5}


parse_accel = parse_mag


def parse_light(text):
    m = RE_LUX.search(text)
    return {"lux": float(m.group(1))} if m else None


def parse_monitor(text):
    m = re.search(r"Compass heading changed:\s*(\S+)", text)
    if m:
        try:
            return {"compass_deg": float(m.group(1))}
        except ValueError:
            return None
    m = re.search(r"Light changed:\s*([\d.]+)", text)
    if m:
        return {"lux": float(m.group(1))}
    m = re.search(r"Accelerometer orientation changed:\s*(\S+)", text)
    if m:
        return {"orientation": m.group(1)}
    m = re.search(r"Tilt changed:\s*(\S+)", text)
    if m:
        return {"tilt": m.group(1)}
    m = re.search(r"Proximity sensor measurement:\s*(\S+)", text)
    if m:
        return {"proximity": m.group(1)}
    return None


def parse_attached(text):
    m = re.search(r"(?:attached=)?\s*([01])\s*$", text)
    return {"attached": float(m.group(1))} if m else None


def parse_gpio(text):
    vals = {}
    for m in re.finditer(r"gpio(\d+)=(-?[\d.]+)", text):
        vals[f"gpio{m.group(1)}"] = float(m.group(2))
    return vals or None


def parse_evtest(text):
    """Normalise 'Event: time 1.2, type 1 (EV_KEY), code 30 (KEY_A), value 1'"""
    if not text.startswith("Event:"):
        return None
    t = re.search(r"type\s+(\d+)\s*\(([^)]+)\)", text)
    c = re.search(r"code\s+(\d+)\s*\(([^)]+)\)", text)
    v = re.search(r"value\s+(-?\d+)", text)
    if not (t and c and v):
        if "SYN_REPORT" in text:
            return {"syn": "SYN_REPORT"}
        return None
    return {"event": f"{t.group(2)} {c.group(2)}={v.group(1)}"}


def parse_kern(text):
    m = re.search(r"oneplus-pogo-kbd.*?:\s*(.*)$", text)
    return {"klog": m.group(1).strip()} if m else None


PARSERS = {
    "mag": parse_mag,
    "accel": parse_accel,
    "light": parse_light,
    "monitor": parse_monitor,
    "attached": parse_attached,
    "gpio": parse_gpio,
    "kbd": parse_evtest,
    "touchpad": parse_evtest,
    "touchscreen": parse_evtest,
    "kern": parse_kern,
}

# channels whose values are categorical strings, not numbers
STRING_CHANNELS = {"orientation", "tilt", "proximity", "event", "klog", "syn"}

# set by analyze(write_config=...) so main() can report a usable exit status
LAST_CONFIG_OK = False


# --------------------------------------------------------------------------
# samplers
# --------------------------------------------------------------------------
def base_cmd():
    """stdbuf keeps the child line-buffered through a pipe (block-buffering lies)."""
    return ["stdbuf", "-oL"] if shutil.which("stdbuf") else []


# which live lines are worth showing in --watch mode
TAG_RE = {
    "mag": lambda t: t if "X=" in t else None,
    "accel": lambda t: t if "X=" in t else None,
    "light": lambda t: t if "Lux" in t else None,
    "monitor": lambda t: t if "changed:" in t or "appeared" in t else None,
}


class StreamSampler(threading.Thread):
    """(Re)spawns a command until the deadline; timestamps every output line."""

    def __init__(self, key, cmd, outdir, deadline):
        super().__init__(daemon=True, name=f"sampler-{key}")
        self.key = key
        self.cmd = cmd
        self.path = os.path.join(outdir, f"{key}.log")
        self.deadline = deadline
        self.proc = None
        self.stop_flag = False
        self.lines = 0

    def run(self):
        with open(self.path, "w") as fh:
            while not self.stop_flag and time.time() < self.deadline:
                try:
                    self.proc = subprocess.Popen(
                        self.cmd, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, bufsize=1,
                        errors="replace")
                except FileNotFoundError:
                    fh.write(f"{time.time():.3f} ERROR tool missing: {self.cmd[0]}\n")
                    return
                except Exception as exc:            # defensive
                    fh.write(f"{time.time():.3f} ERROR spawn failed: {exc}\n")
                    return
                try:
                    for line in self.proc.stdout:
                        fh.write(f"{time.time():.3f} {line.rstrip()}\n")
                        self.lines += 1
                        if self.stop_flag or time.time() >= self.deadline:
                            break
                except Exception:
                    pass
                finally:
                    fh.flush()
                    try:
                        self.proc.terminate()
                    except Exception:
                        pass
                if self.stop_flag or time.time() >= self.deadline:
                    break
                time.sleep(0.3)                     # tool exited early -> respawn

    def stop(self):
        self.stop_flag = True
        p = self.proc
        if p and p.poll() is None:
            try:
                p.terminate()
            except Exception:
                pass


class Poller(threading.Thread):
    """Calls a function every `interval` s, writing 'ts k=v k=v' lines."""

    def __init__(self, key, fn, outdir, deadline, interval=0.2):
        super().__init__(daemon=True, name=f"poller-{key}")
        self.key = key
        self.fn = fn
        self.path = os.path.join(outdir, f"{key}.log")
        self.deadline = deadline
        self.interval = interval
        self.stop_flag = False

    def run(self):
        with open(self.path, "w") as fh:
            while not self.stop_flag and time.time() < self.deadline:
                try:
                    vals = self.fn()
                except Exception as exc:
                    vals = {"error": repr(exc)}
                if vals:
                    body = " ".join(f"{k}={v:g}" if isinstance(v, (int, float))
                                    else f"{k}={v}" for k, v in vals.items())
                    fh.write(f"{time.time():.3f} {body}\n")
                    fh.flush()
                time.sleep(self.interval)

    def stop(self):
        self.stop_flag = True


def read_attached(dev_glob=ATTACHED_GLOB):
    for p in sorted(glob.glob(dev_glob)):
        try:
            with open(p) as fh:
                return {"attached": float(fh.read().strip())}
        except Exception:
            continue
    return None


def make_gpio_reader(sudo):
    """Parse the TLMM section of debugfs gpio for the interesting lines."""
    def reader():
        out = subprocess.run([*sudo, "cat", "/sys/kernel/debug/gpio"],
                             capture_output=True, text=True, timeout=3).stdout
        in_tlmm = False
        vals = {}
        for line in out.splitlines():
            if re.match(r"^gpiochip\d+:", line):
                in_tlmm = TLMM_MARK in line
                continue
            if not in_tlmm:
                continue
            m = re.match(r"^\s*gpio(\d+)\s*:\s*(.*)$", line)
            if not m or int(m.group(1)) not in GPIO_LINES:
                continue
            f = m.group(2).split()
            state = f[1] if (len(f) > 1 and f[0] in ("in", "out")) \
                else (f[0] if f else "?")
            vals[f"gpio{m.group(1)}"] = {"high": 1.0, "low": 0.0}.get(state, -1.0)
        return vals or None
    return reader


def find_event_node(name):
    for p in sorted(glob.glob("/sys/class/input/input*/name")):
        try:
            with open(p) as fh:
                nm = fh.read().strip()
        except Exception:
            continue
        if name in nm:
            nodes = sorted(glob.glob(os.path.join(os.path.dirname(p), "event*")))
            if nodes:
                return "/dev/input/" + os.path.basename(nodes[0])
    return None


# --------------------------------------------------------------------------
# root / tool discovery
# --------------------------------------------------------------------------
def resolve_sudo(need_root):
    """argv prefix for root commands; [] when already root; None when unusable."""
    if not need_root:
        return []
    if os.geteuid() == 0:
        return []
    if shutil.which("sudo") is None:
        say("!! sudo not found — GPIO and evtest sources disabled")
        return None
    if subprocess.run(["sudo", "-n", "true"], stdout=DEVNULL,
                      stderr=DEVNULL).returncode == 0:
        return ["sudo", "-n"]
    if sys.stdin.isatty():
        say("Root is needed for the GPIO + evtest captures; asking sudo once ...")
        if subprocess.run(["sudo", "-v"]).returncode == 0:
            return ["sudo", "-n"]
    say("!! sudo unavailable (no cached credentials) — GPIO/evtest sources disabled")
    say("   Run `sudo -v` once first (or run this script as root) to include them.")
    return None


# --------------------------------------------------------------------------
# guided capture
# --------------------------------------------------------------------------
def take_window(name, dur, windows):
    t0 = time.time()
    end = t0 + dur
    say(f"    [window {name}] sampling {dur:g} s ...")
    while True:
        left = end - time.time()
        if left <= 0:
            break
        say(f"      {int(left) + 1} s")
        time.sleep(min(1.0, left))
    t1 = time.time()
    windows.append((name, t0, t1))
    say(f"    [window {name}] done ({t1 - t0:.2f} s)")


def build_timeline(repeat, window, settle, lead):
    """-> (steps, estimated_seconds)  steps: ('say'|'cue'|'wait'|'window', arg, dur)"""
    steps = []
    steps.append(("say", "STEP 0 — put the tablet down with the folio OPEN "
                         "(keyboard flat in front of the screen), hands off.", 0))
    steps.append(("cue", "ready", 0))
    steps.append(("wait", "sampling starts in", lead))
    for i in range(1, repeat + 1):
        steps.append(("window", f"open{i - 1}", window))
        steps.append(("say", f">>> FOLD THE KEYBOARD UP NOW — lay it onto the "
                             f"screen (cycle {i}/{repeat}) <<<", 0))
        steps.append(("cue", "fold", 0))
        steps.append(("wait", "hold it folded", settle))
        steps.append(("window", f"folded{i}", window))
        steps.append(("say", ">>> FOLD IT BACK NOW — open the folio again, "
                             "keyboard flat in front <<<", 0))
        steps.append(("cue", "open", 0))
        steps.append(("wait", "back to the open position", settle))
    steps.append(("window", f"open{repeat}", window))
    total = sum(s[2] for s in steps)
    return steps, total


def start_sources(outdir, deadline, sudo):
    """Start every sampler; returns the list of threads to stop later."""
    threads = []
    base = base_cmd()

    streams = [
        ("mag", ["ssccli", "--sensor", "magnetometer", "--timeout",
                 str(int(deadline - time.time()) + 5)]),
        ("accel", ["ssccli", "--sensor", "accelerometer", "--timeout",
                   str(int(deadline - time.time()) + 5)]),
        ("light", ["ssccli", "--sensor", "light", "--timeout",
                   str(int(deadline - time.time()) + 5)]),
        ("monitor", ["monitor-sensor", "--all"]),
    ]
    for key, cmd in streams:
        exe = shutil.which(cmd[0])
        if not exe:
            say(f"!! {cmd[0]} not found — '{key}' source skipped")
            continue
        t = StreamSampler(key, [*base, exe, *cmd[1:]], outdir, deadline)
        t.start()
        threads.append(t)

    # kernel log lines of the pogo driver (user is in 'adm')
    if shutil.which("journalctl"):
        t = StreamSampler("kern", ["stdbuf", "-oL", "journalctl", "-k", "-f",
                                   "-n", "0"], outdir, deadline)
        t.start()
        threads.append(t)

    t = Poller("attached", read_attached, outdir, deadline, interval=0.2)
    t.start()
    threads.append(t)

    if sudo is not None:
        t = Poller("gpio", make_gpio_reader(sudo), outdir, deadline, interval=0.25)
        t.start()
        threads.append(t)
        if not shutil.which("evtest"):
            say("!! evtest not found — input-event sources skipped")
        else:
            for key, name in EVTEST_SOURCES.items():
                node = find_event_node(name)
                if not node:
                    say(f"!! input device '{name}' not present — '{key}' skipped")
                    continue
                t = StreamSampler(key, [*base, *sudo, "evtest", node],
                                  outdir, deadline)
                t.start()
                threads.append(t)
    return threads


# --------------------------------------------------------------------------
# capture driver
# --------------------------------------------------------------------------
def write_meta(outdir, a, sudo, windows, nodes, extra=()):
    with open(os.path.join(outdir, "meta.txt"), "w") as fh:
        fh.write(f"date            {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        fh.write(f"kernel          {os.uname().release}\n")
        fh.write(f"host            {os.uname().nodename}\n")
        fh.write(f"window          {a.window:g} s\n")
        fh.write(f"settle          {a.settle:g} s\n")
        fh.write(f"cycles          {a.repeat}\n")
        fh.write(f"root sources    {'yes' if sudo is not None else 'no'}\n")
        fh.write(f"event nodes     {nodes}\n")
        for key, val in extra:
            fh.write(f"{key:<15} {val}\n")
    with open(os.path.join(outdir, "windows.tsv"), "w") as fh:
        fh.write("# name\tstart_epoch\tend_epoch\tseconds\n")
        for name, t0, t1 in windows:
            fh.write(f"{name}\t{t0:.3f}\t{t1:.3f}\t{t1 - t0:.3f}\n")


def run_capture(a):
    sudo = resolve_sudo(not a.no_root)
    steps, total = build_timeline(a.repeat, a.window, a.settle, a.lead)
    outdir = a.out or os.path.join(DEFAULT_OUT, time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)

    nodes = {k: find_event_node(v) for k, v in EVTEST_SOURCES.items()}
    banner("erhai pogo-folio FOLD probe")
    say(f"  state window : {a.window:g} s")
    say(f"  settle time  : {a.settle:g} s")
    say(f"  fold cycles  : {a.repeat}")
    say(f"  total time   : ~{total:.0f} s")
    say(f"  output dir   : {outdir}")
    say(f"  root sources : {'yes (gpio + evtest)' if sudo is not None else 'NO'}")
    if a.selftest:
        say("  SELFTEST     : plumbing check — the fold instructions may be "
            "ignored")
    say()
    say("  The probe only READS sensors; it never touches the backlight.")
    say("  Beeps mark the fold / unfold moments (the screen is covered while "
        "folded).")

    deadline = time.time() + total + 30
    threads = start_sources(outdir, deadline, sudo)
    windows = []
    try:
        for step in steps:
            kind = step[0]
            if kind == "say":
                say()
                say("  " + step[1])
            elif kind == "cue":
                cue(step[1], not a.no_beep)
            elif kind == "wait":
                say(f"    ({step[1]} — {step[2]:g} s)")
                time.sleep(step[2])
            elif kind == "window":
                take_window(step[1], step[2], windows)
    except KeyboardInterrupt:
        say()
        say("!! interrupted by the user — stopping the samplers")
    finally:
        for t in threads:
            t.stop()
        time.sleep(0.5)
        for t in threads:
            t.join(timeout=2.0)

    write_meta(outdir, a, sudo, windows, nodes)
    say()
    say("  capture complete — analysing ...")
    return outdir


# --------------------------------------------------------------------------
# live watch mode
# --------------------------------------------------------------------------
def run_watch(secs, sudo):
    banner(f"live fold-signal readout for {secs:g} s "
           "(fold the keyboard up/down and watch what moves)")
    stop = threading.Event()

    def stream(tag, cmd):
        try:
            p = subprocess.Popen([*base_cmd(), *cmd], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, bufsize=1)
        except Exception as exc:
            say(f"!! {tag}: {exc}")
            return
        for line in p.stdout:
            if stop.is_set():
                break
            m = TAG_RE.get(tag, lambda t: t)(line.rstrip())
            if m:
                say(f"[{time.strftime('%H:%M:%S')}] {tag:14s} {m}")
        try:
            p.terminate()
        except Exception:
            pass

    threads = []
    for tag, cmd in (("mag", ["ssccli", "--sensor", "magnetometer",
                              "--timeout", str(int(secs) + 3)]),
                     ("accel", ["ssccli", "--sensor", "accelerometer",
                                "--timeout", str(int(secs) + 3)]),
                     ("light", ["ssccli", "--sensor", "light",
                                "--timeout", str(int(secs) + 3)]),
                     ("monitor", ["monitor-sensor", "--all"])):
        if not shutil.which(cmd[0]):
            continue
        t = threading.Thread(target=stream, args=(tag, cmd), daemon=True)
        t.start()
        threads.append(t)

    end = time.time() + secs
    try:
        while time.time() < end:
            vals = read_attached() or {}
            if sudo is not None:
                try:
                    vals.update(make_gpio_reader(sudo)() or {})
                except Exception:
                    pass
            body = " ".join(f"{k}={v:g}" if isinstance(v, (int, float))
                            else f"{k}={v}" for k, v in vals.items())
            say(f"[{time.strftime('%H:%M:%S')}] {'state':14s} {body}")
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=2.0)
    say("watch finished")


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------
def load_samples(outdir):
    """-> {source: [(ts, {channel: value}), ...]}"""
    data = {}
    for key, parser in PARSERS.items():
        path = os.path.join(outdir, f"{key}.log")
        if not os.path.exists(path):
            continue
        samples = []
        with open(path, errors="replace") as fh:
            for line in fh:
                parts = line.rstrip("\n").split(" ", 1)
                if len(parts) != 2:
                    continue
                try:
                    ts = float(parts[0])
                except ValueError:
                    continue
                try:
                    vals = parser(parts[1])
                except Exception:
                    vals = None
                if vals:
                    samples.append((ts, vals))
        data[key] = samples
    return data


def load_windows(outdir):
    wins = []
    with open(os.path.join(outdir, "windows.tsv")) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            name, t0, t1, _secs = line.split("\t")
            wins.append((name, float(t0), float(t1)))
    return wins


def bucket(data, windows):
    """-> {source: {channel: {window: [values]}}}"""
    out = {}
    for src, samples in data.items():
        chans = out.setdefault(src, {})
        for ts, vals in samples:
            win = None
            for name, t0, t1 in windows:
                if t0 <= ts <= t1:
                    win = name
                    break
            if win is None:
                continue
            for ch, v in vals.items():
                chans.setdefault(ch, {}).setdefault(win, []).append(v)
    return out


def num_stats(vals):
    n = len(vals)
    if n == 0:
        return None
    mean = sum(vals) / n
    sd = (sum((v - mean) ** 2 for v in vals) / n) ** 0.5
    return {"n": n, "mean": mean, "sd": sd, "lo": min(vals), "hi": max(vals)}


def verdict_of(so, sf, delta, gap):
    if min(so["n"], sf["n"]) < 3:
        return "NO DATA"
    if gap is not None:
        return "CLEAN SEPARATION"
    if so["sd"] == 0 and sf["sd"] == 0 and delta > 0:
        return "CLEAN SEPARATION"
    noise = max(so["sd"], sf["sd"], 1e-9)
    score = delta / noise
    if score >= 5:
        return "STRONG"
    if score >= 2:
        return "USABLE"
    if score >= 1:
        return "WEAK"
    return "none"


def fold_signal_for(src, ch):
    """Map a probe channel onto an erhai-folio-fold.conf SIGNAL."""
    if src == "mag" and ch in ("x", "y", "z", "abs"):
        return f"mag:{ch}"
    if src == "gpio" and ch.startswith("gpio"):
        return "gpio:" + ch[4:]
    return None


def pick_fold_channel(good):
    """Best channel the fold daemon can actually read.

    Prefer mag:abs — the magnetometer vector length is fixed in the tablet's
    own frame, so it barely moves when the pad is rotated/moved and only steps
    when the folio magnets change geometry (i.e. on the fold).
    """
    for r in good:
        if (r["src"], r["ch"]) == ("mag", "abs") and \
                r["verdict"] in ("CLEAN SEPARATION", "STRONG"):
            return r
    for r in good:
        if fold_signal_for(r["src"], r["ch"]):
            return r
    return None


def write_fold_config(path, outdir, row):
    """Write the daemon config for @row; returns the SIGNAL string used."""
    sig = fold_signal_for(row["src"], row["ch"])
    so, sf = row["so"], row["sf"]
    thr = (sf["mean"] + so["mean"]) / 2.0
    polarity = "above" if sf["mean"] > so["mean"] else "below"
    delta = abs(sf["mean"] - so["mean"])
    noise = max(so["sd"], sf["sd"])
    hyst = min(max(3.0 * noise, 0.1 * delta), 0.5 * delta)
    if not hyst > 0:
        hyst = max(abs(thr) * 0.02, 1.0)
    body = [
        "# erhai folio-fold watcher config — generated by erhai-folio-calibrate",
        f"# date            {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"# capture         {outdir}",
        f"# channel         {row['src']}.{row['ch']}  "
        f"({row['verdict']}, score {row['score']:.1f})",
        f"#   open          {so['mean']:+.4f} ± {so['sd']:.4f} "
        f"[{so['lo']:.4f} .. {so['hi']:.4f}] n={so['n']}",
        f"#   folded        {sf['mean']:+.4f} ± {sf['sd']:.4f} "
        f"[{sf['lo']:.4f} .. {sf['hi']:.4f}] n={sf['n']}",
        "#",
        "# ENABLED=0 disables the watcher without stopping the service.",
        "ENABLED=1",
        "",
        "# signal: mag:x|mag:y|mag:z|mag:abs (libssc stream),",
        "#         gpio:N (tlmm debugfs line), file:/path (one number per poll)",
        f"SIGNAL={sig}",
        "# folded state is the side of THRESHOLD given by POLARITY",
        f"POLARITY={polarity}",
        f"THRESHOLD={thr:.6f}",
        f"HYSTERESIS={hyst:.6f}",
        "",
        "# fold must persist this long before the backlight is switched",
        "DWELL_MS=400",
        "# extra dwell applied to the state seen at service start",
        "STARTUP_DWELL_MS=1500",
        "# poll rate for gpio:/file: signals (mag uses the sensor stream)",
        "POLL_HZ=5",
        "# magnetometer sample rate requested from ssccli (SSC hardware floor)",
        "MAG_RATE_HZ=5",
        "# grab the panel's own input while the panel is blanked by the fold",
        "# (touchscreen AND pen node; GRAB_DEVICES overrides the name list)",
        "GRAB_TOUCHSCREEN=1",
        "",
        "LOG_SIGNAL=0",
        "TOGGLE_CMD=/usr/local/sbin/erhai-display-toggle.sh",
    ]
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w") as fh:
        fh.write("\n".join(body) + "\n")
    return sig


def analyze(outdir, write_config=None):
    windows = load_windows(outdir)
    if not windows:
        return f"no windows.tsv in {outdir} — nothing to analyse"
    data = load_samples(outdir)
    buckets = bucket(data, windows)
    open_wins = [w for w, _, _ in windows if w.startswith("open")]
    fold_wins = [w for w, _, _ in windows if w.startswith("folded")]

    out = []
    add = out.append
    add("=" * 78)
    add(f"erhai pogo-folio FOLD probe — report")
    add("=" * 78)
    add(f"capture      : {outdir}")
    add(f"states       : open  = {', '.join(open_wins)}")
    add(f"               folded= {', '.join(fold_wins)}")
    add(f"windows      : " + "  ".join(
        f"{n}({(t1 - t0):.1f}s)" for n, t0, t1 in windows))
    add("")
    add("samples per source (total / inside a window):")
    for key in PARSERS:
        path = os.path.join(outdir, f"{key}.log")
        if not os.path.exists(path):
            add(f"  {key:12s} {'-':>6s} / {'-':>6s}   <-- MISSING FILE "
                "(tool missing or source skipped)")
            continue
        samples = data.get(key, [])
        inside = sum(1 for ts, _v in samples
                     if any(t0 <= ts <= t1 for _, t0, t1 in windows))
        flag = ""
        if not samples:
            if key in EVTEST_SOURCES:
                flag = "   <-- device opened, NO input events in the capture"
            elif key == "kern":
                flag = "   <-- no oneplus-pogo-kbd log lines in the capture"
            else:
                flag = "   <-- file present but nothing parsed"
        add(f"  {key:12s} {len(samples):6d} / {inside:6d}{flag}")

    att = buckets.get("attached", {}).get("attached", {})
    att_vals = [v for w, _, _ in windows for v in att.get(w, [])]
    if att_vals:
        if max(att_vals) < 1:
            add("  !! WARNING: the pogo keyboard read DETACHED during the whole "
                "run —")
            add("     these results say nothing about the folio fold. Re-dock "
                "the keyboard and re-run.")
        elif min(att_vals) < 1:
            add("  !! NOTE: attached went 0 during the run (keyboard or its "
                "stream dropped) — check 'attached' below.")
        else:
            add("  keyboard: continuously ATTACHED during the run (good)")
    else:
        add("  !! no 'attached' samples — cannot confirm the keyboard stayed "
            "docked")

    # ---------------- numeric channels ----------------
    rows = []
    for src, chans in buckets.items():
        for ch, per in chans.items():
            if ch in STRING_CHANNELS:
                continue
            folded = [v for w in fold_wins for v in per.get(w, [])]
            opened = [v for w in open_wins for v in per.get(w, [])]
            sf, so = num_stats(folded), num_stats(opened)
            if not sf or not so:
                continue
            delta = abs(sf["mean"] - so["mean"])
            gap = None
            if sf["lo"] > so["hi"]:
                gap = sf["lo"] - so["hi"]
            elif so["lo"] > sf["hi"]:
                gap = so["lo"] - sf["hi"]
            noise = max(so["sd"], sf["sd"], 1e-9)
            rows.append({"score": delta / noise, "src": src, "ch": ch,
                         "sf": sf, "so": so, "delta": delta, "gap": gap,
                         "verdict": verdict_of(so, sf, delta, gap),
                         "per": per})
    rows.sort(key=lambda r: -r["score"])
    add("")
    add("-" * 78)
    add("NUMERIC CHANNELS  (open = union of open windows, folded = union of "
        "folded windows)")
    add("-" * 78)
    if not rows:
        add("  (no numeric channel produced samples)")
    for r in rows:
        add(f"  {r['src']}.{r['ch']}")
        add(f"      open   : mean {r['so']['mean']:+12.4f}  sd {r['so']['sd']:9.4f}"
            f"  range [{r['so']['lo']:.2f} .. {r['so']['hi']:.2f}]  n={r['so']['n']}")
        add(f"      folded : mean {r['sf']['mean']:+12.4f}  sd {r['sf']['sd']:9.4f}"
            f"  range [{r['sf']['lo']:.2f} .. {r['sf']['hi']:.2f}]  n={r['sf']['n']}")
        gaptxt = f"  gap {r['gap']:.4f}" if r["gap"] is not None else ""
        add(f"      delta {r['delta']:.4f}  score {r['score']:.2f}"
            f"{gaptxt}   ==> {r['verdict']}")
        strip = "  ".join(
            f"{w}={wmean(r['per'].get(w))}"
            for w, _, _ in windows if r["per"].get(w))
        if strip:
            add(f"      per-window means: {strip}")

    # ---------------- categorical / event channels ----------------
    add("")
    add("-" * 78)
    add("EVENT / CATEGORICAL CHANNELS")
    add("-" * 78)
    any_cat = False
    for src in sorted(buckets):
        for ch in sorted(buckets[src]):
            if ch not in STRING_CHANNELS:
                continue
            per = buckets[src][ch]
            if not any(per.values()):
                continue
            any_cat = True
            counts = "  ".join(f"{w}={len(per.get(w, []))}"
                               for w, _, _ in windows)
            fold_vals = sorted({v for w in fold_wins for v in per.get(w, [])})
            open_vals = sorted({v for w in open_wins for v in per.get(w, [])})
            add(f"  {src}.{ch}")
            add(f"      counts : {counts}")
            add(f"      open   : {', '.join(open_vals) if open_vals else '-'}")
            add(f"      folded : {', '.join(fold_vals) if fold_vals else '-'}")
            only_f = sorted(set(fold_vals) - set(open_vals))
            only_o = sorted(set(open_vals) - set(fold_vals))
            if only_f:
                add(f"      FOLDED-ONLY   : {', '.join(only_f)}")
            if only_o:
                add(f"      OPEN-ONLY     : {', '.join(only_o)}")
    if not any_cat:
        add("  (no events / categorical values captured)")

    # ---------------- recommendation ----------------
    add("")
    add("=" * 78)
    add("RECOMMENDATION")
    add("=" * 78)
    good = [r for r in rows if r["verdict"] in ("CLEAN SEPARATION", "STRONG",
                                                "USABLE")]
    if good:
        b = good[0]
        thr = (b["sf"]["mean"] + b["so"]["mean"]) / 2.0
        direction = "HIGHER" if b["sf"]["mean"] > b["so"]["mean"] else "LOWER"
        add(f"  best single-channel fold detector: {b['src']}.{b['ch']} "
            f"({b['verdict']}, score {b['score']:.1f})")
        add(f"    open   {b['so']['mean']:+.4f}  (sd {b['so']['sd']:.4f}, "
            f"range [{b['so']['lo']:.3f} .. {b['so']['hi']:.3f}])")
        add(f"    folded {b['sf']['mean']:+.4f}  (sd {b['sf']['sd']:.4f}, "
            f"range [{b['sf']['lo']:.3f} .. {b['sf']['hi']:.3f}])")
        add(f"    folded is {direction} -> threshold ~ {thr:+.4f}")
        others = [r for r in good[1:]]
        if others:
            add("  other usable channels: " + ", ".join(
                f"{r['src']}.{r['ch']}({r['score']:.1f})" for r in others[:6]))
        add("  next step: a watcher can poll this channel and drive the "
            "backlight/DPMS on the transition.")
    else:
        add("  No single channel separated the two states cleanly.")
        add("  Look at the per-window means above for drift, check the raw "
            "logs in this directory,")
        add("  and consider a longer --window / more --repeat cycles before "
            "giving up on these sensors.")
    if write_config:
        global LAST_CONFIG_OK
        choice = pick_fold_channel(good)
        add("")
        if choice:
            sig = write_fold_config(write_config, outdir, choice)
            LAST_CONFIG_OK = True
            add(f"  config written : {write_config}")
            add(f"    SIGNAL={sig}  (polarity/threshold/hysteresis from the "
                "stats above)")
        else:
            LAST_CONFIG_OK = False
            try:
                with open(write_config, "w") as fh:
                    fh.write("# erhai folio-fold watcher config\n"
                             f"# no usable fold channel found by the probe on "
                             f"{time.strftime('%Y-%m-%d %H:%M:%S')}\n"
                             f"# capture: {outdir}\n"
                             "ENABLED=0\n")
                add(f"  config written : {write_config} (ENABLED=0)")
            except OSError as exc:
                add(f"  config NOT written: {exc}")
            add("  no channel the fold watcher can read separated the states;")
            add("  the watcher stays idle until a config with ENABLED=1 exists")
    add("")
    return "\n".join(out)


def wmean(vals):
    if not vals:
        return "n/a"
    return f"{sum(vals) / len(vals):+.3f}"


# --------------------------------------------------------------------------
# cli
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="pogo-fold-probe.py",
        description="Guided A/B/A probe: which signal follows the pogo "
                    "keyboard folio fold?",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  ./pogo-fold-probe.py                  # 5 s per state\n"
               "  ./pogo-fold-probe.py --window 8 --repeat 2\n"
               "  ./pogo-fold-probe.py --selftest       # plumbing check\n"
               "  ./pogo-fold-probe.py --watch 20       # live readout\n"
               "  ./pogo-fold-probe.py --analyze DIR\n")
    ap.add_argument("--window", type=float, default=5.0, metavar="SECS",
                    help="seconds held in each state (default 5)")
    ap.add_argument("--settle", type=float, default=2.0, metavar="SECS",
                    help="seconds allowed for moving the folio (default 2)")
    ap.add_argument("--lead", type=float, default=5.0, metavar="SECS",
                    help="get-ready countdown before the first window (default 5)")
    ap.add_argument("--repeat", type=int, default=1, metavar="N",
                    help="open->fold->open cycles (default 1)")
    ap.add_argument("--out", metavar="DIR", default=None,
                    help="capture directory (default tmp/pogo-fold-probe/<stamp>)")
    ap.add_argument("--no-beep", action="store_true", help="silence the cues")
    ap.add_argument("--no-root", action="store_true",
                    help="skip the root-only sources (gpio, evtest)")
    ap.add_argument("--selftest", action="store_true",
                    help="short run to check the plumbing (3 s windows, 1 s settle)")
    ap.add_argument("--watch", type=float, nargs="?", const=20.0, default=None,
                    metavar="SECS", help="live readout, then exit (default 20 s)")
    ap.add_argument("--analyze", metavar="DIR", default=None,
                    help="re-print the report of an existing capture directory")
    ap.add_argument("--write-config", metavar="FILE", default=None,
                    help="also write the fold-watcher config for the best "
                         "usable channel (exit 3 if none was found)")
    a = ap.parse_args(argv)

    if a.selftest:
        a.window = min(a.window, 3.0)
        a.settle = min(a.settle, 1.0)
        a.lead = min(a.lead, 2.0)
        a.repeat = max(1, min(a.repeat, 1))

    if a.analyze:
        report = analyze(a.analyze, write_config=a.write_config)
        say(report)
        return config_rc(a)

    if a.watch is not None:
        run_watch(max(3.0, a.watch), resolve_sudo(not a.no_root))
        return 0

    outdir = run_capture(a)
    report = analyze(outdir, write_config=a.write_config)
    with open(os.path.join(outdir, "report.txt"), "w") as fh:
        fh.write(report + "\n")
    say(report)
    say()
    say(f"  raw logs : {outdir}")
    say(f"  report   : {os.path.join(outdir, 'report.txt')}")
    return config_rc(a)


def config_rc(a):
    """0 unless --write-config was asked for and found no usable channel."""
    if a.write_config and not LAST_CONFIG_OK:
        return 3
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        say("\ninterrupted")
        sys.exit(130)

