# oneplus-erhai-ptyxis — touch-friendly Ptyxis (GNOME terminal)

Touch additions and a header-bar **Paste** button for Ubuntu's **Ptyxis
50.1** on the OnePlus Pad 3 (erhai):

1. **One-finger drag scrolls the terminal contents** — 1:1 with the finger for
   slow movement (so precise positioning stays exact), accelerated up to 1.3×
   for fast swipes, and the content keeps **gliding after you lift the finger**
   (a fling with exponential deceleration).  In an application that scrolls
   itself (micro, vim, less — any mouse-tracking app, which lives on the
   alternate screen with no scrollback) the drag is forwarded as **mouse wheel
   notches** so the *application* scrolls, exactly as a real wheel does.
2. **Long press then drag marks text.**  A long press selects the word under
   the finger; dragging the finger extends the selection.  When the finger is
   lifted the marked region is **colour-inverted**, the text is **copied to the
   clipboard** and a **"Copied to clipboard" toast** is shown, and two round
   **handles** appear at the left and right ends of the marked region.  Dragging
   a handle changes the region (crossing the other handle swaps them); releasing
   a handle copies again.
3. **A plain tap is a mouse click.**  When the running application wants mouse
   events (micro, vim, less, mc, ...) a tap is forwarded to it as a left
   button 1 click — **always, including while a marked region is up** — so
   exactly as in unpatched Ptyxis, tapping in a text editor moves its cursor to
   the tapped cell.  This is the behaviour taking touch input over would
   otherwise lose (see below).
4. **A Paste button in the header bar.**  One icon button sits to the right of
   the new-tab (**+**) button so pasting is one tap away on a touchscreen (a long
   press is not a discoverable way to paste, and the context menu is a
   two-finger/hold gesture).  It acts on the **active tab's** terminal and
   follows its state — insensitive while there is nothing pastable — exactly
   like the terminal's own context-menu entry.

Ptyxis is a C/GTK4/libadwaita application built on VTE (`VteTerminal`), so this
is a **source patch**, not an extension.  The patch touches three files:
`src/ptyxis-terminal.c` (all touch handling, and the clipboard-state push),
`src/ptyxis-window.c` (the `win.paste` action behind the button)
and `src/ptyxis-window.ui` (the button in the header bar).

## Why a patch is needed

* `VteTerminal` installs a non-touch-only `GtkGestureClick`; a touch press is
  delivered to it as **button 1**, and `Terminal::widget_mouse_press()` starts a
  selection and **claims** the sequence.  That claim is what stops the enclosing
  `GtkScrolledWindow` from ever scrolling with a finger (its own touch drag
  gesture only claims once it is already moving).
* VTE's touch-only `GtkGestureLongPress` opens the terminal context menu.

The patch therefore takes touch input over completely:

* a **capture-phase `GtkEventControllerLegacy`** on the terminal
  consumes every `GDK_TOUCH_*` event *before* VTE's bubble-phase gestures see
  it (so VTE never claims the sequence);
* because that also hides the tap from VTE's click gesture, a plain tap has to
  be **turned back into a click by hand** or tap-to-position in micro/vim/...
  is lost.  When the application has mouse tracking on, a tap writes a press +
  release of button 1 in the xterm SGR (1006) encoding to the pty
  (`vte_terminal_feed_child()`).  Whether the application wants mouse events is
  not public VTE API, but its widget cursor is: `apply_mouse_cursor()` selects
  VTE's "default" (mousing) cursor exactly while mouse tracking is enabled and
  the "text" cursor otherwise, so the tap is forwarded only when it should be
  (verified against a child that emits `?1000h/?1002h/?1006h`);
* the containing `GtkScrolledWindow`'s **kinetic touch scrolling is turned off**
  (`gtk_scrolled_window_set_kinetic_scrolling (…, FALSE)` — it only affects
  touchscreen devices), because otherwise its capture-phase gestures would run
  before ours and double-scroll; scrolling is implemented in the controller
  instead (`GtkAdjustment` value, which is exactly what VTE watches — see
  `Widget::vadjustment_value_changed()`);
* the selection is **our own** (VTE exposes no API to *set* a selection range —
  only `select_all`/`unselect_all`).  It is rendered by feeding the terminal's
  own `GskRenderNode` (plus a rect of the terminal background colour — ptyxis
  drops VTE's background node) as the **bottom** image and white rects over the
  marked cells as the **top** image of a
  `gtk_snapshot_push_blend (…, GSK_BLEND_MODE_DIFFERENCE)`, which inverts the
  marked pixels while keeping the glyphs readable.  Note the two-`pop()`
  protocol of `push_blend()`, and that the Cairo renderer cannot do blend nodes
  (guarded against);
* cell↔pixel conversion mirrors VTE's own math (`pixel_to_row()` /
  `row_to_pixel()` in `vte.cc`), including the pixel-based `vadjustment`
  (`scroll-unit-is-pixels`) and the grid origin from VTE's `xalign`/`yalign`
  (`widget_size_allocate()`); the highlight is placed with the exact
  `row * cell_height - scroll_pixel` mapping, which matters because pixel
  scrolling leaves the view on fractional rows;
* those rows are **absolute**, not viewport-relative: VTE's `scroll_delta` is
  `adjustment_value / cell_height + row_data->delta()`, and `delta` (rows
  already dropped from the ring) stays 0 on the primary screen until the
  scrollback wraps but is large on the **alternate screen** — where micro, vim
  and less live.  Using the adjustment alone was therefore tens or hundreds of
  rows short there, and every mark/copy missed entirely.  VTE exposes
  neither `first_displayed_row()` nor `delta`, so `ptyxis_terminal_ring_delta()`
  recovers it from the text API (rows below the ring extract as empty, so the
  first non-empty row is `delta`, verified against the displayed text);
  `ptyxis_terminal_scroll_delta_pixel()` adds it to every row↔pixel conversion;
* because a `GtkEventControllerLegacy` does **not** get GTK's widget
  translation, the raw `gdk_event_get_position()` (surface coordinates) is
  converted to terminal-widget coordinates exactly like GTK's own
  `translate_event_coordinates()` (subtract the native surface transform, then
  `gtk_widget_compute_point()` from the root widget);
* the header bar's **Paste** button cannot use the terminal's own
  `clipboard.paste` action: `GtkActionable` resolves an `action-name` by walking
  the widget's *ancestors*, and the terminal is a
  sibling of the header bar (`AdwHeaderBar` → … → `PtyxisWindow`), not an
  ancestor.  The window therefore installs `win.paste`, which
  forwards the activation to the **active tab's** terminal with
  `gtk_widget_activate_action()`.  That call honours the target action's
  enabled state, so an insensitive terminal action (nothing pastable) is a no-op
  even if the button is still sensitive.  The window action mirrors the
  terminal's state — the terminal pushes it whenever the clipboard changes, but
  *only while it is the active terminal*, and `notify::selected-page` refreshes
  it on every page switch.

## Build

```
oneplus-erhai-ubuntu/oneplus-erhai-ptyxis/build-ptyxis-erhai.sh
```

Extracts the Ubuntu source for the installed ptyxis version, applies
`ptyxis-erhai-touch.patch` and builds it with meson (release), staging the
patched `ptyxis` binary into `./build/`.

Build deps (not auto-installed): `meson ninja libglib2.0-dev libgtk-4-dev
libadwaita-1-dev libjson-glib-dev libportal-gtk4-dev libvte-2.91-gtk4-dev`.

## Install / revert

```
sudo oneplus-erhai-ubuntu/oneplus-erhai-ptyxis/install-ptyxis-erhai.sh apply
sudo oneplus-erhai-ubuntu/oneplus-erhai-ptyxis/install-ptyxis-erhai.sh status
sudo oneplus-erhai-ubuntu/oneplus-erhai-ptyxis/install-ptyxis-erhai.sh revert
```

`apply` uses `dpkg-divert --local --add --no-rename` on `/usr/bin/ptyxis`,
keeps Ubuntu's original as `/usr/bin/ptyxis.erhai-orig`, and installs the
patched binary.  `revert` restores it exactly and drops the diversion.
Already-running ptyxis windows keep the old binary; **new windows/tabs use the
patched one**, so no logout is needed.

Only `/usr/bin/ptyxis` is replaced — `/usr/libexec/ptyxis-agent` (the container
agent) is unchanged and stock.

## Gesture reference

| gesture | effect |
|---|---|
| one-finger drag | scroll: 1:1 with the finger for slow movement (precise positioning), accelerated up to 1.3× for fast swipes, and the content keeps gliding (exponential deceleration) after the finger is lifted; in a mouse-tracking application (micro, vim, less, ...) the drag is forwarded as mouse wheel notches so the app scrolls itself (no glide there) |
| long press (>450 ms) | select the word under the finger |
| long press **then drag** | extend the marked region |
| lift the finger | invert the marked region, copy it, toast, show the handles |
| drag a handle | adjust the region; releasing re-copies |
| drag a handle or the selection end into the top/bottom 10 % of the view | auto-scroll (the closer to the edge, the faster, up to ~25 lines/s); the region keeps growing while it scrolls |
| tap | a mouse click: when the application wants mouse events (micro, vim, less, ...) the tap is forwarded as a left button 1 click, so its text cursor moves to the tapped cell exactly as in unpatched ptyxis — **always**, including while a marking is up; otherwise nothing happens |
| tap outside the marked region | also dismisses the marking (a scroll, a handle drag, or a tap INSIDE the region keeps it) |
| **type, or paste** | dismisses a marking, exactly like VTE's own selection — while a marking is up the marked cells are drawn inverted, so text pasted into a region somebody marked earlier would otherwise have looked inverted |
| mouse | unchanged (VTE's own click/drag selection, right-click menu) |

The header bar also carries a **Paste** button immediately to the right of the
new-tab (**+**) button; it acts on the active tab's terminal and follows its
state (insensitive while there is nothing pastable).

## Status

In development on the owner's pad (built + installed locally; **0.3 is the
PUBLISHED revision** in the PPA main suite, with 0.2 and 0.1 below it as
downgrade targets).  Verified so far: the
patch applies cleanly to a pristine Ubuntu source tree and reproduces the
working tree byte-for-byte; the release build is warning-free (the only warning
in the log is upstream's own unused `self` in
`ptyxis_window_show_keyboard_shortcuts_action()`); the patched
binary carries the touch callbacks and both SGR format strings.  The header-bar
button is compiled in — the embedded `ptyxis-window.ui` carries
`erhai_paste_button` and the `win.paste` action name — and a 6 s smoke run of the
patched binary under a private D-Bus
session (`dbus-run-session -- ./build/ptyxis --new-window`) opened its window
with **zero Gtk/GLib criticals**, so the new template instantiates cleanly.  The
three mechanisms the mouse-mode support rests on are **unit-verified off-device**
(see
`tmp/ptyxis-erhai-build/`): the widget cursor reads `default` exactly while the
child enables mouse tracking (`?1000h/?1002h/?1006h`) and stays unset/text
otherwise (`cursor-mode-test.c`); `vte_terminal_feed_child()` hands our bytes to
the child verbatim (`feed-child-test.c`); and `ring_delta` recovers VTE's
absolute row offset (`normal: delta=0 first_disp=41 MATCH`,
`alt: delta=60 first_disp=60 MATCH` — `ringdelta-test.c`).  The click itself was
confirmed against **real micro in a pty** (`micro-click-probe.py`): clicking cell
(col 10, row 5) moved micro's caret from `(1,4)` to `(5,10)` — and the pad log of
2026-10-06 confirms the same click is really sent on the device (`tap: sent
row=… first=29 view_row=… offset=29`).  The input-dismissal rule is verified
against VTE's own source (`Terminal::commit` is emitted from `feed_child()`, so
one hook covers typing, the on-screen keyboard, the paste action and
middle-click paste; `vte_terminal_paste_clipboard()` emits BEFORE the paste is
fed) **and unit-verified off-device by `commit-signal-test.c`** (PASS:
`feed_child -> commit` yes, `paste -> commit` yes, and at the moment the signal
fires for a paste the child has NOT received the text yet — so the marking is
already cleared when the pasted bytes are echoed).  The gestures themselves need
a hand test on the pad.

### Known limitations

* The row↔pixel mapping pins VTE's ring offset once per gesture (on touch-down),
  so a selection left up while the application scrolls a lot underneath it can
  drift by that much.
* Blank rows at the very start of the ring before any content could nudge the
  ring-offset detection; it then falls back to the old (no-offset) behaviour.
* The `Copied to clipboard` toast is shown unconditionally (the
  `toast-on-copy-clipboard` setting is not consulted for the touch gesture).
* Region marking is a *linear* selection (like dragging with a mouse); there is
  no rectangular/block mode.

## Package / publish

`build-ptyxis-deb.sh` (in `oneplus-erhai-ubuntu/`, next to the other builders)
packages this project's `build/` output as
`oneplus-erhai-ptyxis_<ver>_arm64.deb` (`SKIP_BUILD=1` reuses the existing
build).  The deb ships the payload under `/usr/share/oneplus-erhai-ptyxis/`
(`ptyxis`, this README and the source patch) and its `DEBIAN/postinst`/`postrm`
divert Ubuntu's `/usr/bin/ptyxis`
(`dpkg-divert --package oneplus-erhai-ptyxis --add --no-rename --divert
<file>.erhai-orig`, original kept) and restore it on removal, so
`dpkg -V ptyxis` stays clean and `apt remove oneplus-erhai-ptyxis` returns to
stock Ptyxis.

The payload is moved into place with a **temp file + rename**: overwriting a
*running* executable with `cp`/`install` fails with `ETXTBSY` ("Text file
busy"), and the package deliberately does **not** kill ptyxis (that would close
the user's shells).  Already-running windows keep the old binary — the app is
single-instance, so **close all Ptyxis windows once** after installing.

Published in the public **main** suite (`ppa/oneplus-erhai/ubuntu/main/`):

* **0.3** (current) — the marking-dismissal fix (a marking is dismissed by the
  next input you send) and the removal of the temporary tap diagnostic: size
  253 388 B, sha256
  `0a295c3366a8728b14041147a4af7ce3bde54cd3bec9091d0d1310992bdd5476`; published
  by COPYING the built deb into `ubuntu/main/` + `update-ppa.sh` (GitHub commit
  `d0a0bb6`, push `5c00cca..d0a0bb6`).
* **0.2** — size 253 382 B, sha256
  `626bcab32b897afdbac8724345bc4e48ddc49ad798b9a9799d6cb1ac1b3806c3`; published
  with `SKIP_BUILD=1 PUBLISH=1 PPA_DEB_DIR=<…>/ubuntu/main
  ./build-ptyxis-deb.sh` + `update-ppa.sh` (GitHub commit `4722f7f`, push
  `8e5ec9c..4722f7f`).  Upgraded in place on the pad (`dpkg -i` over 0.1) and the
  payload verified byte-for-byte against `build/ptyxis`
  (md5 `25312bc208ac717119dd437e57f0c1e4`).
* **0.1** — size 244 080 B, sha256
  `d3856d0df8e29f3811d0c09c058dbb87c3b3f979f018da443edd9792f1dcad25` (GitHub
  commit `dd135a6`, push `10055b3..dd135a6`), kept as the downgrade target.

Install lifecycle verified end to end (`dpkg -i` → patched binary +
package-owned diversion; `dpkg -r` → stock restores, diversion + backup gone;
`dpkg -i` again).

An image only picks it up once `oneplus-erhai-ptyxis` is added to
`build-root.sh`'s package set (build-root installs from **main**, where it now
is) — not done yet.


