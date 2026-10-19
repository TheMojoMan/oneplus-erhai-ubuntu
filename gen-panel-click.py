#!/usr/bin/env python3
# Generate the erhai panel-off click WAV shipped by oneplus-erhai-desktop as
# /usr/share/sounds/erhai/panel-off.wav.
#
#   ./gen-panel-click.py [OUT.wav] ["on"|"off"]
#
# "off" (default) is the subtle downward tick played when the backlight is
# switched off (power button / folio fold); "on" is the slightly brighter tick
# for the restore, unused unless CLICK_ON is configured.
#
# Stdlib only; the output is deterministic, so build-desktop-deb.sh regenerates
# the file on every build (same pattern as the sensors package compiling its
# gschemas).
import math
import random
import struct
import sys
import wave

RATE = 48000


def click(rising=False):
    """~35 ms UI tick: noise transient + a short decaying tone."""
    dur = 0.035
    n = int(RATE * dur)
    f0 = 2100.0 if rising else 1500.0
    f1 = 2600.0 if rising else 900.0        # sweep direction
    rng = random.Random(1 if rising else 0)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        # frequency sweep down (off) / up (on)
        f = f0 + (f1 - f0) * (t / dur)
        phase += 2.0 * math.pi * f / RATE
        env = math.exp(-t / (0.0045 if rising else 0.0060))
        tone = math.sin(phase) * env
        # 1.5 ms noise transient gives the "click" its edge
        noise = rng.uniform(-1.0, 1.0) * math.exp(-t / 0.0012) * 0.5
        out.append(0.42 * (0.75 * tone + noise))
    # 1 ms fade at both ends so nothing snaps
    fade = int(RATE * 0.001)
    for i in range(fade):
        out[i] *= i / fade
        out[-1 - i] *= i / fade
    return out


def main(argv):
    path = argv[1] if len(argv) > 1 else "panel-off.wav"
    rising = len(argv) > 2 and argv[2] == "on"
    samples = [max(-1.0, min(1.0, s)) for s in click(rising)]
    data = b"".join(struct.pack("<h", int(s * 32767)) for s in samples)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    print(f"wrote {path} ({len(samples)} samples, {RATE} Hz mono 16-bit)")


if __name__ == "__main__":
    main(sys.argv)