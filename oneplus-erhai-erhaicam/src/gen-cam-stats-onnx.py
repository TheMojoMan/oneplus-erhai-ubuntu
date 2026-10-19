#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate erhai-cam-stats.onnx — the ErhaiCam NPU statistics graph.

The graph is the per-frame image analysis used by ErhaiCam's autofocus and
auto-exposure loops.  It is deliberately tiny and made only of QNN-friendly
operators (Conv / Mul / Add / Clip / ReduceMean) so the whole thing is placed
on the Hexagon Tensor Processor (HTP) by ONNX Runtime's QNN execution
provider:

    input  img  float32[1,1,240,320]   (grey, 0..1)

    gx    = Conv(img, SOBEL_X)         # VALID 3x3 Sobel, no padding
    gy    = Conv(img, SOBEL_Y)
    sq    = gx*gx + gy*gy
    sharp = ReduceMean(sq)             # Tenengrad focus metric (mean |grad|^2)
    luma  = ReduceMean(img)            # mean scene luminance
    hi    = ReduceMean(Clip(img,0.9,1))    # 0.9 + E[(x-0.9)+]  highlight window
    lo    = ReduceMean(Clip(img,0,0.1))    # E[min(x,0.1)]      shadow window

The two clipping windows are 10 % wide and the caller subtracts their floor:
the interesting signal is `hi - 0.9` = E[(x-0.9)+] (how much of a pixel is
above 90 % white) and `lo` (how bright the darkest decile is).  A slightly
narrower window (0.98 / 0.02) would be a purer "clipping fraction", but the
HTP executes this graph in fixed point and its ~2e-4 relative error on a mean
sitting at 0.98 is as large as the signal itself — at a 0.9 floor the signal
is two orders of magnitude above that noise.

VALID convolution (no padding) is intentional: the border pixels of a padded
convolution see the zero-padded edge and their artificial gradients would add
a scene-independent constant to `sharp`.  VALID matches the reference
implementation (src/erhai-cam.cpp `tenengrad()`, same kernels, interior only)
exactly, so the CPU fallback and the NPU path produce the same numbers — see
the --self-test mode of src/erhai-npu-analyze.py.

The fixed input shape is required: the QNN EP wants static shapes, so the app
decimates whatever region or frame it wants analysed into this exact size
(stride sampling, hence the small size — the AF region of a 3200x2400 preview
is 1920x1440, sampled 1/6).

Usage:
    gen-cam-stats-onnx.py [OUT.onnx]      # default: ./erhai-cam-stats.onnx
    gen-cam-stats-onnx.py --verify MODEL  # CPU-EP vs numpy reference
"""
import argparse
import sys

import numpy as np

H, W = 240, 320

# Standard 3x3 Sobel kernels (as used by the C++ reference implementation).
SOBEL_X = np.array([[[[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]]]], dtype=np.float32)
SOBEL_Y = np.array([[[[-1, -2, -1], [0, 0, 0], [1, 2, 1]]]], dtype=np.float32)

HI_FLOOR, HI_CEIL = np.float32(0.9), np.float32(1.00)
LO_FLOOR, LO_CEIL = np.float32(0.0), np.float32(0.10)


def numpy_reference(gray):
    """Reference stats for a uint8 (H,W) grey image — the exact target the
    graph (and the C++ fallback) must reproduce."""
    x = np.asarray(gray, dtype=np.float32) * np.float32(1.0 / 255.0)
    a, b, c = x[:-2, :-2], x[:-2, 1:-1], x[:-2, 2:]
    d, _, f = x[1:-1, :-2], x[1:-1, 1:-1], x[1:-1, 2:]
    g, h, i = x[2:, :-2], x[2:, 1:-1], x[2:, 2:]
    gx = (c - a) + 2.0 * (f - d) + (i - g)
    gy = (g - a) + 2.0 * (h - b) + (i - c)
    sharp = float(np.mean(gx * gx + gy * gy))
    luma = float(np.mean(x))
    hi = float(np.mean(np.clip(x, HI_FLOOR, HI_CEIL)))
    lo = float(np.mean(np.clip(x, LO_FLOOR, LO_CEIL)))
    return sharp, luma, hi, lo


def build_model():
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    def const(name, arr):
        return numpy_helper.from_array(arr, name)

    nodes = [
        # VALID 3x3 Sobel (kernel 3x3, no padding).
        helper.make_node("Conv", ["img", "kx"], ["gx"], name="sobel_x"),
        helper.make_node("Conv", ["img", "ky"], ["gy"], name="sobel_y"),
        helper.make_node("Mul", ["gx", "gx"], ["gx2"], name="gx2"),
        helper.make_node("Mul", ["gy", "gy"], ["gy2"], name="gy2"),
        helper.make_node("Add", ["gx2", "gy2"], ["sq"], name="sq"),
        helper.make_node("ReduceMean", ["sq"], ["sharp"],
                         axes=[1, 2, 3], keepdims=1, name="sharp"),
        helper.make_node("ReduceMean", ["img"], ["luma"],
                         axes=[1, 2, 3], keepdims=1, name="luma"),
        helper.make_node("Clip", ["img", "hi_min", "hi_max"], ["hi_c"],
                         name="clip_hi"),
        helper.make_node("ReduceMean", ["hi_c"], ["hi"],
                         axes=[1, 2, 3], keepdims=1, name="hi"),
        helper.make_node("Clip", ["img", "lo_min", "lo_max"], ["lo_c"],
                         name="clip_lo"),
        helper.make_node("ReduceMean", ["lo_c"], ["lo"],
                         axes=[1, 2, 3], keepdims=1, name="lo"),
    ]
    initializers = [
        const("kx", SOBEL_X),
        const("ky", SOBEL_Y),
        const("hi_min", np.array(HI_FLOOR, dtype=np.float32)),
        const("hi_max", np.array(HI_CEIL, dtype=np.float32)),
        const("lo_min", np.array(LO_FLOOR, dtype=np.float32)),
        const("lo_max", np.array(LO_CEIL, dtype=np.float32)),
    ]
    graph = helper.make_graph(
        nodes, "erhai-cam-stats",
        [helper.make_tensor_value_info("img", TensorProto.FLOAT, [1, 1, H, W])],
        [helper.make_tensor_value_info(n, TensorProto.FLOAT, [1, 1, 1, 1])
         for n in ("sharp", "luma", "hi", "lo")],
        initializers)
    # opset 17: ReduceMean axes are attributes (opset 18 moved them to an
    # input), Clip takes min/max as inputs (opset 11+).
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def verify(model_path):
    """Run the model on the CPU EP and compare with the numpy reference.
    (The NPU path is verified by erhai-npu-analyze.py --self-test.)"""
    import onnxruntime as ort

    rng = np.random.default_rng(7)
    img = rng.integers(0, 256, size=(H, W), dtype=np.uint8)
    # add structure so the metric is not pure noise
    img[60:180, 80:240] = np.linspace(0, 255, 160, dtype=np.uint8)[None, :]
    x = (img.astype(np.float32) / 255.0)[None, None]

    sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    out = sess.run(None, {"img": x})
    ref = numpy_reference(img)
    ok = True
    for name, got, want in zip(("sharp", "luma", "hi", "lo"),
                               [float(v.ravel()[0]) for v in out], ref):
        diff = abs(got - want)
        # float32 accumulation over 76800 pixels differences the ReduceMean
        # sums by ~1e-5 absolute; 1e-4 relative is far below anything the
        # AF/AE control loops care about.
        good = diff <= 1e-4 * max(1.0, abs(want))
        ok = ok and good
        print(f"  {name:6s} model={got:.8f} numpy={want:.8f} diff={diff:.2e} "
              f"{'OK' if good else 'MISMATCH'}")
    print("verify:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="erhai-cam-stats.onnx")
    ap.add_argument("--verify", action="store_true",
                    help="treat the positional arg as a model to verify")
    args = ap.parse_args()

    if args.verify:
        return verify(args.out)

    import onnx
    model = build_model()
    onnx.save(model, args.out)
    print(f"wrote {args.out}: input float32[1,1,{H},{W}], "
          f"outputs sharp/luma/hi/lo float32[1,1,1,1]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
