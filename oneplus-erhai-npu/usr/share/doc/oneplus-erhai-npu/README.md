# oneplus-erhai-npu

The **NPU / compute-DSP (CDSP) workload layer** for the OnePlus Pad 3 (`erhai`,
SM8750).  The Hexagon NPU is not a standalone device: it is the **HTP** hanging
off the CDSP, driven by DSP-side libraries the AP loads over **FastRPC**.  This
package owns the file tree the DSP is served, the health report, the recovery
tool and the user front end.

## The three-package split

| package | owns |
|---|---|
| `oneplus-erhai-fastrpc` | the **transport**: `libcdsprpc`/`libadsprpc`/`libsdsprpc`, `cdsprpcd`, the `fastrpc` group + udev rule, the unsigned-PD env, the DSP payload trees (`/usr/lib/rfsa/{adsp,cdsp}`, `/usr/share/qcom/hexagon/v79`), `fastrpc_test` |
| **`oneplus-erhai-npu`** (this) | the **workload layer**: `/var/lib/hexagonfs-cdsp` (the remotefs tree), `hexagonrpcd-cdsp.service` (disabled fallback), `erhai-npu-status.sh`, `erhai-cdsp-restart.sh`, `npu-llm` |
| `oneplus-erhai-sensors` | the **ADSP** path: `hexagonrpcd-erhai` + `/var/lib/hexagonfs-erhai`, patched iio-sensor-proxy, `erhai-alsd` (runs as `User=fastrpc`) |

The daemon that works with this firmware is Qualcomm's **cdsprpcd** (fastrpc
package); `hexagonrpcd-cdsp.service` here is a polished-off fallback and stays
**disabled** — two remotefs daemons on one `/dev/fastrpc-cdsp` fight over the
DSP's one-shot shell load.

## Using it

```sh
erhai-npu-status.sh              # whole-chain report (read-only as a user)
sudo erhai-npu-status.sh --pd-test   # prove the DSP runs a host-uploaded shell
npu-llm doctor                   # tool-chain check (fastrpc group, CDSP, cache)
npu-llm pull unsloth/Qwen3-4B-GGUF:Q4_0
npu-llm run  Qwen3-4B "explain mmap in two sentences" --ctx 8192
npu-llm chat Qwen3-4B
sudo erhai-cdsp-restart.sh       # recovery when a load fails with -100201
```

`npu-llm` drives llama.cpp's Hexagon backend through the **GenieX SDK**, which is
a Python package installed per user:

```sh
pip install --user geniex        # provides ~/.local/bin/geniex-py
```

## Rules that save hours

* **Q4_0 / MXFP4 only** for NPU offload — the Hexagon kernels and the GenieX
  support tables do not cover Q2_K and friends, and the loader then silently
  keeps those layers on the CPU.
* **Never run ggml on the Adreno (GPUOpenCL) while the desktop is up**: an
  OpenCL fault takes the panel down with it (2026-09-22: hangcheck → drm recover
  loop → hard reset).  The safe accelerators are the CPU and this NPU.
* **The FastRPC mapping arena is finite** (~4 GiB IOVA under a 32-bit DMA mask).
  A failed load strands mappings; 7B/8B need two virtual sessions
  (`npu-llm run … --sessions 2`) and a big context needs its KV cache budget
  checked (4 KiB/token per KV layer per NPU session; ~1 GiB is the wall).
  When a load dies with `-100201` / `failed to map buffer`, restart the CDSP.
* **The `fastrpc` group only applies to a fresh session** — after installing the
  fastrpc package, log out and back in once.
* `fastrpc_test` (fastrpc package) proves the transport but is a mapping-churn
  tool: do not soak with it, restart the CDSP afterwards.

## Prerequisites

* kernel `oneplus-erhai-linux-7.2` >= `7.2.0-sm8750-themojoman+b63`
  (`&remoteproc_cdsp` enabled) — a reboot is needed to boot a new kernel;
* `oneplus-erhai-firmware` >= 1.2 (the OxygenOS `cdsp.mdt` split image);
* `oneplus-erhai-fastrpc` >= 0.1 (transport, hard dependency);
* for the GenieX route: `geniex` from PyPI (per user), and the model cache
  bind-mounted off the root filesystem if `/` is tight.

Measurement history, the mapping-arena saga and the model/context tables live in
the erhai workspace memory files (`npu-MEMORY-*.md`).

## die-budget (whole-die thermal budget, 0.3)

The display/touch complex fails at die >= ~97.5-98.5 C and the NPU has no clock
to throttle, so the shipped policy is a die budget instead of a static context
cap (charging shifts the die).  DIE_BUDGET_MC (default 94000 = 94 C, 15 s soak)
controls it; 0 disables.

  die-budget status | wait [mc [soak [deadline]]] | watch <pid> [mc [soak]]

- `npu-llm run`  refuses to start hot and kills the load if the budget is
  breached; interactive `chat` refuses to start hot but is never killed
  mid-answer.
- Library: /usr/local/lib/oneplus-erhai-npu/die-budget.sh (die_now_mc,
  die_wait_idle, die_watch).  Depends on /run/erhai-thermal.state from
  oneplus-erhai-thermal (falls back to thermal_zone42 without it).
