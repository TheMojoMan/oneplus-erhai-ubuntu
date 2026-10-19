# oneplus-erhai-llamacpp

Patched **llama.cpp** for the OnePlus Pad 3 (`erhai`, SM8750) — the build that
makes CPU, the **Adreno GPU (OpenCL/rusticl)** and the **Hexagon NPU (HTP/CDSP)**
available from a single set of binaries.

Everything is installed under **`/opt/llama.cpp`** so it never shadows a distro
build:

```
/opt/llama.cpp/bin/                 llama-cli, llama-server, llama-bench, … + all libs
                                    (RPATH is $ORIGIN — the binaries find their own
                                    libs) plus the vendor Hexagon backend,
                                    libggml-hexagon.so + libggml-htp-v79.so
/usr/local/bin/llama-erhai          llama-cli   + erhai environment + GPU guard
/usr/local/bin/llama-erhai-server   llama-server + erhai environment + GPU guard
/usr/share/doc/oneplus-erhai-llamacpp/patches/   the full patch set + base commit
```

Built from upstream `6f41ac5` (ggml 0.24.0) with the erhai patch set in
`patches/llama-cpp-erhai.patch`.  The builder is
`oneplus-erhai-ubuntu/build-llamacpp-deb.sh` in the erhai workspace; it stages
from `tmp/llama-cpp-ref/build-opencl/bin` and re-derives the patch file from
that tree, so the payload and the patch set can never drift apart.

## Usage

```sh
llama-erhai --list-devices                       # CPU / GPUOpenCL / HTP0 (+HTP1)
llama-erhai -m m.gguf -p hello -n 128            # CPU only (safe default)
llama-erhai -m m.gguf -c 13312 \
   --offload-spec 'HTP0:layers=0-17,HTP1:layers=18-36'          # NPU, 2 sessions
llama-erhai-server -m m.gguf -c 13312 --alias qwen3-4b --port 8081 \
   --offload-spec 'HTP0:layers=0-17,HTP1:layers=18-36'
```

The wrappers export what the device stack needs and nothing else:

* `GGML_BACKEND_PATH=/opt/llama.cpp/bin/libggml-hexagon.so`
  (the loader needs a **file**; a plain run never reads it — see below),
* `FASTRPC_UNSIGNED_PD=1` (the QNN / ggml-htp skels are unsigned only).

**With no device option the wrappers add `--device none` (CPU only).**  Ask for a
device explicitly:

| want | option |
|---|---|
| CPU | `--device none` (the default) |
| Hexagon NPU | `--offload-spec 'HTP0:layers=0-17,HTP1:layers=18-36'` (or `'HTP0:bytes=3G'`) |
| Adreno GPU | `-dev GPUOpenCL` — **read the warning below first** |

## Do NOT run ggml on the Adreno while the desktop is up

On 2026-09-22 a `llama-server` on Qwen3-4B auto-selected `GPUOpenCL` (rusticl) and
died in `ggml-opencl.cpp` (`GGML_ASSERT(0)`, `clWaitForEvents error -14`).  The GPU
lockup took the display with it: `hangcheck detected gpu lockup`, a `gpu fault …
dir=READ type=TRANSLATION` storm, `a8xx_recover: cx gdsc didn't collapse`, then an
endless `preemption timed out` loop.  The SoC stayed alive (journal kept logging)
but **the panel froze and the device needed a hard reset**.

The safe local-LLM devices on erhai are the **CPU** and the **Hexagon NPU**.
That is why the wrappers default to `--device none` and warn when your arguments
name a GPU; `LLAMA_ERHAI_GPU_OK=1` silences the warning if you accept the risk
(away from a live session, e.g. from a TTY).

## What the patches do

1. **OpenCL/Adreno (rusticl) bring-up** — accept the OpenCL 3.0 subgroup family
   instead of the legacy `cl_khr_subgroups` name; `-DADRENO_GPU=1` with empty
   `REQD_SUBGROUP_SIZE_*` for a compiler without `cl_qcom_reqd_sub_group_size`;
   keep the qcom shuffle mapping in `mul_mv_f16_f32_l4.cl` conditional;
   portable `float32` (struct of 4× float8) in the nine `gemm_moe_*_ns.cl`
   kernels; and `is_qualcomm_adreno_compiler()`, because the unparsable Mesa
   driver version used to **skip** the `1d_8x_flat` decode kernels that the
   dispatch path still uses — that one crashed *every* single-token decode with
   `clSetKernelArg … error -48`.
2. **Hexagon backend loading** — `ggml-backend-reg.cpp` also accepts the legacy
   `ggml_backend_<name>_reg()` entry point, which is what Qualcomm's prebuilt
   `libggml-hexagon.so` exports (it has no `ggml_backend_init`).
3. **`--offload-spec` / `-os`** (env `LLAMA_ARG_OFFLOAD_SPEC`) — explicit device
   placement: `'HTP0:8'`, `'HTP0:layers=0-3,GPUOpenCL:layers=4-5,CPU:rest'`,
   `'HTP0:bytes=2G'`.  Rules are applied left→right, the first matching rule wins,
   layers nobody names stay on the CPU.  It also names the devices itself, so it
   is the one option that reliably activates the Hexagon backend in a plain run.

## Two traps the wrappers exist to hide

* `GGML_BACKEND_PATH` must point at a **file** — and every backend-load site is
  guarded by `if (!ggml_backend_reg_count())`.  OpenCL and CPU self-register, so
  only `--offload-spec`/`-os`, `-dev`/`--device` and `--list-devices` reach
  `ggml_backend_load_all()`.  A plain `llama-cli -m …` never sees the NPU even
  with the variable set.
* One HTP session cannot hold a whole 4B model **and** its KV cache (the CDSP IOVA
  arena is ~3 GiB).  Use explicit layer ranges for two sessions
  (`GGML_HEXAGON_DEVICES=2` is set for you by naming `HTP0`+`HTP1` in the spec);
  `'HTP0:bytes=1.5G,HTP1:bytes=1.5G'` splits **greedily** (36 layers on HTP0, 1 on
  HTP1) and the ~1 GiB KV mapping then fails with
  `ggml-hex: HTP0 buffer mapping failed … size 1073745920` — after which the CDSP
  must be restarted before any retry.

## Measured on this pad (2026-09-21/22, greedy, n_ctx 2048-13312)

| config | prefill | decode |
|---|---|---|
| Qwen3-0.6B Q4_0, CPU | 15 t/s | - |
| Qwen3-0.6B Q4_0, GPUOpenCL | 293 t/s (pp128) | 31-32 t/s |
| Qwen3-0.6B Q4_0, NPU (HTP0) | 491 t/s | 72 t/s |
| Qwen2.5-7B Q4_0, NPU 2 sessions | 74-77 t/s | 8.9-10.1 t/s |
| Qwen2.5-7B Q4_0, GPU + 2 NPU sessions | 32 t/s | 7.7 t/s |
| Qwen3-4B Q4_0, NPU 2 sessions, n_ctx 13312 | 752 t/s (5.7k prompt in 7.6 s) | 7.5-17 t/s |

NPU offload is a **capacity** feature first: a single accelerator is faster than a
hybrid split, but only the split fits models that exceed one device.  Full NPU
offload works up to ~2 GiB of Q4_0 weights per session set; the `gemma-2-9B` class
needs GPU + 2 NPU sessions (3 devices).

## NPU prerequisites (not shipped here)

The Hexagon route needs the CDSP userspace stack, which is currently installed
out of band (it is *not* provided by any package yet):

* kernel >= `7.2.0-sm8750-themojoman+b63` (`/dev/fastrpc-cdsp`) + firmware >= 1.2,
* the CDSP remotefs daemon (`cdsprpcd`) and `libcdsprpc` from Qualcomm's
  `qualcomm/fastrpc` build, with `/usr/lib/rfsa/{adsp,cdsp}` and
  `/usr/share/qcom/hexagon/v79` populated,
* the invoking user in the **`fastrpc`** group (`/dev/fastrpc-cdsp` is root:fastrpc
  0640) - group membership only takes effect after a re-login,
* a fresh CDSP before a big load (`fastrpc` mappings accumulate until the arena is
  full; restarting the compute DSP clears them).

Without those, `llama-erhai --list-devices` still shows `HTP0`/`HTP1` (the vendor
backend loads its host half) but a load fails with a CDSP/FastRPC error.

## Known limitations

* **Q2_K and friends silently stay on the CPU**: `select_weight_buft()` asks
  `weight_buft_supported()`, a real op-graph test, and the Adreno/Hexagon support
  tables do not cover every low-bit type - the `--offload-spec` plan is then not
  realized.  Q4_0 / MXFP4 are the covered types.  `-ot <regex>=HTP0` bypasses the
  check (proven to load 834 MiB of Q2_K experts) but that arm measured 4x slower
  than CPU-only, so do not offload Q2_K.
* `test-backend-ops -b GPUOpenCL` has one known crash (`type_a=q1_0`, a rare
  quant); everything else passes.  Do not run it from inside a live session.
* The KV cache is fp16 with no quantization option, and each NPU session holds its
  KV as **one** mapping: ~4 KiB/token per KV layer, so a 1 GiB mapping (what the
  CDSP refuses) caps the window at ~13k tokens for a full-NPU 4B split.
