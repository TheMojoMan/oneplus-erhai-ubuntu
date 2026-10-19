// erhai Bypass Charging — a Quick Settings switch that stops the battery from
// charging while a charger is attached, so the tablet runs from the brick.
//
// Measured on this pad (2026-09-30): pack current 3.93 A -> 0.117 A (0.49 W,
// i.e. system-only) with the switch on, and back to 1.74 A with it off; the
// panel/CPU/GPU keep running on adapter power throughout.
//
// The switch itself writes nothing: the ADSP property write needs root, so a
// click starts erhai-bypass-charge@<on|off>.service, which polkit lets an
// active local user start without a prompt (50-erhai-bypass-charge.rules).
// That service runs /usr/local/sbin/erhai-bypass-charge.sh, which holds the
// ADSP charge-enable property (BATT_CHG_EN) down while bypass is on, parks the
// in-kernel max_soc limiter (it drives the same property), and applies a
// low-SoC safety floor.  The script persists the choice to
// /etc/default/erhai-charge, and erhai-bypass-charge.service re-applies it at
// boot — so this extension only READS state, exactly like the thermal toggle
// reads /etc/default/erhai-thermal.
//
// GNOME 50 notes: QuickMenuToggle (ui/quickSettings.js) is constructed with
// `title`/`iconName` and gets its row switch from `checked`; a click flips
// `checked` and then emits 'clicked', which is why the request below is
// computed from the state we actually applied (_bypass) instead of from
// `checked` — that way the switch cannot invert itself if the click semantics
// ever change, and a failed systemctl call is corrected by the resync.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const STATE_FILE = '/etc/default/erhai-charge';
const GAUGE = '/sys/class/power_supply/bq27z561-0';
const POWER_SUPPLY_DIR = '/sys/class/power_supply';
// The in-kernel charge limit (erhai_otg_glink): 1..99 = stop/bypass at the
// cap, 0/100 = charge to full.  The holder also bypasses at this cap, so the
// subtitle can say when the limit is what is holding the pack.
const MAX_SOC = '/sys/module/erhai_otg_glink/parameters/max_soc';

// The two modes the helper accepts; also the template unit's instance names.
const MODE_ON = 'on';
const MODE_OFF = 'off';

const ICON_ON = 'ac-adapter-symbolic';
const ICON_OFF = 'battery-good-charging-symbolic';

// How often the live part of the subtitle (pack current / adapter presence) is
// re-read. The switch state itself is event-driven (the state file monitor).
const LIVE_POLL_SECONDS = 5;

function readText(path) {
    try {
        const [, contents] = Gio.File.new_for_path(path).load_contents(null);
        return new TextDecoder().decode(contents).trim();
    } catch (e) {
        return null;
    }
}

// Persisted choice: 1 = bypass charging on. Anything unreadable is "off".
function readBypass() {
    const contents = readText(STATE_FILE);
    if (contents === null)
        return false;
    const match = /^ERHAI_BYPASS_CHARGE=([01])\s*$/m.exec(contents);
    return match !== null && match[1] === '1';
}

// Any USB-C source (charger / powered hub) online, as the ADSP reports it.
// Enumerated rather than globbed: the psy name carries a pmic_glink suffix that
// can change with the port/UCSI instance.
function adapterOnline() {
    let dir;
    try {
        dir = Gio.File.new_for_path(POWER_SUPPLY_DIR);
        const iter = dir.enumerate_children('standard::name',
            Gio.FileQueryInfoFlags.NONE, null);
        let info;
        while ((info = iter.next_file(null)) !== null) {
            if (!info.get_name().startsWith('ucsi-source-psy-'))
                continue;
            if (readText(`${POWER_SUPPLY_DIR}/${info.get_name()}/online`) === '1')
                return true;
        }
    } catch (e) {
        return false;
    }
    return false;
}

// Pack current in mA (this gauge reports positive = current INTO the pack).
function packCurrentMa() {
    const raw = readText(`${GAUGE}/current_now`);
    const value = Number.parseInt(raw ?? '', 10);
    return Number.isNaN(value) ? null : value;
}

// Pack state of charge in % (null when the gauge is unreadable).
function packSoc() {
    const raw = readText(`${GAUGE}/capacity`);
    const value = Number.parseInt(raw ?? '', 10);
    return Number.isNaN(value) ? null : value;
}

// The in-kernel charge limit in %, or null when it is disabled (0/100).
function chargeLimit() {
    const value = Number.parseInt(readText(MAX_SOC) ?? '', 10);
    return Number.isInteger(value) && value >= 1 && value < 100 ? value : null;
}

const BypassToggle = GObject.registerClass(
class BypassToggle extends QuickMenuToggle {
    _init() {
        super._init({
            title: 'Bypass charging',
            iconName: ICON_OFF,
            toggleMode: true,
        });

        this._bypass = readBypass();
        this._destroyed = false;

        // The click flips `checked` first, so the request is derived from the
        // state we believe is applied — never from `checked` — and `_sync()`
        // below reconciles the UI with the state file afterwards.
        this.connect('clicked', () => this._set(this._bypass ? MODE_OFF : MODE_ON));

        this._onItem = new PopupMenu.PopupImageMenuItem(
            'Bypass charging (adapter power only)', ICON_ON);
        this._onItem.connect('activate', () => this._set(MODE_ON));
        this.menu.addMenuItem(this._onItem);

        this._offItem = new PopupMenu.PopupImageMenuItem('Normal charging', ICON_OFF);
        this._offItem.connect('activate', () => this._set(MODE_OFF));
        this.menu.addMenuItem(this._offItem);

        this.menu.setHeader(ICON_OFF, 'Bypass charging',
            'Run from the adapter, hold the battery');

        // The helper is also usable from a terminal (and re-applied by the boot
        // service), so follow the file instead of assuming this toggle is the
        // only writer.
        try {
            this._monitor = Gio.File.new_for_path(STATE_FILE)
                .monitor_file(Gio.FileMonitorFlags.NONE, null);
            this._monitor.connect('changed', () => this._sync());
        } catch (e) {
            this._monitor = null;
        }

        this._liveId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT,
            LIVE_POLL_SECONDS, () => {
                this._syncSubtitle();
                return GLib.SOURCE_CONTINUE;
            });

        // The shell destroys the widget on logout / extension disable; the
        // timer and the file monitor must go with it (a live timer on a
        // destroyed actor is a GJS "already disposed" crash).
        this.connect('destroy', () => this._onDestroy());

        this._sync();
    }

    _set(mode) {
        const proc = Gio.Subprocess.new(
            ['systemctl', 'start', `erhai-bypass-charge@${mode}.service`],
            Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_PIPE);

        proc.communicate_utf8_async(null, null, (source, result) => {
            let stderr = '';
            try {
                [, , stderr] = source.communicate_utf8_finish(result);
            } catch (e) {
                stderr = e.message;
            }
            if (this._destroyed)
                return;
            if (!source.get_successful())
                Main.notifyError('Bypass charging',
                    stderr.trim() || `Could not switch to “${mode}”.`);
            // A refused write leaves the state file alone, so re-read either way
            // instead of assuming the click took effect.
            this._sync();
        });
    }

    _sync() {
        const bypass = readBypass();
        this._bypass = bypass;

        this.checked = bypass;
        this.iconName = bypass ? ICON_ON : ICON_OFF;
        this.menu.setHeader(bypass ? ICON_ON : ICON_OFF, 'Bypass charging',
            bypass ? 'Battery held · tablet on adapter power'
                   : 'Battery charges normally');

        this._onItem.setOrnament(bypass
            ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NO_DOT);
        this._offItem.setOrnament(bypass
            ? PopupMenu.Ornament.NO_DOT : PopupMenu.Ornament.CHECK);

        this._syncSubtitle();
    }

    _syncSubtitle() {
        if (this._destroyed)
            return;

        const online = adapterOnline();
        const current = packCurrentMa();
        const amps = current === null ? null : Math.abs(current) / 1000;

        if (this._bypass) {
            if (!online)
                this.subtitle = 'Waiting for a charger';
            else if (amps === null || amps < 0.3)
                this.subtitle = 'Adapter power only · pack idle';
            else
                this.subtitle = `Adapter power only · pack ${amps.toFixed(2)} A`;
        } else {
            // With no manual bypass the in-kernel charge limit still bypasses
            // at the cap, so say so when the pack is sitting there.
            const limit = chargeLimit();
            const soc = packSoc();
            if (limit !== null && soc !== null && soc >= limit)
                this.subtitle = `Charge limit ${limit}% · adapter power only`;
            else if (limit !== null && online)
                this.subtitle = `Charging normally · limit ${limit}%`;
            else
                this.subtitle = online ? 'Charging normally' : 'Not charging';
        }
    }

    _onDestroy() {
        this._destroyed = true;

        if (this._liveId) {
            GLib.source_remove(this._liveId);
            this._liveId = 0;
        }
        this._monitor?.cancel();
        this._monitor = null;
    }
});

const BypassIndicator = GObject.registerClass(
class BypassIndicator extends SystemIndicator {
    _init() {
        super._init();

        // Always offered, charger or not: bypass is a POLICY ("when I plug in,
        // do not charge"), so it is meaningful to set it up while unplugged —
        // the boot service and the holder loop then keep it across attaches.
        this.quickSettingsItems.push(new BypassToggle());
    }
});

export default class ErhaiBypassExtension extends Extension {
    enable() {
        this._indicator = new BypassIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator?.quickSettingsItems.forEach(item => item.destroy());
        this._indicator?.destroy();
        this._indicator = null;
    }
}
