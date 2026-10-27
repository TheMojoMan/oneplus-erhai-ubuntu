// Shared by extension.js and prefs.js, so no gi:// imports here.
//
// `rect` is a fraction of the monitor's work area: [x, y, width, height].
// Sections marked `portrait` are transposed on monitors taller than they are
// wide, so thirds become rows instead of columns (like Magnet does).

export const SECTIONS = [
    {
        title: 'Halves',
        actions: [
            {key: 'left-half', name: 'Left', rect: [0, 0, 1 / 2, 1]},
            {key: 'right-half', name: 'Right', rect: [1 / 2, 0, 1 / 2, 1]},
            {key: 'top-half', name: 'Top', rect: [0, 0, 1, 1 / 2]},
            {key: 'bottom-half', name: 'Bottom', rect: [0, 1 / 2, 1, 1 / 2]},
        ],
    },
    {
        title: 'Quarters',
        actions: [
            {key: 'top-left', name: 'Top Left', rect: [0, 0, 1 / 2, 1 / 2]},
            {key: 'top-right', name: 'Top Right', rect: [1 / 2, 0, 1 / 2, 1 / 2]},
            {key: 'bottom-left', name: 'Bottom Left', rect: [0, 1 / 2, 1 / 2, 1 / 2]},
            {key: 'bottom-right', name: 'Bottom Right', rect: [1 / 2, 1 / 2, 1 / 2, 1 / 2]},
        ],
    },
    {
        title: 'Thirds',
        portrait: true,
        actions: [
            {key: 'left-third', name: 'Left Third', rect: [0, 0, 1 / 3, 1]},
            {key: 'center-third', name: 'Center Third', rect: [1 / 3, 0, 1 / 3, 1]},
            {key: 'right-third', name: 'Right Third', rect: [2 / 3, 0, 1 / 3, 1]},
            {key: 'left-two-thirds', name: 'Left Two Thirds', rect: [0, 0, 2 / 3, 1]},
            {key: 'center-two-thirds', name: 'Center Two Thirds', rect: [1 / 6, 0, 2 / 3, 1]},
            {key: 'right-two-thirds', name: 'Right Two Thirds', rect: [1 / 3, 0, 2 / 3, 1]},
        ],
    },
    {
        title: 'Fourths',
        portrait: true,
        actions: [
            {key: 'first-fourth', name: 'First Fourth', rect: [0, 0, 1 / 4, 1]},
            {key: 'second-fourth', name: 'Second Fourth', rect: [1 / 4, 0, 1 / 4, 1]},
            {key: 'third-fourth', name: 'Third Fourth', rect: [2 / 4, 0, 1 / 4, 1]},
            {key: 'fourth-fourth', name: 'Fourth Fourth', rect: [3 / 4, 0, 1 / 4, 1]},
            {key: 'left-three-fourths', name: 'Left Three Fourths', rect: [0, 0, 3 / 4, 1]},
            {key: 'right-three-fourths', name: 'Right Three Fourths', rect: [1 / 4, 0, 3 / 4, 1]},
        ],
    },
    {
        title: 'Sixths',
        actions: [
            {key: 'top-left-sixth', name: 'Top Left Sixth', rect: [0, 0, 1 / 3, 1 / 2]},
            {key: 'top-center-sixth', name: 'Top Center Sixth', rect: [1 / 3, 0, 1 / 3, 1 / 2]},
            {key: 'top-right-sixth', name: 'Top Right Sixth', rect: [2 / 3, 0, 1 / 3, 1 / 2]},
            {key: 'bottom-left-sixth', name: 'Bottom Left Sixth', rect: [0, 1 / 2, 1 / 3, 1 / 2]},
            {key: 'bottom-center-sixth', name: 'Bottom Center Sixth', rect: [1 / 3, 1 / 2, 1 / 3, 1 / 2]},
            {key: 'bottom-right-sixth', name: 'Bottom Right Sixth', rect: [2 / 3, 1 / 2, 1 / 3, 1 / 2]},
        ],
    },
    {
        title: 'Other',
        actions: [
            {key: 'maximize-window', name: 'Maximize'},
            {key: 'center', name: 'Center', description: 'Keeps the window size'},
            {key: 'restore', name: 'Restore', description: 'Back to the size before snapping'},
        ],
    },
    {
        title: 'Displays',
        actions: [
            {key: 'next-display', name: 'Next Display'},
            {key: 'previous-display', name: 'Previous Display'},
        ],
    },
];

for (const section of SECTIONS) {
    for (const action of section.actions)
        action.portrait = !!section.portrait;
}

export const ACTIONS = SECTIONS.flatMap(s => s.actions);

export const ACTION_BY_KEY = Object.fromEntries(ACTIONS.map(a => [a.key, a]));

// Canonical form of an accelerator so "<Primary><Alt>t" and "<Control><Alt>T"
// compare equal.
export function normalizeAccel(accel) {
    const mods = new Set();
    let rest = accel;
    let match;
    while ((match = rest.match(/^<([^>]+)>/))) {
        let mod = match[1].toLowerCase();
        if (mod === 'primary' || mod === 'ctrl')
            mod = 'control';
        else if (mod === 'mod1')
            mod = 'alt';
        else if (mod === 'mod4')
            mod = 'super';
        mods.add(mod);
        rest = rest.slice(match[0].length);
    }
    return [...[...mods].sort(), rest.toLowerCase()].join('+');
}

const KEY_LABELS = {
    Left: '←', Right: '→', Up: '↑', Down: '↓',
    Return: 'Enter', BackSpace: 'Backspace', period: '.',
};
const MOD_LABELS = {control: 'Ctrl', primary: 'Ctrl', ctrl: 'Ctrl', alt: 'Alt', super: 'Super', shift: 'Shift'};

// "<Control><Alt>Left" -> "Ctrl+Alt+←"
export function formatAccel(accel) {
    if (!accel)
        return '';
    const parts = [];
    let rest = accel;
    let match;
    while ((match = rest.match(/^<([^>]+)>/))) {
        parts.push(MOD_LABELS[match[1].toLowerCase()] ?? match[1]);
        rest = rest.slice(match[0].length);
    }
    parts.push(KEY_LABELS[rest] ?? rest.toUpperCase());
    return parts.join('+');
}
