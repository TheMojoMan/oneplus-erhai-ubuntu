# oneplus-erhai-gtk4 — the erhai patched libgtk-4 set

Fixes an **upstream GTK4/GDK crash** that closes GTK4 apps — on this pad, the
one the owner hit as *"3-finger swipe closes nautilus"*.

* `_gdk_wayland_seat_get_implicit_grab_serial()`
  (`gdk/wayland/gdkseat-wayland.c`) picks "the first sequence" out of
  `seat->touches` in its `device == logical_touch` branch **without checking that
  the table has one**:

  ```c
  else if (device == wayland_seat->logical_touch)
    {
      GdkWaylandTouchData *touch;
      GHashTableIter iter;
      /* Pick the first sequence */
      g_hash_table_iter_init (&iter, wayland_seat->touches);
      g_hash_table_iter_next (&iter, NULL, (gpointer *) &touch);
      return touch->touch_down_serial;      /* <-- SEGV when the table is empty */
    }
  ```

* GDK removes a touch when the **compositor cancels it** (`wl_touch.cancel`) —
  which is exactly what a GNOME Shell gesture does when it takes the sequence
  over (the 3-finger workspace swipe grabs it).  A `GtkDragSource` that is still
  updating at that moment (its internal drag gesture gets one more update) calls
  `gtk_drag_begin()` → `gdk_drag_begin()` with the now sequence-less touch
  device, lands in that branch with an EMPTY table, and the unguarded read
  dereferences NULL.
* **Measured on erhai 2026-10-15** (GTK 4.22.4+ds-0ubuntu0.1), fully symbolized
  with the ddebs dbgsym and a live gdb attach:

  ```
  #0 _gdk_wayland_seat_get_implicit_grab_serial  gdkseat-wayland.c:4574  (SEGV)
  #1 _gdk_wayland_surface_drag_begin             gdkdrag-wayland.c:419
  #2 gtk_drag_source_drag_begin                  gtkdragsource.c:610
  #3 gtk_drag_source_update                      gtkdragsource.c:296/313
  ...  gtk_gesture_handle_event -> gtk_main_do_event
  ```

  and Ubuntu's own shipped library has the unguarded instruction right where the
  crash PC pointed — `libgtk-4.so.1.2200.4 + 0x50b440: ldr w0, [x0, #32]`
  (= `touch->touch_down_serial`, `x0` = the untouched stack slot).  The patched
  build instead branches first (`0x50cf28: cbz w0, …`).
* The fix is one guard: only read `touch` when `g_hash_table_iter_next()`
  returned TRUE, else fall through and return 0 ("no implicit grab serial" — the
  draggers then pass serial 0 to `wl_data_device.start_drag`, which the
  compositor ignores).  `gtk-implicit-grab-null-guard.patch` applies cleanly to
  Ubuntu's `gtk4 4.22.4+ds-0ubuntu0.1` source (`patch -p1`, roundtrip-verified).
* Upstream's current `main` still has the unguarded read, so this is worth an
  upstream report; the erhai build carries it locally.

## What ships

Ubuntu-named packages, all at `<base>+erhai1` (currently
`4.22.4+ds-0ubuntu0.1+erhai1`):

| package | contents |
|---|---|
| `libgtk-4-1` | Ubuntu's own deb with the patched + stripped library swapped in (its `DEBIAN/md5sums` regenerated) |
| `libgtk-4-dev` | Ubuntu's deb; only `Version` and its exact `libgtk-4-1 (= <base>)` pin re-versioned |
| `libgtk-4-1-dbgsym` | Ubuntu's dbgsym; only `Version` + the same pin re-versioned (symbols inside are the UNPATCHED build's) |

`libgtk-4-dev` pins the library EXACTLY (`libgtk-4-1 (= <base>)`), and
`libgtk-4-1-dbgsym` pins it the same way, so installing the patched library
alone leaves apt refusing every install/upgrade ("You might want to run 'apt
--fix-broken install'", which would REMOVE the desktop).  **Never run it** —
bump the siblings instead (the builder does).  The dev package's pin on
`gir1.2-gtk-4.0` is deliberately LEFT at `<base>`: the typelibs are Ubuntu's and
unchanged, so `gir1.2-gtk-4.0` needs no rebuild.

## Why the patched library is a checked-in artifact

`dpkg-buildpackage` for gtk4 is not usable on this pad — `dh_shlibdeps` refuses
on the mesa `/usr` replacement's `libgbm.so.1` (the same reason the mutter set is
packaged surgically).  So the recipe is: build GTK4 from Ubuntu's source with the
guard applied, take the library, strip it, and swap it into Ubuntu's own deb:

```sh
apt-get source gtk4                       # 4.22.4+ds-0ubuntu0.1
cd gtk4-4.22.4+ds && patch -p1 < .../gtk-implicit-grab-null-guard.patch
apt-get build-dep -y gtk4
env -u PKG_CONFIG_PATH -u LD_LIBRARY_PATH meson setup _build \
    --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=plain \
    -Dc_args=-O2 -Dcpp_args=-O2 \
    -Dauto_features=enabled -Dbroadway-backend=true -Dx11-backend=true \
    -Dprint-cpdb=disabled -Dprint-cups=enabled -Dcolord=enabled \
    -Dman-pages=false -Dsysprof=enabled -Dwayland-backend=true \
    -Ddocumentation=false -Dbuild-demos=false -Dbuild-examples=false \
    -Dinstall-tests=false -Dcloudproviders=disabled -Dbuild-tests=false \
    -Dintrospection=disabled
env -u PKG_CONFIG_PATH -u LD_LIBRARY_PATH ninja -C _build
objcopy --only-keep-debug _build/gtk/libgtk-4.so.1.2200.4 libgtk-4.debug
strip --strip-unneeded        _build/gtk/libgtk-4.so.1.2200.4
```

(the meson flags mirror Ubuntu's `debian/rules`; `-Dman-pages/-Dbuild-tests/
-Dintrospection=disabled` only skip work we do not ship).  The stripped result
lives here as **`libgtk-4.so.1.2200.4.patched`**, md5
**`6095562e07c3aa9c741c5c91a48b9bb6`** — the builder verifies it before packing
(`EXPECT_LIB_MD5`) and also reports whether the guard branch is present
(non-fatal, the md5 is authoritative).  The unstripped build's debug info is kept
## Build / publish / install

    ./build-gtk4-deb.sh                     # build + copy into the TESTING suite
    NO_PPA=1 ./build-gtk4-deb.sh            # build locally only
    ./build-gtk4-deb.sh --install           # build + dpkg -i all three
    PPA_DEB_DIR="$PPA_MAIN_DEB_DIR" ./build-gtk4-deb.sh      # straight to MAIN
    BASE=4.22.5+ds-0ubuntu0.1 ./build-gtk4-deb.sh            # after an Ubuntu update

The pristine Ubuntu debs it packages are fetched from the archive (sha256 checked
against the apt metadata) and kept in `rollback/` for offline rollback.

## Verification (2026-10-15, live)

The bug reproduces WITHOUT any finger: a GTK4 process that starts a drag after
its touch was cancelled dies a few seconds after start.  `tmp/ding-touch-probe/
nau-crash-capture.sh` runs Nautilus under gdb with `handle SIGSEGV stop print
nopass` and logs the backtrace.

| | runs | result |
|---|---|---|
| before the fix | 3 | **3 × SIGSEGV**, pc = Ubuntu's libgtk-4 `+0x50b440` |
| after the fix | 3 | **0 crashes**, the processes stayed alive |

After installing: the library md5 == the artifact, `apt-get -s check` rc=0,
`dpkg -V libgtk-4-1 libgtk-4-dev libgtk-4-1-dbgsym` silent, a normal Nautilus
window opens and stays up, no GTK warnings in the journal.

## Rollback

    sudo dpkg -i rollback/libgtk-4-1_<base>_arm64.deb \
                 rollback/libgtk-4-dev_<base>_arm64.deb \
                 rollback/libgtk-4-1-dbgsym_<base>_arm64.ddeb

then **re-login** (a running gnome-shell keeps the library it mapped at start;
every other GTK4 app picks up whichever library is on disk when it starts).

## Activation, and the next Ubuntu update

Every GTK4 process picks the new library up when it STARTS.  A future Ubuntu gtk4
update (`4.22.5+…`) compares HIGHER than `<base>+erhai1`, so it installs Ubuntu's
package and the guard is gone: re-apply the patch and rebuild with
`BASE=<new version>` (that ordering is also why `+erhai1` can never outrank a
newer Ubuntu revision — the safe failure mode is "the fix is upgraded away", not
"a stale library lingers").

## Note for future GTK4 work on this pad

`libgtk-4-dev` was installed here by `apt-get build-dep gtk4` (together with the
rest of the GTK4 dev stack: libadwaita-1-dev, libwebkitgtk-6.0-dev,
libvte-2.91-gtk4-dev, libportal-gtk4-dev, libglycin-gtk4-2-dev), so it is in the
exact-pin set on THIS pad even though a fresh image does not have it.  Purging it
would drag those other -dev packages out, so the builder bumps it instead.
at `tmp/gtk4-build/libgtk-4.debug`.