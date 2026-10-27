# Vendoring / provenance

This directory is a **vendored copy** of upstream **iris-vaapi**
(<https://github.com/CFM880/iris-vaapi>), taken at upstream commit
`fe8aa30` — *"decode: serialize hardware sessions across seeks"* — with the
**erhai port changes applied in the tree**.

Until 2026-10-19 this path was a nested git clone, i.e. the parent repository
only recorded a gitlink (submodule reference) to it **without a `.gitmodules`
entry**, so a clone of `oneplus-erhai-ubuntu` got an EMPTY directory here.  The
files are plain files now; the upstream history is not carried along.

The erhai changes are also shipped as a reviewable diff next to this file:
[`erhai-changes-vs-upstream.patch`](erhai-changes-vs-upstream.patch) — 5 files,
**258 insertions / 23 deletions**:

| file | what the erhai changes do |
|---|---|
| `src/decode.c` | decode-order output: recycle the firmware's `ts == 0` internal pictures, recover from a firmware stall after an EOS drain (flush/seek), and fix the sync path (no eager flush when the client syncs the newest queued buffer; force the held target out after a genuine wait timeout) |
| `src/v4l2_dec.c` / `src/v4l2_dec.h` | resolve the Iris V4L2 node (`$IRIS_VAAPI_DEVICE` first, else scan by card/driver), handle the 10-bit profiles that also expose the plain YUV420 RT format, close-path fix, and the `IRIS_UNCLEAN_CLOSE` debug switch |
| `src/iris_vaapi.c` | profile → fourcc handling and `__vaDriverInit_1_23` tweaks |
| `src/dma_heap.h` (NEW) | minimal `/dev/dma_heap/system` allocator for the test harnesses that need dma-bufs |

`oneplus-erhai-video`'s `build-video-deb.sh` **rebuilds `iris_drv_video.so` from
this tree on every build** and packs this tree under
`/usr/src/oneplus-erhai-video/iris-vaapi/`, so the shipped driver always matches
the shipped source.
