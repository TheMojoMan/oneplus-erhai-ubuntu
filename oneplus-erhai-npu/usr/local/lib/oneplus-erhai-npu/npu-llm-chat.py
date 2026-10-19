#!/usr/bin/env python3
"""npu-llm chat / one-shot REPL driving the GenieX Python API directly.

Why this exists: geniex-py chat hardcodes enable_thinking=True in its chat
template (cli.py _run_turn), so a Qwen3-style thinking model spends its whole
max-token budget on invisible ` thinking` blocks (measured 2026-09-24: a "say
hello" prompt = 96 chunks of reasoning; with enable_thinking=False the template
emits the ` thinking\n\n response\n\n` suppression block and the same prompt
answers in 2 chunks / 0.2 s).  npu-llm routes --reasoning off here so answers
come out direct -- Qwen3-4B at ~17 tok/s decode makes thinking very expensive.

Usage:
  python3 npu-llm-chat.py <model> <device> <n_ctx> <max_new> [<system>] [<prompt>]

  <model>   full repo id (must be cached; npu-llm resolves short names)
  <device>  llama_cpp:HTP0[,HTP1...] / llama_cpp:CPU / ...
  <n_ctx>   0 = model default; the KV cache is allocated in full at load
  <max_new> per-reply token cap
  <system>  optional system prompt ('' = none)
  <prompt>  optional last argument = run a SINGLE turn and exit (one-shot)

Env:
  ENABLE_THINKING=1  let the model show its reasoning (default 0 = direct)
  GEN_TEMP=0.7       sampling temperature (default 0.7)
"""
import os
import sys

from geniex import AutoModelForCausalLM

ENABLE_THINKING = os.environ.get('ENABLE_THINKING') == '1'
GEN_TEMP = float(os.environ.get('GEN_TEMP', '0.7'))

# Optional SDK debug log (the loader's device/`ggml-hex:` lines and every HTP
# mapping) -- GENIEX_DEBUG=1, same convention as geniex-ngl.py.
if os.environ.get('GENIEX_DEBUG'):
    try:
        import logging
        from geniex._ffi._api import set_log_level
        set_log_level('debug')
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(logging.Formatter('%(levelname)s %(name)s: %(message)s'))
        logger = logging.getLogger('geniex')
        logger.addHandler(handler)
        logger.setLevel(logging.DEBUG)
        logger.propagate = False
    except Exception as exc:      # pragma: no cover - diagnostic only
        print('could not enable debug logging:', exc, flush=True)


def one_turn(model, history, user, max_new):
    history.append({'role': 'user', 'content': user})
    prompt = model.tokenizer.apply_chat_template(
        history,
        tokenize=False,
        add_generation_prompt=True,
        enable_thinking=ENABLE_THINKING,
    )
    streamer = model.generate(
        prompt, max_new_tokens=max_new, temperature=GEN_TEMP, stream=True
    )
    parts: list[str] = []
    for chunk in streamer:
        if not chunk:
            continue
        sys.stdout.write(chunk)
        sys.stdout.flush()
        parts.append(chunk)
    print()
    reply = ''.join(parts)
    if reply.strip():
        history.append({'role': 'assistant', 'content': reply})
    prof = getattr(getattr(streamer, 'output', None), 'profile', None)
    if prof is not None:
        print(
            f'\n- {prof.decode_speed:.1f} tok/s | {prof.generated_tokens} tok'
            f' | {prof.ttft / 1e6:.1f} s first token | stop: {prof.stop_reason} -\n'
        )


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    model, device = sys.argv[1], sys.argv[2]
    n_ctx = int(sys.argv[3])
    max_new = int(sys.argv[4])
    system = sys.argv[5] if len(sys.argv) > 5 else ''
    one_shot = sys.argv[6] if len(sys.argv) > 6 else None

    kwargs: dict = {'device_map': device}
    if n_ctx:
        kwargs['n_ctx'] = n_ctx
    model_obj = AutoModelForCausalLM.from_pretrained(model, **kwargs)
    history: list[dict] = []
    if system:
        history.append({'role': 'system', 'content': system})

    if one_shot is not None:
        one_turn(model_obj, history, one_shot, max_new)
    else:
        print('Use Ctrl+D or /exit to exit.\n')
        while True:
            try:
                line = input('> ')
            except (EOFError, KeyboardInterrupt):
                print()
                break
            line = line.strip()
            if not line:
                continue
            if line in ('/exit', '/quit'):
                break
            if line == '/reset':
                history = [history[0]] if system else []
                print('(history cleared)')
                continue
            one_turn(model_obj, history, line, max_new)
    model_obj.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())