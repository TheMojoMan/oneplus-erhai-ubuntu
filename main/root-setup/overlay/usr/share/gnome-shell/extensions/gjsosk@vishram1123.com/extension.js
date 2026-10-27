import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Shell from 'gi://Shell';


import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import * as KeyboardManager from 'resource:///org/gnome/shell/misc/keyboardManager.js';
import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as InputSourceManager from 'resource:///org/gnome/shell/ui/status/keyboard.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js'
const [major, minor] = Config.PACKAGE_VERSION.split('.').map(s => Number(s));
import { Dialog } from 'resource:///org/gnome/shell/ui/dialog.js';
let EdgeDragAction = null;
if (major < 49) {
    try {
        const edgedragimport = await import('resource:///org/gnome/shell/ui/edgeDragAction.js')
        EdgeDragAction = edgedragimport ?? null
    } catch { }
}
import { Extension, gettext as _ } from 'resource:///org/gnome/shell/extensions/extension.js';
// [erhai] folio (pogo keyboard) attach state -- see pogoGate.js
import { pogoKeyboardAttached } from './pogoGate.js';

const State = {
    OPENED: 0,
    CLOSED: 1,
    OPENING: 2,
    CLOSING: 3,
};

/* [erhai] Shell 51 key injection.  gjs-osk injects hardware KEYCODES through
 * notify_key() everywhere, but text actors INSIDE the shell (a shell dialog's
 * entry: the Wi-Fi secret prompt, the GDM auth prompt) only take KEYVALS -- the
 * same thing Shell 51's own keyboard sends (ui/keyboard.js keyvalPress).  The
 * keyval comes from the label the key shows at that moment, so the Shift /
 * CapsLock / Alt layers produce their real character. */
const OSK_SPECIAL_KEYVALS = {
    esc: 0xff1b, escape: 0xff1b, backspace: 0xff08, tab: 0xff09,
    enter: 0xff0d, return: 0xff0d, space: 0x20, capslock: 0xffe5,
    shift: 0xffe1, ctrl: 0xffe3, control: 0xffe3, alt: 0xffe9,
    super: 0xffeb, menu: 0xff67, left: 0xff51, up: 0xff52,
    right: 0xff53, down: 0xff54, delete: 0xffff, del: 0xffff,
    home: 0xff50, end: 0xff57, pageup: 0xff55, pagedown: 0xff56,
    pgup: 0xff55, pgdn: 0xff56, insert: 0xff63, ins: 0xff63,
    print: 0xff61, sysrq: 0xff15, pause: 0xff13, numlock: 0xff7f,
    f1: 0xffbe, f2: 0xffbf, f3: 0xffc0, f4: 0xffc1, f5: 0xffc2,
    f6: 0xffc3, f7: 0xffc4, f8: 0xffc5, f9: 0xffc6, f10: 0xffc7,
    f11: 0xffc8, f12: 0xffc9,
};

/** Label shown on a key -> XKB keyval; 0 when the label is not usable. */
/* Highest keyval we will hand to notify_keyval(): the Unicode keysym range
 * ends at 0x0110ffff (unicode_to_keysym() returns 0x01000000 + codepoint for
 * characters without a legacy keysym). */
const OSK_MAX_KEYVAL = 0x0110ffff;

function keyvalFromLabel(label) {
    if (label == null)
        return 0;
    const cps = [...String(label)];
    let keyval;
    if (cps.length === 1) {
        /* Shell 51's own keyboard uses the same call for a character
         * (ui/keyboard.js _getKeyvalsFromString): it maps a codepoint to its
         * real keysym, which matters for every non-Latin-1 character. */
        keyval = cps[0].codePointAt(0);
        if (typeof Clutter !== 'undefined' && Clutter.unicode_to_keysym)
            keyval = Clutter.unicode_to_keysym(keyval);
    } else {
        keyval = OSK_SPECIAL_KEYVALS[String(label).toLowerCase()] ?? 0;
    }
    /* notify_keyval() takes a guint32 keysym, so refuse anything else -- an
     * out-of-range value aborts the call and loses the keystroke. */
    if (!Number.isInteger(keyval) || keyval <= 0 || keyval > OSK_MAX_KEYVAL)
        return 0;
    return keyval;
}

/* [erhai] These five helpers MUST be module-level functions, never Keyboard
 * methods: initializeKeyboard() replaces every Keyboard method with an `async`
 * wrapper (withErrorHandler), so `this.keyvalForCode(x)` returns a PROMISE and
 * not a number.  That is exactly what broke 2.32-test-02..04 on 26.10 -- a
 * Promise reached notify_keyval() ("value is out of range for uint32" /
 * "[object Promise]"), and `this.shellTextFocused()` was always truthy, so
 * every app also took the shell-dialog path.  Keep them pure and synchronous. */
function oskButtonForCode(kb, code) {
    return (kb.keys || []).find(k => k.char != undefined && k.char.code == code);
}

/* The label layer the keyboard is showing right now (same string
 * updateKeyLabels() builds, i.e. a key of the layout's `layers` object). */
function oskLabelLayer(kb) {
    const layer = (kb.alt ? 'alt' : '') + (kb.shift ? 'shift' : '') +
        (kb.numsL ? 'num' : '') + (kb.capsL ? 'caps' : '') +
        (kb.numsL || kb.capsL ? 'lock' : '');
    return layer == '' ? 'default' : layer;
}

/* Label of a key in one layer.  Icon keys ("show-icons") have their labels
 * moved to "_<layer>" and the visible layer set to null, so look there too. */
function oskLabelForLayer(char, layer) {
    const layers = char.layers || {};
    let label = layers[layer];
    if (label == null)
        label = layers['_' + layer];
    if (label == null)
        label = layers.default;
    if (label == null)
        label = layers._default;
    return label;
}

/* Keyval of a key in its default layer (cached; the label never changes). */
function oskKeyvalForCode(kb, code) {
    if (kb._keyvalCache === undefined)
        kb._keyvalCache = Object.create(null);
    if (kb._keyvalCache[code] !== undefined)
        return kb._keyvalCache[code];
    const btn = oskButtonForCode(kb, code);
    const label = btn && btn.char ? oskLabelForLayer(btn.char, 'default') : undefined;
    const keyval = keyvalFromLabel(label);
    kb._keyvalCache[code] = keyval;
    return keyval;
}

/* Keyval to inject for `code` into a text actor INSIDE the shell: the
 * character the key shows at that moment, and NOTHING for a modifier key --
 * its state is the keyboard's own (it selects the label layer), and a shell
 * entry would just see a Shift_L keysym it cannot use. */
function oskShellKeyvalForCode(kb, code) {
    const btn = oskButtonForCode(kb, code);
    if (btn && btn.char && btn.char.isMod)
        return 0;
    if (btn && btn.char && btn.char.layers) {
        const keyval = keyvalFromLabel(oskLabelForLayer(btn.char, oskLabelLayer(kb)));
        if (keyval)
            return keyval;
    }
    return oskKeyvalForCode(kb, code);
}

/* True while the stage's key focus is a text actor INSIDE the shell (a shell
 * dialog's entry).  Application windows focus a Meta.Window instead, which is
 * why they are not covered by this test -- the same rule Shell 51's own
 * keyboard uses (ui/keyboard.js _onKeyFocusChanged). */
function oskShellTextFocused() {
    return global.stage.key_focus instanceof Clutter.Text;
}

/* Bounded diagnostic for the Shell 51 shell-dialog typing path: the first
 * events are logged with their keyvals so a broken injection can be read from
 * the journal instead of guessed at. */
function oskDbg(kb, msg) {
    if (kb._oskDbgN === undefined)
        kb._oskDbgN = 0;
    if (kb._oskDbgN >= 12)
        return;
    kb._oskDbgN++;
    console.debug(`GJS-OSK dbg ${kb._oskDbgN}: ${msg}`);
}

class KeyboardMenuToggle extends QuickSettings.QuickMenuToggle {
    static {
        GObject.registerClass(this);
    }

    constructor(extensionObject) {
        super({
            title: _('Screen Keyboard'),
            iconName: 'input-keyboard-symbolic',
            toggleMode: true,
        });

        this.extensionObject = extensionObject;
        this.settings = extensionObject.getSettings();

        this.menu.setHeader('input-keyboard-symbolic', _('Screen Keyboard'), _('Opening Mode'));
        this._itemsSection = new PopupMenu.PopupMenuSection();
        this._itemsSection.addMenuItem(new PopupMenu.PopupImageMenuItem(_('Never'), this.settings.get_int("enable-tap-gesture") == 0 ? 'emblem-ok-symbolic' : null));
        this._itemsSection.addMenuItem(new PopupMenu.PopupImageMenuItem(_("Only on Touch"), this.settings.get_int("enable-tap-gesture") == 1 ? 'emblem-ok-symbolic' : null));
        this._itemsSection.addMenuItem(new PopupMenu.PopupImageMenuItem(_("Always"), this.settings.get_int("enable-tap-gesture") == 2 ? 'emblem-ok-symbolic' : null));
        for (var i in this._itemsSection._getMenuItems()) {
            const item = this._itemsSection._getMenuItems()[i]
            const num = i
            item.connect('activate', () => this.settings.set_int("enable-tap-gesture", num))
        }

        this.menu.addMenuItem(this._itemsSection);
        this.settings.bind('indicator-enabled',
            this, 'checked',
            Gio.SettingsBindFlags.DEFAULT);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        const settingsItem = this.menu.addAction(_('More Settings'),
            () => this.extensionObject.openPreferences());
        settingsItem.visible = Main.sessionMode.allowSettings;
        this.menu._settingsActions[this.extensionObject.uuid] = settingsItem;
    }

    _refresh() {
        for (var i in this._itemsSection._getMenuItems()) {
            this._itemsSection._getMenuItems()[i].setIcon(this.settings.get_int("enable-tap-gesture") == i ? 'emblem-ok-symbolic' : null)
        }
    }
};


let keycodes;
let layouts;
let currentMonitorId = 0;
let extract_dir = GLib.get_user_cache_dir() + "/gjs-osk";
// [erhai] Momentary modifier keycodes (Shift/Ctrl/Alt/Super, both sides).  These
// are the ones a user can also HOLD down with one finger while tapping another
// key (multi-touch), the way a physical modifier behaves; CapsLock/NumLock are
// locks, not momentary, so they keep their tap-to-toggle behaviour only.
const erhaiMomentaryMods = [29, 56, 97, 100, 125, 126, 42, 54];

function normalizeCustomLayouts(rawValue) {
    try {
        const parsed = JSON.parse(rawValue || "[]");
        if (Array.isArray(parsed) && parsed.length > 0 && Array.isArray(parsed[0]) && typeof parsed[parsed.length - 1] === 'object' && !Array.isArray(parsed[parsed.length - 1])) {
            return [JSON.stringify(parsed)];
        }
        return Array.isArray(parsed) ? parsed : [];
    } catch (e) {
        logError(e);
        return [];
    }
}

// [insert handwriting 1]

export default class GjsOskExtension extends Extension {
    _openKeyboard(instant) {
        if (this.Keyboard.state == State.CLOSED) {
            this.Keyboard.open(null, !instant ? null : true);
            this._setKeyboardVisible(true);
        }
    }

    _closeKeyboard(instant) {
        if (this.Keyboard.state == State.OPENED) {
            this.Keyboard.close(!instant ? null : true);
            this._setKeyboardVisible(false);
        }
    }

    // [erhai] Every write of the sticky `keyboard-visible` flag that THIS
    // object performs goes through here, so the `changed::keyboard-visible`
    // handler can tell our own bookkeeping (the result of an open/close we just
    // did) from an outside write -- only the latter may be gated as an
    // auto-show request.
    _setKeyboardVisible(value) {
        if (this.openBit == null)
            return;
        this._selfVisibleWrite = value;
        this.openBit.set_boolean('keyboard-visible', value);
    }

    _toggleKeyboard(instant = false) {
        if (!this.Keyboard.opened) {
            this._openKeyboard(instant);
            this.Keyboard.openedFromButton = true;
            this.Keyboard.closedFromButton = false
        } else {
            this._closeKeyboard(instant);
            this.Keyboard.openedFromButton = false;
            this.Keyboard.closedFromButton = true;
        }
    }

    // [erhai] pogo folio keyboard edge (see pogoGate.js).
    //
    // ATTACH: a real keyboard just appeared, so retract an automatically
    // opened OSK (one the user opened from the indicator is left alone) and
    // clear the sticky `keyboard-visible` flag -- that flag is what made the
    // OSK come back by itself at the next login / shell restart.
    // DETACH: the configured auto-show (touch / tap) applies again.
    _onFolioChanged(attached) {
        if (this.Keyboard == null || this.openBit == null)
            return;
        if (attached) {
            if (this.Keyboard.opened && !this.Keyboard.openedFromButton) {
                this._closeKeyboard(true);          // also clears the flag
                this.Keyboard.closedFromButton = true;
                log('GJS-OSK: pogo keyboard attached - OSK closed');
            } else if (!this.Keyboard.opened) {
                this._setKeyboardVisible(false);
            }
        } else {
            if (!this.Keyboard.opened)
                this.Keyboard.closedFromButton = false;
            this._folioGateLogged = null;
            log('GJS-OSK: pogo keyboard detached - touch auto-show enabled');
        }
    }

    // [erhai] one journal line per attach period for each auto-show path the
    // folio gate suppressed, so the gate can be verified from the journal
    // (gnome-shell's log) without a debugger.
    _folioGateNote(what) {
        if (this._folioGateLogged == null)
            this._folioGateLogged = {};
        if (this._folioGateLogged[what])
            return;
        this._folioGateLogged[what] = true;
        log(`GJS-OSK: pogo keyboard attached - ${what} suppressed`);
    }


    _rebuildToggle() {
        try {
            if (this._toggle) {
                this._quick_settings_indicator.quickSettingsItems =
                    this._quick_settings_indicator.quickSettingsItems.filter(item => item !== this._toggle);
                this._toggle.destroy();
            }
        } catch (e) {
        }
        this._toggle = new KeyboardMenuToggle(this);
        this._quick_settings_indicator.quickSettingsItems.push(this._toggle);
    }

    _onWake() {
        this._rebuildToggle();
        if (typeof this.refresh === 'function') {
            this.refresh();
        }
    }

    open_interval() {
        if (this.tapConnect && GObject.signal_handler_is_connected(global.stage, this.tapConnect))
            global.stage.disconnect(this.tapConnect)
        if (this.openInterval !== null) {
            clearInterval(this.openInterval);
            this.openInterval = null;
        }
        this.openInterval = setInterval(() => {
            if (this.Keyboard != null) {
                if (global.stage.key_focus == this.Keyboard && this.Keyboard.prevKeyFocus != null) {
                    global.stage.key_focus = this.Keyboard.prevKeyFocus
                }
                this.Keyboard.get_parent().set_child_at_index(this.Keyboard, this.Keyboard.get_parent().get_n_children() - 1);
                this.Keyboard.set_child_at_index(this.Keyboard.box, this.Keyboard.get_n_children() - 1);
                // [erhai] folio gate: watch the pogo keyboard attach state here
                // (the interval already runs) and act on the edges only.
                if (!this.Keyboard.capsLockConnect && typeof this.Keyboard.updateCapsLock === 'function')
                    this.Keyboard.updateCapsLock();
                if (!this.Keyboard.numLockConnect && typeof this.Keyboard.updateNumLock === 'function')
                    this.Keyboard.updateNumLock();
                const folio = pogoKeyboardAttached();
                if (folio !== this._folioAttached) {
                    this._folioAttached = folio;
                    this._onFolioChanged(folio);
                }
                const wantsAutoOpen = !this.Keyboard.openedFromButton && this.lastInputMethod;
                // TEXTS INSIDE THE SHELL ITSELF.  Main.inputMethod only knows
                // about Wayland-surface text fields, so a shell dialog's entry
                // (the Wi-Fi/network password prompt, GDM's auth prompt, ...)
                // never opened the keyboard on 26.10: Shell 51 removed
                // Main.keyboard.maybeHandleEvent, which was the old tap hook
                // (owner report 2026-10-27).  ui/keyboard.js decides by the
                // stage key focus instead -- do the same.
                const shellText = global.stage.key_focus instanceof Clutter.Text;
                if (wantsAutoOpen && folio)
                    this._folioGateNote('touch auto-open');
                if (wantsAutoOpen && !folio) {
                    const imFocus = Main.inputMethod.currentFocus;
                    if (((imFocus != null && imFocus.is_focused()) || shellText) && !this.Keyboard.closedFromButton) {
                        this._openKeyboard();
                    } else if (!this.Keyboard.closedFromButton && !this.Keyboard._dragging) {
                        this._closeKeyboard();
                        this.Keyboard.closedFromButton = false
                    } else if (imFocus == null && !shellText) {
                        this.Keyboard.closedFromButton = false
                    }
                }
            }
        }, 300);
        this.tapConnect = global.stage.connect("event", (_actor, event) => {
            if (event.type() !== 4 && event.type() !== 5) {
                this.lastInputMethod = [false, event.type() >= 9 && event.type() <= 12, true][this.settings.get_boolean("indicator-enabled") ? this.settings.get_int("enable-tap-gesture") : 0]
            }
        })
    }

    fail(error, method = null, instance = null) {
        Main.notifyError("Error with extension GJS-OSK"
            + (instance != null || method != null ? " in " : "")
            + (instance != null ? instance + "." : "")
            + (method != null ? method + "\n" : ""),
            error.message
            + "\n" + error.stack);
    }

    enable() {
        this.settings = this.getSettings();
        if (this.settings == null) {
            return;
        }
        // [erhai] seed the folio gate before the open interval starts.
        this._folioAttached = pogoKeyboardAttached();
        this._folioGateLogged = null;
        this._selfVisibleWrite = null;      // last `keyboard-visible` we wrote
        log(`GJS-OSK: folio keyboard ${this._folioAttached
            ? 'attached - OSK auto-show gated'
            : 'detached - touch auto-show active'}`);
        this.disableEdgeSwipeSettings = this.settings.get_boolean("disable-edge-swipe");
        this.darkSchemeSettings = this.getSettings("org.gnome.desktop.interface");
        this.inputLanguageSettings = InputSourceManager.getInputSourceManager();
        this.gnomeKeyboardSettings = this.getSettings('org.gnome.desktop.a11y.applications');
        this.isGnomeKeyboardEnabled = this.gnomeKeyboardSettings.get_boolean('screen-keyboard-enabled');
        this.gnomeKeyboardSettings.set_boolean('screen-keyboard-enabled', false)
        this.isGnomeKeyboardEnabledHandler = this.gnomeKeyboardSettings.connect('changed', () => {
            this.gnomeKeyboardSettings.set_boolean('screen-keyboard-enabled', false)
        });
        this.settings.scheme = ""
        if (this.darkSchemeSettings.get_string("color-scheme") == "prefer-dark")
            this.settings.scheme = "-dark"
        this.openBit = this.settings.get_child("indicator");

        this.openPrefs = () => { this.openPreferences() }

        let [okL, contentsL] = GLib.file_get_contents(this.path + '/physicalLayouts.json');
        if (okL) {
            layouts = JSON.parse(contentsL);
        }

        let rawCustomLayouts = this.settings.get_string("custom-layout") || "[]";
        this.customLayouts = normalizeCustomLayouts(rawCustomLayouts);
        if (this.customLayouts.length > 0) {
            try {
                const parsed = JSON.parse(rawCustomLayouts);
                if (!(Array.isArray(parsed) && parsed.length > 0 && Array.isArray(parsed[0]) && typeof parsed[parsed.length - 1] === 'object' && !Array.isArray(parsed[parsed.length - 1]))) {
                    this.settings.set_string("custom-layout", JSON.stringify(this.customLayouts));
                }
            } catch (_e) {
                this.settings.set_string("custom-layout", JSON.stringify(this.customLayouts));
            }
        }

        let refresh = () => {
            (async () => {
                // [insert handwriting 2]
                let rawCustomLayouts = this.settings.get_string("custom-layout") || "[]";
                this.customLayouts = normalizeCustomLayouts(rawCustomLayouts);
                if (this.customLayouts.length > 0) {
                    try {
                        const parsed = JSON.parse(rawCustomLayouts);
                        if (!(Array.isArray(parsed) && parsed.length > 0 && Array.isArray(parsed[0]) && typeof parsed[parsed.length - 1] === 'object' && !Array.isArray(parsed[parsed.length - 1]))) {
                            this.settings.set_string("custom-layout", JSON.stringify(this.customLayouts));
                        }
                    } catch (_e) {
                        this.settings.set_string("custom-layout", JSON.stringify(this.customLayouts));
                    }
                }
                let prevOpenState = false;
                if (this.Keyboard != null) {
                    prevOpenState = this.Keyboard.opened;
                }
                let currentMonitors = this.settings.get_string("default-monitor").split(";")
                let currentMonitorMap = {};
                let monitors = Main.layoutManager.monitors;
                if (Main.layoutManager.primaryMonitor == null) {
                    this.waitRefresh = setTimeout(refresh, 1000)
                    return;
                }
                for (var i of currentMonitors) {
                    let tmp = i.split(":");
                    currentMonitorMap[tmp[0]] = tmp[1] + "";
                }
                if (!Object.keys(currentMonitorMap).includes(monitors.length + "")) {
                    let allConfigs = Object.keys(currentMonitorMap).map(Number.parseInt).sort();
                    currentMonitorMap[monitors.length + ""] = allConfigs[allConfigs.length - 1];
                }
                try {
                    currentMonitorId = global.backend.get_monitor_manager().get_monitor_for_connector(currentMonitorMap[monitors.length + ""]);
                    if (currentMonitorId == -1) {
                        currentMonitorId = 0;
                    }
                } catch {
                    currentMonitorId = 0;
                }
                const fileExists = async (file) => {
                    try {
                        return await new Promise((resolve) => {
                            file.query_info_async(
                                '*',
                                Gio.FileQueryInfoFlags.NONE,
                                GLib.PRIORITY_DEFAULT,
                                null,
                                (source, res) => {
                                    try {
                                        source.query_info_finish(res);
                                        resolve(true);
                                    } catch (e) {
                                        resolve(false);
                                    }
                                }
                            );
                        });
                    } catch (e) {
                        return false;
                    }
                };

                const ensureDirectory = async (dir) => {
                    try {
                        if (!(await fileExists(dir))) {
                            await new Promise((resolve, reject) => {
                                dir.make_directory_async(GLib.PRIORITY_DEFAULT, null, (source, res) => {
                                    try {
                                        source.make_directory_finish(res);
                                        resolve();
                                    } catch (e) {
                                        if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS)) {
                                            reject(e);
                                        } else {
                                            resolve();
                                        }
                                    }
                                });
                            });
                        }
                        return true;
                    } catch (e) {
                        console.error(`Failed to create directory ${dir.get_path()}: ${e.message}`);
                        return false;
                    }
                };

                const runCommand = (argv) => {
                    return new Promise((resolve) => {
                        const proc = Gio.Subprocess.new(
                            argv,
                            Gio.SubprocessFlags.STDOUT_PIPE |
                            Gio.SubprocessFlags.STDERR_PIPE
                        );
                        proc.wait_check_async(null, (source, res) => {
                            try {
                                const success = source.wait_check_finish(res);
                                resolve(success);
                            } catch (e) {
                                console.error(`Command failed: ${argv.join(' ')}`);
                                resolve(false);
                            }
                        });
                    });
                };

                const extractKeycodes = async () => {
                    const baseDir = Gio.File.new_for_path(extract_dir);
                    const keycodesDir = Gio.File.new_for_path(extract_dir + "/keycodes");
                    const layoutId = KeyboardManager.getKeyboardManager().currentLayout?.id || "us";
                    const targetFile = Gio.File.new_for_path(`${extract_dir}/keycodes/${layoutId}.json`);

                    if (await fileExists(targetFile)) {
                        return true;
                    }

                    if (!(await ensureDirectory(baseDir))) {
                        console.error("Failed to create base directory");
                        return false;
                    }

                    if (!(await ensureDirectory(keycodesDir))) {
                        console.error("Failed to create keycodes directory");
                        return false;
                    }

                    try {
                        const success = await runCommand([
                            "tar",
                            "-Jxf",
                            this.path + "/keycodes.tar.xz",
                            "-C",
                            keycodesDir.get_path(),
                            "--strip-components=1"
                        ]);

                        if (!success) {
                            throw new Error("Failed to extract keycodes archive");
                        }

                        if (!(await fileExists(targetFile))) {
                            throw new Error("Keycodes file not found after extraction");
                        }

                        return true;
                    } catch (error) {
                        console.error(`Error extracting keycodes: ${error.message}`);
                        return false;
                    }
                };

                const initializeKeyboard = async () => {
                    const layoutId = KeyboardManager.getKeyboardManager().currentLayout?.id || "us";
                    const keycodesPath = GLib.build_filenamev([
                        extract_dir,
                        "keycodes",
                        `${layoutId}.json`
                    ]);

                    const [ok, contents] = GLib.file_get_contents(keycodesPath);
                    if (!ok) {
                        throw new Error(`Failed to read keycodes from ${keycodesPath}`);
                    }

                    keycodes = JSON.parse(contents);

                    if (this.Keyboard) {
                        this.Keyboard.destroy();
                        this.Keyboard = null;
                    }

                    if (this.settings == null) {
                        return;
                    }

                    function withErrorHandler(TargetClass, handler) {
                        return new Proxy(TargetClass, {
                            construct(Target, args) {
                                const instance = new Target(...args);
                                for (const key of Object.getOwnPropertyNames(Target.prototype)) {
                                    const fn = instance[key];
                                    if (typeof fn === "function" && key !== "constructor") {
                                        instance[key] = async (...fnArgs) => {
                                            try {
                                                return await fn.apply(instance, fnArgs);
                                            } catch (err) {
                                                handler(err, key, instance);
                                            }
                                        };
                                    }
                                }

                                return instance;
                            }
                        });
                    }
                    const SafeKeyboard = withErrorHandler(Keyboard, this.fail);
                    this.Keyboard = new SafeKeyboard(this.settings, this);
                    this.Keyboard.refresh = refresh;
                    if (prevOpenState) {
                        this.Keyboard.open(null, true);
                        this._setKeyboardVisible(true);
                    }
                };
                await extractKeycodes();
                await initializeKeyboard();
            })().catch(this.fail)
        }
        this.refresh = refresh;
        refresh()

        this._originalLastDeviceIsTouchscreen = KeyboardUI.KeyboardManager.prototype._lastDeviceIsTouchscreen;
        KeyboardUI.KeyboardManager.prototype._lastDeviceIsTouchscreen = () => { return false };

        this._indicator = null;
        this.openInterval = null;
        if (this.settings.get_boolean("indicator-enabled")) {
            this._indicator = new PanelMenu.Button(0, "GJS OSK Indicator", true);
            let icon = new St.Icon({
                gicon: new Gio.ThemedIcon({
                    name: 'input-keyboard-symbolic'
                }),
                style_class: 'system-status-icon'
            });
            this._indicator.add_child(icon);
            this._indicator.clear_actions();
            this._indicator.connect("button-press-event", () => {
                this._toggleKeyboard();
                return Clutter.EVENT_STOP;
            });
            this._indicator.connect("touch-event", (_actor, event) => {
                if (event.type() == Clutter.EventType.TOUCH_END) {
                    this._toggleKeyboard();
                    return Clutter.EVENT_STOP;
                }
                return Clutter.EVENT_PROPAGATE;
            });
            Main.panel.addToStatusArea("GJS OSK Indicator", this._indicator);
        }

        this._toggle = new KeyboardMenuToggle(this);
        this._quick_settings_indicator = new QuickSettings.SystemIndicator();
        this._quick_settings_indicator.quickSettingsItems.push(this._toggle);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._quick_settings_indicator);
        this.open_interval();

        this.keyboardVisibilityHandler = this.openBit.connect('changed::keyboard-visible', () => {
            const shouldBeVisible = this.openBit.get_boolean('keyboard-visible');
            // [erhai] our own bookkeeping write (open/close above): it is the
            // result of something we just did, never an auto-show request.
            if (this._selfVisibleWrite === shouldBeVisible) {
                this._selfVisibleWrite = null;
                return;
            }
            if (shouldBeVisible && pogoKeyboardAttached()) {    // [erhai] folio gate
                this._folioGateNote('external show request');
                this._setKeyboardVisible(false);
                return;
            }
            if (shouldBeVisible !== this.Keyboard?.opened) {
                this._toggleKeyboard(true);
            }
        });

        this.openFromCommandHandler = this.openBit.connect("changed::opened", () => {
            if (this.openBit.get_boolean("opened")) {
                this.openBit.set_boolean("opened", false);
                this._toggleKeyboard();
            }
        });

        if (this.openBit.get_boolean('keyboard-visible') && this.Keyboard) {
            if (this._folioAttached) {
                // [erhai] Never restore the OSK while the pogo folio keyboard
                // is attached (login, shell restart, extension reload), and
                // clear the sticky flag so a later detached session does not
                // inherit it either.
                this.Keyboard.closedFromButton = true;
                this._setKeyboardVisible(false);
                log('GJS-OSK: pogo keyboard attached - OSK not restored');
            } else {
                this._openKeyboard(true);
            }
        }
        let settingsChanged = () => {
            if (this.darkSchemeSettings.get_string("color-scheme") == "prefer-dark")
                this.settings.scheme = "-dark"
            else
                this.settings.scheme = ""
            refresh()
            this._toggle._refresh();
            if (this.settings.get_boolean("indicator-enabled")) {
                if (this._indicator != null) {
                    this._indicator.destroy();
                    this._indicator = null;
                }
                this._indicator = new PanelMenu.Button(0.0, "GJS OSK Indicator", true);
                let icon = new St.Icon({
                    gicon: new Gio.ThemedIcon({
                        name: 'input-keyboard-symbolic'
                    }),
                    style_class: 'system-status-icon'
                });
                this._indicator.add_child(icon);

                this._indicator.connect("button-press-event", () => this._toggleKeyboard());
                this._indicator.connect("touch-event", (_actor, event) => {
                    if (event.type() == Clutter.EventType.TOUCH_END) this._toggleKeyboard()
                });
                Main.panel.addToStatusArea("GJS OSK Indicator", this._indicator);
            } else {
                if (this._indicator != null) {
                    this._indicator.destroy();
                    this._indicator = null;
                }
            }
            global.stage.disconnect(this.tapConnect)
            if (this.openInterval !== null) {
                clearInterval(this.openInterval);
                this.openInterval = null;
            }
            this.open_interval();
            let disableEdgeSwipe = this.settings.get_boolean("disable-edge-swipe");
            if (this.disableEdgeSwipeSettings !== disableEdgeSwipe) {
                this.disableEdgeSwipeSettings = disableEdgeSwipe;
                if (disableEdgeSwipe) {
                    if (EdgeDragAction != null) {
                        global.stage.remove_action_by_name('osk');
                    } else {
                        global.stage.remove_action_by_name('GJS-OSK Edge Drag Action');
                    }
                } else {
                    if (this.keyboard != null && this.keyboard.bottomDragAction != null) {
                        if (EdgeDragAction != null) {
                            global.stage.add_action_full('osk', Clutter.EventPhase.CAPTURE, this.keyboard.bottomDragAction);
                        } else {
                            global.stage.add_action(this.keyboard.bottomDragAction);
                        }
                    }
                }
            }
        }
        this.settingsHandlers = [
            this.settings.connect("changed", settingsChanged),
            this.darkSchemeSettings.connect("changed", (_, key) => { if (key == "color-scheme") settingsChanged() }),
            this.inputLanguageSettings.connect("current-source-changed", settingsChanged)
        ];
    }

    disable() {
        this.gnomeKeyboardSettings.disconnect(this.isGnomeKeyboardEnabledHandler)
        this.gnomeKeyboardSettings.set_boolean('screen-keyboard-enabled', this.isGnomeKeyboardEnabled);

        this._quick_settings_indicator.quickSettingsItems.forEach(item => item.destroy());
        this._quick_settings_indicator.destroy();
        this._quick_settings_indicator = null;

        if (this._indicator !== null) {
            this._indicator.destroy();
            this._indicator = null;
        }
        if (this.Keyboard != null)
            this.Keyboard.destroy();
        this.settings.disconnect(this.settingsHandlers[0]);
        this.darkSchemeSettings.disconnect(this.settingsHandlers[1])
        this.inputLanguageSettings.disconnect(this.settingsHandlers[2])
        this.settings = null;
        this.darkSchemeSettings = null;
        this.inputLanguageSettings = null;
        this.gnomeKeyboardSettings = null;
        if (this.keyboardVisibilityHandler) {
            this.openBit.disconnect(this.keyboardVisibilityHandler);
            this.keyboardVisibilityHandler = null;
        }
        if (this.openFromCommandHandler) {
            this.openBit.disconnect(this.openFromCommandHandler);
            this.openFromCommandHandler = null;
        }
        this.openBit = null;
        global.stage.disconnect(this.tapConnect)
        if (this.openInterval !== null) {
            clearInterval(this.openInterval);
            this.openInterval = null;
        }
        if (this._toggle !== null) {
            try {
                this._toggle.destroy()
            } catch { }
            this._toggle = null
        }
        this.settings = null
        this.Keyboard = null
        keycodes = null
        if (this._originalLastDeviceIsTouchscreen !== null) {
            KeyboardUI.KeyboardManager.prototype._lastDeviceIsTouchscreen = this._originalLastDeviceIsTouchscreen;
            this._originalLastDeviceIsTouchscreen = null;
        }
    }
}

// [insert handwriting 3]

class Keyboard extends Dialog {
    static [GObject.signals] = {
        'drag-begin': {},
        'drag-end': {}
    };

    static {
        GObject.registerClass(this);
    }

    _init(settings, extensionObject) {
        this.settingsOpenFunction = extensionObject.openPrefs
        this.inputDevice = global.stage.context.get_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this.settings = settings;
        // [erhai] the extension's `indicator` child settings: holds the sticky
        // `keyboard-visible` state the CLOSE key must clear (closeFromKey()).
        this.openBit = extensionObject.openBit;
        this.extensionObject = extensionObject;    // [erhai] folio gate notes
        this.customLayouts = extensionObject.customLayouts;
        let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor;
        super._init(Main.uiGroup, 'db-keyboard-content');
        this.box = new St.Widget({
            reactive: true,
            layout_manager: new Clutter.GridLayout({
                orientation: Clutter.Orientation.HORIZONTAL,
            })
        });
        this.box.clear_actions();
        this.widthPercent = (monitor.width > monitor.height) ? settings.get_int("landscape-width-percent") / 100 : settings.get_int("portrait-width-percent") / 100;
        this.heightPercent = (monitor.width > monitor.height) ? settings.get_int("landscape-height-percent") / 100 : settings.get_int("portrait-height-percent") / 100;
        this.nonDragBlocker = new Clutter.Actor();
        this.buildUI();
        this.draggable = false;
        // [insert handwriting 4]
        this.add_child(this.box);
        this.close();
        this.box.set_name("osk-gjs")
        this.mod = [];
        this.modBtns = [];
        this.capsL = false;
        this.shift = false;
        this.alt = false;
        this.opened = false;
        this.state = State.CLOSED;
        this.delta = [];
        this.monitorChecker = global.backend.get_monitor_manager().connect('monitors-changed', () => {
            if (Main.layoutManager.monitors.length > 0) {
                this.refresh();
            }
        });
        this._dragging = false;
        let side = null;
        switch (this.settings.get_int("default-snap")) {
            case 0:
            case 1:
            case 2:
                side = St.Side.TOP;
                break;
            case 3:
                side = St.Side.LEFT;
                break;
            case 5:
                side = St.Side.RIGHT;
                break;
            case 6:
            case 7:
            case 8:
                side = St.Side.BOTTOM;
                break;
        }
        this.oldBottomDragAction = global.stage.get_action(EdgeDragAction == null ? 'OSK show bottom drag' : 'osk');
        if (this.oldBottomDragAction !== null && this.oldBottomDragAction instanceof Clutter.Action)
            global.stage.remove_action(this.oldBottomDragAction);
        if (side != null) {
            const mode = Shell.ActionMode.ALL & ~Shell.ActionMode.LOCK_SCREEN;
            let oskEdgeDragAction;
            if (EdgeDragAction != null) {
                oskEdgeDragAction = new EdgeDragAction.EdgeDragAction(side, mode);
                oskEdgeDragAction.connect('activated', () => {
                    if (pogoKeyboardAttached()) {   // [erhai] folio gate
                        this.extensionObject?._folioGateNote('bottom-edge swipe');
                        this.gestureInProgress = false;
                        return;
                    }
                    this.open(true);
                    this.openedFromButton = true;
                    this.closedFromButton = false;
                    this.gestureInProgress = false;
                });
                oskEdgeDragAction.connect('gesture-cancel', () => {
                    if (this.gestureInProgress) {
                        this.close()
                        this.openedFromButton = false;
                        this.closedFromButton = true;
                    }
                    this.gestureInProgress = false;
                    return Clutter.EVENT_PROPAGATE;
                });
                oskEdgeDragAction.connect('progress', (_action, progress) => {
                    if (pogoKeyboardAttached()) {   // [erhai] folio gate
                        this.extensionObject?._folioGateNote('bottom-edge swipe');
                        return;
                    }
                    if (!this.gestureInProgress)
                        this.open(false)
                    this.setOpenState(Math.min(Math.max(0, (progress / (side % 2 == 0 ? this.box.height : this.box.width)) * 100), 100))
                    this.gestureInProgress = true;
                });
                if (!this.settings.get_boolean("disable-edge-swipe")) {
                    global.stage.add_action_full('osk', Clutter.EventPhase.CAPTURE, oskEdgeDragAction);
                }
                this.bottomDragAction = oskEdgeDragAction;
            } else {
                oskEdgeDragAction = new Shell.EdgeDragGesture({
                    name: 'GJS-OSK Edge Drag Action',
                    side
                })
                oskEdgeDragAction.connect('may-recognize', () => {
                    if (pogoKeyboardAttached()) {   // [erhai] folio gate: no swipe
                        this.extensionObject?._folioGateNote('bottom-edge swipe');
                        return 0;
                    }
                    return Shell.ActionMode.ALL & ~Shell.ActionMode.LOCK_SCREEN & Main.actionMode;
                })
                oskEdgeDragAction.connect('end', () => {
                    if (pogoKeyboardAttached()) {   // [erhai] folio gate
                        this.extensionObject?._folioGateNote('bottom-edge swipe');
                        this.gestureInProgress = false;
                        return;
                    }
                    this.open(true);
                    this.openedFromButton = true;
                    this.closedFromButton = false;
                    this.gestureInProgress = false;
                });
                oskEdgeDragAction.connect('cancel', () => {
                    if (this.gestureInProgress) {
                        this.close()
                        this.openedFromButton = false;
                        this.closedFromButton = true;
                    }
                    this.gestureInProgress = false;
                    return Clutter.EVENT_PROPAGATE;
                });
                oskEdgeDragAction.connect('progress', (_action, progress) => {
                    if (pogoKeyboardAttached()) {   // [erhai] folio gate
                        this.extensionObject?._folioGateNote('bottom-edge swipe');
                        return;
                    }
                    if (!this.gestureInProgress)
                        this.open(false)
                    this.setOpenState(Math.min(Math.max(0, (progress / (side % 2 == 0 ? this.box.height : this.box.width)) * 100), 100))
                    this.gestureInProgress = true;
                });
                if (!this.settings.get_boolean("disable-edge-swipe")) {
                    global.stage.add_action(oskEdgeDragAction);
                }
                this.bottomDragAction = oskEdgeDragAction;
            }
        } else {
            this.bottomDragAction = null;
        }
        // Shell <= 50 routes touch events through Main.keyboard.maybeHandleEvent;
        // Shell 51's KeyboardManager has no such method (its Keyboard tracks
        // global.stage's key focus), so the hook below is a no-op there and the
        // interval above takes over.  Keep both: one shell, one path.
        this._oldMaybeHandleEvent = Main.keyboard.maybeHandleEvent
        if (this._oldMaybeHandleEvent !== undefined) {
            Main.keyboard.maybeHandleEvent = (e) => {
                let lastInputMethod = [e.type() == 11, e.type() == 11, e.type() == 7 || e.type() == 11][this.settings.get_boolean("indicator-enabled") ? this.settings.get_int("enable-tap-gesture") : 0]
                let ac = global.stage.get_event_actor(e)
                if (this.contains(ac)) {
                    ac.event(e, true);
                    ac.event(e, false);
                    return true;
                } else if (ac instanceof Clutter.Text && lastInputMethod && !this.opened) {
                    if (pogoKeyboardAttached())     // [erhai] folio gate
                        this.extensionObject?._folioGateNote('tap auto-open');
                    else
                        this.open();
                }
                return false
            }
        }
    }

    destroy() {
        if (major >= 48) {
            global.display.get_compositor().enable_unredirect()
        }
        if (this._oldMaybeHandleEvent !== undefined)
            Main.keyboard.maybeHandleEvent = this._oldMaybeHandleEvent
        if (this.oldBottomDragAction !== null && this.oldBottomDragAction instanceof Clutter.Action && EdgeDragAction != null) {
            global.stage.remove_action_by_name('osk')
            global.stage.add_action_full('osk', Clutter.EventPhase.CAPTURE, this.oldBottomDragAction)
        }
        else if (this.oldBottomDragAction != null) {
            global.stage.remove_action_by_name('GJS-OSK Edge Drag Action')
            global.stage.add_action(this.oldBottomDragAction)
        }
        if (this.textboxChecker !== null) {
            clearInterval(this.textboxChecker);
            this.textboxChecker = null;
        }
        if (this.stateTimeout !== null) {
            clearTimeout(this.stateTimeout);
            this.stateTimeout = null;
        }
        if (this.keyTimeout !== null) {
            clearTimeout(this.keyTimeout);
            if (typeof this.keyTimeoutFunc === 'function') {
                this.keyTimeoutFunc();
            }
            this.keyTimeout = null;
            this.keyTimeoutFunc = null;
        }
        if (this.capsLockConnect && GObject.signal_handler_is_connected(this.keymap, this.capsLockConnect))
            this.keymap.disconnect(this.capsLockConnect);
        if (this.numLockConnect && GObject.signal_handler_is_connected(this.keymap, this.numLockConnect))
            this.keymap.disconnect(this.numLockConnect);
        global.backend.get_monitor_manager().disconnect(this.monitorChecker)
        super.destroy();
        if (this.nonDragBlocker !== null) {
            Main.layoutManager.removeChrome(this.nonDragBlocker)
        }
    }

    startDragging(event, delta) {
        if (this.draggable) {
            if (this._dragging)
                return Clutter.EVENT_PROPAGATE;
            this._dragging = true;
            this.box.set_opacity(255);
            this.box.ease({
                opacity: 200,
                duration: 100,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onComplete: () => { }
            });
            let sequence = event.get_event_sequence();
            this._grab = global.stage.grab(this);
            this._grabbedSequence = sequence;
            this.emit('drag-begin');
            let [absX, absY] = event.get_coords();
            this.snapMovement(absX - delta[0], absY - delta[1]);
            return Clutter.EVENT_STOP;
        } else {
            return Clutter.EVENT_PROPAGATE;
        }
    }

    endDragging() {
        if (this.draggable) {
            if (this._dragging) {
                if (this._releaseId) {
                    this.disconnect(this._releaseId);
                    this._releaseId = 0;
                }
                if (this._grab) {
                    this._grab.dismiss();
                    this._grab = null;
                }

                this.box.set_opacity(200);
                this.box.ease({
                    opacity: 255,
                    duration: 100,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                    onComplete: () => { }
                });
                this._grabbedSequence = null;
                this._dragging = false;
                this.delta = [];
                this.emit('drag-end');
                this._dragging = false;
            }
            this.draggable = false;
            return Clutter.EVENT_STOP;
        } else {
            return Clutter.EVENT_STOP;
        }
    }

    motionEvent(event) {
        if (this.draggable) {
            let [absX, absY] = event.get_coords();
            this.snapMovement(absX - this.delta[0], absY - this.delta[1]);
            return Clutter.EVENT_STOP
        } else {
            return Clutter.EVENT_STOP
        }
    }

    snapMovement(xPos, yPos) {
        let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor
        if (xPos < monitor.x || yPos < monitor.y || xPos > monitor.x + monitor.width || yPos > monitor.y + monitor.width) {
            this.set_translation(xPos, yPos, 0);
            return;
        }
        xPos -= monitor.x;
        yPos -= monitor.y;
        let snap_px = this.settings.get_int("snap-spacing-px")
        if (Math.abs(xPos - ((monitor.width * .5) - ((this.width * .5)))) <= 50) {
            xPos = ((monitor.width * .5) - ((this.width * .5)));
        } else if (Math.abs(xPos - snap_px) <= 50) {
            xPos = snap_px;
        } else if (Math.abs(xPos - (monitor.width - this.width - snap_px)) <= 50) {
            xPos = monitor.width - this.width - snap_px
        }
        if (Math.abs(yPos - (monitor.height - this.height - snap_px)) <= 50) {
            yPos = monitor.height - this.height - snap_px;
        } else if (Math.abs(yPos - snap_px) <= 50) {
            yPos = snap_px;
        } else if (Math.abs(yPos - ((monitor.height * .5) - (this.height * .5))) <= 50) {
            yPos = (monitor.height * .5) - (this.height * .5);
        }
        this.set_translation(xPos + monitor.x, yPos + monitor.y, 0);
    }

    setOpenState(percent) {
        let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor;
        let posX = [this.settings.get_int("snap-spacing-px"), ((monitor.width * .5) - ((this.width * .5))), monitor.width - this.width - this.settings.get_int("snap-spacing-px")][(this.settings.get_int("default-snap") % 3)];
        let posY = [this.settings.get_int("snap-spacing-px"), ((monitor.height * .5) - ((this.height * .5))), monitor.height - this.height - this.settings.get_int("snap-spacing-px")][Math.floor((this.settings.get_int("default-snap") / 3))];
        let mX = [-this.box.width, 0, this.box.width][(this.settings.get_int("default-snap") % 3)];
        let mY = [-this.box.height, 0, this.box.height][Math.floor((this.settings.get_int("default-snap") / 3))]
        let [dx, dy] = [posX + mX * ((100 - percent) / 100) + monitor.x, posY + mY * ((100 - percent) / 100) + monitor.y]
        let op = 255 * (percent / 100);
        this.set_translation(dx, dy, 0)
        this.box.set_opacity(op)
    }

    open(noPrep = null, instant = null) {
        if (this.updateCapsLock) this.updateCapsLock()
        if (this.updateNumLock) this.updateNumLock()
        if (noPrep == null || !noPrep) {
            if (major >= 48) {
                global.display.get_compositor().disable_unredirect()
            }
            this.prevKeyFocus = global.stage.key_focus
            this.inputDevice = global.stage.context.get_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
            this.state = State.OPENING
            this.show();
            Main.uiGroup.set_child_above_sibling(this, null);
        }
        if (noPrep == null || noPrep) {
            if (major >= 48) {
                global.display.get_compositor().disable_unredirect()
            }
            let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor;
            let posX = [this.settings.get_int("snap-spacing-px"), ((monitor.width * .5) - ((this.width * .5))), monitor.width - this.width - this.settings.get_int("snap-spacing-px")][(this.settings.get_int("default-snap") % 3)];
            let posY = [this.settings.get_int("snap-spacing-px"), ((monitor.height * .5) - ((this.height * .5))), monitor.height - this.height - this.settings.get_int("snap-spacing-px")][Math.floor((this.settings.get_int("default-snap") / 3))];
            if (noPrep == null) {
                let mX = [-this.box.width, 0, this.box.width][(this.settings.get_int("default-snap") % 3)];
                let mY = [-this.box.height, 0, this.box.height][Math.floor((this.settings.get_int("default-snap") / 3))]
                this.set_translation(posX + mX + monitor.x, posY + mY + monitor.y, 0)
            }
            this.box.ease({
                opacity: 255,
                duration: instant == null || !instant ? 100 : 0,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onComplete: () => {
                    if (this.stateTimeout !== null) {
                        clearTimeout(this.stateTimeout);
                        this.stateTimeout = null;
                    }
                    this.stateTimeout = setTimeout(() => {
                        this.state = State.OPENED
                    }, 500);
                }
            });
            this.ease({
                translation_x: posX + monitor.x,
                translation_y: posY + monitor.y,
                duration: instant == null || !instant ? 100 : 0,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD
            })
            if (!this.settings.get_boolean("enable-drag") && this.nonDragBlocker !== null) {
                Main.layoutManager.addChrome(this.nonDragBlocker, {
                    affectsStruts: true,
                    trackFullscreen: true,
                })
            }
            this.opened = true;
            Main.uiGroup.set_child_above_sibling(this, null);
            // [insert handwriting 5]
        }
    }

    close(instant = null) {
        this.prevKeyFocus = null;
        let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor;
        let posX = [this.settings.get_int("snap-spacing-px"), ((monitor.width * .5) - ((this.width * .5))), monitor.width - this.width - this.settings.get_int("snap-spacing-px")][(this.settings.get_int("default-snap") % 3)];
        let posY = [this.settings.get_int("snap-spacing-px"), ((monitor.height * .5) - ((this.height * .5))), monitor.height - this.height - this.settings.get_int("snap-spacing-px")][Math.floor((this.settings.get_int("default-snap") / 3))];
        let mX = [-this.box.width, 0, this.box.width][(this.settings.get_int("default-snap") % 3)];
        let mY = [-this.box.height, 0, this.box.height][Math.floor((this.settings.get_int("default-snap") / 3))]
        this.state = State.CLOSING
        this.box.ease({
            opacity: 0,
            duration: instant == null || !instant ? 100 : 0,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => {
                this.opened = false;
                this.hide();
                if (this.stateTimeout !== null) {
                    clearTimeout(this.stateTimeout);
                    this.stateTimeout = null;
                }
                this.stateTimeout = setTimeout(() => {
                    this.state = State.CLOSED
                }, 500);
            },
        });
        this.ease({
            translation_x: posX + mX + monitor.x,
            translation_y: posY + mY + monitor.y,
            duration: instant == null || !instant ? 100 : 0,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD
        })
        if (!this.settings.get_boolean("enable-drag") && this.nonDragBlocker !== null) {
            Main.layoutManager.removeChrome(this.nonDragBlocker, {
                affectsStruts: true,
                trackFullscreen: true,
            })
        }
        this.openedFromButton = false
        this.releaseAllKeys();
        // [insert handwrting 6]
    }

    // [erhai] The on-screen CLOSE key must clear the sticky `keyboard-visible`
    // flag too.  Only the indicator toggle used to do that (through
    // _closeKeyboard), so closing the keyboard with the key left the flag true
    // and the next extension enable -- i.e. every login / shell restart --
    // restored an OSK nobody had asked for.  (While the folio keyboard is
    // attached the restore is gated anyway, see pogoGate.js; detached it must
    // not come back either.)
    closeFromKey() {
        this.close();
        this.closedFromButton = true;
        // [erhai] `_setKeyboardVisible` is a method of the EXTENSION object
        // (it also records the value in _selfVisibleWrite, which is how the
        // changed::keyboard-visible handler tells our own bookkeeping from an
        // external auto-show request) -- the Keyboard only holds references to
        // it (`extensionObject` / `openBit`), so it must be called ON THAT
        // OBJECT.  Calling it on the Keyboard itself threw
        // "this._setKeyboardVisible is not a function" (measured 2026-09-29
        // 18:48, the CLOSE key), which left the sticky flag true: the OSK did
        // close, but the flag booked it as still visible.
        this.extensionObject?._setKeyboardVisible(false);
    }

    vfunc_button_press_event() {
        this.delta = [Clutter.get_current_event().get_coords()[0] - this.translation_x, Clutter.get_current_event().get_coords()[1] - this.translation_y];
        return this.startDragging(Clutter.get_current_event(), this.delta)
    }

    vfunc_button_release_event() {
        if (this._dragging && !this._grabbedSequence) {
            return this.endDragging();
        }
        return Clutter.EVENT_PROPAGATE;
    }

    vfunc_motion_event() {
        let event = Clutter.get_current_event();
        if (this._dragging && !this._grabbedSequence) {
            this.motionEvent(event);
        }
        return Clutter.EVENT_PROPAGATE;
    }

    vfunc_key_press_event() {
        return Clutter.EVENT_PROPAGATE;
    }

    vfunc_key_release_event() {
        return Clutter.EVENT_PROPAGATE;
    }

    vfunc_touch_event() {
        let event = Clutter.get_current_event();
        let sequence = event.get_event_sequence();

        if (!this._dragging && event.type() == Clutter.EventType.TOUCH_BEGIN) {
            this.delta = [event.get_coords()[0] - this.translation_x, event.get_coords()[1] - this.translation_y];
            this.startDragging(event, this.delta);
            return Clutter.EVENT_STOP;
        } else if (this._grabbedSequence && sequence.get_slot() === this._grabbedSequence.get_slot()) {
            if (event.type() == Clutter.EventType.TOUCH_UPDATE) {
                return this.motionEvent(event);
            } else if (event.type() == Clutter.EventType.TOUCH_END) {
                return this.endDragging();
            }
        }

        return Clutter.EVENT_PROPAGATE;
    }

    buildUI() {
        this.box.set_opacity(0);
        this.keys = [];
        let monitor = Main.layoutManager.monitors[currentMonitorId] ?? Main.layoutManager.primaryMonitor
        let layoutIdx = (monitor.width > monitor.height) ? this.settings.get_int("layout-landscape") : this.settings.get_int("layout-portrait")
        let currentLayout;
        let numBuiltIn = Object.keys(layouts).length;
        if (layoutIdx < numBuiltIn) {
            let layoutName = Object.keys(layouts)[layoutIdx];
            currentLayout = layouts[layoutName];
        } else {
            let customIdx = layoutIdx - numBuiltIn;
            if (customIdx < this.customLayouts.length) {
                try {
                    currentLayout = JSON.parse(this.customLayouts[customIdx]);
                } catch (e) {
                    this.extensionObject.fail(`Failed to parse custom layout ${customIdx + 1}: ${e.message}`);
                    currentLayout = layouts[Object.keys(layouts)[0]];
                }
            } else {
                currentLayout = layouts[Object.keys(layouts)[0]];
            }
        }
        this.box.width = Math.round((monitor.width - this.settings.get_int("snap-spacing-px") * 2) * (currentLayout[currentLayout.length - 1].split ? 1 : this.widthPercent))
        this.box.height = Math.round((monitor.height - this.settings.get_int("snap-spacing-px") * 2) * this.heightPercent)

        if (!this.settings.get_boolean("enable-drag")) {
            this.nonDragBlocker = new Clutter.Actor();
            switch (this.settings.get_int("default-snap")) {
                case 0:
                case 1:
                case 2:
                    this.nonDragBlocker.x = monitor.x;
                    this.nonDragBlocker.y = monitor.y;
                    this.nonDragBlocker.width = monitor.width;
                    this.nonDragBlocker.height = this.box.height + 2 * this.settings.get_int("snap-spacing-px");
                    break;
                case 3:
                    this.nonDragBlocker.x = monitor.x;
                    this.nonDragBlocker.y = monitor.y;
                    this.nonDragBlocker.width = this.box.width + 2 * this.settings.get_int("snap-spacing-px");
                    this.nonDragBlocker.height = monitor.height;
                    break;
                case 5:
                    this.nonDragBlocker.x = monitor.x + monitor.width - (this.box.width + 2 * this.settings.get_int("snap-spacing-px"));
                    this.nonDragBlocker.y = monitor.y;
                    this.nonDragBlocker.width = this.box.width + 2 * this.settings.get_int("snap-spacing-px");
                    this.nonDragBlocker.height = monitor.height;
                    break;
                case 6:
                case 7:
                case 8:
                    this.nonDragBlocker.x = monitor.x;
                    this.nonDragBlocker.y = monitor.y + monitor.height - (this.box.height + 2 * this.settings.get_int("snap-spacing-px"));
                    this.nonDragBlocker.width = monitor.width;
                    this.nonDragBlocker.height = this.box.height + 2 * this.settings.get_int("snap-spacing-px");
                    break;
            }
            if (this.settings.get_int("default-snap") == 4) {
                this.nonDragBlocker.destroy();
                this.nonDragBlocker = null;
            }
        } else {
            this.nonDragBlocker.destroy();
            this.nonDragBlocker = null;
        }

        const grid = this.box.layout_manager
        grid.set_row_homogeneous(true)
        grid.set_column_homogeneous(!currentLayout[currentLayout.length - 1].split)

        let gridLeft;
        let gridRight;
        let currentGrid = grid;
        let left;
        let right;
        let topBtnWidth;

        if (currentLayout[currentLayout.length - 1].split) {
            this.box.reactive = false;
            left = new St.Widget({
                reactive: true,
                layout_manager: new Clutter.GridLayout({
                    orientation: Clutter.Orientation.HORIZONTAL,
                    row_homogeneous: true,
                    column_homogeneous: true
                }),
                width: Math.round((monitor.width - this.settings.get_int("snap-spacing-px") * 2) * this.widthPercent) / 2
            })
            gridLeft = left.layout_manager;
            let middle = new St.Widget({
                reactive: false,
                width: this.box.width * (1 - this.widthPercent) - 10 + this.settings.get_int("border-spacing-px")
            });
            right = new St.Widget({
                reactive: true,
                layout_manager: new Clutter.GridLayout({
                    orientation: Clutter.Orientation.HORIZONTAL,
                    row_homogeneous: true,
                    column_homogeneous: true
                }),
                width: Math.round((monitor.width - this.settings.get_int("snap-spacing-px") * 2) * this.widthPercent) / 2
            })
            gridRight = right.layout_manager;
            this.box.add_child(left)
            this.box.add_child(middle)
            this.box.add_child(right)
        }

        this.shiftButtons = [];
        // [insert handwriting 7]

        let width = 0;
        for (const c of currentLayout[0]) {
            width += (("width" in c) ? c.width : 1)
        }
        let rowSize;
        let halfSize;
        let r = 0;
        let c;
        const hasTopbar = (currentLayout[currentLayout.length - 1].topbar !== false) &&
            (currentLayout[currentLayout.length - 1].settings !== false || currentLayout[currentLayout.length - 1].close !== false);
        const topOffset = hasTopbar ? 5 : 0;
        const doAddKey = (keydef) => {
            if (("key" in keydef) && (keydef.key == "SETTINGS" || keydef.key == "CLOSE")) {
                const specialBtn = new St.Button({
                    x_expand: true,
                    y_expand: true
                });
                if (keydef.key == "SETTINGS") {
                    specialBtn.add_style_class_name("settings_btn");
                    specialBtn.connect("button-press-event", () => {
                        this.settingsOpenFunction();
                    });
                    specialBtn.connect("touch-event", () => {
                        if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN)
                            this.settingsOpenFunction();
                    });
                } else {
                    specialBtn.add_style_class_name("close_btn");
                    specialBtn.connect("button-press-event", () => {
                        this.closeFromKey();        // [erhai] clears the sticky flag
                    });
                    specialBtn.connect("touch-event", () => {
                        if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN) {
                            this.closeFromKey();    // [erhai] clears the sticky flag
                        }
                    });
                }
                specialBtn.add_style_class_name("key");
                const w = ("width" in keydef) ? keydef.width : 1;
                const h = ("height" in keydef) ? keydef.height : 1;
                currentGrid.attach(specialBtn, c, topOffset + r, w * 2, r == 0 ? 3 : h * 4);
                specialBtn.visible = true;
                c += w * 2;
                this.keys.push(specialBtn);
                return;
            }
            const i = ("key" in keydef) ? keycodes[keydef.key] : ("split" in keydef) ? "split" : "empty space";
            if (i != null && typeof i !== 'string') {
                if (i.layers.default == null) {
                    for (var key of Object.keys(i.layers)) {
                        i.layers[key] = i.layers["_" + key]
                    }
                }
                let params = {
                    x_expand: true,
                    y_expand: true
                }

                let iconKeys = ["left", "up", "right", "down", "space"]
                if (this.settings.get_boolean("show-icons")) {
                    iconKeys = ["left", "up", "right", "down", "backspace", "tab", "capslock", "shift", "enter", "ctrl", "super", "alt", "space", "menu"]
                }
                // [insert handwriting 8]
                if (iconKeys.some(j => { return i.layers.default.toLowerCase() == j })) {
                    params.style_class = i.layers.default.toLowerCase() + "_btn"
                    for (var key of Object.keys(i.layers)) {
                        i.layers["_" + key] = i.layers[key]
                        i.layers[key] = null
                    }
                } else {
                    params.label = i.layers.default
                }
                i.isMod = false
                if ([42, 54, 29, 125, 126, 56, 100, 97, 58, 69].some(j => { return i.code == j })) {
                    i.isMod = true;
                }
                const keyBtn = new St.Button(params)
                keyBtn.add_style_class_name('key')
                keyBtn.char = i
                if (i.code == 58) {
                    this.keymap = global.stage.context.get_backend().get_default_seat().get_keymap()
                    try {
                        this.capsLockConnect = this.keymap.connect("state-changed", (a, e) => {
                            this.setCapsLock(keyBtn, this.keymap.get_caps_lock_state())
                        })
                    } catch (e) {
                        // Clutter 51 dropped the signal: the 300 ms interval polls instead
                        this.capsLockConnect = null;
                        this._folioGateNote?.("capslock state polled") // [erhai]
                    }
                    this.updateCapsLock = () => this.setCapsLock(keyBtn, this.keymap.get_caps_lock_state())
                } else if (i.code == 69) {
                    this.keymap = global.stage.context.get_backend().get_default_seat().get_keymap()
                    try {
                        this.numLockConnect = this.keymap.connect("state-changed", (a, e) => {
                            this.setNumLock(keyBtn, this.keymap.get_num_lock_state())
                        })
                    } catch (e) {
                        this.numLockConnect = null;
                    }
                    this.updateNumLock = () => this.setNumLock(keyBtn, this.keymap.get_num_lock_state())
                } else if (i.code == 42 || i.code == 54) {
                    this.shiftButtons.push(keyBtn)
                }
                currentGrid.attach(keyBtn, c, topOffset + r, (("width" in keydef) ? keydef.width : 1) * 2, r == 0 ? 3 : (("height" in keydef) ? keydef.height : 1) * 4)
                keyBtn.visible = true
                c += (("width" in keydef) ? keydef.width : 1) * 2
                this.keys.push(keyBtn)
                // [insert handwriting 9]
            } else if (i == "empty space") {
                c += (("width" in keydef) ? keydef.width : 1) * 2
            } else if (i == "split") {
                currentGrid = gridRight
                const size = c
                if (!halfSize) halfSize = size
            }
        }

        for (const kRow of currentLayout.slice(0, -1)) {
            c = 0;
            if (currentLayout[currentLayout.length - 1].split) {
                currentGrid = gridLeft;
            }
            for (const keydef of kRow) {
                if (keydef instanceof Array) {
                    keydef.forEach(i => { doAddKey(i); r += 2; c -= (("width" in i) ? i.width : 1) * 2 });
                    c += (("width" in keydef[0]) ? keydef[0].width : 1) * 2;
                    r -= 4;
                } else {
                    doAddKey(keydef)
                }
            }
            if (!topBtnWidth) topBtnWidth = ((("width" in kRow[kRow.length - 1]) && ("key" in kRow[kRow.length - 1])) ? kRow[kRow.length - 1].width : 1)
            const size = c;
            if (!rowSize) rowSize = size;
            r += r == 0 ? 3 : 4
        }

        if (left != null) {
            this.set_reactive(false)
            left.add_style_class_name("boxLay");
            right.add_style_class_name("boxLay");
            if (this.settings.get_boolean("system-accent-col") && major >= 47) {
                if (this.settings.scheme == "-dark") {
                    left.set_style("background-color: st-darken(-st-accent-color, 30%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                    right.set_style("background-color: st-darken(-st-accent-color, 30%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                } else {
                    left.set_style("background-color: st-lighten(-st-accent-color, 10%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                    right.set_style("background-color: st-lighten(-st-accent-color, 10%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                }
            } else {
                left.set_style("background-color: rgba(" + this.settings.get_double("background-r" + this.settings.scheme) + "," + this.settings.get_double("background-g" + this.settings.scheme) + "," + this.settings.get_double("background-b" + this.settings.scheme) + ", " + this.settings.get_double("background-a" + this.settings.scheme) + "); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                right.set_style("background-color: rgba(" + this.settings.get_double("background-r" + this.settings.scheme) + "," + this.settings.get_double("background-g" + this.settings.scheme) + "," + this.settings.get_double("background-b" + this.settings.scheme) + ", " + this.settings.get_double("background-a" + this.settings.scheme) + "); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
            }
            if (this.lightOrDark()) {
                left.add_style_class_name("inverted");
                right.add_style_class_name("inverted");
            } else {
                left.add_style_class_name("regular");
                right.add_style_class_name("regular");
            }

            if (hasTopbar) {
            let mvBtnStartLeft = 2 * topBtnWidth
            let mvBtnEndRight = 2 * topBtnWidth

            if (currentLayout[currentLayout.length - 1].settings) {
                const settingsBtn = new St.Button({
                    x_expand: true,
                    y_expand: true
                })
                settingsBtn.add_style_class_name("settings_btn")
                settingsBtn.add_style_class_name("key")
                settingsBtn.connect("button-press-event", () => {
                    this.settingsOpenFunction();
                })
                settingsBtn.connect("touch-event", () => {
                    if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN)
                        this.settingsOpenFunction();
                })
                this.keys.push(settingsBtn)
                gridLeft.attach(settingsBtn, 0, 0, 2 * topBtnWidth, 3)
            } else {
                mvBtnStartLeft = 0
            }

            if (currentLayout[currentLayout.length - 1].close) {
                const closeBtn = new St.Button({
                    x_expand: true,
                    y_expand: true
                })
                closeBtn.add_style_class_name("close_btn")
                closeBtn.add_style_class_name("key")
                closeBtn.connect("button-press-event", () => {
                    this.closeFromKey();            // [erhai] clears the sticky flag
                })
                closeBtn.connect("touch-event", () => {
                    if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN) {
                        this.closeFromKey();        // [erhai] clears the sticky flag
                    }
                })
                gridRight.attach(closeBtn, (rowSize - 2 * topBtnWidth), 0, 2 * topBtnWidth, 3)
                this.keys.push(closeBtn)
            } else {
                mvBtnEndRight = 0
            }

            let moveHandleLeft = new St.Button({
                x_expand: true,
                y_expand: true
            })
            moveHandleLeft.clear_actions()
            moveHandleLeft.add_style_class_name("moveHandle")
            moveHandleLeft.set_style("font-size: " + this.settings.get_int("font-size-px") + "px; border-radius: " + (this.settings.get_boolean("round-key-corners") ? "5px" : "0") + "; background-size: " + this.settings.get_int("font-size-px") + "px; font-weight: " + (this.settings.get_boolean("font-bold") ? "bold" : "normal") + "; border: " + this.settings.get_int("border-spacing-px") + "px solid transparent;");
            if (this.lightOrDark()) {
                moveHandleLeft.add_style_class_name("inverted");
            } else {
                moveHandleLeft.add_style_class_name("regular");
            }

            moveHandleLeft.connect("event", (actor, event) => {
                if (event.type() == Clutter.EventType.BUTTON_PRESS || event.type() == Clutter.EventType.TOUCH_BEGIN) {
                    this.draggable = this.settings.get_boolean("enable-drag");
                }
                this.event(event, false)
            })
            gridLeft.attach(moveHandleLeft, mvBtnStartLeft, 0, (halfSize - mvBtnStartLeft), 3)

            let moveHandleRight = new St.Button({
                x_expand: true,
                y_expand: true
            })
            moveHandleRight.clear_actions()
            moveHandleRight.add_style_class_name("moveHandle")
            moveHandleRight.set_style("font-size: " + this.settings.get_int("font-size-px") + "px; border-radius: " + (this.settings.get_boolean("round-key-corners") ? "5px" : "0") + "; background-size: " + this.settings.get_int("font-size-px") + "px; font-weight: " + (this.settings.get_boolean("font-bold") ? "bold" : "normal") + "; border: " + this.settings.get_int("border-spacing-px") + "px solid transparent;");
            if (this.lightOrDark()) {
                moveHandleRight.add_style_class_name("inverted");
            } else {
                moveHandleRight.add_style_class_name("regular");
            }

            moveHandleRight.connect("event", (actor, event) => {
                if (event.type() == Clutter.EventType.BUTTON_PRESS || event.type() == Clutter.EventType.TOUCH_BEGIN) {
                    this.draggable = this.settings.get_boolean("enable-drag");
                }
                this.event(event, false)
            })
            gridRight.attach(moveHandleRight, halfSize, 0, (rowSize - halfSize - mvBtnEndRight), 3)
            gridLeft.attach(new St.Widget({ x_expand: true, y_expand: true }), 0, 3, halfSize, 1)
            gridRight.attach(new St.Widget({ x_expand: true, y_expand: true }), halfSize, 3, (rowSize - halfSize), 1)
            }
        } else {
            this.box.add_style_class_name("boxLay");
            if (this.settings.get_boolean("system-accent-col") && major >= 47) {
                if (this.settings.scheme == "-dark") {
                    this.box.set_style("background-color: st-darken(-st-accent-color, 30%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                } else {
                    this.box.set_style("background-color: st-lighten(-st-accent-color, 10%); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
                }
            } else {
                this.box.set_style("background-color: rgba(" + this.settings.get_double("background-r" + this.settings.scheme) + "," + this.settings.get_double("background-g" + this.settings.scheme) + "," + this.settings.get_double("background-b" + this.settings.scheme) + ", " + this.settings.get_double("background-a" + this.settings.scheme) + "); padding: " + this.settings.get_int("outer-spacing-px") + "px;")
            }
            if (this.lightOrDark()) {
                this.box.add_style_class_name("inverted");
            } else {
                this.box.add_style_class_name("regular");
            }

            if (hasTopbar) {
            let mvBtnStartLeft = 2 * topBtnWidth
            let mvBtnEndRight = 2 * topBtnWidth

            if (currentLayout[currentLayout.length - 1].settings) {
                const settingsBtn = new St.Button({
                    x_expand: true,
                    y_expand: true
                })
                settingsBtn.add_style_class_name("settings_btn")
                settingsBtn.add_style_class_name("key")
                settingsBtn.connect("button-press-event", () => {
                    this.settingsOpenFunction();
                })
                settingsBtn.connect("touch-event", () => {
                    if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN)
                        this.settingsOpenFunction();
                })
                this.keys.push(settingsBtn)
                grid.attach(settingsBtn, 0, 0, 2 * topBtnWidth, 3)
            } else {
                mvBtnStartLeft = 0
            }
            if (currentLayout[currentLayout.length - 1].close) {
                const closeBtn = new St.Button({
                    x_expand: true,
                    y_expand: true
                })
                closeBtn.add_style_class_name("close_btn")
                closeBtn.add_style_class_name("key")
                closeBtn.connect("button-press-event", () => {
                    this.closeFromKey();            // [erhai] clears the sticky flag
                })
                closeBtn.connect("touch-event", () => {
                    if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN) {
                        this.closeFromKey();        // [erhai] clears the sticky flag
                    }
                })
                grid.attach(closeBtn, (rowSize - 2 * topBtnWidth), 0, 2 * topBtnWidth, 3)
                this.keys.push(closeBtn)
            } else {
                mvBtnEndRight = 0;
            }

            // [insert handwriting 10]

            let moveHandle = new St.Button({
                x_expand: true,
                y_expand: true
            })
            moveHandle.clear_actions();
            moveHandle.add_style_class_name("moveHandle")
            moveHandle.set_style("font-size: " + this.settings.get_int("font-size-px") + "px; border-radius: " + (this.settings.get_boolean("round-key-corners") ? "5px" : "0") + "; background-size: " + this.settings.get_int("font-size-px") + "px; font-weight: " + (this.settings.get_boolean("font-bold") ? "bold" : "normal") + "; border: " + this.settings.get_int("border-spacing-px") + "px solid transparent;");
            if (this.lightOrDark()) {
                moveHandle.add_style_class_name("inverted");
            } else {
                moveHandle.add_style_class_name("regular");
            }

            moveHandle.connect("event", (actor, event) => {
                if (event.type() == Clutter.EventType.BUTTON_PRESS || event.type() == Clutter.EventType.TOUCH_BEGIN) {
                    this.draggable = this.settings.get_boolean("enable-drag");
                }
                this.event(event, false)
            })
            grid.attach(moveHandle, mvBtnStartLeft, 0, (rowSize - mvBtnStartLeft - mvBtnEndRight), 3) // [insert handwriting 11]
            grid.attach(new St.Widget({ x_expand: true, y_expand: true }), 0, 3, rowSize, 1)
            }
        }

        this.keys.forEach(item => {
            item.set_style("font-size: " + this.settings.get_int("font-size-px") + "px; border-radius: " + (this.settings.get_boolean("round-key-corners") ? (this.settings.get_int("border-spacing-px") + 5) + "px" : "0") + "; background-size: " + this.settings.get_int("font-size-px") + "px; font-weight: " + (this.settings.get_boolean("font-bold") ? "bold" : "normal") + "; border: " + this.settings.get_int("border-spacing-px") + "px solid transparent;");
            if (this.lightOrDark()) {
                item.add_style_class_name("inverted");
            } else {
                item.add_style_class_name("regular");
            }
            item.set_pivot_point(0.5, 0.5)
            item.connect("destroy", () => {
                if (item.button_pressed !== null) {
                    clearTimeout(item.button_pressed)
                    item.button_pressed = null
                }
                if (item.button_repeat !== null) {
                    clearInterval(item.button_repeat)
                    item.button_repeat = null
                }
                if (item.tap_pressed !== null) {
                    clearTimeout(item.tap_pressed)
                    item.tap_pressed = null
                }
                if (item.tap_repeat !== null) {
                    clearInterval(item.tap_repeat)
                    item.tap_repeat = null
                }
            })
            let pressEv = (evType) => {
                // [erhai] remember a momentary modifier that this finger is
                // holding down, so a simultaneous tap on another key can
                // combine with it (multi-touch "hold Shift + tap Enter").
                if (item.char && erhaiMomentaryMods.includes(item.char.code))
                    item._modDown = true;
                this.box.set_child_at_index(item, this.box.get_children().length - 1);
                item.space_motion_handler = null
                item.set_scale(1.2, 1.2)
                item.add_style_pseudo_class("pressed")
                let player = global.display.get_sound_player();
                if (this.settings.get_boolean("play-sound")) {
                    if (this.settings.get_string("sound-file") != "") {
                        const sound_file = Gio.File.new_for_path(this.settings.get_string("sound-file"))
                        player.play_from_file(sound_file, "tap", null)
                    } else {
                        player.play_from_theme("dialog-information", "tap", null)
                    }
                }
                if ((this.settings.get_boolean('enable-key-repeat') && item.char != null) ||
                    ["delete_btn", "backspace_btn", "up_btn", "down_btn", "left_btn", "right_btn"].some(e => item.has_style_class_name(e))) {
                    item.button_pressed = setTimeout(() => {
                        const oldModBtns = this.modBtns
                        item.button_repeat = setInterval(() => {
                            if (this.settings.get_boolean("play-sound")) {
                                if (this.settings.get_string("sound-file") != "") {
                                    const sound_file = Gio.File.new_for_path(this.settings.get_string("sound-file"))
                                    player.play_from_file(sound_file, "tap", null)
                                } else {
                                    player.play_from_theme("dialog-information", "tap", null)
                                }
                            }
                            this.decideMod(item.char)

                            for (var i of oldModBtns) {
                                this.decideMod(i.char, i)
                            }
                        }, this.settings.get_int("key-repeat-rate"));
                    }, 750);
                } else if (item.has_style_class_name("space_btn")) {
                    item.button_pressed = setTimeout(() => {
                        let lastPos = (item.get_transformed_position()[0] + item.get_transformed_size()[0] / 2)
                        if (evType == "mouse") {
                            item.space_motion_handler = item.connect("motion_event", (actor, event) => {
                                let absX = event.get_coords()[0];
                                if (Math.abs(absX - lastPos) > 20) {
                                    if (absX > lastPos) {
                                        this.sendKey([106])
                                    } else {
                                        this.sendKey([105])
                                    }
                                    lastPos = absX
                                }
                            })
                        } else {
                            item.space_motion_handler = item.connect("touch_event", (actor, event) => {
                                if (event.type() == Clutter.EventType.TOUCH_UPDATE) {
                                    let absX = event.get_coords()[0];
                                    if (Math.abs(absX - lastPos) > 20) {
                                        if (absX > lastPos) {
                                            this.sendKey([106])
                                        } else {
                                            this.sendKey([105])
                                        }
                                        lastPos = absX
                                    }
                                }
                            })
                        }
                    }, 750)
                } else {
                    item.key_pressed = true;
                    item.button_pressed = setTimeout(() => {
                        releaseEv()
                    }, 1000);
                }
            }
            let releaseEv = () => {
                item.remove_style_pseudo_class("pressed")
                item.ease({
                    scale_x: 1,
                    scale_y: 1,
                    duration: 100,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                    onComplete: () => { item.set_scale(1, 1); }
                })
                if (item.button_pressed !== null) {
                    clearTimeout(item.button_pressed)
                    item.button_pressed = null
                }
                if (item.button_repeat !== null) {
                    clearInterval(item.button_repeat)
                    item.button_repeat = null
                }
                if (item.space_motion_handler !== null) {
                    item.disconnect(item.space_motion_handler)
                    item.space_motion_handler = null;
                } else if (item.key_pressed == true || item.space_motion_handler == null) {
                    try {
                        if (!item.char.isMod) {
                            this.decideMod(item.char)
                        } else if (item._modUsed) {
                            // [erhai] this momentary modifier was held while
                            // another key was pressed: it was already applied to
                            // that key, so do NOT latch it now (a held modifier
                            // must not leave a stuck Shift/Ctrl/Alt behind).
                            item._modUsed = false;
                        } else {
                            const modButton = item;
                            this.decideMod(item.char, modButton)
                        }
                    } catch { }
                }
                item.key_pressed = false;
                item._modDown = false;      // [erhai] finger left this key
            }
            item.clear_actions();
            item.connect("button-press-event", () => pressEv("mouse"))
            item.connect("button-release-event", releaseEv)
            item.connect("touch-event", () => {
                if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_BEGIN) {
                    pressEv("touch")
                } else if (Clutter.get_current_event().type() == Clutter.EventType.TOUCH_END || Clutter.get_current_event().type() == Clutter.EventType.TOUCH_CANCEL) {
                    releaseEv()
                }
            })
        });
    }

    lightOrDark() {
        let r, g, b;
        if (this.settings.get_boolean("system-accent-col")) {
            return this.settings.scheme != "-dark"
        } else {
            r = this.settings.get_double("background-r" + this.settings.scheme);
            g = this.settings.get_double("background-g" + this.settings.scheme);
            b = this.settings.get_double("background-b" + this.settings.scheme);
        }
        var hsp;
        hsp = Math.sqrt(
            0.299 * (r * r) +
            0.587 * (g * g) +
            0.114 * (b * b)
        );
        return hsp > 127.5
    }
    releaseAllKeys() {
        this.keys.forEach(item => {
            item.key_pressed = false;
            if (item.button_pressed !== null) {
                clearTimeout(item.button_pressed)
                item.button_pressed = null
            }
            if (item.button_repeat !== null) {
                clearInterval(item.button_repeat)
                item.button_repeat = null
            }
            if (item.space_motion_handler !== null && GObject.signal_handler_is_connected(item, item.space_motion_handler)) {
                item.disconnect(item.space_motion_handler)
                item.space_motion_handler = null;
            }
        })
    }
    /* KEYCODE -> KEYVAL.
     * Shell 51's own keyboard injects KEYVALS (ui/keyboard.js:
     *   _virtualDevice.notify_keyval(t * 1000, keyval, state)), while gjs-osk
     * has always injected HARDWARE KEYCODES through notify_key().  On 26.10 that
     * no longer reaches a text field (owner report 2026-10-27: the Wi-Fi
     * password entry stays empty, CapsLock does nothing), while the identical
     * build types fine on 26.04.  Mutters' Clutter 51 no longer exposes
     * get_entries_for_keycode() either, so the keyval is derived from the key's
     * own label in the layout (a printable key is exactly its default-layer
     * character) plus a table for the named keys.  26.04 keeps keycodes.
     * The helpers live at module level (oskKeyvalForCode & friends) because
     * every Keyboard method is async-wrapped -- see the note up there. */

    /* One key event.  Application windows (and anything before Shell 51) keep
     * the KEYCODE path that has always worked; text fields inside the shell get
     * a KEYVAL, which is what Shell 51's own keyboard sends and the only thing
     * those actors accept.  `keyval` is passed in by sendKey() so press and
     * release always agree, even if the layer changes in between. */
    notifyKeyEvent(code, state, time, keyval) {
        const textMode = major >= 51 &&
            (keyval !== undefined || oskShellTextFocused());
        if (textMode) {
            if (keyval === undefined)
                keyval = oskShellKeyvalForCode(this, code);
            if (!keyval)
                return;                 // modifier or unknown: nothing to inject
            /* Never hand anything but a real keysym to notify_keyval(): a
             * Promise (async-wrapped helper) or NaN throws out of range and
             * loses the keystroke. */
            if (!Number.isInteger(keyval) || keyval <= 0 || keyval > OSK_MAX_KEYVAL) {
                if (!this._keyvalInvalid) {
                    this._keyvalInvalid = true;
                    console.warn(`GJS-OSK: refusing keyval ${keyval} ` +
                        `(${typeof keyval})`);
                }
                return;
            }
            try {
                this.inputDevice.notify_keyval(time, keyval, state);
            } catch (e) {
                if (!this._keyvalFailed) {
                    this._keyvalFailed = true;
                    logError(e, `GJS-OSK: notify_keyval(${keyval}) failed`);
                }
            }
            return;
        }
        this.inputDevice.notify_key(time, code, state);
    }

    sendKey(keys) {
        try {
            if (this.keyInProgress) {
                return;
            }
            this.keyInProgress = true;
            let event_time = Clutter.get_current_event_time() * 1000;
            /* shell text actor: resolve every keyval BEFORE the layer is
             * touched again (decideMod resets the modifiers right after
             * sendKey), so the release reuses exactly the pressed keyval. */
            let keyvals = null;
            const shellText = major >= 51 && oskShellTextFocused();
            if (shellText) {
                keyvals = keys.map(k => oskShellKeyvalForCode(this, k));
                const focus = global.stage.key_focus;
                oskDbg(this, `keys=[${keys}] keyvals=[${keyvals}] ` +
                    `layer=${oskLabelLayer(this)} shift=${this.shift} ` +
                    `caps=${this.capsL} ` +
                    `focus=${focus ? focus.constructor.name : 'none'} ` +
                    `grab=${global.stage.get_grab_actor() ? 'yes' : 'no'}`);
            }
            for (var i = 0; i < keys.length; i++) {
                this.notifyKeyEvent(keys[i], Clutter.KeyState.PRESSED, event_time,
                    keyvals ? keyvals[i] : undefined);
            }
            if (this.keyTimeout !== null) {
                clearTimeout(this.keyTimeout);
                if (typeof this.keyTimeoutFunc === 'function') {
                    this.keyTimeoutFunc();
                }
                this.keyTimeout = null;
                this.keyTimeoutFunc = null;
            }
            this.keyTimeoutFunc = () => {
                const currKeys = keys
                const currVals = keyvals;
                for (var j = currKeys.length - 1; j >= 0; j--) {
                    this.notifyKeyEvent(currKeys[j], Clutter.KeyState.RELEASED, event_time,
                        currVals ? currVals[j] : undefined);
                }
                this.keyInProgress = false;
            }
            this.keyTimeout = setTimeout(this.keyTimeoutFunc, 5);
        } catch (err) {
            this.keyInProgress = false;
            throw new Error("GJS-OSK: An unknown error occured. Please report this bug to the Issues page (https://github.com/Vishram1123/gjs-osk/issues):\n\n" + err + "\n\nKeys Pressed: " + keys);
        }
    }

    decideMod(i, mBtn) {
        if (!i) return;
        const needsButton = [29, 56, 97, 125, 126, 100, 42, 54, 58, 69].includes(i.code);
        if (needsButton && (!mBtn || !mBtn.char)) return;

        if (i.code == 29 || i.code == 56 || i.code == 97 || i.code == 125 || i.code == 126) {
            this.setNormMod(mBtn);
        } else if (i.code == 100) {
            this.setAlt(mBtn);
        } else if (i.code == 42 || i.code == 54) {
            this.setShift(mBtn);
        } else if (i.code == 58 || i.code == 69) {
            this.sendKey([mBtn.char.code]);
        } else {
            // [erhai] combine this key with any momentary modifier that is
            // currently held down by ANOTHER finger (multi-touch hold), in
            // addition to the tapped/latched modifiers already in `this.mod`.
            // A plain tap of a modifier never sets `_modDown` at this point
            // (it was released before the key was tapped), so this leaves the
            // normal tap-to-latch path untouched.
            for (const k of this.keys) {
                if (k.char && k._modDown &&
                    erhaiMomentaryMods.includes(k.char.code) &&
                    !this.mod.includes(k.char.code)) {
                    this.mod.push(k.char.code);
                    k._modUsed = true;  // do not latch it when it is released
                }
            }
            this.mod.push(i.code);
            this.sendKey(this.mod);
            this.mod = [];
            this.modBtns.forEach(button => {
                button.remove_style_class_name("selected");
            });
            this.shiftButtons.forEach(i => { i.remove_style_class_name("selected") })
            this.resetAllMod();
            this.modBtns = [];
        }
    }

    setCapsLock(button, state) {
        // polling (Shell 51 has no Keymap state-changed signal) must not relabel
        // every key 3x per second: only a real change does anything
        if (state === this.capsL)
            return;
        if (state) {
            button.add_style_class_name("selected");
            this.capsL = true;
        } else {
            button.remove_style_class_name("selected");
            this.capsL = false;
        }
        this.updateKeyLabels();
    }

    setNumLock(button, state) {
        if (state === this.numsL)
            return;
        if (state) {
            button.add_style_class_name("selected");
            this.numsL = true;
        } else {
            button.remove_style_class_name("selected");
            this.numsL = false;
        }
        this.updateKeyLabels();
    }

    setAlt(button) {
        if (!button || !button.char) return;
        this.alt = !this.alt;
        this.updateKeyLabels();
        if (!this.alt) {
            this.sendKey([button.char.code]);
        }
        this.setNormMod(button);
    }

    setShift(button) {
        if (!button || button.char == undefined) return;
        this.shift = !this.shift;
        this.updateKeyLabels();
        if (!this.shift) {
            this.sendKey([button.char.code]);
            this.shiftButtons.forEach(i => { i.remove_style_class_name("selected") })
        } else {
            this.shiftButtons.forEach(i => { i.add_style_class_name("selected") })
        }
        this.setNormMod(button);
    }

    updateKeyLabels() {
        this.keys.forEach(key => {
            if (key.char != undefined) {
                let layer = (this.alt ? 'alt' : '') + (this.shift ? 'shift' : '') + (this.numsL ? 'num' : '') + (this.capsL ? 'caps' : '') + (this.numsL || this.capsL ? 'lock' : '')
                if (layer == '') layer = 'default'
                key.label = key.char.layers[layer];
            }
        });
    }


    setNormMod(button) {
        if (!button || button.char == undefined) return;
        if (this.mod.includes(button.char.code)) {
            this.mod.splice(this.mod.indexOf(button.char.code), this.mod.indexOf(button.char.code) + 1);
            if (!(button.char.code == 42) && !(button.char.code == 54))
                button.remove_style_class_name("selected");
            this.modBtns.splice(this.modBtns.indexOf(button), this.modBtns.indexOf(button) + 1);
            this.notifyKeyEvent(button.char.code, Clutter.KeyState.RELEASED,
                                Clutter.get_current_event_time() * (major >= 51 ? 1000 : 1));
            this.sendKey([button.char.code])
        } else {
            if (!(button.char.code == 42) && !(button.char.code == 54))
                button.add_style_class_name("selected");
            this.mod.push(button.char.code);
            this.modBtns.push(button);
            this.notifyKeyEvent(button.char.code, Clutter.KeyState.PRESSED,
                                Clutter.get_current_event_time() * (major >= 51 ? 1000 : 1));
        }
    }

    resetAllMod() {
        this.shift = false;
        this.alt = false;
        this.updateKeyLabels()
    }
}