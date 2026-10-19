// erhai Thermal Profile — a Quick Settings menu toggle for erhai-thermal.sh.
//
// Deliberately separate from the shell's own Power Mode toggle: power-profiles-daemon finds
// no driver on this device and falls back to its placeholder (balanced + power-saver only),
// and its D-Bus API is limited to three fixed profile names, which cannot express our four
// temperature ceilings. Modelled on ui/status/powerProfiles.js so it behaves the same way.
//
// The sysfs writes need root, so a switch starts erhai-thermal@<profile>.service, which
// polkit lets the local active user start without a prompt (50-erhai-thermal.rules). The
// script persists the choice and the boot service re-applies it; this extension only reads
// /etc/default/erhai-thermal for state, and never writes it.

import Gio from 'gi://Gio';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const STATE_FILE = '/etc/default/erhai-thermal';
// The stock ceiling, and the one the toggle returns to when clicked while highlighted.
const DEFAULT_PROFILE = 'performance';

const PROFILES = [
    {id: 'performance', name: 'Performance (Android-like)', iconName: 'power-profile-performance-symbolic'},
    {id: 'balanced', name: 'Balanced (~90 °C)', iconName: 'power-profile-balanced-symbolic'},
    {id: 'cool', name: 'Cool (~80 °C)', iconName: 'power-profile-power-saver-symbolic'},
    {id: 'cold', name: 'Cold (~70 °C)', iconName: 'power-profile-power-saver-symbolic'},
];

// The script's aliases, so a state file written by hand -- or before the android ->
// performance rename -- still selects the right entry.
const ALIASES = {
    'android': 'performance',
    'stock': 'performance',
    'perf': 'performance',
    'gaming': 'cold',
    '100': 'performance',
    '90': 'balanced',
    '80': 'cool',
    '70': 'cold',
};

function readProfile() {
    let contents;
    try {
        [, contents] = Gio.File.new_for_path(STATE_FILE).load_contents(null);
    } catch (e) {
        return DEFAULT_PROFILE;
    }

    const match = /^ERHAI_THERMAL_PROFILE=(\S+)/m.exec(new TextDecoder().decode(contents));
    if (!match)
        return DEFAULT_PROFILE;

    const id = ALIASES[match[1]] ?? match[1];
    return PROFILES.some(p => p.id === id) ? id : DEFAULT_PROFILE;
}

const ThermalProfilesToggle = GObject.registerClass(
class ThermalProfilesToggle extends QuickMenuToggle {
    _init() {
        super._init({
            title: 'Thermal',
            menuButtonAccessibleName: 'Open thermal profiles menu',
        });

        this._lastProfile = 'balanced';

        this.connect('clicked', () => {
            this._setProfile(this.checked ? DEFAULT_PROFILE : this._lastProfile);
        });

        this._items = new Map();
        for (const {id, name, iconName} of PROFILES) {
            const item = new PopupMenu.PopupImageMenuItem(name, iconName);
            item.connect('activate', () => this._setProfile(id));
            this.menu.addMenuItem(item);
            this._items.set(id, item);
        }

        this.menu.setHeader('power-profile-performance-symbolic', 'Thermal Profile',
            'Whole-die governor (CPU + GPU + NPU + DDR)');

        // The script is also usable from a terminal, so follow the file rather than assume
        // this toggle is the only writer.
        this._monitor = Gio.File.new_for_path(STATE_FILE).monitor_file(Gio.FileMonitorFlags.NONE, null);
        this._monitor.connect('changed', () => this._sync());
        this.connect('destroy', () => this._monitor.cancel());

        this._sync();
    }

    _setProfile(id) {
        const proc = Gio.Subprocess.new(
            ['systemctl', 'start', `erhai-thermal@${id}.service`],
            Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_PIPE);

        proc.communicate_utf8_async(null, null, (source, result) => {
            let stderr = '';
            try {
                [, , stderr] = source.communicate_utf8_finish(result);
            } catch (e) {
                stderr = e.message;
            }
            if (!source.get_successful())
                Main.notifyError('Thermal profile', stderr.trim() || `Could not apply ${id}.`);
            // A failed switch leaves the state file alone, so re-read either way instead of
            // assuming the click took effect.
            this._sync();
        });
    }

    _sync() {
        const active = readProfile();
        const {name, iconName} = PROFILES.find(p => p.id === active);

        if (active !== DEFAULT_PROFILE)
            this._lastProfile = active;

        this.subtitle = name.replace(/ \(.*/, '');
        this.iconName = iconName;
        this.menu.setHeader(iconName, 'Thermal Profile', name);
        // Highlight whenever the device is off its stock ceiling, matching how the shell's
        // own Power Mode toggle signals a deviation from the default.
        this.checked = active !== DEFAULT_PROFILE;

        for (const [id, item] of this._items) {
            item.setOrnament(id === active
                ? PopupMenu.Ornament.CHECK
                : PopupMenu.Ornament.NO_DOT);
        }
    }
});

const ThermalIndicator = GObject.registerClass(
class ThermalIndicator extends SystemIndicator {
    _init() {
        super._init();
        this.quickSettingsItems.push(new ThermalProfilesToggle());
    }
});

export default class ErhaiThermalExtension extends Extension {
    enable() {
        this._indicator = new ThermalIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator.quickSettingsItems.forEach(item => item.destroy());
        this._indicator.destroy();
        this._indicator = null;
    }
}
