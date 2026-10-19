// erhai Torch — Quick Settings torch for the rear camera flash.
//
// HARDWARE
// The rear flash is the pm8550 SPMI flash-LED controller
// (c400000.spmi:pmic@1:led-controller@ee00) driven by leds-qcom-flash, which
// registers TWO LEDs (see erhai.dts "Rear camera flash"):
//
//   white:flash-0   channels 1+4
//   white:flash-1   channels 2+3
//
// Physically both sit behind the same rear flash window, so the tablet has
// one visible torch that is made of two half-power strings. The extension
// therefore always drives BOTH LEDs together — driving only flash-0 lights
// half the window and looks broken.
//
// TORCH vs FLASH
// The driver's LED class 'brightness' attribute is TORCH mode, not flash:
// qcom_flash_led_brightness_set() computes
//     current_ma = brightness * max_torch_current_ma / LED_FULL
// and enables the LED module for any non-zero value, with brightness 0
// disabling it again. It also clears the flash timeout, so a torch stays on
// until it is explicitly switched off. That is precisely a torch, and it is
// why the slider can drive 'brightness' directly instead of using
// flash_strobe (a momentary pulse) or flash_brightness (flash-mode current).
// led-max-microamp = 500000 in the DT, divided over the two channels of each
// LED = 250 mA per channel, so the full 0..255 range is a safe torch range.
//
// PERMISSIONS
// A sysfs attribute write is privileged, and TAG+="uaccess" does NOT help
// here: uaccess adds a logind ACL to a DEVICE NODE and LED class devices have
// none. The package's 71-erhai-torch.rules rule instead chgrp's
// /sys/class/leds/white:flash-*/ to the erhai-torch group and makes it group
// writable at add time, and DEBIAN/postinst puts the desktop users in that
// group. So the shell writes the LED itself — no root helper, no polkit
// prompt, no per-drag D-Bus round trip, and the slider stays responsive while
// it is dragged.
//
// STATE
// The LED is the single source of truth: the slider reads the current
// 'brightness' back instead of remembering it. Nothing here survives a
// logout, and nothing should — a torch left running with no UI to switch it
// off would just drain the battery, so the extension switches the LEDs off
// when it is disabled (and the shell unloading extensions on logout runs
// through this same path).
//
// GNOME 50 notes: QuickSlider lives in ui/quickSettings.js (the icon button
// is clickable and emits 'icon-clicked'; the slider itself is 'this.slider'
// with a 0..1 'value'); menuEnabled shows the chevron, so the brightness
// submenu is only offered when it has something to show.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Clutter from 'gi://Clutter';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickSlider, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Slider} from 'resource:///org/gnome/shell/ui/slider.js';

const LEDS_DIR = '/sys/class/leds';

// Node basename prefix of the rear flash LEDs (see the HARDWARE note above).
const LED_GLOB = 'white:flash-';

// The DT caps each LED at 500 mA over 2 channels; 255 is the driver's full
// scale (LED_FULL) and maps onto that led-max-microamp. Never exceed
// max_brightness (it is read back at runtime and used as the ceiling).
const FALLBACK_MAX_BRIGHTNESS = 255;

// Fraction of the controller's own range that the slider's TOP end drives.
//
// The slider's 0..1 is rescaled onto 0..MAX_LEVEL of max_brightness, so the
// UI offers the full 0..100 % while the LED is only ever driven to 80 %
// (204/255) of what the driver would allow. Requesting the very top of the
// range is what reliably provokes the controller misbehaviour described at
// PACE_MS, so the top fifth of the range is simply not offered. Set this to
// 1.0 to hand the whole range back to the slider (safe once writes are paced:
// 255 on both LEDs holds indefinitely with an empty flash_fault).
const MAX_LEVEL = 0.8;

// Torch level used when the slider is switched on from 0. About a third of the
// range is a sensible walking-around torch that does not blind or roast the LED.
const DEFAULT_ON_FRACTION = 0.33;

// Minimum spacing between two writes to the flash controller, in ms.
//
// leds-qcom-flash must not be driven with a burst of writes:
//
//   * every brightness write runs set_flash_module_en(false) and then
//     set_flash_module_en(true), so EACH write blinks that LED off and back
//     on. Dragging a slider therefore flickers by construction, and the only
//     cure is to write rarely. That is the flicker that becomes obvious from
//     ~80 % upwards, where the brief off is easy to see against a bright LED;
//   * the driver's shared current budget (update_allowed_flash_current:
//     avail_ma = therm_ma - flash_data->total_ma) combined with the LED core's
//     asynchronous set_brightness_work makes back-to-back writes race.
//     Measured on this pad: a burst ending in "255" settled at 25, 102 or 229,
//     and a pair written with no gap left white:flash-0 at 0 while
//     white:flash-1 read 255. None of that returns an error to userspace, so
//     it shows up only as flicker, wrong brightness, or an LED that just
//     switches itself off;
//   * spaced writes are reliable at EVERY level: 255 on both LEDs held for
//     15 s with an empty flash_fault, so the ceiling is the write rate and
//     not the current.
//
// Hence the writer below touches at most ONE LED per tick and counts the
// request as done only when every LED reads back the wanted value, which makes
// completion itself the verification.
const PACE_MS = 90;

// How long a slider must hold still before its value is written, in ms.
//
// A drag fires 'notify::value' many times a second, and EVERY write blinks the
// LED (see the PACE_MS note: the driver toggles its module off and on around
// each change). Writing each intermediate value therefore strobes the torch for
// the entire drag. So a moving slider only updates the UI, and the hardware is
// written once the movement stops. Discrete choices (the icon toggle, the menu
// presets) skip this wait and apply at once.
const SETTLE_MS = 220;

// Give up after this many ticks without the LEDs agreeing, so a controller
// that refuses a value cannot spin a timer forever (~4 s at PACE_MS).
const PACE_MAX_TICKS = 45;

// Panel icons. 'torch-enabled/disabled-symbolic' is the exactly-right pair and
// is what this image's theme (Yaru-dark -> Yaru) ships, so it is preferred;
// 'media-flash-symbolic' comes from Adwaita and is the fallback when the user
// switches to a theme without the torch pair (a missing icon name renders as a
// blank glyph, so it must never be used unverified). QuickSlider has no text
// label on its panel row, so this icon is the only on/off cue there.
const ICON_ON = 'torch-enabled-symbolic';
const ICON_OFF = 'torch-disabled-symbolic';
const ICON_FALLBACK = 'media-flash-symbolic';

// Paths, relative to a theme root, where themes keep the icons this extension
// wants. Kept as an explicit list rather than a recursive walk: the whole tree
// is ~30k files, and checking a handful of well-known locations on startup is
// both faster and easier to reason about. 'phosh/' is where Yaru puts the
// torch pair; 'devices/' is where Adwaita/Yaru put media-flash-symbolic.
const ICON_SUBDIRS = [
    'scalable/phosh',
    'scalable/devices',
    'scalable/status',
    'scalable/camera',
    'symbolic/devices',
    'symbolic/status',
];

// Walk the active icon theme's inheritance chain and report whether `name`
// resolves to a real file, the way St looks it up. Without this a theme swap
// (e.g. to plain Adwaita) would silently blank the slider icon.
function iconExists(name) {
    const roots = [
        GLib.build_filenamev([GLib.get_home_dir(), '.local/share/icons']),
        '/usr/share/icons',
    ];

    // Gtk.Settings is the authority on the active theme, but it lives in
    // gi://Gtk which this extension does not otherwise need; the interface
    // setting is the same value the shell's own St.Settings picks up.
    let theme = null;
    try {
        theme = Gio.Settings.new('org.gnome.desktop.interface')
            .get_string('icon-theme');
    } catch {
        theme = null;
    }
    if (!theme)
        theme = 'Adwaita';

    const seen = new Set();
    const walk = themeName => {
        if (!themeName || seen.has(themeName))
            return false;
        seen.add(themeName);

        // Look in each root in order. A user theme (~/.local/share/icons) must
        // win over the system copy of the same name, so the first root that
        // actually contains the theme is the one that is inspected -- but if
        // the theme only exists in a later root, that one is used instead of
        // giving up.
        for (const root of roots) {
            const themeDir = GLib.build_filenamev([root, themeName]);
            if (!Gio.File.new_for_path(themeDir).query_exists(null))
                continue;

            for (const subdir of ICON_SUBDIRS) {
                for (const ext of ['svg', 'png']) {
                    const path = GLib.build_filenamev(
                        [themeDir, subdir, `${name}.${ext}`]);
                    if (Gio.File.new_for_path(path).query_exists(null))
                        return true;
                }
            }

            // Follow Inheritance= so a name provided by a parent theme (the
            // usual case: Yaru-dark has almost no icons of its own and
            // Inherits=Yaru) is still found.
            try {
                const [, bytes] = Gio.File.new_for_path(
                    GLib.build_filenamev([themeDir, 'index.theme']))
                    .load_contents(null);
                const match = new TextDecoder().decode(bytes)
                    .match(/^\s*Inherits\s*=\s*(.*)$/m);
                if (match) {
                    for (const parent of match[1].split(',')) {
                        if (walk(parent.trim()))
                            return true;
                    }
                }
            } catch {
                /* no/unreadable index.theme -> leaf theme */
            }

            // This root holds the theme, so the search stops here: a
            // same-named theme in a later root is the lower-priority copy,
            // and the inheritance chain above has already been followed.
            return false;
        }

        // No root had the theme at all.
        return false;
    };

    return walk(theme);
}

// Resolve the on/off icon pair once, at startup: prefer the torch pair and
// fall back to Adwaita's media-flash-symbolic when the active theme lacks it
// (a missing icon name renders as a blank glyph, so it must never be used
// unverified).
function resolveIcons() {
    if (iconExists(ICON_ON) && iconExists(ICON_OFF))
        return {on: ICON_ON, off: ICON_OFF};
    return {on: ICON_FALLBACK, off: ICON_FALLBACK};
}

// Quick levels in the chevron menu, strongest first so the list reads the way
// the owner asked for, top to bottom: Maximum, High, Medium, Low, Off. The
// explicit "Off" entry beats dragging a slider all the way back to zero.
const PRESETS = [
    ['Maximum', 1.00],
    ['High', 0.75],
    ['Medium', 0.40],
    ['Low', 0.15],
    ['Off', 0.00],
];

function ledsPath(name) {
    return `${LEDS_DIR}/${name}`;
}

function readFile(path) {
    try {
        const [, contents] = Gio.File.new_for_path(path).load_contents(null);
        return new TextDecoder().decode(contents).trim();
    } catch {
        return null;
    }
}

// Write an attribute the same way `echo N > attr` does, but without a shell.
// sysfs attributes must be written as a single whole value and never
// partially, so this uses replace_contents (open + write + close) rather than
// an append/stream write. Failure is returned instead of thrown so callers
// can report a missing udev rule or a missing group membership as the
// actionable problem that it is.
function writeFile(path, value) {
    try {
        Gio.File.new_for_path(path).replace_contents(
            new TextEncoder().encode(value),
            null, false, Gio.FileCreateFlags.NONE, null);
        return null;
    } catch (e) {
        return e;
    }
}

// Enumerate the rear flash LEDs as they exist right now. Discovered at call
// time on purpose: the attribute perms arrive with the udev rule, the LEDs
// appear only once leds-qcom-flash has probed, and a suspend/resume cycle can
// in principle re-register them.
function findTorchLeds() {
    const found = [];
    let enumerator;
    try {
        enumerator = Gio.File.new_for_path(LEDS_DIR).enumerate_children(
            'standard::name', Gio.FileQueryInfoFlags.NONE, null);
    } catch {
        return found;
    }

    let info;
    while ((info = enumerator.next_file(null)) !== null) {
        const name = info.get_name();
        if (!name.startsWith(LED_GLOB))
            continue;
        found.push({
            name,
            brightnessPath: ledsPath(`${name}/brightness`),
        });
    }
    enumerator.close(null);
    found.sort((a, b) => a.name.localeCompare(b.name));
    return found;
}

function maxBrightnessOf(led) {
    const raw = readFile(ledsPath(`${led.name}/max_brightness`));
    const value = Number.parseInt(raw ?? '', 10);
    return Number.isInteger(value) && value > 0 ? value : FALLBACK_MAX_BRIGHTNESS;
}

// The controller level that corresponds to a 0..1 UI value, in the driver's
// own 0..max_brightness units, with the top of the slider mapped onto
// MAX_LEVEL rather than onto the full range (the rescale described there).
function levelForValue(led, value) {
    const clamped = Math.min(1, Math.max(0, value));
    return Math.round(clamped * MAX_LEVEL * maxBrightnessOf(led));
}

// Inverse of levelForValue: the UI value that a level read back from the LED
// represents. A level left ABOVE the MAX_LEVEL ceiling by something else (the
// camera app, a terminal) saturates the slider at 1.0 instead of overflowing.
function valueForLevel(led, level) {
    return Math.min(1, Math.max(0, level / (maxBrightnessOf(led) * MAX_LEVEL)));
}

// Current torch level of the LED group as a 0..1 UI value. The LEDs are
// written as a set, so the first one is representative; a level left higher on
// the second LED by something else is reported through valueForLevel's clamp
// rather than papered over.
function readTorchValue(leds) {
    if (leds.length === 0)
        return 0;
    const raw = Number.parseInt(readFile(leds[0].brightnessPath) ?? '', 10);
    if (!Number.isInteger(raw))
        return 0;
    return valueForLevel(leds[0], raw);
}

// The chevron menu holds a second, roomier copy of the brightness slider plus
// the quick torch levels, mirroring how the shell's own brightness slider
// offers per-monitor sliders through its menu.
class TorchLevelMenu extends PopupMenu.PopupMenuSection {
    addSlider(name) {
        const text = new PopupMenu.PopupMenuItem(name, {reactive: false});
        this.addMenuItem(text);

        const slider = new Slider(0);
        slider.accessible_name = name;

        const sliderBin = new St.Bin({
            style_class: 'slider-bin',
            child: slider,
            reactive: true,
            can_focus: true,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });

        const sliderMenuItem = new PopupMenu.PopupBaseMenuItem({reactive: false});
        sliderMenuItem.add_child(sliderBin);
        this.addMenuItem(sliderMenuItem);

        return slider;
    }
}

const TorchSlider = GObject.registerClass(
class TorchSlider extends QuickSlider {
    _init() {
        // Decided before super._init() because QuickSlider takes the icon name
        // as a construct property; the pair is then reused by _syncUI.
        this._icons = resolveIcons();

        super._init({
            iconName: this._icons.off,
            menuButtonAccessibleName: 'Open torch menu',
        });

        this.slider.accessible_name = 'Torch brightness';

        // The chevron only makes sense when there is a torch to drive, and the
        // shell shows it based on 'menu-enabled'.
        this.menuEnabled = this._hasLeds();

        this.menu.setHeader(this._icons.off, 'Torch',
            'Rear camera flash');

        this._levelsSection = new TorchLevelMenu();
        this.menu.addMenuItem(this._levelsSection);
        this._menuSlider = this._levelsSection.addSlider('Torch brightness');
        this._menuSliderChangedId = this._menuSlider.connect('notify::value', () => {
            if (this._syncing)
                return;
            this._requestValue(this._menuSlider.value, {source: this._menuSlider});
        });

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        for (const [label, value] of PRESETS) {
            const item = new PopupMenu.PopupMenuItem(label);
            item.connect('activate', () => this._requestValue(value, {immediate: true}));
            this.menu.addMenuItem(item);
        }

        // Clicking the icon toggles, matching how the shell's other quick
        // sliders treat their icon.
        this.connect('icon-clicked', () => this.toggle());

        // 'notify::value' covers the drag, the keyboard and the menu slider in
        // one place. A drag fires this many times a second, so the handler only
        // records the target; the settle timer (SETTLE_MS) and then the paced
        // writer (PACE_MS) do the actual hardware writes.
        this._sliderChangedId = this.slider.connect('notify::value', () => {
            if (this._syncing)
                return;
            this._requestValue(this.slider.value, {source: this.slider});
        });

        // Paced-writer state (see PACE_MS/SETTLE_MS): the level being driven
        // towards, the settle and write timers, and the tick count used to give
        // up on a controller that will not accept a value.
        this._target = null;
        this._settleId = 0;
        this._pumpId = 0;
        this._pumpTicks = 0;
        this._syncing = false;

        // Follow writes made outside the shell (the camera app, a terminal, a
        // udev re-add after resume). Polled: LED class devices emit a uevent
        // on every write, so a Gio file monitor would mostly report the
        // extension's own writes back at it. Skipped while a request is pending
        // or in flight: the sliders own the value then, and overwriting them
        // from the read-back is what made a fast drag fight itself.
        this._pollId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT, 2, () => {
                if (!this._pumpId && !this._settleId)
                    this._syncFromHardware();
                return GLib.SOURCE_CONTINUE;
            });

        this._syncFromHardware();
    }

    toggle() {
        // A click is a discrete choice, so it skips the settle wait.
        this._requestValue(this.slider.value > 0 ? 0 : DEFAULT_ON_FRACTION,
            {immediate: true});
    }

    _hasLeds() {
        return findTorchLeds().length > 0;
    }

    // Ask for a level.
    //
    // A drag is NOT written as it moves: every write blinks the LED (see the
    // PACE_MS note), so applying each intermediate value would strobe the torch
    // for the whole drag. A slider move therefore only records the target and
    // restarts the settle timer; the hardware is written once the movement
    // stops. Discrete choices (the icon toggle, the menu presets) pass
    // immediate = true and skip the wait.
    //
    // The slider that ORIGINATED the request is never written back to. Doing so
    // re-enters this method through 'notify::value' and overwrites the value the
    // user is still dragging towards, which made a fast drag fight itself and
    // could leave the torch reading a level nobody asked for.
    _requestValue(value, {immediate = false, source = null} = {}) {
        this._target = Math.min(1, Math.max(0, value));
        this._pumpTicks = 0;

        // Icon and header follow at once, so the control feels alive even
        // though the hardware lags behind.
        this._syncUI(this._target);
        this._syncSliderValues(this._target, source);

        if (this._settleId) {
            GLib.source_remove(this._settleId);
            this._settleId = 0;
        }

        if (immediate) {
            this._startPump();
            return;
        }

        this._settleId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT, SETTLE_MS, () => {
                this._settleId = 0;
                this._startPump();
                return GLib.SOURCE_REMOVE;
            });
    }

    _startPump() {
        if (this._pumpId)
            return;
        this._pumpId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT, PACE_MS, () => this._pump());
    }

    // Keep both sliders showing the same level. The 'source' slider (the one
    // the user is moving) is skipped on purpose; see _requestValue.
    _syncSliderValues(value, source) {
        this._syncing = true;
        try {
            for (const slider of [this.slider, this._menuSlider]) {
                if (slider === source)
                    continue;
                if (Math.abs(slider.value - value) > 0.0001)
                    slider.value = value;
            }
        } finally {
            this._syncing = false;
        }
    }

    // One tick writes at most ONE LED. Returns SOURCE_CONTINUE while any LED
    // still disagrees with the target, so reaching the end doubles as the
    // verification that every LED really took the value.
    _pump() {
        const leds = findTorchLeds();
        if (leds.length === 0 || this._target === null) {
            this._pumpId = 0;
            return GLib.SOURCE_REMOVE;
        }

        for (const led of leds) {
            const want = levelForValue(led, this._target);
            const have = Number.parseInt(
                readFile(led.brightnessPath) ?? '', 10);

            if (have === want)
                continue;

            const error = writeFile(led.brightnessPath, `${want}\n`);
            if (error) {
                this._pumpId = 0;
                this._reportWriteError(error);
                this._syncFromHardware();
                return GLib.SOURCE_REMOVE;
            }

            if (++this._pumpTicks > PACE_MAX_TICKS) {
                this._pumpId = 0;
                Main.notifyError('Torch',
                    'The flash controller did not accept the requested brightness.');
                this._syncFromHardware();
                return GLib.SOURCE_REMOVE;
            }

            // Wrote one LED; the other one waits for the next tick.
            return GLib.SOURCE_CONTINUE;
        }

        // Every LED reads back the wanted level, so the request is done. The
        // sliders are deliberately NOT resynced from the read-back here: the
        // value the user chose is already on screen, and writing the quantised
        // level back would re-enter _requestValue and fight a drag that is
        // still in progress.
        this._pumpId = 0;
        this._pumpTicks = 0;
        this._target = null;
        return GLib.SOURCE_REMOVE;
    }

    _reportWriteError(error) {
        // EACCES almost always means the udev rule has not run yet (the
        // package was just installed) or the user is not in the erhai-torch
        // group (the postinst adds it; it applies at the next login), so say
        // that instead of the bare errno.
        Main.notifyError('Torch',
            error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.PERMISSION_DENIED)
                ? 'Not allowed to drive the torch. Log out and back in so the erhai-torch group applies.'
                : error.message);
    }

    // Reflect a known-good value in the UI without writing it back to sysfs.
    _applyValue(value) {
        this._syncing = true;
        try {
            if (Math.abs(this.slider.value - value) > 0.0001)
                this.slider.value = value;
            if (Math.abs(this._menuSlider.value - value) > 0.0001)
                this._menuSlider.value = value;
        } finally {
            this._syncing = false;
        }
        this._syncUI(value);
    }

    _syncFromHardware() {
        const leds = findTorchLeds();

        // The LEDs can show up after the shell has started (the driver may
        // probe late), so keep the chevron availability honest rather than
        // deciding once at construction time.
        this.menuEnabled = leds.length > 0;

        this._applyValue(readTorchValue(leds));
    }

    _syncUI(value) {
        const on = value > 0;
        const percent = Math.round(value * 100);

        // QuickSlider's panel row is icon + slider + chevron only -- it has no
        // title/subtitle label (unlike QuickToggle), so the level readout has
        // to live in the menu header. Setting a 'subtitle' property here would
        // just create a dead JS property.
        this.iconName = on ? this._icons.on : this._icons.off;
        this.menu.setHeader(this.iconName, 'Torch',
            on ? `Rear camera flash · ${percent} %` : 'Rear camera flash · Off');
    }

    destroy() {
        // Leaving the panel must not leave the torch burning: switch the LEDs
        // off before the widget (and its bus) goes away. A zero write needs no
        // current allocation and clears that LED's channel-enable bits, so the
        // controller's module goes off once no channels are left. Both timers
        // are stopped first so that neither the settle nor the pump can relight
        // the LEDs behind us.
        if (this._settleId) {
            GLib.source_remove(this._settleId);
            this._settleId = 0;
        }
        if (this._pumpId) {
            GLib.source_remove(this._pumpId);
            this._pumpId = 0;
        }

        this._target = null;
        for (const led of findTorchLeds()) {
            // A zero write does not touch the driver's current budget (with
            // enable == false it releases the allocation and returns early),
            // so this is not the racy path that level writes hit, and two of
            // them back-to-back measured reliable. Read back once regardless:
            // it costs a read, and the failure mode is a torch left burning
            // with no UI left to switch it off.
            writeFile(led.brightnessPath, '0\n');
            if ((readFile(led.brightnessPath) ?? '0') !== '0')
                writeFile(led.brightnessPath, '0\n');
        }

        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
        if (this._sliderChangedId) {
            this.slider.disconnect(this._sliderChangedId);
            this._sliderChangedId = 0;
        }
        if (this._menuSliderChangedId) {
            this._menuSlider.disconnect(this._menuSliderChangedId);
            this._menuSliderChangedId = 0;
        }

        super.destroy();
    }
});

const TorchIndicator = GObject.registerClass(
class TorchIndicator extends SystemIndicator {
    _init() {
        super._init();

        // No torch LEDs -> no indicator at all, rather than a dead slider on a
        // machine where leds-qcom-flash did not probe.
        if (findTorchLeds().length === 0)
            return;

        this.quickSettingsItems.push(new TorchSlider());
    }
});

export default class ErhaiTorchExtension extends Extension {
    enable() {
        this._indicator = new TorchIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator?.quickSettingsItems.forEach(item => item.destroy());
        this._indicator?.destroy();
        this._indicator = null;
    }
}