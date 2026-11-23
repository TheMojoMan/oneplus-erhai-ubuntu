# AV1 hardware-decode patches for the private /opt client stack

The erhai VA-API stack lets the *client* tell the driver how a picture refreshes
the reference frame map.  Browsers never need this: they submit framed OBUs and
the driver parses the encoder's refresh set out of the blob.  libavcodec (mpv,
VLC, ffplay) submits headerless tile payloads, so the set can only arrive as
data - and VA-API's `VADecPictureParameterBufferAV1` has no field for it.

Two libraries carry the information; the third patch is a separate ffmpeg fix:

| patch | project | what it does |
|---|---|---|
| `0001-libva-av1-carry-refresh-frame-flags.patch` | libva 2.23.0 | adds `refresh_frame_flags` + `refresh_frame_flags_valid` to `VADecPictureParameterBufferAV1` and passes them through |
| `0002-ffmpeg-vaapi-av1-carry-refresh-frame-flags.patch` | ffmpeg 8.0.1 | fills those fields from the AV1 frame header in `vaapi_decode.c` |
| `0006-ffmpeg-vaapi-av1-fix-loop-restoration-type.patch` | ffmpeg 8.0.1 | maps the loop-restoration frame type the way VA-API expects it |

`../build-av1fix-deb.sh` applies them to the upstream release tarballs, builds
both projects with the prefix `/opt/oneplus-erhai-av1fix` and packages the
result as `oneplus-erhai-av1fix`.  Nothing lands in the system library path:
Ubuntu keeps owning `/usr/lib`, and only the player wrappers
(`/usr/local/bin/mpv`, `/usr/local/bin/ffplay`, the diverted `/usr/bin/vlc`)
prepend `/opt/oneplus-erhai-av1fix/lib` to `LD_LIBRARY_PATH`.  The driver
(`oneplus-erhai-video`) is built against the patched `va/` headers this package
ships, which `build-video-deb.sh` finds through
`PKG_CONFIG_PATH=/opt/oneplus-erhai-av1fix/lib/pkgconfig`.

The chromium equivalent (`0003-...chromium`) is NOT shipped: chromium comes
