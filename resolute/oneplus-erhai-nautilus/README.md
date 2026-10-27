# oneplus-erhai-nautilus — touch-friendly GNOME Files (Nautilus)

Two touch-oriented additions to Ubuntu's **Nautilus 50.2.2** for the OnePlus
Pad 3 (erhai):

1. **Long-tap on the empty background opens the context menu** — the touch
   equivalent of a right click (before, only the mouse secondary button did).
2. **"Show Select Boxes" preference** (Files ▸ Preferences ▸ General) — a
   selection checkbox is shown to the left of every file/folder, so several
   items can be selected on a touchscreen. Tapping a box toggles that item;
   **swiping a finger across the checkboxes** paints them to the same state
   (select or deselect), which is the fast multi-select gesture.

Nautilus is a C/GTK4 application (unlike the shell extensions), so this is a
**source patch**, not an extension. The patch touches 9 files:
`data/org.gnome.nautilus.gschema.xml`, `src/nautilus-global-preferences.h`,
`src/nautilus-preferences-dialog.c`, `src/resources/ui/nautilus-preferences-dialog.blp`,
`src/nautilus-view-cell.{c,h}`, `src/resources/ui/nautilus-name-cell.blp`,
`src/nautilus-name-cell.c`, `src/nautilus-list-base.c`.

## Build

```
oneplus-erhai-ubuntu/oneplus-erhai-nautilus/build-nautilus-erhai.sh
```

Extracts the Ubuntu source for the installed version, applies
`nautilus-select-boxes.patch`, builds with meson (release) and stages the
patched `nautilus` binary + the updated gschema into `./build/`.

**Meson options MUST match Ubuntu's `debian/rules`** — most importantly
`-Dxdg-terminal-exec=true`: without it the built-in **"Open in Terminal"**
context-menu action is compiled out (it is *not* an extension; Ubuntu enables
it via this flag + the `files-view-Add-xdg-terminal-exec-action.patch`, and it
needs the `xdg-terminal-exec` package in PATH). The script also passes
`-Dcloudproviders=false` (Ubuntu disables it: not in main), `-Dselinux=true` and
`-Dunity-launcher=true` (needs `libunity-dev` + `libdbusmenu-glib-dev`; the
binary then links `libunity.so.9` / `libdbusmenu-glib.so.4`, as Ubuntu's does).
So **every** Ubuntu build option that affects the shipped `/usr/bin/nautilus` is
matched; only the packaging-only ones (`docs`, `introspection`, `tests`) stay at
their no-op values — they only produce artifacts we do not ship (the
`.gir`/typelib, docs, test binaries).

Build deps (Debian Build-Depends subset that is actually needed):
`blueprint-compiler meson ninja libadwaita-1-dev libgnome-autoar-0-dev
libgnome-desktop-4-dev libportal-gtk4-dev libtinysparql-dev libicu-dev
libglycin-gtk4-2-dev libgexiv2-dev libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev libcloudproviders-dev libgbm-dev`.

**libgbm-dev caveat (erhai pad):** Ubuntu's `libgbm-dev` Depends
`libgbm1 (= <ubuntu version>)`, but this pad ships the newer erhai mesa in
place of `libgbm1`, so apt refuses it. Install a locally relaxed
`libgbm-dev` (control `Depends: libgbm1 (>= <ubuntu version>)`) or knit it in
manually; the header/pc are all that is needed to build.

## Install / revert

```
sudo oneplus-erhai-ubuntu/oneplus-erhai-nautilus/install-nautilus-erhai.sh apply
sudo oneplus-erhai-ubuntu/oneplus-erhai-nautilus/install-nautilus-erhai.sh status
sudo oneplus-erhai-ubuntu/oneplus-erhai-nautilus/install-nautilus-erhai.sh revert
```

`apply` uses `dpkg-divert --local --add --no-rename` on `/usr/bin/nautilus`
and `/usr/share/glib-2.0/schemas/org.gnome.nautilus.gschema.xml`, keeps
Ubuntu's originals as `*.erhai-orig`, installs the patched files and runs
`glib-compile-schemas`. `revert` restores them exactly and drops the
diversions. Nautilus restarts on the next "Files" launch.

The feature is **off by default**; turn it on in
**Files ▸ Preferences ▸ General ▸ Show Select Boxes**.

## Status

In development on the owner's pad (built + installed locally, NOT published to
the PPA). Verified: builds clean; with the setting on, one `GtkCheckButton`
labelled "Select" per row appears in the list view, and its state tracks the
selection model (a deterministic `nautilus --select <file>` marks exactly that
row's box checked). The touch tap-toggle and the swipe gesture were not
exercised interactively in the dev session (Wayland input injection could not
get keyboard focus onto the window); they need a hand test.

**Fix (same day):** a swipe that starts on a checkbox must never drag the item.
The item's `GtkDragSource` (on the cell) and the swipe `GtkGestureDrag` (on the
list base) are not in the same gesture group, so claiming the sequence did not
deny the drag and the first item got moved. `on_item_drag_prepare()` now returns
NULL when the drag starts over a checkbox, and the swipe toggles whatever item
the finger is over once started (drifting off the checkbox column still selects,
never moves).

See `nautilus-MEMORY-2026-10-05.md` and `nautilus-MEMORY-2026-10-15.md` in the
workspace root for the full notes.

## Fix 2: the on-screen keyboard no longer closes the rename popover (0.2)

Owner report: *"long-press on file → rename opens the rename overlay; as soon as I
start typing with the OSK the window is closed."*

Root cause is **not** in Nautilus: the rename UI is a `GtkPopover` with GTK's
default `autohide = TRUE`, which makes GTK take a **Wayland popup grab**.  Mutter
(`popup_grab_release()`) ends such a grab on the *release* of any press whose
surface belongs to a different client — and the on-screen keyboard is a **shell
surface, which has no client**, so the first OSK key tap dismissed the popover and
the typed character was lost (mutter even stops that release event).

The fix, in `src/nautilus-rename-file-popover.c`:

* `gtk_popover_set_autohide (popover, FALSE)` — no grab, so an OSK tap cannot
  dismiss the popover;
* the entry is focused explicitly when the popover is shown (without the grab GTK
  no longer does that by itself, and the OSK follows the focused entry);
* the "click outside closes it" behaviour the grab used to give is re-implemented
  with a capture-phase `GtkGestureClick` on the popover's parent widget (the files
  view) that pops it down only for presses **inside the app** — an OSK tap never
  reaches the app.  Escape still closes it (GTK's own handling).

Verified with an instrumented GTK4 harness (a popover + entry that logs
`POPOVER OPEN/CLOSED` and `ENTRY CHANGED`): with GTK's default the first OSK key
tap closes the popover and the key is lost; with the fix the popover stays and the
character lands (`tmp/popover-osk-test.py`, `tmp/popover-osk-repro.sh`).

Version 0.2, built + installed locally (`/usr/bin/nautilus` md5
`f0b4bc17f33df51de2162016acb44607`).


## Package / publish

`build-nautilus-deb.sh` (in `oneplus-erhai-ubuntu/`, next to the other
builders) packages this project's `build/` output as
`oneplus-erhai-nautilus_<ver>_arm64.deb`. The deb ships the payload under
`/usr/share/oneplus-erhai-nautilus/` and its `DEBIAN/postinst`/`postrm` divert
Ubuntu's `/usr/bin/nautilus` and `org.gnome.nautilus.gschema.xml`
(`dpkg-divert --no-rename --divert <file>.erhai-orig`, originals kept) and
restore them on removal — so `dpkg -V nautilus nautilus-data` stays clean and
`apt remove oneplus-erhai-nautilus` returns to stock Nautilus.

Published: **0.1** in the public **main** suite
(`ppa/oneplus-erhai/ubuntu/main/`, GitHub push `10055b3`),
sha256 `6fb992135ca7199e734ba115a4583748fd36bd83b013f1f7848e3f6a6267c634`.

An image only picks it up once `oneplus-erhai-nautilus` is added to
`build-root.sh`'s package set.
