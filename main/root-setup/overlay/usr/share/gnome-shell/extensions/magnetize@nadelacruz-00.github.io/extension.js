import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {ACTIONS, ACTION_BY_KEY, normalizeAccel} from './actions.js';
import {MagnetizeIndicator} from './indicator.js';
import {applyPreset} from './presets.js';

const ACTION_KEYS = new Set(ACTIONS.map(a => a.key));

// Schemas whose shortcuts we take over when they clash with ours, e.g.
// Ctrl+Alt+Arrows (workspaces), Ctrl+Alt+T (terminal), Ctrl+Alt+D (desktop).
const CONFLICT_SCHEMAS = [
    'org.gnome.desktop.wm.keybindings',
    'org.gnome.mutter.keybindings',
    'org.gnome.mutter.wayland.keybindings',
    'org.gnome.shell.keybindings',
    'org.gnome.settings-daemon.plugins.media-keys',
];

const MOVE_OPS = [Meta.GrabOp.MOVING, Meta.GrabOp.MOVING_UNCONSTRAINED];
const SNAPPABLE_TYPES = [Meta.WindowType.NORMAL, Meta.WindowType.DIALOG];

const EDGE = 10; // px from a screen edge that counts as touching it
const TOUCH_EDGE = 40; // the same for fingers and pens, which can't reach the very edge
const CORNER = 0.15; // share of an edge, from each end, that counts as a corner
const UNSNAP_DISTANCE = 8; // px of drag before a snapped window gets its size back
const TOUCH_UNSNAP_DISTANCE = 16; // fingers wobble more than a mouse
// Sprites (one per mouse, pen and finger) arrived in GNOME 49; older
// versions fall back to the mouse pointer.
const SpriteRole = Clutter.SpriteRole;
const SPRITE_RANK = SpriteRole
    ? {[SpriteRole.TOUCHPOINT]: 2, [SpriteRole.TABLET]: 1, [SpriteRole.POINTER]: 0}
    : {};
const BOTTOM_THIRDS = ['left-third', 'center-third', 'right-third'];
// Top edge setting -> [what you get at once, what holding still there gives].
const TOP_EDGE = {
    'maximize-hold-top-half': ['maximize-window', 'top-half'],
    'top-half-hold-maximize': ['top-half', 'maximize-window'],
    'maximize': ['maximize-window', null],
};

// GNOME 49 replaced get_maximized() and dropped the flags argument from
// maximize()/unmaximize(); these keep GNOME 47 and 48 working too.
const MAXIMIZE_BOTH = Meta.MaximizeFlags.BOTH;

function maximizeFlags(win) {
    return win.get_maximize_flags ? win.get_maximize_flags() : win.get_maximized();
}

function isMaximized(win) {
    return win.is_maximized ? win.is_maximized() : win.get_maximized() === MAXIMIZE_BOTH;
}

function maximize(win) {
    if (win.maximize.length > 0)
        win.maximize(MAXIMIZE_BOTH);
    else
        win.maximize();
}

function unmaximize(win) {
    if (win.unmaximize.length > 0)
        win.unmaximize(MAXIMIZE_BOTH);
    else
        win.unmaximize();
}

export default class MagnetizeExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._inputSettings = new Gio.Settings({schema_id: 'org.gnome.desktop.input-sources'});
        this._states = new WeakMap(); // Meta.Window -> {original, key}
        this._drag = null;
        this._sources = new Set();

        this._applyPreset();
        this._refreshConflicts();
        this._syncEdgeTiling();
        this._syncIndicator();

        for (const action of ACTIONS) {
            Main.wm.addKeybinding(action.key, this._settings,
                Meta.KeyBindingFlags.NONE, Shell.ActionMode.NORMAL,
                () => this.runAction(action.key));
        }

        this._settingsId = this._settings.connect('changed', (_s, key) => {
            if (this._applyingPreset || key === 'stash' || key === 'applied-preset')
                return;
            if (key === 'drag-snapping')
                this._syncEdgeTiling();
            else if (key === 'show-indicator')
                this._syncIndicator();
            else if (key === 'keyboard-layout' || key === 'modifiers')
                this._applyPreset();
            else if (key === 'remove-conflicts' || ACTION_KEYS.has(key))
                this._refreshConflicts();
        });
        // Follow the keyboard layout when it's set to auto-detect.
        this._inputId = this._inputSettings.connect('changed::sources', () => this._applyPreset());

        this._displayIds = [
            global.display.connect('grab-op-begin', (_d, win, op) => this._onGrabBegin(win, op)),
            global.display.connect('grab-op-end', (_d, win) => this._onGrabEnd(win)),
        ];
    }

    disable() {
        for (const action of ACTIONS)
            Main.wm.removeKeybinding(action.key);

        this._displayIds.forEach(id => global.display.disconnect(id));
        this._settings.disconnect(this._settingsId);
        this._inputSettings.disconnect(this._inputId);
        this._indicator?.destroy();
        this._indicator = null;
        this._sources.forEach(id => GLib.source_remove(id));
        this._sources.clear();
        this._endDrag();
        this._preview?.destroy();
        this._preview = null;

        // Extensions are disabled while the screen is locked and re-enabled on
        // unlock; only hand GNOME its settings back when really turned off.
        if (!Main.sessionMode.isLocked) {
            this._restoreConflicts();
            this._restoreEdgeTiling();
        }

        this._settings = null;
        this._inputSettings = null;
        this._states = null;
    }

    _syncIndicator() {
        const show = this._settings.get_boolean('show-indicator');
        if (show && !this._indicator) {
            this._indicator = new MagnetizeIndicator(this);
            Main.panel.addToStatusArea(this.uuid, this._indicator);
        } else if (!show && this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }

    _applyPreset() {
        const sources = this._inputSettings.get_value('sources').deep_unpack();
        this._applyingPreset = true;
        let changed;
        try {
            changed = applyPreset(this._settings, sources);
        } finally {
            this._applyingPreset = false;
        }
        if (changed)
            this._refreshConflicts();
    }

    // Shortcuts

    // Also used by the top bar menu.
    runAction(key, win = global.display.get_focus_window()) {
        if (!this._isSnappable(win))
            return;

        switch (key) {
        case 'center':
            if (!win.is_fullscreen())
                this._center(win);
            break;
        case 'restore':
            this._restore(win);
            break;
        case 'next-display':
            this._moveToDisplay(win, 1);
            break;
        case 'previous-display':
            this._moveToDisplay(win, -1);
            break;
        default:
            this._snap(win, key, win.get_monitor());
        }
    }

    _isSnappable(win) {
        return !!win &&
            SNAPPABLE_TYPES.includes(win.get_window_type()) &&
            // Mutter reports fullscreen windows as unmovable; we leave
            // fullscreen before moving them.
            (win.allows_move() || win.is_fullscreen());
    }

    // Mutter reports fullscreen and maximized windows as not resizable; they
    // are again once they leave that state, which _moveResize takes care of.
    _canResize(win) {
        return win.allows_resize() || win.is_fullscreen() || maximizeFlags(win) !== 0;
    }

    _snap(win, key, monitorIndex) {
        this._remember(win).key = key;

        // With gaps, maximizing fills the screen minus the gap instead.
        if (key === 'maximize-window' && win.can_maximize() && this._settings.get_int('window-gap') === 0) {
            if (win.is_fullscreen())
                win.unmake_fullscreen();
            if (win.get_monitor() !== monitorIndex)
                win.move_to_monitor(monitorIndex);
            maximize(win);
        } else if (this._canResize(win)) {
            this._moveResize(win, this._targetRect(key, monitorIndex));
        }
    }

    _center(win) {
        if (isMaximized(win))
            return;
        const area = win.get_work_area_current_monitor();
        const frame = win.get_frame_rect();
        win.move_frame(true,
            area.x + Math.round((area.width - frame.width) / 2),
            area.y + Math.round((area.height - frame.height) / 2));
    }

    _restore(win) {
        const state = this._states.get(win);
        if (!state?.original)
            return;
        this._states.delete(win);
        if (state.original.fullscreen)
            win.make_fullscreen();
        else if (state.original.maximized)
            maximize(win);
        else
            this._moveResize(win, state.original);
    }

    _moveToDisplay(win, step) {
        const display = global.display;
        const count = display.get_n_monitors();
        if (count < 2)
            return;

        const current = win.get_monitor();
        const direction = step > 0 ? Meta.DisplayDirection.RIGHT : Meta.DisplayDirection.LEFT;
        let target = display.get_monitor_neighbor_index(current, direction);
        if (target < 0)
            target = (current + step + count) % count;

        // Keep a snapped window in the same layout on the other display.
        const state = this._states.get(win);
        if (state && (state.key !== 'maximize-window' || isMaximized(win) || this._settings.get_int('window-gap') > 0))
            this._snap(win, state.key, target);
        else
            win.move_to_monitor(target);
    }

    // Geometry

    _targetRect(key, monitorIndex) {
        const area = Main.layoutManager.getWorkAreaForMonitor(monitorIndex);
        const action = ACTION_BY_KEY[key];
        let [fx, fy, fw, fh] = action.rect ?? [0, 0, 1, 1];
        if (action.portrait && area.height > area.width)
            [fx, fy, fw, fh] = [fy, fx, fh, fw];

        // Round edges rather than sizes so neighbouring windows meet exactly.
        const x1 = area.x + Math.round(fx * area.width);
        const y1 = area.y + Math.round(fy * area.height);
        const x2 = area.x + Math.round((fx + fw) * area.width);
        const y2 = area.y + Math.round((fy + fh) * area.height);

        // Full gap at the screen's edges, half a gap on each side of an edge
        // shared with a neighbouring window, so all gaps come out equal.
        const gap = this._settings.get_int('window-gap');
        const left = x1 === area.x ? gap : Math.floor(gap / 2);
        const right = x2 === area.x + area.width ? gap : Math.ceil(gap / 2);
        const top = y1 === area.y ? gap : Math.floor(gap / 2);
        const bottom = y2 === area.y + area.height ? gap : Math.ceil(gap / 2);
        return {x: x1 + left, y: y1 + top, width: x2 - x1 - left - right, height: y2 - y1 - top - bottom};
    }

    _moveResize(win, rect) {
        // Leave fullscreen first: a fullscreen window can still be maximized
        // underneath, and that has to be undone as well.
        const wasFullscreen = win.is_fullscreen();
        if (wasFullscreen)
            win.unmake_fullscreen();
        const wasMaximized = maximizeFlags(win) !== 0;
        if (wasMaximized)
            unmaximize(win);
        win.move_resize_frame(true, rect.x, rect.y, rect.width, rect.height);

        // Leaving fullscreen or maximized puts the old size back once the
        // app has answered, which can land after our resize; apply it again.
        if (wasFullscreen || wasMaximized) {
            const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, wasFullscreen ? 150 : 0, () => {
                this._sources.delete(id);
                if (win.get_compositor_private())
                    win.move_resize_frame(true, rect.x, rect.y, rect.width, rect.height);
                return GLib.SOURCE_REMOVE;
            });
            this._sources.add(id);
        }
    }

    // Keep the pre-snap geometry across repeated snaps. It is dropped on
    // restore, and when the user resizes or drags the window themselves.
    _remember(win) {
        let state = this._states.get(win);
        if (!state) {
            const frame = win.get_frame_rect();
            let original = {x: frame.x, y: frame.y, width: frame.width, height: frame.height};
            if (win.is_fullscreen())
                original = {fullscreen: true};
            else if (isMaximized(win))
                original = {maximized: true};
            state = {original};
            this._states.set(win, state);
        }
        return state;
    }

    // Drag snapping

    _onGrabBegin(win, op) {
        // Resized by hand: that size is the one to keep now.
        if (win && !MOVE_OPS.includes(op) && op !== Meta.GrabOp.KEYBOARD_MOVING)
            this._states.delete(win);

        if (!this._settings.get_boolean('drag-snapping') ||
            !MOVE_OPS.includes(op) || !this._isSnappable(win) || !this._canResize(win))
            return;

        this._endDrag();
        const sprite = this._findDragSprite(win);
        const drag = {win, sprite, moved: false, zone: null, bottomAnchor: null, topSince: null};
        drag.topEdge = TOP_EDGE[this._settings.get_string('top-edge')] ?? TOP_EDGE.maximize;
        drag.holdTime = this._settings.get_int('top-edge-hold-time') * 1000; // to µs
        drag.touch = !!sprite && sprite.role !== SpriteRole.POINTER;
        [drag.startX, drag.startY] = this._dragPosition(drag);
        this._drag = drag;
        this._dragTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 16, () => {
            this._onDragTick();
            return GLib.SOURCE_CONTINUE;
        });
    }

    _onGrabEnd(win) {
        if (this._drag?.win !== win)
            return;
        const zone = this._drag.zone;
        this._endDrag();
        if (zone)
            this._snap(win, zone.key, zone.monitorIndex);
    }

    _endDrag() {
        if (this._dragTimer) {
            GLib.source_remove(this._dragTimer);
            this._dragTimer = 0;
        }
        this._drag = null;
        this._hidePreview();
    }

    // On Wayland a finger or pen has its own sprite and doesn't move the mouse
    // pointer, so work out which one is dragging. A finger on the screen wins
    // (where the drag started can be just outside the frame, under the top
    // bar); a pen only counts near the window, since it often hovers nearby.
    _findDragSprite(win) {
        if (!global.stage.foreach_sprite)
            return null;
        const frame = win.get_frame_rect();
        const distance = ({x, y}) => Math.hypot(
            Math.max(frame.x - x, 0, x - (frame.x + frame.width)),
            Math.max(frame.y - y, 0, y - (frame.y + frame.height)));

        let best = null;
        let bestDistance = Infinity;
        global.stage.foreach_sprite((_stage, sprite) => {
            const d = distance(sprite.get_coords());
            const usable = sprite.role === SpriteRole.TOUCHPOINT || d <= TOUCH_EDGE;
            const better = !best || SPRITE_RANK[sprite.role] > SPRITE_RANK[best.role] ||
                (sprite.role === best.role && d < bestDistance);
            if (usable && better) {
                best = sprite;
                bestDistance = d;
            }
            return true;
        });
        return best;
    }

    _dragPosition(drag) {
        try {
            if (drag.sprite) {
                const {x, y} = drag.sprite.get_coords();
                drag.last = [x, y];
                return drag.last;
            }
        } catch {
            // The finger lifted and its sprite is gone; keep the last spot.
            return drag.last;
        }
        return global.get_pointer().slice(0, 2);
    }

    _onDragTick() {
        const drag = this._drag;
        const [x, y] = this._dragPosition(drag);

        // A click on the title bar also starts a grab; wait for a real drag.
        if (!drag.moved) {
            const distance = drag.touch ? TOUCH_UNSNAP_DISTANCE : UNSNAP_DISTANCE;
            if (Math.hypot(x - drag.startX, y - drag.startY) < distance)
                return;
            drag.moved = true;
            this._unsnapForDrag(drag.win, x);
        }

        const zone = this._zoneAt(x, y, drag);
        if (zone?.key === drag.zone?.key && zone?.monitorIndex === drag.zone?.monitorIndex)
            return;
        drag.zone = zone;
        if (zone)
            this._showPreview(drag.win, this._targetRect(zone.key, zone.monitorIndex), x, y);
        else
            this._hidePreview();
    }

    // Magnet gives a snapped window its old size back when you drag it out.
    _unsnapForDrag(win, pointerX) {
        const state = this._states.get(win);
        if (!state)
            return;
        this._states.delete(win);
        // Mutter unmaximizes dragged maximized windows on its own.
        if (state.original.width === undefined || maximizeFlags(win) !== 0)
            return;

        const frame = win.get_frame_rect();
        const {width, height} = state.original;
        const ratio = (pointerX - frame.x) / frame.width;
        win.move_resize_frame(true, Math.round(pointerX - ratio * width), frame.y, width, height);
    }

    _zoneAt(x, y, drag) {
        const display = global.display;
        // The monitor under the drag, not under the mouse: they differ for touch.
        let monitorIndex = Main.layoutManager.monitors.findIndex(mon =>
            x >= mon.x && x < mon.x + mon.width && y >= mon.y && y < mon.y + mon.height);
        if (monitorIndex < 0)
            monitorIndex = display.get_current_monitor();
        const m = display.get_monitor_geometry(monitorIndex);
        // Edges shared with another monitor are for crossing, not snapping.
        const outer = dir => display.get_monitor_neighbor_index(monitorIndex, dir) < 0;

        const edge = drag.touch ? TOUCH_EDGE : EDGE;
        const left = x <= m.x + edge && outer(Meta.DisplayDirection.LEFT);
        const right = x >= m.x + m.width - 1 - edge && outer(Meta.DisplayDirection.RIGHT);
        const top = y <= m.y + edge && outer(Meta.DisplayDirection.UP);
        const bottom = y >= m.y + m.height - 1 - edge && outer(Meta.DisplayDirection.DOWN);

        const nearLeft = x < m.x + m.width * CORNER;
        const nearRight = x > m.x + m.width * (1 - CORNER);
        const nearTop = y < m.y + m.height * CORNER;
        const nearBottom = y > m.y + m.height * (1 - CORNER);

        let key = null;
        if ((top && nearLeft) || (left && nearTop))
            key = 'top-left';
        else if ((top && nearRight) || (right && nearTop))
            key = 'top-right';
        else if ((bottom && nearLeft) || (left && nearBottom))
            key = 'bottom-left';
        else if ((bottom && nearRight) || (right && nearBottom))
            key = 'bottom-right';
        else if (top)
            key = this._topZone(drag);
        else if (left)
            key = 'left-half';
        else if (right)
            key = 'right-half';
        else if (bottom)
            key = this._bottomZone(x, m, drag);

        if (!BOTTOM_THIRDS.includes(key) && !key?.endsWith('two-thirds'))
            drag.bottomAnchor = null;
        if (!top || key !== this._topZone(drag, false))
            drag.topSince = null;
        return key ? {key, monitorIndex} : null;
    }

    // Top edge: one layout straight away, the other after holding there for
    // a while (like Ubuntu's Tiling Assistant). Moving along the edge keeps
    // the hold going; leaving it starts over.
    _topZone(drag, track = true) {
        const [first, held] = drag.topEdge;
        const now = GLib.get_monotonic_time();
        if (track)
            drag.topSince ??= now;
        const holding = drag.topSince !== null && now - drag.topSince >= drag.holdTime;
        return held && holding ? held : first;
    }

    // Bottom edge: thirds, and sliding into a neighbouring third makes it two
    // thirds.
    _bottomZone(x, m, drag) {
        const third = Math.min(2, Math.floor((x - m.x) / (m.width / 3)));
        drag.bottomAnchor ??= third;
        const [a, b] = [drag.bottomAnchor, third].sort();
        if (a === b)
            return BOTTOM_THIRDS[third];
        if (a === 0 && b === 1)
            return 'left-two-thirds';
        if (a === 1 && b === 2)
            return 'right-two-thirds';
        drag.bottomAnchor = third;
        return BOTTOM_THIRDS[third];
    }

    _showPreview(win, rect, pointerX, pointerY) {
        if (!this._preview) {
            this._preview = new St.Widget({style_class: 'tile-preview magnetize-preview', visible: false});
            global.window_group.add_child(this._preview);
        }
        const preview = this._preview;
        if (!preview.visible || preview.opacity === 0)
            preview.set({x: pointerX, y: pointerY, width: 0, height: 0, opacity: 0, visible: true});

        const actor = win.get_compositor_private();
        if (actor?.get_parent() === global.window_group)
            global.window_group.set_child_below_sibling(preview, actor);

        preview.ease({
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: rect.height,
            opacity: 255,
            duration: 150,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }

    _hidePreview() {
        if (!this._preview?.visible)
            return;
        this._preview.ease({
            opacity: 0,
            duration: 120,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => this._preview?.hide(),
        });
    }

    // GNOME settings we change while enabled. What we change is recorded in
    // the `stash` key so it can be put back even after a crash or logout.

    _readStash() {
        try {
            return {conflicts: [], edgeTiling: false, ...JSON.parse(this._settings.get_string('stash'))};
        } catch {
            return {conflicts: [], edgeTiling: false};
        }
    }

    _writeStash(stash) {
        this._settings.set_string('stash', JSON.stringify(stash));
    }

    _otherSettings(schemaId) {
        const schema = Gio.SettingsSchemaSource.get_default().lookup(schemaId, true);
        return schema ? [new Gio.Settings({settings_schema: schema}), schema] : [null, null];
    }

    // Put back what we took, then take what clashes with the current shortcuts.
    _refreshConflicts() {
        this._restoreConflicts();
        this._removeConflicts();
    }

    _removeConflicts() {
        if (!this._settings.get_boolean('remove-conflicts'))
            return;

        const ours = new Set(ACTIONS
            .flatMap(a => this._settings.get_strv(a.key))
            .filter(Boolean)
            .map(normalizeAccel));
        const stash = this._readStash();

        for (const schemaId of CONFLICT_SCHEMAS) {
            const [settings, schema] = this._otherSettings(schemaId);
            if (!settings)
                continue;
            for (const key of schema.list_keys()) {
                if (schema.get_key(key).get_value_type().dup_string() !== 'as')
                    continue;
                const accels = settings.get_strv(key);
                const clashing = accels.filter(a => ours.has(normalizeAccel(a)));
                if (clashing.length === 0)
                    continue;
                clashing.forEach(accel => stash.conflicts.push({schema: schemaId, key, accel}));
                settings.set_strv(key, accels.filter(a => !clashing.includes(a)));
            }
        }
        this._writeStash(stash);
    }

    _restoreConflicts() {
        const stash = this._readStash();
        for (const {schema: schemaId, key, accel} of stash.conflicts) {
            const [settings, schema] = this._otherSettings(schemaId);
            if (!settings || !schema.has_key(key))
                continue;
            const accels = settings.get_strv(key);
            if (!accels.includes(accel))
                settings.set_strv(key, [...accels, accel]);
        }
        stash.conflicts = [];
        this._writeStash(stash);
    }

    // GNOME's own edge tiling would fight with drag snapping.
    _syncEdgeTiling() {
        const mutter = new Gio.Settings({schema_id: 'org.gnome.mutter'});
        const stash = this._readStash();
        if (this._settings.get_boolean('drag-snapping')) {
            if (mutter.get_boolean('edge-tiling')) {
                mutter.set_boolean('edge-tiling', false);
                stash.edgeTiling = true;
            }
        } else if (stash.edgeTiling) {
            mutter.set_boolean('edge-tiling', true);
            stash.edgeTiling = false;
        }
        this._writeStash(stash);
    }

    _restoreEdgeTiling() {
        const stash = this._readStash();
        if (stash.edgeTiling)
            new Gio.Settings({schema_id: 'org.gnome.mutter'}).set_boolean('edge-tiling', true);
        stash.edgeTiling = false;
        this._writeStash(stash);
    }
}
