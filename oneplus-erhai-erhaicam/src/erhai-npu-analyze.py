#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ErhaiCam frame analyzer — the autofocus / auto-exposure statistics of
ErhaiCam, computed on the Hexagon NPU (HTP / Hexagon Tensor Processor).

One process, one long-lived ONNX session: the app spawns it once, it answers
one request per analysed video frame.

Protocol (line oriented, one request/response per line; the payload of a
request is raw binary):

    app -> helper   ANALYZE <w> <h> <nbytes>\\n   followed by nbytes of 8-bit
                                                  grey pixels (row major)
    helper -> app   STAT <sharp> <luma> <hi> <lo>\\n
    helper -> app   ERR <message>\\n              request failed
    helper -> app   READY <backend>\\n            once, before the first STAT

Backends, best first:

    npu       the QNN execution provider on the HTP — the NPU path
    ort-cpu   the same ONNX graph on ONNX Runtime's CPU EP
    numpy     the reference implementation, no onnxruntime at all

`ERHAI_CAM_NPU=auto|npu|ort-cpu|numpy` selects (default auto).  The app shows
which one is live, and every path returns the *same numbers* (the graph,
gen-cam-stats-onnx.py's numpy_reference() and erhai-cam.cpp's tenengrad() are
three implementations of one metric), so AF/AE behaviour does not change when
the NPU is unavailable.

`--self-test` checks that: it runs one synthetic frame through every available
backend and prints their numbers side by side.
`--serve` is the app-facing mode (the default when run without arguments).
"""
import argparse
import os
import sys
import time

H, W = 240, 320
MODEL_BASENAME = "erhai-cam-stats.onnx"
BACKENDS = ("npu", "ort-cpu", "numpy")
# Clipping windows of the stats graph (see gen-cam-stats-onnx.py): the
# highlight signal the caller uses is `hi - HI_FLOOR`, the shadow one `lo`.
HI_FLOOR, HI_CEIL = 0.9, 1.0
LO_FLOOR, LO_CEIL = 0.0, 0.1
# The HTP runs the graph in fixed point; ~2e-4 relative error on a mean is
# normal there, while the float backends agree with the reference to 1e-4.
TOLERANCE = {"npu": 5e-3, "ort-cpu": 1e-4, "numpy": 0.0}


def find_model(explicit=None):
    def rel_candidates():
        """Walk up from this script: works both in the git tree
        (<pkg>/src/x.py with <pkg>/usr/share/...) and installed
        (/usr/local/lib/oneplus-erhai-erhaicam/x.py with /usr/share/...)."""
        here = os.path.dirname(os.path.abspath(__file__))
        for _ in range(6):
            yield os.path.join(here, "share", "oneplus-erhai-erhaicam",
                               MODEL_BASENAME)
            yield os.path.join(here, "usr", "share",
                               "oneplus-erhai-erhaicam", MODEL_BASENAME)
            parent = os.path.dirname(here)
            if parent == here:
                break
            here = parent

    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("ERHAI_CAM_STATS_MODEL")
    if env:
        candidates.append(env)
    candidates.append(f"/usr/share/oneplus-erhai-erhaicam/{MODEL_BASENAME}")
    candidates.extend(rel_candidates())
    for path in candidates:
        if path and os.path.isfile(path):
            return os.path.realpath(path)
    return None


# --------------------------------------------------------------- backends ---

class NumpyBackend:
    name = "numpy"
    detail = "reference implementation"

    def __init__(self, model=None):
        import numpy  # noqa: F401  (fail here if numpy is missing too)

    def run(self, gray):
        import numpy as np
        x = gray.astype(np.float32) * np.float32(1.0 / 255.0)
        a, b, c = x[:-2, :-2], x[:-2, 1:-1], x[:-2, 2:]
        d, _, f = x[1:-1, :-2], x[1:-1, 1:-1], x[1:-1, 2:]
        g, h, i = x[2:, :-2], x[2:, 1:-1], x[2:, 2:]
        gx = (c - a) + 2.0 * (f - d) + (i - g)
        gy = (g - a) + 2.0 * (h - b) + (i - c)
        return (float(np.mean(gx * gx + gy * gy)),
                float(np.mean(x)),
                float(np.mean(np.clip(x, HI_FLOOR, HI_CEIL))),
                float(np.mean(np.clip(x, LO_FLOOR, LO_CEIL))))


class OrtBackend:
    """onnxruntime session, optionally with the QNN execution provider."""

    def __init__(self, model, want_npu):
        import numpy as np  # noqa: F401
        import onnxruntime as ort

        self.ort = ort
        self.model = model
        self._cpu = None
        self.sess = None
        self.name = "ort-cpu"
        self.detail = f"onnxruntime {ort.__version__} CPU EP"
        if want_npu:
            self._try_npu()

    def _try_npu(self):
        # The DSP loads an *unsigned* user PD image: our libcdsprpc patch
        # reads this knob.  The desktop session already sets it through
        # /etc/environment.d/57-erhai-fastrpc-unsigned.conf; make the helper
        # self-sufficient when started from elsewhere.
        os.environ.setdefault("FASTRPC_UNSIGNED_PD", "1")
        try:
            import onnxruntime_qnn as qnn
        except Exception as exc:
            self.detail += f" (QNN EP unavailable: {exc})"
            return
        try:
            self.ort.register_execution_provider_library(
                "QNNExecutionProvider", qnn.get_library_path())
            devices = [d for d in self.ort.get_ep_devices()
                       if "QNN" in d.ep_name]
            if not devices:
                self.detail += " (no QNN device)"
                return
            so = self.ort.SessionOptions()
            so.log_severity_level = 3
            so.add_provider_for_devices(devices, {
                "backend_path": qnn.get_qnn_htp_path(),
                "htp_performance_mode": "balanced",
                "vtcm_mb": "0",
            })
            sess = self.ort.InferenceSession(self.model, so)
            if "QNNExecutionProvider" not in sess.get_providers():
                self.detail += " (QNN EP not in session providers)"
                return
            self.sess = sess
            self.name = "npu"
            self.detail = (f"onnxruntime {self.ort.__version__} + QNN EP "
                           f"{qnn.__version__} on the HTP")
        except Exception as exc:
            self.detail += f" (QNN EP failed: {exc})"

    def run(self, gray):
        if self.sess is not None:
            sess = self.sess
        else:
            if self._cpu is None:
                so = self.ort.SessionOptions()
                so.log_severity_level = 3
                self._cpu = self.ort.InferenceSession(
                    self.model, so, providers=["CPUExecutionProvider"])
            sess = self._cpu
        x = (gray.astype("float32") / 255.0)[None, None]
        out = sess.run(None, {"img": x})
        return tuple(float(v.ravel()[0]) for v in out)


def open_backend(selection, model):
    """Return the best available backend for `selection` ('auto' included)."""
    tried = []
    order = BACKENDS if selection == "auto" else (selection,)
    for name in order:
        try:
            if name == "numpy":
                return NumpyBackend(model), tried
            if model is None:
                tried.append(f"{name}: no model file")
                continue
            if name == "ort-cpu":
                return OrtBackend(model, want_npu=False), tried
            ort = OrtBackend(model, want_npu=True)
            if ort.name == "npu":
                return ort, tried
            tried.append("npu: " + ort.detail)
        except Exception as exc:
            tried.append(f"{name}: {exc}")
    return None, tried


# ------------------------------------------------------------ app server ---

def serve(model_override=None, selection="auto"):
    """Read ANALYZE requests from stdin, answer STAT lines on stdout."""
    out = sys.stdout
    model = find_model(model_override)
    backend, tried = open_backend(selection, model)
    if backend is None:
        out.write("READY none\n")
        out.write(f"ERR no analyzer backend (tried: {tried})\n")
        out.flush()
        return 1
    for note in tried:
        print(f"erhai-npu-analyze: skipped {note}", file=sys.stderr,
              flush=True)
    print(f"erhai-npu-analyze: {backend.name} — {backend.detail}"
          + (f" (model {model})" if model else " (no model file)"),
          file=sys.stderr, flush=True)
    out.write(f"READY {backend.name}\n")
    out.flush()

    import numpy as np
    stdin = sys.stdin.buffer
    while True:
        line = stdin.readline()
        if not line:
            return 0                      # app closed the pipe
        line = line.strip()
        if not line:
            continue
        if line == b"PING":
            out.write("PONG\n")
            out.flush()
            continue
        if line == b"QUIT":
            return 0
        parts = line.split()
        if len(parts) != 4 or parts[0] != b"ANALYZE":
            out.write("ERR bad request\n")
            out.flush()
            continue
        try:
            w, h, nbytes = (int(parts[1]), int(parts[2]), int(parts[3]))
        except ValueError:
            out.write("ERR bad request numbers\n")
            out.flush()
            continue
        if w != W or h != H or nbytes != w * h:
            out.write(f"ERR expected {W}x{H} grey, got {w}x{h} {nbytes}\n")
            out.flush()
            continue
        payload = stdin.read(nbytes)
        if payload is None or len(payload) != nbytes:
            out.write("ERR short payload\n")
            out.flush()
            return 1
        try:
            gray = np.frombuffer(payload, dtype=np.uint8).reshape(h, w)
            sharp, luma, hi, lo = backend.run(gray)
        except Exception as exc:
            out.write(f"ERR analyze failed: {exc}\n")
            out.flush()
            continue
        out.write(f"STAT {sharp:.6f} {luma:.6f} {hi:.6f} {lo:.6f}\n")
        out.flush()


def self_test(model_override=None):
    """One synthetic frame through every available backend, side by side."""
    import numpy as np
    model = find_model(model_override)
    print(f"model: {model or 'NOT FOUND'}")
    rng = np.random.default_rng(11)
    gray = rng.integers(0, 256, size=(H, W), dtype=np.uint8)
    gray[40:200, 60:260] = np.linspace(0, 255, 200, dtype=np.uint8)[None, :]
    gray[::7, :] = 0                        # a few hard edges
    results = {}
    for name in BACKENDS:
        backend, tried = open_backend(name, model)
        if backend is None or backend.name != name:
            detail = tried[-1] if tried else "unavailable"
            print(f"{name:8s} unavailable ({detail})")
            continue
        t0 = time.perf_counter()
        stats = backend.run(gray)
        cold = time.perf_counter() - t0
        t0 = time.perf_counter()
        for _ in range(20):
            backend.run(gray)
        warm = (time.perf_counter() - t0) / 20.0
        results[name] = stats
        print(f"{name:8s} sharp={stats[0]:.6f} luma={stats[1]:.6f} "
              f"hi={stats[2]:.6f} lo={stats[3]:.6f} | "
              f"{cold * 1000:.1f} ms cold, {warm * 1000:.2f} ms/frame warm")
        print(f"         {backend.detail}")
    ref = results.get("numpy")
    if ref is None:
        print("self-test: no reference backend, cannot compare")
        return 1
    ok = True
    for name, stats in results.items():
        tol = TOLERANCE.get(name, 1e-4)
        for i, (got, want) in enumerate(zip(stats, ref)):
            if abs(got - want) > tol * max(1.0, abs(want)):
                print(f"  MISMATCH {name}[{i}]: {got} vs numpy {want} "
                      f"(tol {tol:g})")
                ok = False
    print("self-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true", default=None,
                    help="app-facing pipe server (default)")
    ap.add_argument("--self-test", action="store_true",
                    help="compare every backend on one synthetic frame")
    ap.add_argument("--backend", default=os.environ.get("ERHAI_CAM_NPU", "auto"),
                    choices=("auto",) + BACKENDS)
    ap.add_argument("--model", default=None, help="override the .onnx path")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args.model)
    return serve(args.model, args.backend)


if __name__ == "__main__":
    sys.exit(main())
