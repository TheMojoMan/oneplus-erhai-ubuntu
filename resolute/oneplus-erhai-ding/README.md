# Desktop Icons NG (DING) — touch long-press context menu

`ding@rastersoft.com` draws the GNOME desktop (background **and** icons) and
runs as its own GTK3/gjs process (`app/ding.js`).  It opens both context menus
only on a **secondary (mouse) button press** (`button == 3`):

* `app/desktopManager.js` — `onPressButton()` → desktop background menu
* `app/desktopIconItem.js` — `_onPressButton()` → per-icon menu

A touch tap or **long tap** is delivered to GTK as an emulated **primary
(button 1)** press, so the `button == 3` branch never runs and a long tap on
the desktop or an icon did nothing.

## The patch

A **touch-only single-finger** long-press shim: a finger held still for
`TOUCH_LONGPRESS_MS` (500 ms) over the desktop or an icon opens the *same* menu
the right mouse button would.  It is cancelled if the finger moves more than
`TOUCH_LONGPRESS_MAX_MOVE` (14 px) and only reacts to events whose
`Gdk.Device` source is `Gdk.InputSource.TOUCHSCREEN`, so mouse/trackpad
behaviour is untouched (a held mouse button does **not** open the menu).

### Single-finger gate + touch-cancel (2026-10-15, `oneplus-erhai-desktop` 2.11)

The timer must only ever be armed for ONE finger, and it must die when the
compositor cancels the touch.  Both matter for the shell's 3-finger workspace
swipe, which is what the owner reported as *"a slow 3-finger swipe snaps back
and only works when I do it fast"* plus *"the right-click menu appears"*:

* a touch press reaches GTK as an emulated **button-1** press, which is
  indistinguishable with one or with three fingers down — so the 2.9/2.10 shim
  armed its 500 ms timer for **three** fingers too.  Three fingers resting on
  the glass (or the rest phase of a slow swipe, before the fingers move >14 px)
  therefore opened the desktop menu 500 ms in;
* a client popup grab (the menu) **cancels the shell's 3-finger gesture**:
  `SwipeTracker._cancelPanGesture()` finishes at `_cancelProgress`, i.e. the
  **current** workspace — the swipe *snaps back*.  A fast swipe completes
  before the 500 ms timer, so it kept working;
* when the shell grabs the sequence for a gesture, the compositor sends the
  client a **`TOUCH_CANCEL` and NO `button-release`** (measured: a 3-finger
  touch gets `TOUCH_BEGIN ×3`, then `TOUCH_CANCEL`, no release) — so a timer
  armed during the swipe survived it whole.

The raw touch events carry what the emulated button events do not, provided the
widget's window has `Gdk.EventMask.TOUCH_MASK` set (GTK3 then delivers one
`touch-event` per finger, each with a real `GdkEventSequence`):

* `_trackTouchEvent()` counts live touch points and keeps a
  "more than one finger was seen" latch (cleared only when the count returns to
  0, or on the emulated `button-release` that ends the set);
* `_maybeStartTouchLongPress()` refuses to arm while that latch/count says
  multi-touch;
* `TOUCH_CANCEL` cancels the timer, as does a second `TOUCH_BEGIN`.

Verified on the pad by injecting gestures over the exposed desktop (tools in
`tmp/ding-touch-probe/`, log via a temporary `showDesktopContextMenu`
instrumentation): 1 finger ⇒ menu opens; **2 and 3 fingers ⇒ no menu** (before
the fix the 3-finger hold opened it every time); 3 fingers with a 700 ms rest
followed by a slow 500 px move ⇒ no menu.

Files changed (all in `app/`):

| file | change |
|------|--------|
| `desktopGrid.js` | `TOUCH_MASK` + `touch-event` tracking + single-finger gate; start/cancel the long-press timer on the desktop event box; `_doTouchLongPress()` calls the new manager method |
| `desktopManager.js` | new `showDesktopContextMenu(x, y, widget, triggerEvent)` that pops the desktop menu anchored at the finger (plus `guardMenuRelease()`) |
| `desktopIconItem.js` | same touch tracking/gate on the icon + label event boxes; `_doTouchLongPress()` opens the icon menu, and a long-press release no longer counts as a click |

Each file carries the marker `_initTouchLongPress` so `DEBIAN/postinst` /
`install-ding-touch-longpress.sh` can tell a patched file from Ubuntu's.

`touch-longpress.patch` is regenerated from a pristine tree with

```
cd oneplus-erhai-ubuntu
D=/usr/share/gnome-shell/extensions/ding@rastersoft.com/app
P=oneplus-erhai-ding/usr/share/gnome-shell/extensions/ding@rastersoft.com/app
for f in desktopGrid desktopManager desktopIconItem; do
    diff -u $D/$f.js.erhai-orig $P/$f.js
done > oneplus-erhai-ding/touch-longpress.patch
```

(the `*.erhai-orig` files ARE Ubuntu's originals — `dpkg-divert --no-rename`
leaves them next to the patched ones).  The 2.9/2.10 version is kept as
`touch-longpress.patch.2.9-2.10`.

### What GTK4 does (checked, in case this is ever ported)

`GtkGestureLongPress` is a `GtkGestureSingle`; GTK4 DENIES a sequence once the
number of points exceeds `n_points` (gtkgesture.c: `if
(_gtk_gesture_get_n_physical_points (gesture, FALSE) > priv->n_points) state =
GTK_EVENT_SEQUENCE_DENIED`) and its `check` requires exactly `n_points` active
points, so the gesture emits `cancelled` instead of `pressed` with three
fingers (measured on GTK 4.22 with `tmp/ding-touch-probe/
gtk4-longpress-probe.py`).  That is why the Nautilus (GTK4) long-press patches
did not need the same gate — and why this hand-rolled GTK3 timer did.

## Why not the GNOME accessibility setting?

GNOME 50 has *Settings → Accessibility → Pointing & Clicking → Simulated
Secondary Click* (`org.gnome.desktop.a11y.mouse secondary-click-enabled`), but
it is implemented in Mutter/Clutter for **pointer buttons only**:
`clutter_seat_a11y_update()` (mutter `clutter-input-pointer-a11y.c`) reacts
only to `CLUTTER_MOTION` / `CLUTTER_BUTTON_PRESS` / `CLUTTER_BUTTON_RELEASE`,
while Mutter's touch path emits `CLUTTER_TOUCH_BEGIN/UPDATE/END`.  So that
toggle cannot cover touch.  (It also only works for compositor-side handling,
not for a Wayland client like DING doing its own input.)

## Delivery

The DING files belong to `gnome-shell-ubuntu-extensions`, so the patch ships
in `oneplus-erhai-desktop` under a private path
(`/usr/share/oneplus-erhai-desktop/ding-touch/app/*.js`) and `DEBIAN/postinst`
installs it with `dpkg-divert` (Ubuntu's file kept as `*.erhai-orig`); `postrm`
restores Ubuntu's file.  This is the same scheme the Ubuntu-Dock touch-scroll
patch uses.  `build-desktop-deb.sh` copies the payload from this project dir on
every build so the two can never diverge.

For a quick test without a logout, restart DING: `pkill -f ding.js` (gnome-shell
respawns it with the new files).

## Manual use

```
sudo ./install-ding-touch-longpress.sh apply|revert|status
```