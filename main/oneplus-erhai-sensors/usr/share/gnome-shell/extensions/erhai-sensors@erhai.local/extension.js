// erhai Sensor Toggles — Quick Settings toggles for the sensor stack.
//
//   - Auto-rotate: org.gnome.settings-daemon.peripherals.touchscreen
//     orientation-lock (rotation is allowed when the key is false). The
//     accelerometer is the SSC/ADSP BMI3x0 exposed via iio-sensor-proxy.
//     A virtual tablet-mode switch (pinned on) is provided by the
//     erhai-tablet-mode service in this package — mutter's touch mode is
//     therefore always active and mutter's PanelOrientationManaged stays
//     true. The keyboard attach/detach edge is detected from the
//     "attached" sysfs attribute that the oneplus-pogo-kbd driver exports
//     (polled; falls back to mutter's PanelOrientationManaged on older
//     kernels whose switch still followed the keyboard). On ATTACH the
//     current auto-rotate value is saved aside and auto-rotate is forced
//     OFF; on DETACH the saved value is recovered and the save dropped. The user
//     may re-enable rotation at any time (e.g. a portrait photo with the
//     folio still attached). Pending saves persist across sessions
//     (~/.config/erhai-sensors/auto-rotate-restore), so a logout/restart
//     with the keyboard still seated does not lose the original
//     preference — the next detach, even in a later session, recovers it.
//     The toggle itself is ALWAYS enabled — the user may flip it freely
//     at any time; the key is only touched again on the next
//     attach/detach edge.
//   - Auto-brightness: org.erhai.sensors ambient-enabled (our own schema)
//     drives the erhai-alsd user daemon (absolute log curve via logind
//     SetBrightness). gsd's own ambient is force-disabled — its relative
//     normalization anchors 67% at claim-time lux * 1.5, which collapses
//     the whole brightness range into a dim room's few lux on this sensor.
//
// Both are plain dconf keys read/written by gnome-settings-daemon, so the
// toggles only flip them and follow external changes (Settings app, ...) via
// GSettings::changed. The shell's built-in rotation toggle is PERMANENTLY
// kept out of the quick settings grid while this extension is enabled
// (disable() hands it back).
//
// GNOME 50 notes: the quick settings grid moved from quickSettings._grid to
// quickSettings.menu._grid (QuickSettings is a PanelMenu.Button defined in
// panel.js now), and the built-in indicators — including Auto Rotate — are
// created asynchronously in QuickSettings._setupIndicators(). The old
// one-shot removal crashed on GNOME 50 ("quickSettings._grid is undefined"),
// which left the native toggle visible; the logic below handles both facts.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const AutoRotateToggle = GObject.registerClass(
class AutoRotateToggle extends QuickToggle {
    _init() {
        super._init({
            title: 'Auto-rotate',
            iconName: 'rotation-allowed-symbolic',
        });

        this._settings = new Gio.Settings({
            schema_id: 'org.gnome.settings-daemon.peripherals.touchscreen',
        });
        this._changedId = this._settings.connect('changed::orientation-lock',
            () => this._sync());
        this.connect('clicked', () => {
            const locked = this._settings.get_boolean('orientation-lock');
            this._settings.set_boolean('orientation-lock', !locked);
        });
        this.connect('destroy', () => {
            this._settings.disconnect(this._changedId);
            if (this._pollAttachId) {
                GLib.source_remove(this._pollAttachId);
                this._pollAttachId = 0;
            }
        });

        // Track the folio (pogo keyboard) via the driver's "attached" sysfs
        // attribute: the kernel PINS SW_TABLET_MODE on (mutter's touch mode
        // is always active, keeping its rotation machinery available) and
        // exports the live keyboard-stream state as sysfs instead. On ATTACH
        // the current auto-rotate value is saved aside and rotation is
        // forced off; on DETACH the saved value is recovered. The toggle
        // itself stays sensitive at all times.
        this._attachedPath = this._findAttachedFile();

        // Pending pre-attach auto-rotate value, persisted so a logout /
        // shell restart / crash with the keyboard still attached does not
        // lose the original preference: the next DETACH — even in a later
        // session — recovers it.
        this._savePath = GLib.build_filenamev([
            GLib.get_user_config_dir(), 'erhai-sensors', 'auto-rotate-restore',
        ]);

        this._lastCanRotate = null;
        // Apply the attach/detach rules once for the state at (re)load: an
        // already-seated keyboard saves the current value (or keeps an
        // inherited pending save) and forces rotation off; a detached
        // keyboard with a pending save recovers it. Detached with nothing
        // pending, the user's persisted preference is left untouched.
        // With the sysfs attribute the state is exact; without it the first
        // mutter poll below seeds (and applies) the state.
        if (this._attachedPath) {
            const attached = this._readAttached(this._attachedPath);
            if (attached !== null)
                this._applyAttachState(!attached, true);
        }

        // 1 s poll for the attach/detach edge. With the sysfs attribute the
        // edge comes straight from the driver; without it (older kernels) we
        // fall back to mutter's PanelOrientationManaged property, which on
        // those kernels mirrors the tablet-mode switch.
        this._pollAttachId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT, 1,
            () => {
                this._pollAttachState();
                return GLib.SOURCE_CONTINUE;
            });
    }

    // ---- folio state sources ------------------------------------------------

    _findAttachedFile() {
        // /sys/bus/serial/drivers/oneplus-pogo-kbd/<device>/attached
        try {
            const base = '/sys/bus/serial/drivers/oneplus-pogo-kbd';
            const it = Gio.File.new_for_path(base).enumerate_children(
                'standard::name', Gio.FileQueryInfoFlags.NONE, null);
            let info;
            while ((info = it.next_file(null)) !== null) {
                const name = info.get_name();
                const path = `${base}/${name}/attached`;
                if (Gio.File.new_for_path(path).query_exists(null))
                    return path;
            }
        } catch (e) {
            // driver absent (older kernel) — fall back to mutter polling
        }
        return null;
    }

    // Returns true (keyboard attached), false (detached) or null (unknown).
    _readAttached(path) {
        try {
            const [, data] = GLib.file_get_contents(path);
            // GLib.file_get_contents() hands back a Uint8Array; never rely on
            // the deprecated implicit array.toString() (it still decodes the
            // bytes like a string today, but a future GJS returns
            // comma-separated digits -- "1" would then never match and the
            // pogo keyboard would always look DETACHED, i.e. auto-rotate would
            // stay on with the folio seated).
            return new TextDecoder().decode(data).trim() === '1';
        } catch (e) {
            return null;
        }
    }

    // ---- pogo keyboard attach/detach handling ------------------------------

    // Pending save helpers: the file stores the orientation-lock value the
    // user had before the keyboard was attached ('locked' / 'unlocked').
    // Returns true/false, or null when there is no (valid) pending save.
    _loadSaved() {
        try {
            const [ok, data] = GLib.file_get_contents(this._savePath);
            if (ok && data) {
                const value = new TextDecoder().decode(data).trim();
                if (value === 'locked')
                    return true;
                if (value === 'unlocked')
                    return false;
            }
        } catch {
            // absent or unreadable — no pending save
        }
        return null;
    }

    _writeSaved(locked) {
        try {
            GLib.mkdir_with_parents(GLib.path_get_dirname(this._savePath),
                0o700);
            GLib.file_set_contents(this._savePath,
                locked ? 'locked' : 'unlocked');
        } catch (e) {
            log(`erhai-sensors: could not save auto-rotate state: ${e}`);
        }
    }

    _clearSaved() {
        try {
            GLib.unlink(this._savePath);
        } catch {
            // already gone
        }
    }

    // Attach/detach edge handler.
    //   ATTACH (canRotate false): save the current orientation-lock value
    //     (unless a pending save is already inherited from an earlier
    //     attach) and force auto-rotate OFF.
    //   DETACH (canRotate true): recover the saved value and drop the
    //     save; with nothing pending, leave the current value untouched.
    // `initial` marks the one-time application at extension load (no edge,
    // but the same rules).
    _applyAttachState(canRotate, initial = false) {
        // Ignore duplicate reports that carry no state change.
        if (!initial && canRotate === this._lastCanRotate)
            return;
        this._lastCanRotate = canRotate;

        if (canRotate) {
            const saved = this._loadSaved();
            if (saved !== null) {
                this._settings.set_boolean('orientation-lock', saved);
                this._clearSaved();
                log(`erhai-sensors: pogo keyboard detached — auto-rotate recovered to ${saved ? 'LOCKED' : 'ON'}`);
            } else {
                log('erhai-sensors: pogo keyboard detached — no saved auto-rotate state, value left as-is');
            }
        } else {
            if (this._loadSaved() === null)
                this._writeSaved(this._settings.get_boolean('orientation-lock'));
            this._settings.set_boolean('orientation-lock', true);
            log('erhai-sensors: pogo keyboard attached — auto-rotate turned off (previous value saved for recovery)');
        }

        this._sync();
    }

    // ---- folio edge poller -----------------------------------------------
    // On OLD kernels mutter's notify::panel-orientation-managed never fires
    // for later switch toggles (libinput deduplicates the driver's re-asserted
    // switch state, so the seat's TOUCH_MODE signal never fires) — hence the
    // direct D-Bus property poll in the fallback branch below.
    _pollAttachState() {
        // Preferred source: the driver's sysfs attribute (pinned-switch
        // kernels). The 1 s poll turns the stream edge into an edge here.
        if (this._attachedPath) {
            const attached = this._readAttached(this._attachedPath);
            if (attached === null)
                return;
            const canRotate = !attached;
            if (canRotate === this._lastCanRotate)
                return;
            this._applyAttachState(canRotate);
            return;
        }

        // Fallback (older kernels, switch follows keyboard): mutter's
        // PanelOrientationManaged. Mutter updates the D-Bus property even
        // when the GObject notify is swallowed by libinput's event
        // deduplication, hence the direct property poll.
        try {
            const conn = Gio.DBus.session;
            const result = conn.call_sync(
                'org.gnome.Mutter.DisplayConfig',
                '/org/gnome/Mutter/DisplayConfig',
                'org.freedesktop.DBus.Properties', 'Get',
                new GLib.Variant('(ss)', [
                    'org.gnome.Mutter.DisplayConfig',
                    'PanelOrientationManaged',
                ]),
                null, /* reply_type (infer from method) */
                Gio.DBusCallFlags.NONE, -1, null);
            if (!result)
                return;

            const {deep_unpack} = result;
            const value = deep_unpack()[0];
            this._applyAttachState(value === true);
        } catch (e) {
            log(`erhai-sensors: pollAttachState error: ${e}`);
        }
    }
    _sync() {
        const auto = !this._settings.get_boolean('orientation-lock');

        // ALWAYS enabled: the keyboard handling above only writes the dconf
        // key; the user may flip this toggle freely at any time.
        this.reactive = true;
        this.opacity = 255;
        this.checked = auto;
        this.iconName = auto
            ? 'rotation-allowed-symbolic'
            : 'rotation-locked-symbolic';
        this.subtitle = auto ? 'On' : 'Locked';
    }
});

const AutoBrightnessToggle = GObject.registerClass(
class AutoBrightnessToggle extends QuickToggle {
    _init(alsSettings) {
        super._init({
            title: 'Auto-brightness',
            iconName: 'weather-clear-symbolic',
        });

        // Our OWN schema (shipped in the extension's schemas/ dir): drives
        // the erhai-alsd absolute log-curve daemon. gsd's relative
        // normalization is unusable on this sensor (it anchors 67% at
        // claim-time lux * 1.5, collapsing the whole range into a dim
        // room's few lux) and is kept permanently off.
        this._settings = alsSettings;
        this._changedId = this._settings.connect('changed::ambient-enabled',
            () => this._sync());
        this.connect('clicked', () => {
            const enabled = this._settings.get_boolean('ambient-enabled');
            this._settings.set_boolean('ambient-enabled', !enabled);
        });
        this.connect('destroy', () => this._settings.disconnect(this._changedId));

        this._sync();
    }

    _sync() {
        const enabled = this._settings.get_boolean('ambient-enabled');
        this.checked = enabled;
        this.subtitle = enabled ? 'On' : 'Off';
    }
});

const SensorIndicator = GObject.registerClass(
class SensorIndicator extends SystemIndicator {
    _init(alsSettings) {
        super._init();
        this.quickSettingsItems.push(new AutoRotateToggle());
        this.quickSettingsItems.push(new AutoBrightnessToggle(alsSettings));
    }
});

// Timeout knobs for the built-in toggle suppression.
const ROTATE_RETRY_INTERVAL_MS = 250;
const ROTATE_MAX_RETRIES = 60;        // 15 s for the async panel setup
const ROTATE_SWEEP_INTERVAL_S = 10;   // slow safety-net sweep

export default class ErhaiSensorsExtension extends Extension {
    enable() {
        // Our own GSettings schema lives in the extension's schemas/ dir.
        const schemaSource = Gio.SettingsSchemaSource.new_from_directory(
            `${this.dir.get_path()}/schemas`,
            Gio.SettingsSchemaSource.get_default(),
            false);
        const alsSchema = schemaSource.lookup('org.erhai.sensors', false);
        if (!alsSchema)
            logError(new Error('org.erhai.sensors schema missing'));
        this._alsSettings = new Gio.Settings({settings_schema: alsSchema});

        this._indicator = new SensorIndicator(this._alsSettings);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);

        // Permanently suppress the shell's built-in "Auto Rotate" toggle
        // (it would duplicate ours whenever mutter reports tablet mode).
        this._nativeRotateItem = null;
        this._nativeGrid = null;
        this._nativeVisibleId = 0;
        this._childAddedId = 0;
        this._retryId = 0;
        this._retryCount = 0;
        this._sweepId = 0;
        this._suppressNativeRotateToggle();
    }

    // ---- built-in "Auto Rotate" suppression -------------------------------

    _getGrid() {
        const qs = Main.panel?.statusArea?.quickSettings;
        // GNOME >= 50: QuickSettingsMenu owns the grid; GNOME 48/49 had it
        // directly on the QuickSettings indicator.
        return qs?.menu?._grid ?? qs?._grid ?? null;
    }

    _isNativeRotateToggle(child) {
        if ((this._indicator?.quickSettingsItems ?? []).includes(child))
            return false;   // one of ours (they also use rotation-* icons)

        const names = [
            child.constructor?.name,
            child.constructor?.$gtype?.name,
        ].join(' ');
        if (names.includes('RotationToggle'))
            return true;

        const icon = child.icon_name ?? '';
        if (icon.startsWith('rotation-') ||
            icon === 'object-rotate-right-symbolic')
            return true;

        return child.title === 'Auto Rotate';
    }

    _suppressNativeRotateToggle() {
        const grid = this._getGrid();
        if (!grid)
            return this._scheduleRotateRetry();

        if (!this._childAddedId) {
            // Watch the grid so a native toggle re-added at runtime (rebuilt
            // indicators, ...) is thrown out again immediately.
            this._nativeGrid = grid;
            this._childAddedId = grid.connect('child-added', (g, child) => {
                if (this._isNativeRotateToggle(child))
                    this._removeNativeRotateItem(child, g);
            });
        }

        if (!this._removeNativeRotateItem()) {
            // panel.js creates the built-in indicators asynchronously
            // (QuickSettings._setupIndicators), so the toggle may not exist
            // yet — retry for a while before relying on the sweep alone.
            if (this._retryCount < ROTATE_MAX_RETRIES)
                return this._scheduleRotateRetry();
        }

        // Safety net: re-check periodically in case anything escapes the
        // child-added watcher.
        if (!this._sweepId) {
            this._sweepId = GLib.timeout_add_seconds(
                GLib.PRIORITY_DEFAULT, ROTATE_SWEEP_INTERVAL_S,
                () => {
                    this._sweepId = 0;
                    this._suppressNativeRotateToggle();
                    return GLib.SOURCE_REMOVE;
                });
        }
    }

    _scheduleRotateRetry() {
        this._retryCount++;
        if (this._retryId)
            return;
        this._retryId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
            ROTATE_RETRY_INTERVAL_MS, () => {
                this._retryId = 0;
                this._suppressNativeRotateToggle();
                return GLib.SOURCE_REMOVE;
            });
    }

    _removeNativeRotateItem(item = null, grid = null) {
        grid ??= this._getGrid();
        if (!grid)
            return false;

        item ??= grid.get_children().find(c => this._isNativeRotateToggle(c));
        if (!item)
            return false;

        if (this._nativeRotateItem && this._nativeRotateItem !== item)
            return false;   // already suppressing an instance

        this._nativeRotateItem = item;
        this._nativeGrid = grid;
        grid.remove_child(item);
        log('erhai-sensors: removed built-in "Auto Rotate" toggle from quick settings');

        // Belt and braces: the shell only ever flips visibility on the
        // toggle, so make sure it can never actually show again even if it
        // were to end up back inside some parent we do not observe.
        if (!this._nativeVisibleId) {
            this._nativeVisibleId = item.connect('notify::visible',
                () => item.visible = false);
        }
        return true;
    }

    disable() {
        if (this._retryId) {
            GLib.source_remove(this._retryId);
            this._retryId = 0;
        }
        this._retryCount = 0;
        if (this._sweepId) {
            GLib.source_remove(this._sweepId);
            this._sweepId = 0;
        }
        if (this._childAddedId && this._nativeGrid) {
            try {
                this._nativeGrid.disconnect(this._childAddedId);
            } catch {
                // grid may already be torn down during shell shutdown
            }
        }
        this._childAddedId = 0;
        this._nativeGrid = null;

        // Hand the built-in toggle back when this extension goes away.
        const item = this._nativeRotateItem;
        if (item) {
            if (this._nativeVisibleId) {
                try {
                    item.disconnect(this._nativeVisibleId);
                } catch {
                    // item may already be destroyed
                }
                this._nativeVisibleId = 0;
            }
            const grid = this._getGrid();
            try {
                if (grid && item.get_parent() !== grid) {
                    grid.add_child(item);
                    grid.layout_manager.child_set_property(
                        grid, item, 'column-span', 1);
                }
            } catch {
                // cosmetic restore only; ignore teardown races
            }
            this._nativeRotateItem = null;
        }

        this._indicator?.quickSettingsItems.forEach(t => t.destroy());
        this._indicator?.destroy();
        this._indicator = null;
    }
}
