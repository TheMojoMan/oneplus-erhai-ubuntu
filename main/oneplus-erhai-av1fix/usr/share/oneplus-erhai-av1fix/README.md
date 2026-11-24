oneplus-erhai-av1fix — private AV1 client stack for the erhai players
=====================================================================

The erhai VA-API driver (oneplus-erhai-video) decodes AV1 on the Qualcomm Iris
VPU.  Two kinds of client feed it, and only one of them can hand over the
picture's refresh set:

  * Browsers (Brave/Chromium, Firefox) submit whole framed OBUs.  The driver
    parses the encoder's refresh set out of the blob — they need nothing from
    this package.
  * libavcodec (mpv, VLC, ffplay) submits headerless tile payloads, so the
    refresh set can only arrive as data, and VA-API has no field for it.
    The driver then has to guess, and it guesses wrong on altref-heavy
    streams — visible as stuck or repeating pictures.

This package carries the two libraries that fill that gap:

  /opt/oneplus-erhai-av1fix/lib/libva.so.2
      libva 2.23.0 with refresh_frame_flags + refresh_frame_flags_valid added
      to VADecPictureParameterBufferAV1 (size-neutral: the fields live in the
      struct's existing tail padding, so a client built against stock headers
      still passes the size check).
  /opt/oneplus-erhai-av1fix/lib/libavcodec.so.62 (+ libav*)
      FFmpeg 8.0.1, built like a distribution package (VA-API, VDPAU,
      libdav1d, gnutls, full native codec set), forwarding the two fields and
      mapping the loop-restoration frame type as VA-API expects it.
  /opt/oneplus-erhai-av1fix/include/va/, .../lib/pkgconfig/
      The patched headers.  oneplus-erhai-video is built against them
      (PKG_CONFIG_PATH); they are never installed into /usr/include.

Nothing goes into the system library path: Ubuntu keeps owning /usr/lib, so
the ffmpeg tools keep their https and dav1d support and Firefox, Brave and
every other client run on the distribution libva.  The players reach the
private stack through one small shim, installed by the package's postinst at
each player's own absolute path:

  /usr/bin/vlc, /usr/bin/mpv, /usr/bin/ffplay
      the real binary moves to <path>.erhai-orig behind a dpkg diversion, so
      a client that calls the player by absolute path gets the private stack
      as well as one started from PATH

Each shim only prepends /opt/oneplus-erhai-av1fix/lib to LD_LIBRARY_PATH and
then execs the real binary, so plugins and codecs stay the ones the
distribution ships.  A dpkg file trigger (DEBIAN/triggers) installs the shim
again after any vlc/mpv/ffmpeg install, upgrade or removal, which is what keeps
the fix when a newer upstream VLC is installed.  The postinst also warns when a
player links a libavcodec.so the private stack does not carry (a player built
against a newer FFmpeg): decoding still works, but without the client refresh
set.

A source-built player under /usr/local/bin is wrapped the same way if it is
present when the package is configured.

Verify

  LD_DEBUG=libs mpv --no-config --vo=null --ao=null --frames=1 file.mkv 2>&1 \
      | grep 'libva\.so'
      -> /opt/oneplus-erhai-av1fix/lib/libva.so.2

  mpv --msg-level=vd=v file.mkv
      -> "Using hardware decoding (vaapi)"

Removing this package restores /usr/bin/vlc and leaves the distribution stack
in place; oneplus-erhai-video only Recommends it.

If a player is removed while this package stays installed, its shim reports the
missing binary and exits 127; the next `dpkg-reconfigure oneplus-erhai-av1fix`
(also run by any player install or upgrade) removes the shim and the diversion.
