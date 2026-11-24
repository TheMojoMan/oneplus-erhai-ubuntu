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
private stack through small wrappers:

  /usr/local/bin/mpv, /usr/local/bin/ffplay
  /usr/bin/vlc            (the real binary is /usr/bin/vlc.erhai-orig,
                           installed with dpkg-divert)

Each wrapper only prepends /opt/oneplus-erhai-av1fix/lib to LD_LIBRARY_PATH
and then execs the distribution binary, so plugins and codecs stay the ones
the distribution ships.  A player that is started through its own absolute
path (not through the wrapper) decodes AV1 with the distribution stack: it
still uses the VPU, just without the client refresh set.

Verify

  LD_DEBUG=libs mpv --no-config --vo=null --ao=null --frames=1 file.mkv 2>&1 \
      | grep 'libva\.so'
      -> /opt/oneplus-erhai-av1fix/lib/libva.so.2

  mpv --msg-level=vd=v file.mkv
      -> "Using hardware decoding (vaapi)"

Removing this package restores /usr/bin/vlc and leaves the distribution stack
in place; oneplus-erhai-video only Recommends it.
