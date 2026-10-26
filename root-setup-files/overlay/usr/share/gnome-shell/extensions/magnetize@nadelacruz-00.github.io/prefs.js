import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

import {ACTIONS, SECTIONS, formatAccel} from './actions.js';
import {LAYOUTS, MODIFIERS, applyPreset, detectLayout} from './presets.js';

const ShortcutLabel = Adw.ShortcutLabel ?? Gtk.ShortcutLabel;

export default class MagnetizePreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const input = new Gio.Settings({schema_id: 'org.gnome.desktop.input-sources'});
        window._settings = settings;
        window.set_default_size(600, 760);
        window.set_search_enabled(true);

        // Rewrites the shortcuts here too, so changes work while the extension
        // is off.
        this._applyPreset = force => applyPreset(settings,
            input.get_value('sources').deep_unpack(), {force});

        window.add(this._generalPage(settings, input));
        window.add(this._shortcutsPage(window, settings));
    }

    _generalPage(settings, input) {
        const page = new Adw.PreferencesPage({name: 'general', title: 'General', icon_name: 'preferences-system-symbolic'});

        const keyboard = new Adw.PreferencesGroup({
            title: 'Keyboard',
            description: 'Shortcuts sit on the same keys as Magnet on a Mac: U I J K for quarters, D F G for thirds, E R T for two thirds. Pick your layout so they stay on those keys.',
        });
        page.add(keyboard);

        const detected = LAYOUTS[detectLayout(input.get_value('sources').deep_unpack())].short;
        keyboard.add(this._choiceRow(settings, 'keyboard-layout', 'Keyboard Layout', [
            ['auto', `Auto (${detected})`],
            ...Object.entries(LAYOUTS).map(([id, {name}]) => [id, name]),
        ]));
        keyboard.add(this._choiceRow(settings, 'modifiers', 'Modifier Keys',
            Object.entries(MODIFIERS).map(([id, {name}]) => [id, name]),
            'Changing either resets the letter and arrow shortcuts'));

        const snapping = new Adw.PreferencesGroup({title: 'Snapping'});
        page.add(snapping);

        const drag = new Adw.SwitchRow({
            title: 'Snap by Dragging',
            subtitle: 'Edges for halves, corners for quarters, top to maximize, bottom for thirds (slide along it for two thirds)',
        });
        settings.bind('drag-snapping', drag, 'active', Gio.SettingsBindFlags.DEFAULT);
        snapping.add(drag);

        snapping.add(this._choiceRow(settings, 'top-edge', 'Top Edge', [
            ['maximize-hold-top-half', 'Maximize → top half'],
            ['top-half-hold-maximize', 'Top half → maximize'],
            ['maximize', 'Maximize only'],
        ], 'Hold at the top edge to switch to the second'));

        const hold = Adw.SpinRow.new_with_range(200, 3000, 100);
        hold.set({title: 'Hold Time', subtitle: 'Milliseconds at the top edge before it switches'});
        settings.bind('top-edge-hold-time', hold, 'value', Gio.SettingsBindFlags.DEFAULT);
        const syncHold = () => {
            hold.sensitive = settings.get_boolean('drag-snapping') && settings.get_string('top-edge') !== 'maximize';
        };
        syncHold();
        const holdIds = ['changed::top-edge', 'changed::drag-snapping'].map(sig => settings.connect(sig, syncHold));
        hold.connect('destroy', () => holdIds.forEach(id => settings.disconnect(id)));
        snapping.add(hold);

        const gap = Adw.SpinRow.new_with_range(0, 64, 2);
        gap.set({title: 'Window Gap', subtitle: 'Space in pixels around snapped windows'});
        settings.bind('window-gap', gap, 'value', Gio.SettingsBindFlags.DEFAULT);
        snapping.add(gap);

        const topBar = new Adw.PreferencesGroup({title: 'Top Bar'});
        page.add(topBar);
        const indicator = new Adw.SwitchRow({title: 'Show Menu in Top Bar', subtitle: 'Click a layout to snap the focused window'});
        settings.bind('show-indicator', indicator, 'active', Gio.SettingsBindFlags.DEFAULT);
        topBar.add(indicator);

        page.add(this._conflictsGroup(settings));
        return page;
    }

    _choiceRow(settings, key, title, choices, subtitle = '') {
        const row = new Adw.ComboRow({
            title,
            subtitle,
            model: Gtk.StringList.new(choices.map(([, name]) => name)),
        });
        const sync = () => {
            row.selected = Math.max(0, choices.findIndex(([id]) => id === settings.get_string(key)));
        };
        sync();
        const id = settings.connect(`changed::${key}`, sync);
        row.connect('destroy', () => settings.disconnect(id));
        row.connect('notify::selected', () => {
            const value = choices[row.selected][0];
            if (value !== settings.get_string(key)) {
                settings.set_string(key, value);
                if (key === 'keyboard-layout' || key === 'modifiers')
                    this._applyPreset(false);
            }
        });
        return row;
    }

    _conflictsGroup(settings) {
        const group = new Adw.PreferencesGroup({title: 'GNOME Shortcuts'});

        const takeOver = new Adw.SwitchRow({
            title: 'Take Over Clashing Shortcuts',
            subtitle: 'GNOME uses some of the same keys, such as Ctrl+Alt+Arrows for workspaces. They come back when Magnetize is turned off.',
        });
        settings.bind('remove-conflicts', takeOver, 'active', Gio.SettingsBindFlags.DEFAULT);
        group.add(takeOver);

        const list = new Adw.ExpanderRow({title: 'Taken Over Right Now'});
        group.add(list);
        let rows = [];
        const sync = () => {
            rows.forEach(r => list.remove(r));
            let conflicts = [];
            try {
                conflicts = JSON.parse(settings.get_string('stash')).conflicts ?? [];
            } catch {}
            rows = conflicts.map(({key, accel}) => new Adw.ActionRow({
                title: key.replace(/-/g, ' ').replace(/^./, c => c.toUpperCase()),
                subtitle: formatAccel(accel),
            }));
            rows.forEach(r => list.add_row(r));
            list.subtitle = conflicts.length ? `${conflicts.length} shortcuts` : 'None';
            list.sensitive = conflicts.length > 0;
        };
        sync();
        const id = settings.connect('changed::stash', sync);
        group.connect('destroy', () => settings.disconnect(id));
        return group;
    }

    _shortcutsPage(window, settings) {
        const page = new Adw.PreferencesPage({name: 'shortcuts', title: 'Shortcuts', icon_name: 'input-keyboard-symbolic'});

        const top = new Adw.PreferencesGroup({description: 'Click a shortcut to change it. Fourths and sixths start without one, as in Magnet.'});
        const reset = new Adw.ButtonRow({title: 'Reset All Shortcuts'});
        reset.connect('activated', () => {
            ACTIONS.forEach(a => settings.reset(a.key));
            this._applyPreset(true);
        });
        top.add(reset);
        page.add(top);

        for (const section of SECTIONS) {
            const group = new Adw.PreferencesGroup({title: section.title});
            section.actions.forEach(action => group.add(this._shortcutRow(window, settings, action)));
            page.add(group);
        }
        return page;
    }

    _shortcutRow(window, settings, action) {
        const row = new Adw.ActionRow({
            title: action.name,
            subtitle: action.description ?? '',
            activatable: true,
        });
        const label = new ShortcutLabel({disabled_text: 'None', valign: Gtk.Align.CENTER});
        const clear = new Gtk.Button({
            icon_name: 'edit-clear-symbolic',
            tooltip_text: 'Remove Shortcut',
            valign: Gtk.Align.CENTER,
            css_classes: ['flat'],
        });
        row.add_suffix(label);
        row.add_suffix(clear);

        const sync = () => {
            const accel = settings.get_strv(action.key)[0] ?? '';
            label.accelerator = accel;
            clear.sensitive = accel !== '';
        };
        const id = settings.connect(`changed::${action.key}`, sync);
        row.connect('destroy', () => settings.disconnect(id));
        sync();

        clear.connect('clicked', () => settings.set_strv(action.key, []));
        row.connect('activated', () => this._capture(window, action,
            accel => settings.set_strv(action.key, accel ? [accel] : [])));
        return row;
    }

    _capture(window, action, done) {
        const status = new Adw.StatusPage({
            icon_name: 'input-keyboard-symbolic',
            title: action.name,
            description: 'Press the new shortcut\nEsc to cancel, Backspace to remove',
        });
        const view = new Adw.ToolbarView({content: status});
        view.add_top_bar(new Adw.HeaderBar({show_title: false}));
        const dialog = new Adw.Dialog({child: view, content_width: 380});

        const keys = new Gtk.EventControllerKey({propagation_phase: Gtk.PropagationPhase.CAPTURE});
        keys.connect('key-pressed', (_c, keyval, _keycode, state) => {
            const mods = state & Gtk.accelerator_get_default_mod_mask();
            if (mods === 0 && keyval === Gdk.KEY_Escape) {
                dialog.close();
            } else if (mods === 0 && keyval === Gdk.KEY_BackSpace) {
                done(null);
                dialog.close();
            } else if (mods !== 0 && Gtk.accelerator_valid(keyval, mods)) {
                done(Gtk.accelerator_name(Gdk.keyval_to_lower(keyval), mods));
                dialog.close();
            }
            return Gdk.EVENT_STOP;
        });
        dialog.add_controller(keys);

        // Otherwise the shell grabs the combo (it may already be bound) before
        // the dialog sees it.
        const surface = window.get_surface();
        surface?.inhibit_system_shortcuts(null);
        dialog.connect('closed', () => surface?.restore_system_shortcuts());

        dialog.present(window);
    }
}
