import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {ACTION_BY_KEY, formatAccel} from './actions.js';

const GRID = [
    ['left-half', 'right-half', 'top-half', 'bottom-half'],
    ['top-left', 'top-right', 'bottom-left', 'bottom-right'],
    ['left-third', 'center-third', 'right-third', 'maximize-window'],
    ['left-two-thirds', 'center-two-thirds', 'right-two-thirds', 'center'],
];

// What the thumbnails draw for actions that aren't a fixed area.
const THUMB_RECTS = {
    'maximize-window': [0, 0, 1, 1],
    'center': [0.2, 0.2, 0.6, 0.6],
};

const THUMB_WIDTH = 40;
const THUMB_HEIGHT = 26;
const THUMB_INSET = 2;

export const MagnetizeIndicator = GObject.registerClass(
class MagnetizeIndicator extends PanelMenu.Button {
    _init(extension) {
        super._init(0.5, 'Magnetize');
        this._extension = extension;
        this._settings = extension.getSettings();

        this.add_child(new St.Icon({
            gicon: Gio.icon_new_for_string(`${extension.path}/icons/magnetize-symbolic.svg`),
            style_class: 'system-status-icon',
        }));

        // The menu takes keyboard focus while open, so note the window to act on
        // before that happens.
        this.menu.connect('open-state-changed', (_menu, open) => {
            if (open)
                this._target = global.display.get_focus_window();
            else
                this._hint.text = '';
        });

        this._buildGrid();

        this._hint = new St.Label({style_class: 'magnetize-hint', x_align: Clutter.ActorAlign.CENTER, x_expand: true});
        const hintItem = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        hintItem.add_child(this._hint);
        this.menu.addMenuItem(hintItem);

        this.menu.addAction('Restore Window', () => this._apply('restore'));
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        const drag = new PopupMenu.PopupSwitchMenuItem('Snap by Dragging', this._settings.get_boolean('drag-snapping'));
        drag.connect('toggled', (_item, state) => this._settings.set_boolean('drag-snapping', state));
        this._dragId = this._settings.connect('changed::drag-snapping',
            () => drag.setToggleState(this._settings.get_boolean('drag-snapping')));
        this.menu.addMenuItem(drag);

        this.menu.addAction('Settings', () => extension.openPreferences());
    }

    _buildGrid() {
        const layout = new Clutter.GridLayout({column_spacing: 6, row_spacing: 6});
        const grid = new St.Widget({layout_manager: layout, style_class: 'magnetize-grid', x_expand: true, x_align: Clutter.ActorAlign.CENTER});
        GRID.forEach((row, r) => row.forEach((key, c) => layout.attach(this._thumbButton(key), c, r, 1, 1)));

        const item = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false, style_class: 'magnetize-grid-item'});
        item.add_child(grid);
        this.menu.addMenuItem(item);
    }

    _thumbButton(key) {
        const scale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
        const [fx, fy, fw, fh] = ACTION_BY_KEY[key].rect ?? THUMB_RECTS[key];
        const zone = new St.Widget({style_class: 'magnetize-thumb-zone'});
        zone.set_position(Math.round((fx * THUMB_WIDTH + THUMB_INSET) * scale), Math.round((fy * THUMB_HEIGHT + THUMB_INSET) * scale));
        zone.set_size(Math.round((fw * THUMB_WIDTH - 2 * THUMB_INSET) * scale), Math.round((fh * THUMB_HEIGHT - 2 * THUMB_INSET) * scale));

        const screen = new St.Widget({style_class: 'magnetize-thumb'});
        screen.add_child(zone);

        const button = new St.Button({child: screen, style_class: 'magnetize-thumb-button', can_focus: true, accessible_name: ACTION_BY_KEY[key].name});
        button.connect('clicked', () => this._apply(key));
        const showHint = () => {
            const accel = formatAccel(this._settings.get_strv(key)[0]);
            this._hint.text = accel ? `${ACTION_BY_KEY[key].name}   ${accel}` : ACTION_BY_KEY[key].name;
        };
        button.connect('notify::hover', () => button.hover && showHint());
        button.connect('key-focus-in', showHint);
        return button;
    }

    _apply(key) {
        const win = this._target;
        this.menu.close();
        if (win)
            this._extension.runAction(key, win);
    }

    destroy() {
        this._settings.disconnect(this._dragId);
        super.destroy();
    }
});
