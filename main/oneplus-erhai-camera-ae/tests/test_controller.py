import math
import unittest

from camera_ae.controller import (FRONT, REAR, Hold, SetExposure, SetFrame,
                                  State, ae_max, clamp_target, lround, step,
                                  tick)


def run(limits, exposure, vts, k=0.9, target=0.45, frames=40):
    """Drive tick+step against luma = k*exp/ae_max(vts_default); return
    (final luma, analysed frames used, actions seen)."""
    state = State(exposure, vts, settle=0)
    ref = ae_max(limits, limits.vts_default)
    actions = []
    analysed = 0
    luma = k * state.exposure / ref
    for _ in range(frames * 9):
        state, go = tick(state)
        if not go:
            continue
        analysed += 1
        luma = k * state.exposure / ref
        if abs(math.log2(luma / target)) < 0.06:
            return luma, analysed, actions
        state, action = step(state, limits, luma, 0.0, target)
        actions.append(action)
        if analysed >= frames:
            break
    return k * state.exposure / ref, analysed, actions


class Convergence(unittest.TestCase):
    def test_converges_to_target(self):
        for limits in (FRONT, REAR):
            top = ae_max(limits, limits.vts_default)
            for start in (top, top // 32):
                luma, n, actions = run(limits, start, limits.vts_default)
                self.assertLess(abs(math.log2(luma / 0.45)), 0.06,
                                (limits, start, luma, n))
                self.assertLessEqual(n, 40)
                self.assertFalse(any(isinstance(a, SetFrame) for a in actions))

    def test_target_override_clamped(self):
        self.assertEqual(clamp_target('0.9'), 0.7)
        for bad in ('x', 'nan', 'inf', None):
            self.assertEqual(clamp_target(bad), 0.45)
        luma, _, _ = run(FRONT, 3120, 3128, target=0.3)
        self.assertLess(abs(math.log2(luma / 0.3)), 0.06)


class Gate(unittest.TestCase):
    def ticks(self, state, n):
        out = []
        for _ in range(n):
            state, go = tick(state)
            out.append(go)
        return out

    def test_stride(self):
        self.assertEqual(self.ticks(State(400, 3128, settle=0), 3),
                         [False, False, True])

    def test_settle(self):
        state = State(400, 3128, settle=0)
        state, _ = tick(state)
        state, _ = tick(state)
        state, go = tick(state)
        self.assertTrue(go)
        state, action = step(state, FRONT, 0.9, 0.0)
        self.assertIsInstance(action, SetExposure)
        self.assertEqual(self.ticks(state, 9), [False] * 8 + [True])

    def test_start_settle(self):
        self.assertEqual(self.ticks(State(400, 3128), 9), [False] * 8 + [True])


class Step(unittest.TestCase):
    def test_deadband_holds(self):
        _, action = step(State(400, 3128), FRONT, 0.46, 0.0)
        self.assertEqual(action, Hold())

    def test_step_clamped(self):
        state, action = step(State(400, 3128), FRONT, 0.01, 0.0)
        self.assertEqual(action, SetExposure(476))
        self.assertEqual(state.exposure, 476)
        self.assertEqual(state.settle, 2)

    def test_black_frame_clamped(self):
        _, action = step(State(400, 3128), FRONT, 0.0, 0.0)
        self.assertEqual(action, SetExposure(476))

    def test_lround(self):
        self.assertEqual(lround(2.5), 3)
        self.assertEqual(lround(3.5), 4)
        self.assertEqual(lround(475.68), 476)

    def test_highlight_guard_blocks_brightening(self):
        _, action = step(State(400, 3128), FRONT, 0.2, 0.05)
        self.assertEqual(action, Hold())

    def test_floor(self):
        state, action = step(State(1, 3128), FRONT, 0.9, 0.0)
        self.assertEqual(action, Hold())
        self.assertTrue(state.at_limit)

    def test_bottom_rounding(self):
        _, action = step(State(2, 3128), FRONT, 0.6, 0.0)
        self.assertEqual(action, SetExposure(1))

    def test_ladder_up(self):
        state, action = step(State(3120, 3128), FRONT, 0.1, 0.0)
        self.assertEqual(action, SetFrame(6256, 6248))
        self.assertEqual((state.exposure, state.vts, state.settle),
                         (6248, 6256, 4))
        state, action = step(State(12504, 12512), FRONT, 0.1, 0.0)
        self.assertEqual(action, Hold())
        self.assertTrue(state.at_limit)
        _, action = step(State(4800, 6404), REAR, 0.1, 0.0)
        self.assertEqual(action, SetFrame(12808, 12800))

    def test_rear_second_rung(self):
        _, action = step(State(12800, 12808), REAR, 0.1, 0.0)
        self.assertEqual(action, SetFrame(25616, 25608))

    def test_ladder_up_keeps_at_limit(self):
        state, action = step(State(3120, 3128, at_limit=True), FRONT, 0.1, 0.0)
        self.assertIsInstance(action, SetFrame)
        self.assertTrue(state.at_limit)

    def test_highlight_blocks_ladder(self):
        state, action = step(State(3120, 3128), FRONT, 0.1, 0.05)
        self.assertEqual(action, Hold())
        self.assertTrue(state.at_limit)

    def test_ladder_down(self):
        state, action = step(State(1000, 6256), FRONT, 0.9, 0.0)
        self.assertEqual(action, SetFrame(3128, 841))
        self.assertEqual((state.exposure, state.vts, state.settle),
                         (841, 3128, 2))

    def test_ladder_down_threshold(self):
        _, action = step(State(1561, 6256), FRONT, 0.9, 0.0)
        self.assertEqual(action, SetExposure(1313))
        _, action = step(State(1560, 6256), FRONT, 0.9, 0.0)
        self.assertEqual(action, SetFrame(3128, 1312))

    def test_ladder_down_halves_once(self):
        _, action = step(State(1000, 12512), FRONT, 0.9, 0.0)
        self.assertEqual(action, SetFrame(6256, 841))

    def test_rear_long_frame_quirk(self):
        _, action = step(State(25608, 25616), REAR, 0.1, 0.0)
        self.assertEqual(action, SetExposure(24012))


class AfInterplay(unittest.TestCase):
    def test_ladder_up_blocked_while_af_sweeps(self):
        state, action = step(State(3120, 3128), FRONT, 0.1, 0.0,
                             short_frames=True)
        self.assertEqual(action, Hold())
        self.assertTrue(state.at_limit)
        _, action = step(State(3120, 3128), FRONT, 0.1, 0.0)
        self.assertEqual(action, SetFrame(6256, 6248))


if __name__ == '__main__':
    unittest.main()
