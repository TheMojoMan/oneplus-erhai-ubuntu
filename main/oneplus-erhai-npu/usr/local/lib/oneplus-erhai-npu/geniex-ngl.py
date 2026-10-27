#!/usr/bin/env python3
"""Load a GGUF with a chosen number of layers offloaded to the Hexagon NPU.

  python3 geniex-ngl.py <file.gguf> <max_new_tokens> <device> <n_gpu_layers> [n_ctx]

Same as geniex-local.py but exposes n_gpu_layers (the llama.cpp "offload N
layers to the accelerator" knob, which geniex's AutoModelForCausalLM accepts as
a keyword) and n_ctx (the context window / KV-cache size; 0 = model default).
Used to find how much of a big model fits the CDSP's 4 GiB FastRPC mapping
arena: n_gpu_layers=-1 = all layers (fails for >=7B Q4_0), a smaller number
keeps the rest on the CPU but still accelerates the offloaded ones.

Extra env knobs:
  PROMPT       the prompt text (default: a short factual question)
  PROMPT_FILE  read the prompt from a file instead (overrides PROMPT)
  CTX_ONLY=1   load with n_ctx, print the peak RSS and exit WITHOUT generating
               -- a cheap way to test whether a context size fits in RAM / in
               the DSP arena
  GENIEX_DEBUG=1 enable the SDK debug log (llama.cpp loader lines, every mapping)

`peak_rss=<MiB>` is printed at the end (the process high-water mark), so a
context-size run reports success/failure AND what it cost in memory.
"""
import os
import resource
import sys
import time

from geniex import AutoModelForCausalLM

# Optional SDK debug log (shows `load_tensors: layer N assigned to device ...`,
# the HTP session setup and every buffer mapping) -- GENIEX_DEBUG=1.
if os.environ.get('GENIEX_DEBUG'):
    try:
        import logging
        from geniex._ffi._api import set_log_level
        set_log_level('debug')
        # The SDK bridges its logs into the 'geniex' python logger, which needs a
        # handler or every `ggml-hex:` / `load_tensors:` line is dropped (the CLI
        # does exactly this in _apply_log_level(); our harness must match it).
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(logging.Formatter('%(levelname)s %(name)s: %(message)s'))
        logger = logging.getLogger('geniex')
        logger.addHandler(handler)
        logger.setLevel(logging.DEBUG)
        logger.propagate = False
    except Exception as exc:      # pragma: no cover - diagnostic only
        print('could not enable debug logging:', exc, flush=True)

path = sys.argv[1]
max_new = int(sys.argv[2]) if len(sys.argv) > 2 else 16
device = sys.argv[3] if len(sys.argv) > 3 else "llama_cpp:HTP0"
ngl = int(sys.argv[4]) if len(sys.argv) > 4 else -1
n_ctx = int(sys.argv[5]) if len(sys.argv) > 5 else 2048

t0 = time.time()
model = AutoModelForCausalLM.from_pretrained(
    path, device_map=device, n_ctx=n_ctx, n_gpu_layers=ngl)
print(f"ngl={ngl} n_ctx={n_ctx} loaded in {time.time() - t0:.1f}s | {model!r}",
      flush=True)

if os.environ.get("CTX_ONLY"):
    print(f"peak_rss={resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB")
    model.close()
    sys.exit(0)

prompt_file = os.environ.get("PROMPT_FILE")
if prompt_file:
    with open(prompt_file, encoding='utf-8') as fh:
        prompt = fh.read()
else:
    prompt = os.environ.get(
        "PROMPT", "Explain in exactly three sentences why the sky appears blue.")
t0 = time.time()
out = model.generate(prompt, max_new_tokens=max_new)
print("--- answer ---")
print(out)
print(f"--- {time.time() - t0:.1f}s wall for up to {max_new} tokens ---")
print(f"peak_rss={resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB")
model.close()
