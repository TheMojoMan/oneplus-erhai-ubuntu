# oneplus-erhai-mutter — the erhai patched Mutter library set

Fixes a NULL dereference in Mutter's Wayland touch code that **crashed the whole
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

## What ships

Ubuntu-named packages, like the mesa `/usr` replacement — all at
`<base>+erhai1` (currently `50.1-0ubuntu2.4+erhai1`):

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
(md5 **`fcb8a6c21a8d9820fc68b83b28dd2040`**); `build-mutter-deb.sh` verifies that
md5 before packaging and also reports whether the guard instruction
`cbz x2, 0x18d2c0` is at offset `0x18d208`.

To rebuild it from source: `apt-get source mutter`, apply
`touch-drag-null-guard.patch`, build with the env above, then

    objcopy --only-keep-debug src/libmutter-18.so.0.0.0 libmutter-18.debug
    strip --strip-unneeded   src/libmutter-18.so.0.0.0

copy the result here and update `EXPECT_LIB_MD5` in `build-mutter-deb.sh`.
The debug info for the patched build's OWN build-id is on the pad at
`/usr/lib/debug/.build-id/0d/5a3252…debug`; the ddebs dbgsym package does **not**
match the patched library (its `Build-Ids:` are the unpatched build's).

## Build / publish

    ./build-mutter-deb.sh                        # build + publish to the TESTING suite
    NO_PPA=1 ./build-mutter-deb.sh               # build locally only
    PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-mutter-deb.sh    # straight to the public MAIN suite
    BASE=50.1-0ubuntu2.5 ./build-mutter-deb.sh   # after a new Ubuntu mutter update

The pristine Ubuntu debs it packages are fetched from the archive (sha256-checked
against the apt metadata) and kept in `rollback/` for offline rollback.

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
