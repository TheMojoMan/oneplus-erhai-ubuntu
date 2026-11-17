# AV1 hardware-decode patches for the /usr/local drop-in builds

The erhai VA-API stack lets the *client* tell the driver how a picture refreshes
the reference frame map.  Without that, iris-vaapi has to guess the DPB from the
reference map it receives, which is correct for simple streams and wrong for
real ones (the AV1 samples here lost references, re-emitted the sequence header
mid-stream and stalled).

Two libraries carry the information; the third patch is a separate ffmpeg fix:

| patch | project | what it does |
|---|---|---|
| `0001-libva-av1-carry-refresh-frame-flags.patch` | libva 2.23.0 | adds `refresh_frame_flags` + `refresh_frame_flags_valid` to `VADecPictureParameterBufferAV1` and passes them through |
| `0002-ffmpeg-vaapi-av1-carry-refresh-frame-flags.patch` | ffmpeg 8.0.1 | fills those fields from the AV1 frame header in `vaapi_decode.c` |
| `0006-ffmpeg-vaapi-av1-fix-loop-restoration-type.patch` | ffmpeg 8.0.1 | maps the loop-restoration frame type the way VA-API expects it |

`build-libva-av1fix-deb.sh` and `build-ffmpeg-av1fix-deb.sh` apply them to the
upstream release tarballs and package the result as `/usr/local` drop-ins
(`libva-av1fix`, `ffmpeg-av1fix`), so Ubuntu's own packages stay untouched.
The chromium equivalent (`0003-...chromium`) is NOT shipped: chromium comes
from the xtradeb PPA and decodes AV1 through its V4L2 path on this device, so
patching a third-party package buys nothing.

Patent/ABI note: libva's structure only grows at its end (padding), so the
drop-in stays ABI-compatible with the distro `libva.so.2`; ffmpeg is built
against the patched headers, hence the ffmpeg package depends on libva-av1fix.
