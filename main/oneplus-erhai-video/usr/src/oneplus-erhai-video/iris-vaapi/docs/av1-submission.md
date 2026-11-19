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
