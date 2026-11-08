# oneplus-erhai-mutter — the erhai patched Mutter library set

Two source fixes in the compositor library, both in
`src/backends/native/meta-cursor-renderer-native.c` /
`src/wayland/meta-wayland-touch.c`:

1. **A NULL dereference in Mutter's Wayland touch code** that **crashed the whole
compositor** and looked like a "logout":

* `touch_can_grab_surface()` (`src/wayland/meta-wayland-touch.c`) was the only use
  of `touch_info->focus_surface` in that file without a NULL check, while
  `meta_wayland_touch_cancel()` clears the field and leaves the (cancelled) touch
  info in `touch->touches`.  So a client starting a **touch drag**
  (`wl_data_device.start_drag` — e.g. one finger dragging a file row in Files),
  while a cancelled touch was live, dereferenced NULL: `pc` inside
  `touch_can_grab_surface`, fault at address 0, reached from `wl_closure_invoke`
  → `meta_wayland_seat_get_grab_info()` → `data_device_start_drag()`, i.e.
  `org.gnome.Shell@ubuntu.service … status=11/SEGV` — the reported
  **"3-finger swipe in Files logs me out"**.  Owner report + full analysis:
  `nautilus-MEMORY-2026-10-15.md` §4-§6.
* The fix is one guard, `if (touch_info->focus_surface == NULL) return FALSE;`
  (`touch-drag-null-guard.patch`, applies cleanly to Ubuntu's
  `mutter 50.1-0ubuntu2.4` source with `patch -p1`).

2. **The hardware cursor is disabled while an external monitor is active**
   (`mutter-hw-cursor-external-monitor.patch`).  With the mirror design of
   `mirror-MEMORY-2026-11-07.md` §18 the panel keeps its native 3392x2400 timing
   and the DPU scales the desktop down to the sink, but its cursor pipe is a DMA
   pipe without a scaler: the pointer is written 1:1 into the sink and showed up
   ~2.2x too large (owner test 7).  Mutter's fallback is all-or-nothing (the
   composited cursor is only painted when NO view has a hardware cursor, and the
   cursor planes are enumerated at startup), so the driver cannot switch it off
   for the rescaled CRTC alone.  The patch treats an **active** non-builtin
   monitor like a hardware-cursor inhibitor: the compositor cursor is then
   scaled with the rest of the desktop and is correct on both outputs.  With only
   the built-in panel active the hardware cursor is kept, so pointer motion does
   not wake the GPU (owner decision 2026-11-07).

## What ships

Ubuntu-named packages, like the mesa `/usr` replacement — released at
`<base>+erhai1` (`50.1-0ubuntu2.5+erhai1`), test builds at `<base>+erhai2-test-NN`:

| package | contents |
|---|---|
| `libmutter-18-0` | Ubuntu's own deb with the patched + stripped library swapped in |
| `gir1.2-mutter-18` | Ubuntu's deb; only `Version` and its exact pin re-versioned |
| `libmutter-18-0-dbgsym` | Ubuntu's dbgsym; only `Version` and its exact pin re-versioned |

The two siblings exist because both pin the library EXACTLY,
`Depends: libmutter-18-0 (= <base>)`.  Without re-versioning them, `dpkg -i` of the
patched library leaves the system in a state dpkg tolerates but **apt refuses**:
every install/upgrade fails with *"You might want to run 'apt --fix-broken
install'"* — and that command would REMOVE `gnome-shell`, `gdm3`,
`ubuntu-session`, `ubuntu-desktop` and `gir1.2-mutter-18`.  **Never run it.**
`bump-mutter-siblings.sh` does the re-versioning (`--install` applies it to a live
pad, `--dbgsym-drop` purges the dbgsym instead, `BASE=` for a new base version).

## Why the patched library is a checked-in artifact

`dpkg-buildpackage` for mutter is not usable on this pad:

1. the user environment exports `PKG_CONFIG_PATH`/`LD_LIBRARY_PATH` from
   `~/.config/environment.d/90-alligator-kdesrc.conf`, so the build links the KDE
   tree's `libwayland` (build with `env -u PKG_CONFIG_PATH -u LD_LIBRARY_PATH`);
2. `dh_shlibdeps` then refuses regardless, because the mesa `/usr` replacement
   ships a `libgbm.so.1` that dpkg cannot map to `libgbm1`'s shlibs.

So the recipe is surgical: take Ubuntu's own deb, drop in the patched library,
bump the `Version`, regenerate md5sums, repack — the Depends list stays
byte-identical to Ubuntu's and no shlibdeps run is needed.  The patched, stripped
library is therefore stored here as `libmutter-18.so.0.0.0.patched`
(md5 **`6bd05928f69af786f2dc65769b394280`**, 3 119 424 B); `build-mutter-deb.sh`
verifies that md5 before packaging and refuses a library with a `/usr/local`
path baked in (the prefix bug of `mirror-MEMORY-2026-11-07.md` §12).

To rebuild it from source: `apt-get source mutter`, apply
`touch-drag-null-guard.patch` and `mutter-hw-cursor-external-monitor.patch`, then
build with the user environment cleaned (`env -u PKG_CONFIG_PATH -u
LD_LIBRARY_PATH`) and these options -- the prefix is NOT optional, meson's
default `/usr/local` pointed the X11 frames helper at a path that does not exist:

    meson setup build -Dprefix=/usr -Dsysconfdir=/etc -Dlocalstatedir=/var \
                      -Dtests=disabled -Dprofiler=false --buildtype=plain
    ninja -C build
    objcopy --only-keep-debug src/libmutter-18.so.0.0.0 libmutter-18.debug
    strip --strip-unneeded   src/libmutter-18.so.0.0.0

copy the result here and update `EXPECT_LIB_MD5` in `build-mutter-deb.sh`.
The debug info for the patched build's OWN build-id is on the pad at
`/usr/lib/debug/.build-id/0d/5a3252…debug`; the ddebs dbgsym package does **not**
match the patched library (its `Build-Ids:` are the unpatched build's).

## Build / publish

    ./build-mutter-deb.sh                        # build + publish into the resolute suite
    NO_PPA=1 ./build-mutter-deb.sh               # build locally only
    PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-mutter-deb.sh    # straight to the public MAIN suite
    BASE=50.1-0ubuntu2.5 ./build-mutter-deb.sh   # after a new Ubuntu mutter update

The pristine Ubuntu debs it packages are fetched from the archive (sha256-checked
against the apt metadata) and kept in `rollback/` for offline rollback.

Versioning: the released line is `+erhai1`; a test build keeps the released line
and adds `-test-NN` (`ERHAI_SUFFIX=+erhai2-test-01 ./build-mutter-deb.sh`), and
only the released build increments the number.  `+erhai2` was published and
withdrawn on 2026-11-07 (mirror-MEMORY-2026-11-07.md §11), so the next release
is **`+erhai3`** -- test builds must therefore be `+erhai2-test-NN` (they sort
above `+erhai2`), never `+erhai1-test-NN`.

## Rollback

    sudo dpkg -i oneplus-erhai-mutter/rollback/libmutter-18-0_<base>_arm64.deb \
                 oneplus-erhai-mutter/rollback/gir1.2-mutter-18_<base>_arm64.deb
    # the dbgsym, if installed, must go too (else apt is inconsistent again):
    #   sudo dpkg --purge libmutter-18-0-dbgsym   (or bump it with the script)

then re-login.

## Activation, and the next Ubuntu update

The library is mapped when gnome-shell starts, so the fix is live only after a
**re-login** (or a reboot).  `50.1-0ubuntu2.5+` compares HIGHER than
`50.1-0ubuntu2.4+erhai1`, so a future Ubuntu update installs Ubuntu's package and
the fix is gone: re-apply the patch and rebuild with `BASE=<new version>` (that
version ordering is also why `+erhai1` can never outrank a newer Ubuntu revision).
