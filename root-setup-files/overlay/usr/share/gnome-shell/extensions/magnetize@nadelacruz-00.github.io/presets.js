// Shared by extension.js and prefs.js, so no gi:// imports here.
//
// Magnet's letter shortcuts sit on fixed physical keys: U I J K form a 2×2
// grid for quarters, D F G a row for thirds, E R T the row above for two
// thirds. GNOME binds shortcuts by the letter a key types, so for each layout
// we list the letters found on those same keys (QWERTY positions below).

const PHYSICAL_SLOTS = {
    'top-left': 0, // U
    'top-right': 1, // I
    'bottom-left': 2, // J
    'bottom-right': 3, // K
    'left-third': 4, // D
    'center-third': 5, // F
    'right-third': 6, // G
    'left-two-thirds': 7, // E
    'center-two-thirds': 8, // R
    'right-two-thirds': 9, // T
    'center': 10, // C
};

export const LAYOUTS = {
    'qwerty': {name: 'QWERTY, AZERTY, QWERTZ', short: 'QWERTY', keys: [...'uijkdfgertc']},
    'colemak': {name: 'Colemak', short: 'Colemak', keys: [...'lunestdfpgc']},
    'colemak-dh': {name: 'Colemak-DH', short: 'Colemak-DH', keys: [...'lunestgfpbc']},
    'colemak-dh-iso': {name: 'Colemak-DH (ISO)', short: 'Colemak-DH ISO', keys: [...'lunestgfpbd']},
    'dvorak': {name: 'Dvorak', short: 'Dvorak', keys: ['g', 'c', 'h', 't', 'e', 'u', 'i', 'period', 'p', 'y', 'j']},
    'workman': {name: 'Workman', short: 'Workman', keys: [...'funehtgrwbm']},
};

export const MODIFIERS = {
    'ctrl-alt': {name: 'Ctrl+Alt', subtitle: 'Like Magnet’s Ctrl+Option', mods: '<Control><Alt>', display: '<Super>'},
    'super-alt': {name: 'Super+Alt', mods: '<Super><Alt>', display: '<Shift>'},
    'ctrl-super': {name: 'Ctrl+Super', mods: '<Control><Super>', display: '<Alt>'},
};

// Shortcuts that don't depend on the layout. `display` is the extra modifier
// used for moving between displays.
const FIXED_KEYS = {
    'left-half': 'Left',
    'right-half': 'Right',
    'top-half': 'Up',
    'bottom-half': 'Down',
    'maximize-window': 'Return',
    'restore': 'BackSpace',
};

// Picks a layout from org.gnome.desktop.input-sources `sources`, a list of
// [type, id] pairs such as ['xkb', 'us+colemak'].
export function detectLayout(sources) {
    const id = sources.find(([type]) => type === 'xkb')?.[1] ?? 'us';
    const variant = id.split('+')[1] ?? '';
    if (variant.startsWith('colemak_dh'))
        return variant.includes('iso') ? 'colemak-dh-iso' : 'colemak-dh';
    if (variant.startsWith('colemak'))
        return 'colemak';
    if (variant.includes('dvorak') || variant === 'dvp')
        return 'dvorak';
    if (variant.startsWith('workman'))
        return 'workman';
    return 'qwerty';
}

export function presetBindings(layout, modifier) {
    const {keys} = LAYOUTS[layout] ?? LAYOUTS.qwerty;
    const {mods, display} = MODIFIERS[modifier] ?? MODIFIERS['ctrl-alt'];
    const bindings = {
        'next-display': [`${mods}${display}Right`],
        'previous-display': [`${mods}${display}Left`],
    };
    for (const [action, key] of Object.entries(FIXED_KEYS))
        bindings[action] = [`${mods}${key}`];
    for (const [action, slot] of Object.entries(PHYSICAL_SLOTS))
        bindings[action] = [`${mods}${keys[slot]}`];
    return bindings;
}

// Writes the preset's shortcuts when the chosen (or detected) layout or the
// modifier has changed since they were last written, or when forced. Your own
// edits to single shortcuts are kept until then.
export function applyPreset(settings, sources, {force = false} = {}) {
    const choice = settings.get_string('keyboard-layout');
    const layout = choice === 'auto' ? detectLayout(sources) : choice;
    const id = `${layout}:${settings.get_string('modifiers')}`;
    if (!force && settings.get_string('applied-preset') === id)
        return false;

    const bindings = presetBindings(layout, settings.get_string('modifiers'));
    for (const [action, accels] of Object.entries(bindings))
        settings.set_strv(action, accels);
    settings.set_string('applied-preset', id);
    return true;
}
