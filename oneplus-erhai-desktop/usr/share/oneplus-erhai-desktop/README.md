# oneplus-erhai-desktop

Desktop glue for the OnePlus Pad 3 (erhai): GPU/GMU bring-up, power-key panel
power cycle + suspend harness, OSK/pogo keyboard, folio-fold blanking, the
fixed login-screen brightness policy, the battery-backed clock, USB-C host
charging, and the **bypass charging** switch (which the in-kernel charge limit
also engages automatically at the cap — see below).

## Bypass charging (`erhai-bypass@erhai.local`)

Bypass charging stops the battery from charging while a charger is attached:
the tablet runs from the brick and the pack is simply held at whatever level it
had when you flipped the switch.  It is the "powered by the adapter" mode you
want for long plugged-in sessions — no charging heat, no charge cycles.

Measured on this pad (2026-09-30, genuine SuperVOOC brick attached):

| switch | pack current | pack power |
|--------|--------------|------------|
| off    | 3.93 A in    | 16.3 W     |
| on     | 0.117 A      | 0.49 W (system-only) |
| off again | 1.74 A in | 7.2 W      |

### Using it

GNOME Quick Settings has a **Bypass charging** switch (panel row with a
chevron).  Flip it on/off; the menu also offers the two modes explicitly and
the subtitle shows live state (`Adapter power only · pack idle`,
`Waiting for a charger`, `Charging normally`).  From a terminal:

```
erhai-bypass-charge.sh status          # key=value report
erhai-bypass-charge.sh on|off|toggle
```

The choice is persisted in `/etc/default/erhai-charge`, so it survives a
logout and a reboot (`erhai-bypass-charge.service` re-applies it at boot) — but
it is *good practice to switch it off before long unplugged use*; the helper
only re-enables charging below a 10 % safety floor.

### The charge limit is a bypass too

`erhai_otg_glink` has an in-kernel charge limit (`max_soc`, a module parameter,
**default 80 %**).  The kernel stops the ADSP's charger at the cap — but
stopping *alone* does not hold the pack on this hardware, and that is what made
the pad "stop charging at the limit and then run down on battery": the
`erhai_pps` engine reads the deliberately-held pack as a **stall** and
exits/re-negotiates (re-injecting charge current), and a genuine SuperVOOC brick
keeps charging straight through the ADSP inhibit via the SC8547A charge pump.

So while the pack sits **at** the limit the holder takes the same two actions
manual bypass takes and the kernel cannot — it parks the **PPS stall watchdog**
and the **SC8547A charge pump** — and releases both below the limit.  The pad
then runs from the adapter and the pack just sits, exactly like a manual bypass:

```
erhai-bypass-charge.sh status          # charge_limit=80  limit_bypass=on
erhai-bypass-charge.sh limit           # print the limit in force
erhai-bypass-charge.sh limit 70        # set it (runtime; see below)
```

The kernel limiter keeps owning the `BATT_CHG_EN` property itself, so there is
still exactly **one** writer of it: the split is "the kernel says *when* to
stop", "this helper supplies the extra levers *at* the limit".  If the holder
is ever not running, the kernel limiter is still there as the last-resort
fallback.  Set `ERHAI_BYPASS_AUTO_LIMIT=0` (service environment) to turn the
augmentation off, and `ERHAI_BYPASS_HYST` (default 3) to widen/narrow the
release band.  `max_soc` is a **module parameter**, so the `limit` write does
not survive a reboot — make a custom limit permanent with
`erhai_otg_glink.max_soc=<n>` on the kernel command line.

**Only a park the helper recorded is released (2.13).**  "The pump reads 0" is
not evidence of a park — 0 is also the SC8547A driver's *normal idle state* (no
(S)VOOC brick attached, or any auto-decline / auto-finish / PHY stop).  Acting on
that signal alone made the holder re-arm the pump and bounce the VOOC PHY
(`vooc` 0 → 1) on **every** poll, and a PHY stop flips the USB D+/D− crossbar
(`sc8547_vooc_stop()` → `wcd939x_usbss_dpdm_switch_fast_charge(false)`), so every
attached USB device was dropped and re-enumerated once per `POLL + 1 s`, forever
(measured 2026-10-18: a Kingston DataTraveler cycled every 11 s with no charger
even attached).  A limit park is now recorded in
`/run/erhai-charge-limit-parked`, written **before** the park so a park can never
exist without its record, and only a recorded park is released — exactly once.
The manual switch records its park in `/etc/default/erhai-charge` as before.

### How it works

The OnePlus ADSP charger firmware owns charging; the AP-side lever is the
battery-psy `BATT_CHG_EN` property (24) over pmic_glink:

```
echo "24 0 0x31" > /sys/kernel/debug/erhai_otg/cmd   # stop charging
echo "24 1 0x31" > /sys/kernel/debug/erhai_otg/cmd   # allow charging
```

* `erhai-bypass-charge.service` (`Type=simple`, enabled) is the **holder**: it
  applies the persisted choice and re-asserts the inhibit every 10 s (the ADSP
  clears it on its own charge events), and enforces the low-SoC floor.
* `erhai-bypass-charge@on|off.service` is the **switch**: a oneshot the GNOME
  extension starts.  `50-erhai-bypass-charge.rules` lets an active local user
  in group `sudo` start it with no password prompt, scoped to this unit and the
  `start` verb only.
* The legacy `USB_CHARGE_OUTPUT_SUSPEND` (usb prop 19) is **ignored** by this
  firmware — that is why the `max_soc` cap silently stopped working in 2026-09;
  do not use it (`charging-MEMORY-2026-09-23.md`).
* The in-kernel `max_soc` limiter drives the **same** property, so the helper
  **parks it** (`max_soc=100`, its documented "limit disabled" value) while
  bypass is on and restores the parked value when it goes off.  Two charging
  policies writing one property is a charge/no-charge oscillation.
* **Engaging bypass is NOT instant** — the ADSP tapers the charge current down
  instead of cutting it: measured a clean exponential (ratio 0.709 per 5 s,
  τ ≈ 15 s), i.e. ~1 min from 4 A to ~0 A; the reverse edge resumes in 5-15 s.
  That is charger firmware and no AP-side lever shortcuts it (the FCC/prop-10
  clamp is inert, lowering the input limit only makes the pack cover the load,
  the legacy prop 19 is ignored).  Watch the **current** while it settles — the
  meter's `Bypass charging` label is already right during the ramp.
* The PPS engine (`erhai_pps`) must not read the held pack as a *stall*: its
  pack-stall watchdog (`pps_stall_polls`, default 15 polls ≈ 30 s of <100 mA at
  VBUS >7 V) would otherwise exit and re-negotiate every 60-90 s, re-injecting
  charge current, and after a few exits it leaves the ADSP's charger_pd unable
  to deploy PPS at all until a physical replug.  So the helper **parks
  `pps_stall_polls` at 0** ("never") while bypass holds and restores the parked
  (or default) value on `off`.
* **A genuine SuperVOOC brick needs the SC8547A charge pump parked too.**  The
  ADSP `BATT_CHG_EN` inhibit only reaches the ADSP's own charger; the SC8547A is
  a *direct 2:1 path to the cell* driven by the AP kernel, so it kept charging
  straight through bypass — and faster, because it expanded to carry the whole
  pack current once the ADSP path was held (measured 2026-10-03: pack stayed
  4.5→4.7 A with bypass ON until the CP was stopped).  The helper therefore also
  parks the CP master's `fastchg_deliver`/`fastchg_auto` (the `*-006f` debugfs
  dir that carries `fastchg_state`) and, on `off`, restarts the VOOC PHY so the
  ladder re-identifies (a stopped CP drops the adapter to 5 V).  No-op without a
  SuperVOOC session.
* Safety floor: below `ERHAI_BYPASS_MIN_SOC` (10 %) charging is allowed again
  until `ERHAI_BYPASS_RESTORE_SOC` (15 %), so a source that cannot carry the
  load cannot drain the tablet flat while the switch is on.

Tunables (environment of `erhai-bypass-charge.service`): `ERHAI_BYPASS_POLL`
(10 s), `ERHAI_BYPASS_MIN_SOC`, `ERHAI_BYPASS_RESTORE_SOC`,
`ERHAI_BYPASS_AUTO_LIMIT` (1 = bypass at the in-kernel limit, 0 = off) and
`ERHAI_BYPASS_HYST` (release this many % below the limit, default 3); the parked
knobs can be repointed for tests with `ERHAI_BYPASS_MAXSOC` /
`ERHAI_BYPASS_PPS_STALL` / `ERHAI_BYPASS_SC8547_GLOB`.

### Troubleshooting

```
erhai-bypass-charge.sh status                 # what the helper sees
systemctl status erhai-bypass-charge.service  # the holder
journalctl -t erhai-bypass-charge -b          # every edge and floor event
```

The pack can read `Charging` in `status` while bypass holds the current at
~0.1 A: the ADSP does not clear its status flag, so judge bypass by the pack
**current**, not the status string.  If the extension's switch does nothing,
check that the user is in group `sudo` (the polkit rule) and that
`/sys/kernel/debug/erhai_otg/cmd` exists.

If the pack *does* take current while bypass is on, look for churn rather than a
broken inhibit: `dmesg | grep 'erhai_pps: exit'` should NOT show `pack-stall`
entries while the switch is on (the helper parks that watchdog — see above), and
`erhai_pps` should stay in `state charging`.  A `pack-stall` loop means the park
did not take (`erhai-bypass-charge.sh status` shows `pps_stall_polls`), and a
session that stopped re-deploying PPS entirely needs a **physical replug** —
the ADSP does not retry on its own.

## Finger-drag scrolling for the Ubuntu Dock (2.8)

The Ubuntu Dock (`ubuntu-dock@ubuntu.com`) scrolls with a mouse wheel, but a
touch drag on the icons did nothing (an `St.ScrollView` has no touch/kinetic
scrolling built in).  `dash.js` now carries a `Clutter.PanGesture` on the dock's
scroll view, added in the capture phase:

* a quick swipe scrolls the icons with a kinetic glide,
* a press-and-hold then drag still reorders an icon (the icons' drag-and-drop
  needs the shell's 200 ms touch timeout, so the scroll gesture wins a fast
  swipe),
* mouse and trackpad drags are untouched, and a plain tap still launches.

**Why the patch is installed with `dpkg-divert`.**  `ubuntu-dock@ubuntu.com` is
listed in the GNOME session mode (`/usr/share/gnome-shell/modes/ubuntu.json`),
so gnome-shell *refuses* a per-user copy of it: the patched file must sit at
`/usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com/dash.js`, which is
owned by the `gnome-shell-ubuntu-extensions` package.  A file from this package
at that path would be a dpkg file conflict, so the deb ships the patched file
under a private name (`/usr/share/oneplus-erhai-desktop/ubuntu-dock-dash.js`)
and `postinst` installs it with `dpkg-divert`.  Ubuntu's original file is kept
as `dash.js.erhai-orig`, `dpkg -V gnome-shell-ubuntu-extensions` stays clean, an
Ubuntu package upgrade writes to the diverted name (so it cannot clobber the
patch), and `postrm` drops the diversion again on removal (restoring Ubuntu's
file).

Source of truth is the workspace project `oneplus-erhai-dock/`; the dock payload
is copied from there on every build, and
`oneplus-erhai-dock/install-dock-touch-scroll.sh apply|revert|status` applies or
removes the same patch outside the package.  **A change only reaches a running
session after a logout/login** (GNOME 50 cannot reload extension code).

Confirm after a re-login:

```
journalctl -b -o cat | grep 'erhai-dock: touch drag scrolling armed'
```

## Long-press context menu for the desktop and icons (2.9)

The Desktop Icons NG extension (`ding@rastersoft.com`) draws the GNOME desktop
background and the icons in its own GTK3/gjs process (`app/ding.js`).  It opens
**both** context menus only on a **secondary (mouse) button press**
(`button == 3`): `app/desktopManager.js` for the desktop background and
`app/desktopIconItem.js` for a file icon.  A touch tap or long tap is delivered
to GTK as an emulated **primary (button 1)** press, so the menu never opened
with a finger — a long tap on the desktop or an icon did nothing.

The patch adds a **touch-only** long-press shim: a finger held still for 500 ms
(`TOUCH_LONGPRESS_MS`) over the desktop or an icon opens the same menu the right
mouse button would.  It is cancelled when the finger moves more than 14 px
(`TOUCH_LONGPRESS_MAX_MOVE`), and only events whose **source device** is
`Gdk.InputSource.TOUCHSCREEN` are affected, so **mouse and trackpad behaviour is
untouched** and a held mouse button does *not* open the menu.  (GTK's emulated
button event for a touch reports `get_device()` as the "Wayland Touch Master
Pointer" with source MOUSE; the touchscreen is the *source* device, which is
what the check uses.)  A long-press release no longer counts as a click, so it
cannot also open the file.

Files changed (all under the extension's `app/`):

| file | change |
|------|--------|
| `desktopGrid.js` | long-press timer on the desktop event box; `_doTouchLongPress()` |
| `desktopManager.js` | new `showDesktopContextMenu(x, y, widget)` (pops the desktop menu at the finger) |
| `desktopIconItem.js` | long-press timer on the icon; `_doTouchLongPress()` opens the icon menu; release guarded |

**Why not the accessibility setting.**  GNOME 50 has *Settings → Accessibility →
Pointing & Clicking → Simulated Secondary Click*
(`org.gnome.desktop.a11y.mouse secondary-click-enabled`), but it is implemented
in Mutter/Clutter for **pointer buttons only** — `clutter_seat_a11y_update()`
reacts to `CLUTTER_MOTION` / `CLUTTER_BUTTON_PRESS` / `CLUTTER_BUTTON_RELEASE`,
while Mutter's touch path emits `CLUTTER_TOUCH_BEGIN/UPDATE/END`.  It therefore
cannot cover touch (and cannot affect a Wayland client like DING, which does its
own input handling).

**Why `dpkg-divert`.**  `ding@rastersoft.com` is owned by
`gnome-shell-ubuntu-extensions`, so the patched files ship under
`/usr/share/oneplus-erhai-desktop/ding-touch/` and `postinst` installs them with
`dpkg-divert` (Ubuntu's files kept as `*.erhai-orig`); `postrm` restores them.
Source of truth is the workspace project `oneplus-erhai-ding/`; the payload is
copied from there on every build, and
`oneplus-erhai-ding/install-ding-touch-longpress.sh apply|revert|status` applies
or removes the same patch outside the package.

**To load the patch without a logout:** DING is its own process, so
`pkill -f ding.js` makes gnome-shell respawn it with the new files (a full
logout/login or reboot is the safe path).

## Panel blanks have one owner — and a blanked panel is deaf (2.15)

Two features blank the internal panel, and until 2.15 they did not know about
each other:

| blank | marker | what owns it |
|-------|--------|--------------|
| power button (`erhai-panel-power.sh`) | `/run/erhai-panel-off` | the power button, until its next press |
| folio folded onto the screen (`erhai-folio-fold.py`) | `/run/erhai-folio-fold-blanked` | the watcher, until the folio opens |

**The bug this fixes.** Turning the display off with the power button and then
folding the keyboard up (and opening it again) switched the panel back on: the
watcher claimed whatever blank it found, so the unfold "restored" a blank the
power button still held (`/run/erhai-panel-off` was present the whole time) —
and the power button's next press then only deleted its marker, so the screen
stayed on. A fold now claims the blank **only when the panel was still lit when
the fold started**; if the panel was already off (power button, idle DPMS) the
fold just suppresses the input and writes no marker, so nothing the watcher does
later (unfold, stop, restart, unconfigured-idle) can light that panel up. The
power button skips a blank the folio already holds (it checks the folio's marker
AND the backlight, so a stale marker next to a lit panel can never block it),
and `erhai-folio-unblank.sh` (the stop safety net) leaves a power-button blank
alone.

**The ghost input.** While folded, the folio lies on the glass, so the panel's
own input devices must be suppressed — and the *touch* node alone is not enough:
the folded keyboard also makes the panel's **pen node** emit frames, which reach
the compositor as pointer/pen activity and wake the session behind the dark
screen (live proof 2026-10-22 09:23:57 + 09:24:34, 1 s apart, *during* a fold
whose touch node was already grabbed: `libinput error: event5 -
NVTCapacitivePen: Ignoring pressure offset greater than 50%`). Both nodes are
now grabbed while folded (`NVTCapacitiveTouchScreen` + `NVTCapacitivePen`;
`GRAB_DEVICES` in `/etc/erhai/folio-fold.conf` overrides the list).

`erhai-power-watch.py` applies the same suppression for as long as **its** blank
lasts: `EVIOCGRAB` on both panel nodes while `/run/erhai-panel-off` exists *and*
the backlight reads 0, released before the panel is restored. It is
fail-safe by construction — the grab belongs to the fds, so the process dying
(including the deliberate stop the suspend harness does) releases it — and
`PANEL_INPUT_GUARD=0` in `/etc/erhai/display.conf` (or
`ERHAI_PANEL_INPUT_GUARD=0`) disables it. The power key is **never** grabbed: it
is the only way out of a blank.

Verify / drive it from a shell:

```
journalctl -t erhai-folio -b | tail            # FOLDED/UNFOLDED, GRABBED/RELEASED
journalctl -u erhai-power-toggle -b | tail -20 # guard lines while blanked
ls -l /run/erhai-panel-off /run/erhai-folio-fold-blanked
```

Tests (workspace `tmp/folio-test/`): `selftest.sh` (26 cases: state machine +
the ownership rules H-K), `ownership-test.py` (15), `powerwatch-guard-test.py`
(16), `live-grab-test.py` (9 — grabs the REAL nodes as root and proves with a
second `EVIOCGRAB` that they are exclusively held, then released).

## The panel blank is high priority and immediate (2.16)

*"It can take several seconds before the screen turns off when a process with
high CPU load is running"* — and a resume from s2idle looked faster than the
power button. Both were real, and both were ordering/priority, not firmware:

| | before 2.16 | 2.16 |
|---|---|---|
| actions on `off` | park GPU (budget **10 s**) → `SetActive true` → **sleep 2** → backlight off | **backlight off** → marker → bounded park (**1.5 s**) → `SetActive true` |
| actions on `on` | `SetActive false` → **sleep 3** → backlight on → GPU pin → **sleep 1** | `SetActive false` → backlight on → GPU pin |
| the GPU park | waits for a runtime suspend that never comes while the desktop renders (measured >5.5 s idle, 1.7 s under 8 threads) | unchanged ordering (it still precedes the DPU teardown, which is what fixed the silent POR), only the wait is bounded |
| priority | nice 0, same as any compile | service `Nice=-20` + CPU/IO weight, scripts renice themselves to −20 |
| compositor call | `runuser` (PAM): 101 ms under load | `setpriv`: 26 ms |
| `say`/`snap` logging | `date`+`cut`+`tee`, `cat`/`grep`/`awk` per line | bash builtins + redirects |
| the dmesg tail | 53–168 ms on the user path | backgrounded |

Measured after: **backlight dark 16 ms idle / 47 ms under 8 CPU threads**, lamp
restored **13 ms / 35 ms**, and the whole `off` stays under 0.5 s even when the
GPU never parks (it used to be 2–12 s). The `on` path copies what the s2idle
harness has always done at STEP6 (`SetActive false` → backlight on back-to-back);
while the DSI rail is down the lit lamp shows nothing, and `ON_SETTLE_MS` is
there if a flash is ever reported. Nothing in the park-before-teardown order that
fixed the display-off POR was changed — only how long it waits.

Tools (workspace `tmp/panel-latency/`): `order-test.sh` (19 cases — stub-driven
ordering, latency, ownership guards), `probe.sh` (per-stage timings; `LOAD=1`
runs it under 8 sysbench threads), `measure-e2e.sh` (real path, `CONFIRM=1`,
clicks muted through a temporary display.conf).

## The GPU is never parked while an external display is connected (2.17)

The power-button blank parks the Adreno on purpose (`echo auto`) — the park is
what makes the GPU/MMCX collapse safe once the internal DPU has been torn down.
But a **DP monitor over USB-C keeps rendering** while the internal panel is off,
and this pad's policy is that the Adreno stays **pinned always-active**
(`erhai-gpu-pin.sh`, 2026-09-13 — the unpinned path is where the GMU resume hang
and the display silent POR came from). So the two must not meet:

| layer | what it does |
|---|---|
| `park_gpu()` in `erhai-panel-power.sh` | **refuses to park** while `card1-DP-1` reads `connected` and re-pins the GPU instead |
| the `do_off` / `do_on` external fallbacks | call `pin_gpu()` (which only writes when the value is not already `on`) |
| `etc/udev/rules.d/72-erhai-gpu-external.rules` → `usr/local/sbin/erhai-gpu-external-pin.sh` | on every `card*-DP-*` add/change uevent, re-pins the GPU if a monitor is attached and the GPU is not pinned — this covers a monitor plugged in **while the panel is blanked**, and is idempotent (two reads when nothing has to change) |

The postinst fires a `drm` trigger after install, so a monitor that is already
attached is handled immediately instead of waiting for the next hotplug.

Tests: `tmp/panel-latency/gpu-external-test.sh` (16 cases: pinned/parked,
already-pinned (no write), disconnected, no connector, second connector,
read-only control, and the rule's own content) and `order-test.sh` cases G–I
(external monitor: backlight-only blank, GPU stays/re-gets `on`, no compositor
teardown).

## Simulating heavy / full load

```
sudo bash tmp/panel-latency/load-and-measure.sh              # 8 sysbench threads, 120 s
sudo CONFIRM=1 bash tmp/panel-latency/load-and-measure.sh    # + measure the panel path
sudo LOAD=mixed CONFIRM=1 bash tmp/panel-latency/load-and-measure.sh   # + memory load
sudo GPU=1 CONFIRM=1 bash tmp/panel-latency/load-and-measure.sh        # + fullscreen glmark2
```

Without `CONFIRM=1` it only starts the load and tells you what to watch while you
press the power button yourself. The CPU arm alone reproduces the contention
(the panel path is CPU/fork bound); `GPU=1` adds a real GL ES workload **with the
thermal harness's GPU-hang watchdog** (GL only — never OpenCL/llama.cpp on this
Adreno while a session is up). The project's own full-load tool remains
`tmp/thermal-stress-profiles.sh` (CPU profiles × optional GPU).

