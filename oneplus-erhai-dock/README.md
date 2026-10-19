# oneplus-erhai-dock — finger-drag scrolling for the Ubuntu Dock

Adds **touch drag scrolling** to the Ubuntu Dock (`ubuntu-dock@ubuntu.com`):
drag a finger along the dock and the app icons scroll under it, with a
momentum/kinetic glide when you let go.

## Why this is a system-file patch

`ubuntu-dock@ubuntu.com` is listed in the Ubuntu **session mode**
(`/usr/share/gnome-shell/modes/ubuntu.json` -> `enabledExtensions`).  The shell's
extension loader explicitly refuses to load a *per-user* copy of a mode
extension:

```
Found user extension ubuntu-dock@ubuntu.com, but not loading from
~/.local/share/gnome-shell/extensions/... directory as part of session mode.
```

So a `~/.local/share/gnome-shell/extensions/` override is impossible; the change
has to be made at the system path
`/usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js`.

`install-dock-touch-scroll.sh` uses `dpkg-divert` so the original file is kept as
`dash.js.erhai-orig` and an Ubuntu package upgrade of
`gnome-shell-ubuntu-extensions` will not silently overwrite the patch.

**The patch now also ships in the `oneplus-erhai-desktop` package** (from
version **2.8**), which is the delivery path for a normal `apt` install and for
a freshly flashed image: the deb carries the patched file under a private name
(`/usr/share/oneplus-erhai-desktop/ubuntu-dock-dash.js`) and its `postinst`
installs it here with `dpkg-divert`; `postrm` restores Ubuntu's file on removal.
`build-desktop-deb.sh` copies the payload from this directory on every build, so
the two can never diverge.  This standalone script stays for development.

The diversion is registered **without `--rename`** and the original is kept by
the script/deb itself: `dpkg-divert --remove --rename` *refuses* to overwrite a
different existing file (`dash.js` is our patched copy), so a `--rename` round
trip fails on revert/removal and could leave the dock with **no** `dash.js` at
all (measured 2026-10-04).  Removal therefore just copies `dash.js.erhai-orig`
back over `dash.js`.

## What was changed (dash.js only)

* The dash's `St.ScrollView` (which fills the dock) is left `reactive: true`
  — an `St.ScrollView` is reactive by default anyway (`st_scroll_view_init()`
  sets it, and `enable_mouse_scrolling: false` does **not** unset it), so the
  line is explicit documentation and no hit-testing changes. A touch anywhere
  over the dock — including the gaps between icons — reaches the gesture.
* A `Clutter.PanGesture` is attached to that scroll view in the **capture**
  phase:
  * single finger only (`min_n_points: 1`, `max_n_points: 1`),
  * axis locked to the dock orientation (`X` for bottom/top, `Y` for sides),
  * `may-recognize` only returns true for a real `TOUCH_BEGIN` event and only
    when the dash actually overflows, so mouse/trackpad drags are untouched and
    icon reordering keeps working.
* `pan-update` moves the scroll adjustment so the icons follow the finger;
  `end` reads the gesture velocity and starts a decelerating kinetic glide
  (`GLib.timeout_add`, ~16 ms ticks).
* A small `_getScrollAdjustment()` helper now de-duplicates the adjustment
  lookup shared with the existing mouse-wheel handler (`_onScrollEvent`), whose
  behaviour is unchanged.

**Quick swipe = scroll, press-and-hold-then-drag = reorder an icon.**  The app
icons start their drag-and-drop only after the shell's 200 ms touch timeout, so
a swipe that moves before then is claimed by the scroll gesture instead.

Tuning constants live at the top of `dash.js`
(`TOUCH_SCROLL_TICK_MS`, `TOUCH_SCROLL_FRICTION`, `TOUCH_SCROLL_MIN_VELOCITY`).

## Install / revert

From the package (normal path, incl. flashed images):

```bash
sudo apt install oneplus-erhai-desktop     # >= 2.8 carries the dock patch
```

Dev iteration with the standalone script (checks the same diversion):

```bash
sudo ./install-dock-touch-scroll.sh apply     # patch the system extension
sudo ./install-dock-touch-scroll.sh status
sudo ./install-dock-touch-scroll.sh revert     # restore Ubuntu's original
```

Once the diversion is owned by `oneplus-erhai-desktop`, `revert` refuses (undo
it through apt instead — `sudo apt purge oneplus-erhai-desktop`).

GNOME 50 cannot reload extension code, so **log out and back in (or reboot)**
for the change to take effect.

## Source of truth

* `usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js` — the
  patched file that gets installed.
* `touch-scroll.patch` — unified diff against the shipped Ubuntu file
  (`gnome-shell-ubuntu-extensions` 50.26.04.8ubuntu), for review / rebasing
  onto a newer Ubuntu version.
