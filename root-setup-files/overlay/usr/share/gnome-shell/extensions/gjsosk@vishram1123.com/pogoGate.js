// pogoGate.js -- [erhai] OnePlus pogo folio keyboard attach state.
//
// The pad's pogo keyboard driver exports the live keyboard-stream state as
//
//     /sys/bus/serial/drivers/oneplus-pogo-kbd/<dev>/attached
//
// ("1" while the folio keyboard is attached and streaming, "0" while it is
// away).  gjs-osk uses it so the on-screen keyboard never comes up by itself
// while a real keyboard is clipped onto the tablet.
//
// The attribute is world-readable, so no privileges are needed -- the same
// file the erhai-sensors shell extension and the erhai-pogo-osk system
// service watch.  Kernels without the driver (no directory) report "not
// attached", which turns the gate off and keeps the upstream gjs-osk
// behaviour.
//
// ERHAI_POGO_DRV_DIR overrides the driver directory (test seam used by
// tmp/gjs-osk-erhai/pogo-gate-test.js); it is read on every call, so a test
// can switch between fake driver directories in one process.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const DEFAULT_DRV_DIR = '/sys/bus/serial/drivers/oneplus-pogo-kbd';
const READ_CACHE_MS = 500;

let attachedPath = null;
let attached = false;
let readAtMs = -1e15;

function drvDir() {
    return GLib.getenv('ERHAI_POGO_DRV_DIR') || DEFAULT_DRV_DIR;
}

function resolveAttachedPath() {
    if (attachedPath !== null) {
        if (GLib.file_test(attachedPath, GLib.FileTest.EXISTS))
            return attachedPath;
        attachedPath = null;
    }

    const dir = drvDir();
    try {
        const enumerator = Gio.File.new_for_path(dir).enumerate_children(
            'standard::name', Gio.FileQueryInfoFlags.NONE, null);
        let info;
        while ((info = enumerator.next_file(null)) !== null) {
            const path = `${dir}/${info.get_name()}/attached`;
            if (GLib.file_test(path, GLib.FileTest.EXISTS)) {
                attachedPath = path;
                break;
            }
        }
    } catch (e) {
        attachedPath = null;    // no driver (pre-pogo kernel) -> gate off
    }
    return attachedPath;
}

/**
 * @param {boolean} [force] bypass the 500 ms read cache (attach edges)
 * @returns {boolean} true while the folio keyboard is attached and streaming
 */
export function pogoKeyboardAttached(force = false) {
    const nowMs = GLib.get_monotonic_time() / 1000;
    if (!force && nowMs - readAtMs < READ_CACHE_MS)
        return attached;
    readAtMs = nowMs;
    attached = false;

    const path = resolveAttachedPath();
    if (path === null)
        return false;

    try {
        const [ok, contents] = GLib.file_get_contents(path);
        if (ok) {
            const text = typeof contents === 'string'
                ? contents
                : new TextDecoder().decode(contents);
            attached = text.trim() === '1';
        }
    } catch (e) {
        attached = false;
    }
    return attached;
}

/** Drop the cached path/value (tests; driver rebind). */
export function resetPogoCache() {
    attachedPath = null;
    attached = false;
    readAtMs = -1e15;
}
