# SPDX-License-Identifier: GPL-2.0-only
"""erhai-camera-ae.service entry point: follow the PipeWire graph, run the Engine.

Exit 0 after SIGTERM (orderly stop); exit 1 when pw-dump ends on its own
(pipewire went away), so Restart= brings the service back.
"""
import os
import select
import signal
import subprocess
import sys
import time
import traceback

from .controller import clamp_target
from .doctor import drift_warnings
from .engine import Engine
from .focus import open_lens
from .graph import Graph
from .override import DEFAULT_DIR, OverrideCheck, pipewire_pid
from .sampler import GstSampler
from .sensor import open_sensor

OFF_VALUES = ('off', '0', 'false', 'no')
_PRIORITY = {'error': '<3>', 'warning': '<4>', 'info': '<6>'}


def ae_enabled_from_env(env):
    return env.get('CAMERA_AE', '').strip().lower() not in OFF_VALUES


def af_enabled_from_env(env):
    return env.get('CAMERA_AF', '').strip().lower() not in OFF_VALUES


def journal_log(level, msg):
    print(f'{_PRIORITY.get(level, "<6>")}camera-ae: {msg}', flush=True)


def main(argv=None, env=None, spawn=subprocess.Popen, make_engine=Engine,
         install_signal=signal.signal, pid_fn=pipewire_pid,
         sampler_factory=GstSampler, sensor_factory=open_sensor,
         log=journal_log, tuning_dir=DEFAULT_DIR, clock=time.monotonic,
         select_fn=select.select, drift_fn=drift_warnings,
         lens_factory=open_lens,
         remember_path=os.path.expanduser('~/.local/state/camera-ae/remembered.json')):
    env = os.environ if env is None else env
    ae = ae_enabled_from_env(env)
    target = clamp_target(env.get('CAMERA_AE_TARGET'))
    override = OverrideCheck(pid_fn, tuning_dir, log)
    af = af_enabled_from_env(env)
    engine = make_engine(sampler_factory, sensor_factory, override, log, clock,
                         ae, target, af_enabled=af, lens_factory=lens_factory)
    engine.remember_path = remember_path
    if remember_path and hasattr(engine, 'load_remembered'):
        engine.load_remembered()
    log('info', f'start: AE {"on" if ae else "off (CAMERA_AE)"}, target {target}, '
        f'AF {"on (rear)" if af else "off (CAMERA_AF)"}')
    for line in drift_fn():
        log('warning', f'libcamera changed since install ({line}): check '
            '`python3 -m camera_ae.doctor` and the README post-upgrade list')
    if not ae and override():
        log('warning', 'CAMERA_AE=off while the tuning override is active: '
            'browser camera exposure is frozen')

    state = {'term': False, 'dump': None}

    def on_term(signum, frame):                    # no locks here
        state['term'] = True
        dump = state['dump']
        if dump is not None:
            try:
                dump.terminate()
            except Exception:
                pass

    install_signal(signal.SIGTERM, on_term)
    install_signal(signal.SIGINT, on_term)
    graph = Graph()
    try:
        dump = spawn(['pw-dump', '--monitor', '-N'], stdout=subprocess.PIPE,
                     stdin=subprocess.DEVNULL)
        state['dump'] = dump
        if state['term']:
            dump.terminate()
        fd = dump.stdout.fileno()
        while not state['term']:
            ready, _, _ = select_fn([fd, engine.wake_fd], [], [], 1.0)
            if fd in ready:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                try:
                    graph.feed(chunk)
                except Exception:
                    log('error', 'graph: ' + traceback.format_exc(limit=3))
            try:
                engine.evaluate(graph.cameras(own_pids=engine.own_pids()))
            except Exception:
                log('error', 'evaluate: ' + traceback.format_exc(limit=3))
    finally:
        try:
            engine.shutdown()
        except Exception:
            log('error', 'shutdown: ' + traceback.format_exc(limit=3))
        dump = state['dump']
        if dump is not None:
            if dump.poll() is None:
                dump.terminate()
            try:
                dump.wait(2)
            except subprocess.TimeoutExpired:
                dump.kill()
                dump.wait(2)
            try:
                dump.stdout.close()
            except Exception:
                pass
    log('info', 'exit ' + ('(SIGTERM)' if state['term'] else '(pw-dump ended)'))
    return 0 if state['term'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
