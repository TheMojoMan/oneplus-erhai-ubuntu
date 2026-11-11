# SPDX-License-Identifier: GPL-2.0-only
"""ErhaiCam's auto-exposure controller, ported 1:1 from /src/erhai-cam.cpp.

    tick()  = ae_step's stride/settle gate      (erhai-cam.cpp:1471-1478)
    step()  = ae_step after the gate            (erhai-cam.cpp:1491-1583)

A log-domain proportional controller on integration time, with the
low-light frame-length ladder.  Pure: no I/O.  The caller applies the
returned Action to the sensor (VTS/exposure write order is the caller's).
"""
import math
from dataclasses import dataclass, replace

TARGET = 0.45            # kAeTarget
DEADBAND = 0.06          # kAeDeadband (log2)
GAIN = 0.5               # kAeGain
MAX_STEP = 0.25          # kAeMaxStep (log2)
STRIDE = 3               # kAeStride: analyse every 3rd delivered frame
SETTLE = 2               # kAeSettle: stride slots to wait after a change
HI_EPS = 0.02            # kRawExposureEpsilon: highlight guard
LUMA_MIN = 0.004
EXP_MIN = 1              # kExposureMin
VTS_MARGIN = 8           # max_lines = vts - 8


@dataclass(frozen=True)
class SensorLimits:
    ae_reserve: int
    vts_default: int
    vts_max: int
    exp_min: int = EXP_MIN


FRONT = SensorLimits(0, 3128, 12512)
REAR = SensorLimits(1596, 6404, 25616)


def max_lines(vts):
    return vts - VTS_MARGIN


def ae_max(limits, vts):
    return max(EXP_MIN, max_lines(vts) - limits.ae_reserve)


def lround(x):
    """C lround for x >= 0: halves round away from zero."""
    return int(math.floor(x + 0.5))


@dataclass(frozen=True)
class State:
    exposure: int
    vts: int
    settle: int = SETTLE
    stride: int = 0
    at_limit: bool = False


@dataclass(frozen=True)
class Hold:
    pass


@dataclass(frozen=True)
class SetExposure:
    lines: int


@dataclass(frozen=True)
class SetFrame:
    vts: int
    exposure: int


Action = Hold | SetExposure | SetFrame


def tick(state):
    """The stride/settle gate. True only for a frame to analyse."""
    stride = state.stride + 1
    if stride < STRIDE:
        return replace(state, stride=stride), False
    if state.settle > 0:
        return replace(state, stride=0, settle=state.settle - 1), False
    return replace(state, stride=0), True


def clamp_target(value):
    try:
        t = float(value)
    except (TypeError, ValueError):
        return TARGET
    if not math.isfinite(t):
        return TARGET
    return min(0.7, max(0.2, t))


def step(state, limits, luma, hi, target=TARGET, short_frames=False):
    """One analysed frame of ae_step. Returns (new State, Action).

    short_frames: autofocus is sweeping (ErhaiCam's af_wants_short_frames),
    so the low-light ladder must not lengthen the frame."""
    err = math.log2(target / min(1.0, max(LUMA_MIN, luma)))
    s = min(MAX_STEP, max(-MAX_STEP, err * GAIN))
    if abs(err) < DEADBAND:
        s = 0.0                                  # close enough: hold
    if hi > HI_EPS and s > 0.0:
        s = 0.0                                  # highlights washing out
    if s < 0.0 and state.exposure <= EXP_MIN:
        s = 0.0

    exposure, vts = state.exposure, state.vts
    top = ae_max(limits, vts)

    # Ladder up: at the ceiling and still dark -> double the frame.
    if s > 0.0 and exposure >= top and vts < limits.vts_max and not short_frames:
        vts = min(vts * 2, limits.vts_max)
        exposure = max_lines(vts)
        # Returns before at_limit is recomputed: carried over, as in C++.
        return (replace(state, exposure=exposure, vts=vts, settle=SETTLE * 2),
                SetFrame(vts, exposure))

    settle = state.settle
    frame_changed = False
    # Ladder down: too bright and the long frame is no longer needed.
    if (s < 0.0 and vts > limits.vts_default
            and exposure <= (limits.vts_default - VTS_MARGIN) // 2):
        vts = max(limits.vts_default, vts // 2)
        settle = SETTLE * 2
        frame_changed = True
        top = ae_max(limits, vts)

    nxt = exposure
    if s != 0.0:
        nxt = min(top, max(EXP_MIN, lround(exposure * math.exp2(s))))
        if nxt == exposure:                      # rounding at the very bottom
            nxt = min(top, max(EXP_MIN, exposure + (1 if s > 0.0 else -1)))
    if nxt != exposure:
        settle = SETTLE

    dark_end = nxt >= top and err > DEADBAND
    bright_end = nxt <= EXP_MIN and err < -DEADBAND
    new = replace(state, exposure=nxt, vts=vts, settle=settle,
                  at_limit=dark_end or bright_end)
    if frame_changed:
        return new, SetFrame(vts, nxt)
    if nxt != exposure:
        return new, SetExposure(nxt)
    return new, Hold()
