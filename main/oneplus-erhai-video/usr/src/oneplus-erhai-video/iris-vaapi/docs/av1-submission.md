# AV1 submission models

`iris-vaapi` feeds a *stateful* V4L2 decoder: the firmware parses whole access
units and owns the reference state.  VA-API, however, hands over one picture at
a time (parsed picture parameters + tile payloads).  How the two are bridged
matters for AV1, because AV1 hides roughly every second frame from the display
(alternate references, "ARF": `show_frame = 0`).

The Iris firmware only writes a capture buffer for a picture it believes is
displayed.  A hidden picture is decoded into the internal DPB only; the client
gets an empty completion.  That single fact drives both models below.

## Default: one synthesized header per picture (`IRIS_AV1_LIFT` unset)

Every picture is re-serialized from the VA parameters (`av1_params.c`), with
`show_frame` forced to 1 so a capture buffer always comes back.  The driver
mirrors the reference slots itself, remaps `ref_frame_idx` onto its own DPB,
parses the encoder's `refresh_frame_flags` out of the client blob when possible
and otherwise guesses them.  A hidden KEY frame is rewritten to
`INTRA_ONLY` so the decoder does not reset its reference order hints.

This works with every client, including the ones that submit headerless tile
payloads (libavcodec's hwaccel), and is what the shipped package uses.

## Firmware-native: `IRIS_AV1_LIFT=1`

Shown pictures travel **verbatim**: the encoder's own OBUs, including its frame
header (loop-restoration coefficients, global motion, coded order hints) and
the sequence header when it changes.  The firmware parses them and its DPB is
byte-for-byte the encoder's DPB.

Hidden pictures still have to be synthesized (they must be forced shown to
produce a capture buffer), but they use the *encoder's* slot numbering:
`refresh_frame_flags` comes from the parsed original header and
`ref_frame_idx` is passed through unchanged.  Every header - verbatim or
synthesized - therefore describes the same DPB.

An earlier attempt (2026-11-19, reverted) edited the `show_frame` bit inside
the encoder's own header for hidden pictures and kept synthesis for hidden key
frames.  Mixing "the firmware's DPB is the encoder's" with "the driver decides
what a hidden frame means" desynchronized the reference slots; the firmware
returned empty captures a few frames later and Chrome fell back to software.

## Test knobs

| variable | effect |
|---|---|
| `IRIS_AV1_LIFT=1` | the firmware-native model above |
| `IRIS_VAAPI_AV1_DUMP=/path/prefix` | write every submitted AU (`prefix.<ctx>.<au>`) |
| `IRIS_AV1_DUMP_ONLY=1` | build and dump AUs without touching the VPU |
| `IRIS_AV1_TILE_SIZES=0` | omit per-tile size prefixes |

Offline check of the dumped AUs (no VPU involved):

```sh
cat prefix.<ctx>.* | sort -t. -k3 -n | xargs cat > stream.obu   # au index order
aomdec -o out.y4m stream.obu
```

## What the vendor driver does (Snapdragon X1E / X2E / SM8750)

The `qualcomm-linux/video-driver` sources for the same VPU generations
(`iris3` = X1E/hamoa, `iris35` = SM8750/sun, `iris36` = X2E/glymur) document the
firmware contract this driver has to live with:

- AV1 needs at least 11 output buffers: the firmware may hold the OPB of a
  hidden picture until the `show_existing_frame` that displays it arrives
  (`msm_vidc_output_min_count_iris4`: "for AV1, always use firmware min count
  (~11) to avoid double decoding for SEF frames").  The V4L2 side keeps a floor
  of 32 (commit `faa3a3353a82`).
- AV1 + UBWC output must be told its stride/scanlines explicitly
  (`HFI_PROP_UBWC_STRIDE_SCANLINE`), which the kernel driver already does for
  `V4L2_PIX_FMT_AV1`.
- AV1 persist buffer carries per-picture ARP side data (`SIZE_AV1D_ARP`) in the
  vendor formulas; the kernel now budgets it too.
- A hidden picture's OPB has no film-grain information; only the DPB is kept.
  With `show_frame` forced to 1 the picture is complete instead, which is why
  the default model does not expose that limitation.

## Status of the firmware-native model (2026-11-19, negative)

`IRIS_AV1_LIFT=1` is **not usable on this VPU generation** and stays off.  Three
layers of the problem were peeled off in hardware runs:

1. every hidden picture re-emitted a sequence header (the writer compared the
   new config before folding the encoder's parsed flags in) - each reset the
   firmware's reference buffers;
2. the sequence-header parser read `frame_id_numbers_present_flag` wrong (it
   consumed the 4+3 frame-id bits unconditionally, shifting every following
   bit by 7) and parsed `color_config()` out of order, so `enable_cdef` and
   `separate_uv_delta_q` did not match the stream;
3. the writer hardcoded `enable_warped_motion = 1` while the stream signalled 0.
   That bit gates `allow_warped_motion` in *every* frame header, so a
   synthesized hidden picture carried an extra bit and the rest of its header
   was misparsed ("failed to decode tile data" in libaom as well as firmware
   errors).

After (1)-(3) the synthesized sequence header matches the encoder's in every
frame-relevant field (only the operating-point level and the colour
description differ, neither affects frame parsing) and the first hidden ARF
decodes.  What is left is not a flag: a hidden picture synthesized from VA
parameters still has no access to the DPB state of the pictures that travelled
verbatim - `SavedGmParams`, per-slot order hints, reference frame types - so
its global-motion prediction is written against a mirror that only ever saw
synthesized frames.  Pictures with `skip_mode_present`/`use_ref_frame_mvs`
then fail, which is where the hardware runs die (stall from the second ARF on).

Making this work needs one of:
- a firmware that holds the hidden picture's OPB until the SEF arrives (the
  X2E/iris36 behaviour) plus a matching kernel contract, so hidden pictures
  can travel verbatim;
- a full frame-header mirror of every verbatim picture (that is a decoder);
- an explicit, spec-legal `show_existing_frame` AU after each hidden one, so
  the firmware re-decodes the ARF for the client surface (costs one extra
  decode per ARF, ~1.5x at 4K - the vendor "old chipset" behaviour).

Until then the default single-model synthesized path is the supported one and
`IRIS_AV1_LIFT` is a debugging knob, not a feature.
