VLC 4 for erhai (monolithic, release-bound to the resolute suite)
=================================================================

Upstream tree: `/media/data/develop/vlc` (out of this workspace, VLC 4.0 git).
`build-vlc-deb.sh` only stages it: `make install DESTDIR=<scratch>` into
`tmp/vlc-deb-stage`, trims dev artifacts, computes Depends, packs.

Configure the tree once, then keep it current:

    ./configure --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu \
                --enable-vaapi --enable-avcodec --disable-kde
    make -j$(nproc)
    ./oneplus-erhai-ubuntu/resolute/build-vlc-deb.sh      # NO_PPA=1 by hand

Why prefix `/usr` and not `/usr/local`: the package must own `/usr/bin/vlc`,
`/usr/share/vlc` and `/<libdir>/vlc/plugins` exactly as Ubuntu's vlc does, so
`dpkg -S`/`dpkg -V` and the plugin lookup agree.  A `/usr/local` build shadows
the package in PATH *and* has its own compiled plugin path -- never mix them.

Version scheme: `4.0.0~git<date>.<commits>+<sha>-0erhai<N>`.  The `~` makes any
real 4.0.0/4.0.x release sort higher, so `apt upgrade` replaces this package
with VideoLAN's/Ubuntu's own as soon as one exists; meanwhile it outranks
Ubuntu's 3.0.23.  Test builds keep the version and add `-test-NN`.

AV1: this package contains no wrapper.  `oneplus-erhai-av1fix` owns the
dpkg-diverted shim at `/usr/bin/vlc` (real binary at `/usr/bin/vlc.erhai-orig`),
puts `/opt/oneplus-erhai-av1fix/lib` in front of `LD_LIBRARY_PATH`, and its
dpkg file trigger re-installs the shim after every vlc install/upgrade.  It also
warns at dpkg time if a VLC links a `libavcodec.so` the private stack does not
carry (then the AV1 refresh set is lost until av1fix is rebuilt against it).

Packaging shape: one deb, no vlc-data/vlc-bin/plugin split, so it
Breaks/Replaces/Conflicts the Ubuntu vlc 3 set.  Runtime payload only: no
headers, no `.pc`, no `.la`/`.a`, no unversioned `.so` symlinks (those names
belong to `libvlc-dev`).

A VLC 4 from the VideoLAN archive (package name `vlc`) supersedes this one
normally, and the av1fix trigger keeps the private stack in place.
